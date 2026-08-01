/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-streaming.c - FreeBSD-native ISO DMA for Digi 002/003
 *
 * Implements isochronous streaming using FreeBSD firewire stack's
 * native DMA API (fw_open_isodma, xferq, irx_enable/itx_enable).
 *
 * DOT encoding: each 32-bit PCM sample has byte 2 XOR-scrambled using
 * a stateful table-driven transformation discovered by Robin Gareus
 * and Damien Zammit in 2012.  Each data block has an extra quadlet at
 * the start for MIDI messages, followed by pcm_channels quadlets of
 * DOT-encoded audio data.
 *
 * TX (playback) queue flow:
 *   Driver fills chunks → stvalid
 *   fwohci_itxbuf_enable: stvalid → stdma, starts DMA
 *   fwohci_txbuf_update (interrupt): stdma → stfree, calls wakeup(it)
 *   Driver refill (callout): stfree → (fill) → stvalid, calls itx_enable
 *
 * NOTE: fwohci does NOT call a handler callback for TX (it only does
 * for RX).  All TX refilling is driven from the PCM callout.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/mbuf.h>
#include <machine/bus.h>

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>
#include <dev/firewire/fwdma.h>

#include <sys/lock.h>
#include <sys/mutex.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include "digi00x.h"

MALLOC_DECLARE(M_ALSA);
MALLOC_DEFINE(M_DG00X_ISO, "dg00x_iso", "digi00x ISO DMA buffers");

/* dg00x_iso_channel is defined in digi00x.h with void* fields.
 * Accessor macros cast to the proper FreeBSD firewire types. */
#define ISO_XFERQ(ch)	((struct fw_xferq *)(ch)->xferq)
#define ISO_FC(ch)	((struct firewire_comm *)(ch)->fc)
#define ISO_BX(ch)	((struct fw_bulkxfer *)(ch)->bulkxfer)
#define ISO_MB(ch)	((struct mbuf **)(ch)->mbufs)

#define CIP_HEADER_QUADLETS	2
#define CIP_FMT_AM		0x10
#define AMDTP_FDF_AM824		0x00

/* Max payload per ISO packet */
#define DG00X_ISO_PACKET_SIZE	2048

/*
 * Number of ISO chunks to buffer.  At 8000 ISO cycles/sec, each chunk
 * covers 125 us.  The callout refills at hz (typically 1 ms), so we
 * need at least hz/8000 ≈ 8 chunks.  Use 32 for a 4 ms safety margin.
 */
#define DG00X_ISO_NCHUNKS	32

/* ------------------------------------------------------------------ */
/* CIP header helpers                                                  */
/* ------------------------------------------------------------------ */

static inline void
dg00x_build_cip_header(uint32_t *hdr, unsigned int node_id,
		       unsigned int dbs, unsigned int dbc,
		       unsigned int fmt, unsigned int fdf, unsigned int syt)
{
	hdr[0] = (node_id & 0x3f) << 26 |	/* SID */
		 (dbs & 0xff) << 18 |		/* DBS */
		 0 << 16 |			/* FN=0 */
		 0 << 13 |			/* QPC=0 */
		 0 << 12 |			/* SPH=0 */
		 0 << 10 |			/* reserved */
		 (dbc & 0xff);			/* DBC */
	hdr[1] = (1 << 31) |			/* EOH */
		 (fmt & 0x3f) << 24 |		/* FMT */
		 ((fdf & 0xff) << 16) |		/* FDF */
		 (syt & 0xffff);		/* SYT */
}

/* ------------------------------------------------------------------ */
/* ISO DMA channel open/close                                          */
/* ------------------------------------------------------------------ */

static int
dg00x_iso_open(struct firewire_comm *fc, struct dg00x_iso_channel *ch,
	       int is_tx)
{
	struct fw_xferq *xferq;
	int i;

	ch->dmach = fw_open_isodma(fc, is_tx);
	if (ch->dmach < 0) {
		printf("digi00x: fw_open_isodma(%s) failed\n",
		       is_tx ? "tx" : "rx");
		return (-ENOMEM);
	}

	ch->fc = (void *)fc;
	xferq = is_tx ? fc->it[ch->dmach] : fc->ir[ch->dmach];
	ch->xferq = (void *)xferq;

	/* Configure xferq for ISO streaming with handler callbacks.
	 * FWXFERQ_HANDLER is needed for RX (ir->hand is called).
	 * For TX, fwohci calls wakeup(it) instead of a handler;
	 * refilling is driven from the PCM callout. */
	xferq->flag &= ~(FWXFERQ_MODEMASK | FWXFERQ_OPEN |
			 FWXFERQ_STREAM | FWXFERQ_CHTAGMASK);
	xferq->flag |= FWXFERQ_OPEN | FWXFERQ_STREAM |
		       FWXFERQ_HANDLER | FWXFERQ_EXTBUF;
	xferq->psize = DG00X_ISO_PACKET_SIZE;
	xferq->bnchunk = DG00X_ISO_NCHUNKS;
	xferq->bnpacket = 1;
	xferq->queued = 0;
	xferq->sc = (caddr_t)ch;

	/* Allocate bulkxfer array and mbufs.
	 * M_NOWAIT is required: dg00x_streaming_init is called from
	 * pcm_trigger which runs under CHN_LOCK.  Sleeping with a
	 * mutex held causes a kernel panic. */
	ch->bulkxfer = (void *)malloc(
	    sizeof(struct fw_bulkxfer) * DG00X_ISO_NCHUNKS,
	    M_DG00X_ISO, M_NOWAIT | M_ZERO);
	if (ch->bulkxfer == NULL)
		return (-ENOMEM);
	ch->mbufs = (void *)malloc(
	    sizeof(struct mbuf *) * DG00X_ISO_NCHUNKS,
	    M_DG00X_ISO, M_NOWAIT | M_ZERO);
	if (ch->mbufs == NULL) {
		free(ch->bulkxfer, M_DG00X_ISO);
		ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	STAILQ_INIT(&xferq->stfree);
	STAILQ_INIT(&xferq->stdma);
	STAILQ_INIT(&xferq->stvalid);

	/*
	 * Allocate multi-segment DMA data buffer for EXTBUF mode.
	 *
	 * fwohci_db_init() allocates the DMA descriptor buffer (dbch->am)
	 * but NOT the data buffer (xferq->buf).  In EXTBUF mode fwohci
	 * expects the driver to supply the data buffer via xferq->buf.
	 * fwdma_v_addr(xferq->buf, poffset) and fwdma_bus_addr() are
	 * called by both our driver (dg00x_fill_tx_chunk) and fwohci
	 * (fwohci_add_tx_buf / fwohci_itxbuf_enable) to access the
	 * virtual / bus address of each segment — without this
	 * allocation, those calls dereference NULL → panic → reboot.
	 *
	 * Each segment is DG00X_ISO_PACKET_SIZE bytes (2048), one
	 * segment per bulkxfer chunk (bnpacket == 1).
	 */
	xferq->buf = fwdma_malloc_multiseg(fc,
	    DG00X_ISO_PACKET_SIZE,	/* esize: bytes per index */
	    DG00X_ISO_PACKET_SIZE,	/* ssize: bytes per DMA segment */
	    DG00X_ISO_NCHUNKS,		/* nseg: number of segments */
	    M_NOWAIT);			/* flags: M_NOWAIT (safe under CHN_LOCK) */
	if (xferq->buf == NULL) {
		/* Cleanup already allocated mbufs on failure */
		int j;
		for (j = 0; j < DG00X_ISO_NCHUNKS; j++)
			if (ISO_MB(ch)[j] != NULL)
				m_freem(ISO_MB(ch)[j]);
		free(ch->mbufs, M_DG00X_ISO);
		ch->mbufs = NULL;
		free(ch->bulkxfer, M_DG00X_ISO);
		ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		struct mbuf *m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL) {
			/* Cleanup already allocated mbufs on failure */
			int j;
			for (j = 0; j < i; j++)
				m_freem(ISO_MB(ch)[j]);
			free(ch->mbufs, M_DG00X_ISO);
			ch->mbufs = NULL;
			free(ch->bulkxfer, M_DG00X_ISO);
			ch->bulkxfer = NULL;
			fwdma_free_multiseg(xferq->buf);
			xferq->buf = NULL;
			return (-ENOMEM);
		}
		m->m_len = DG00X_ISO_PACKET_SIZE;
		ISO_MB(ch)[i] = m;
		ISO_BX(ch)[i].mbuf = m;
		ISO_BX(ch)[i].start = mtod(m, caddr_t);
		ISO_BX(ch)[i].end = mtod(m, caddr_t) + DG00X_ISO_PACKET_SIZE;
		/*
		 * poffset identifies which segment of the multi-segment
		 * DMA buffer (xferq->buf) this chunk corresponds to.
		 * fwohci uses poffset in fwdma_sync_multiseg() to flush
		 * the correct segment before DMA.  Each chunk maps 1:1
		 * to a segment (bnpacket = 1).
		 */
		ISO_BX(ch)[i].poffset = i;
		STAILQ_INSERT_TAIL(&xferq->stfree, &ISO_BX(ch)[i], link);
	}
	xferq->bulkxfer = ISO_BX(ch);

	return (0);
}

static void
dg00x_iso_close(struct dg00x_iso_channel *ch)
{
	struct fw_xferq *xferq = ISO_XFERQ(ch);
	int i;

	ch->dmach = -1;

	if (ch->mbufs) {
		for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
			if (ISO_MB(ch)[i])
				m_freem(ISO_MB(ch)[i]);
		}
		free(ch->mbufs, M_DG00X_ISO);
		ch->mbufs = NULL;
	}
	if (ch->bulkxfer) {
		free(ch->bulkxfer, M_DG00X_ISO);
		ch->bulkxfer = NULL;
	}
	if (xferq) {
		/*
		 * Free the multi-segment DMA data buffer that was
		 * allocated in dg00x_iso_open().  This MUST happen
		 * before the channel is destroyed — after this point
		 * fwohci must not touch the DMA context.
		 */
		if (xferq->buf != NULL) {
			fwdma_free_multiseg(xferq->buf);
			xferq->buf = NULL;
		}
		xferq->flag &= ~(FWXFERQ_OPEN | FWXFERQ_HANDLER |
				 FWXFERQ_STREAM | FWXFERQ_EXTBUF);
		xferq->sc = NULL;
		xferq->hand = NULL;
	}
	ch->xferq = NULL;
	ch->fc = NULL;
}

/* ------------------------------------------------------------------ */
/* AMDTP fractional framing helper                                     */
/* ------------------------------------------------------------------ */

/*
 * FireWire runs at 8000 ISO cycles/sec.  To achieve the configured
 * sample rate, each ISO packet must carry rate/8000 frames on average.
 * The fractional remainder is distributed using a modulo accumulator so
 * that the long-term frame rate matches exactly.
 *
 * Called once per ISO packet; advances the cycle state.
 */
static unsigned int
dg00x_frames_this_packet(struct dg00x_pcm_stream *ps)
{
	unsigned int frames = ps->frames_per_packet;

	ps->frame_cycle += ps->frame_remainder;
	if (ps->frame_cycle >= 8000) {
		ps->frame_cycle -= 8000;
		frames++;
	}
	return (frames);
}

/* ------------------------------------------------------------------ */
/* Fill one ISO chunk with audio data from the PCM DMA buffer          */
/* ------------------------------------------------------------------ */

/*
 * Fill a single ISO chunk (one packet) with DOT-encoded AMDTP data.
 *
 * With FWXFERQ_EXTBUF, the fwohci transmits from the multi-segment DMA
 * buffer (xferq->buf), NOT from the mbuf attached to the bulkxfer.
 * Each chunk maps to one segment via bx->poffset.
 *
 * Segment layout (as expected by fwohci_txbufdb):
 *   [0..3]:   fw_isohdr quadlet (sy, len) — fwohci reads len for DMA count
 *   [4..7]:   CIP header quadlet 0 (SID, DBS, FN, QPC, SPH, DBC)
 *   [8..11]:  CIP header quadlet 1 (EOH, FMT, FDF, SYT)
 *   [12..]:   Data blocks: [MIDI] [PCM×nch] [MIDI] [PCM×nch] …
 *             where MIDI = 0x80000000 (placeholder — no MIDI data)
 *
 * - Calls dg00x_frames_this_packet() to determine how many audio frames
 *   this packet carries (rate/8000, with fractional distribution).
 * - Sets DBS = pcm_channels + 1 in the CIP header.
 * - Uses the current tx_dbc as the CIP DBC and advances it by the
 *   frame count (DBC counts data blocks, wrapping at 256).
 * - Advances hwptr and calls dg00x_pcm_update_position().
 */
static void
dg00x_fill_tx_chunk(struct snd_dg00x *dg00x, struct fw_xferq *xferq,
		    struct fw_bulkxfer *bx)
{
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	unsigned int dbs = ps->pcm_channels + 1;
	unsigned int frames, dbc;
	unsigned int bytes, pkt_len;
	struct fw_pkt *fp;
	uint32_t *payload;
	unsigned int i;

	frames = dg00x_frames_this_packet(ps);
	dbc = ps->tx_dbc;
	ps->tx_dbc = (dbc + frames) & 0xff;

	/*
	 * Total payload after the 8-byte isochronous header:
	 *   8 (CIP header) + frames * dbs * 4 (data blocks)
	 */
	pkt_len = 8 + frames * dbs * 4;

	/* Get pointer to the DMA buffer segment for this chunk */
	fp = (struct fw_pkt *)fwdma_v_addr(xferq->buf, bx->poffset);
	if (fp == NULL)
		return;

	/*
	 * Set the isochronous header quadlet.
	 *
	 * fw_pkt.stream layout in the uint32_t (LE bit numbering):
	 *   bits [0:15]  = len
	 *   bits [16:23] = chtag
	 *   bits [24:27] = tcode (0xa = isochronous data block)
	 *   bits [28:31] = sy
	 *
	 * The OHCI DMA engine reads this quadlet from host memory in
	 * byte-address order.  On little-endian x86 (the only platform
	 * this driver currently runs on), the uint32_t's LSB is at the
	 * lowest memory address, matching the bit layout above directly.
	 *
	 * On a hypothetical big-endian host, byteswap would be needed.
	 */
	fp->mode.ld[0] = pkt_len | (0xa << 24);

	/*
	 * Payload starts at fp->mode.stream.payload (offset 4 in segment).
	 * Layout: [CIP hdr q0] [CIP hdr q1] [MIDIq] [PCM×nch] [MIDIq] …
	 */
	payload = (uint32_t *)fp->mode.stream.payload;

	dg00x_build_cip_header(&payload[0],
	    dg00x->fwdev->fc->nodeid,
	    dbs, dbc,
	    CIP_FMT_AM, AMDTP_FDF_AM824, 0xffff);

	/* Zero the MIDI quadlet at the start of every data block.
	 * Data blocks start at payload[2] (after 2-quadlet CIP header). */
	for (i = 0; i < frames; i++)
		payload[CIP_HEADER_QUADLETS + i * dbs] = 0x80000000;

	/* DOT-encode PCM data.  PCM starts at payload[3] (after CIP[2] + MIDI[1]).
	 * dbs is the stride between consecutive data blocks (MIDI + PCM). */
	dot_write_pcm(&ps->dot,
	    &payload[CIP_HEADER_QUADLETS + 1],
	    (const int32_t *)ps->substream->runtime->dma_area +
	    (ps->hwptr / 4),
	    ps->pcm_channels, frames, dbs);

	bytes = frames * ps->pcm_channels * 4;
	ps->hwptr += bytes;
	if (ps->hwptr >= ps->buffer_bytes)
		ps->hwptr = 0;

	dg00x_pcm_update_position(ps, bytes);
}

/* ------------------------------------------------------------------ */
/* RX DMA handler — called from fwohci interrupt context               */
/* ------------------------------------------------------------------ */

static void
dg00x_rx_handler(struct fw_xferq *xferq)
{
	struct dg00x_iso_channel *ch;
	struct snd_dg00x *dg00x;
	struct dg00x_pcm_stream *ps;
	struct firewire_comm *fc;
	unsigned int frames, dbs, bytes;

	if (xferq == NULL || xferq->sc == NULL)
		return;

	ch = (struct dg00x_iso_channel *)xferq->sc;
	if (ch->ctx == NULL)
		return;

	dg00x = (struct snd_dg00x *)ch->ctx;
	ps = &dg00x->pcm_capture;
	fc = ISO_FC(ch);

	if (ps->active && ps->substream != NULL &&
	    ps->substream->runtime != NULL &&
	    ps->substream->runtime->dma_area != NULL &&
	    xferq->stproc != NULL) {
		struct fw_bulkxfer *bx = xferq->stproc;
		uint32_t *payload = mtod(bx->mbuf, uint32_t *);

		/*
		 * The device sends the same number of frames per
		 * ISO packet as we expect for TX (rate/8000 average).
		 * Use the same fractional framing logic.
		 */
		frames = dg00x_frames_this_packet(ps);
		dbs = ps->pcm_channels + 1;

		dot_read_pcm(&ps->dot,
			     (int32_t *)ps->substream->runtime->dma_area +
			     (ps->hwptr / 4),
			     payload + CIP_HEADER_QUADLETS + 1,
			     ps->pcm_channels, frames, dbs);

		bytes = frames * ps->pcm_channels * 4;
		ps->hwptr += bytes;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;

		dg00x_pcm_update_position(ps, bytes);
	}

	/* Recycle chunk */
	if (xferq->stproc != NULL) {
		struct fw_bulkxfer *bx = xferq->stproc;
		STAILQ_REMOVE(&xferq->stdma, bx, fw_bulkxfer, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
		xferq->stproc = NULL;
	}

	/* Re-enable DMA if idle and chunks available */
	if (!STAILQ_EMPTY(&xferq->stfree) &&
	    (xferq->flag & FWXFERQ_RUNNING) == 0)
		fc->irx_enable(fc, ch->dmach);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
dg00x_streaming_init(struct snd_dg00x *dg00x)
{
	struct firewire_comm *fc;
	int err;

	if (dg00x->fwdev == NULL || dg00x->fwdev->fc == NULL)
		return (-ENODEV);

	fc = dg00x->fwdev->fc;

	err = dg00x_iso_open(fc, &dg00x->iso_tx, 1);
	if (err < 0)
		return (err);
	dg00x->iso_tx.ctx = dg00x;
	dg00x->iso_tx.direction = SNDRV_PCM_STREAM_PLAYBACK;
	/* NOTE: hand is set but fwohci never calls it for TX.
	 * TX refilling is driven from the PCM callout. */
	ISO_XFERQ(&dg00x->iso_tx)->hand = NULL;

	err = dg00x_iso_open(fc, &dg00x->iso_rx, 0);
	if (err < 0) {
		dg00x_iso_close(&dg00x->iso_tx);
		return (err);
	}
	dg00x->iso_rx.ctx = dg00x;
	dg00x->iso_rx.direction = SNDRV_PCM_STREAM_CAPTURE;
	ISO_XFERQ(&dg00x->iso_rx)->hand = dg00x_rx_handler;

	/*
	 * Program the isochronous channel numbers into the xferq flags.
	 *
	 * dg00x_iso_open() clears FWXFERQ_CHTAGMASK but does not set the
	 * channel.  fwohci reads the channel from the flag bits:
	 *   - TX: fwohci_txbufdb() uses  chtag = xferq->flag & 0xff   to
	 *     transmit on that channel (overriding the chtag field in
	 *     the packet header).
	 *   - RX: fwohci_irx_enable() uses ich = xferq->flag & 0x3f for
	 *     the OHCI context channel match.
	 * With the flags left at zero, TX went out on channel 0 (which
	 * happens to match the hardcoded tx_resources.channel=0) but RX
	 * matched channel 0 while the device transmits on channel 1 —
	 * capture could never receive anything.
	 */
	ISO_XFERQ(&dg00x->iso_tx)->flag =
	    (ISO_XFERQ(&dg00x->iso_tx)->flag & ~FWXFERQ_CHTAGMASK) |
	    (dg00x->tx_resources.channel & FWXFERQ_CHTAGMASK);
	ISO_XFERQ(&dg00x->iso_rx)->flag =
	    (ISO_XFERQ(&dg00x->iso_rx)->flag & ~FWXFERQ_CHTAGMASK) |
	    (dg00x->rx_resources.channel & FWXFERQ_CHTAGMASK);

	return (0);
}

void
dg00x_streaming_fini(struct snd_dg00x *dg00x)
{
	if (dg00x->iso_tx.dmach >= 0) {
		ISO_FC(&dg00x->iso_tx)->itx_disable(
		    ISO_FC(&dg00x->iso_tx), dg00x->iso_tx.dmach);
		dg00x_iso_close(&dg00x->iso_tx);
	}
	if (dg00x->iso_rx.dmach >= 0) {
		ISO_FC(&dg00x->iso_rx)->irx_disable(
		    ISO_FC(&dg00x->iso_rx), dg00x->iso_rx.dmach);
		dg00x_iso_close(&dg00x->iso_rx);
	}
}

int
dg00x_streaming_start_tx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_tx;
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int i;

	if (ch->dmach < 0 || !ps->active)
		return (0);

	/* Safety: don't start if the PCM DMA buffer isn't yet mapped */
	if (ps->substream == NULL ||
	    ps->substream->runtime == NULL ||
	    ps->substream->runtime->dma_area == NULL)
		return (-EINVAL);

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	if (fc == NULL || xferq == NULL)
		return (-ENODEV);

	/*
	 * Drain all chunks back to stfree, fill them with audio data,
	 * then enable TX DMA in a single sequence.
	 *
	 * Holding FW_GLOCK across the drain+fill prevents any in-flight
	 * TX completion interrupts from a previous session from
	 * concurrently modifying the xferq queues.
	 *
	 * A single itx_enable() call handles both the DMA descriptor
	 * allocation (fwohci_db_init) and the chunk move (stvalid →
	 * stdma via fwohci_add_tx_buf).  This MUST NOT be split into
	 * two itx_enable calls: the first call with an empty stvalid
	 * triggers fwohci_db_init with M_NOWAIT, and if the descriptor
	 * buffer allocation fails, fwohci_db_init accesses the NULL
	 * pointer's dma_tag field (offset 0x18) → page fault → reboot.
	 */
	FW_GLOCK(fc);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stdma);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stvalid);
	dot_reset_state(&ps->dot);

	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL)
			break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);

		dg00x_fill_tx_chunk(dg00x, xferq, bx);

		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
	}
	FW_GUNLOCK(fc);

	/* Single itx_enable: allocates descriptors + starts DMA */
	return (fc->itx_enable(fc, ch->dmach));
}

int
dg00x_streaming_start_rx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_rx;

	if (ch->dmach < 0)
		return (-ENODEV);

	dot_reset_state(&dg00x->pcm_capture.dot);
	return ISO_FC(ch)->irx_enable(ISO_FC(ch), ch->dmach);
}

void
dg00x_streaming_stop_tx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_tx;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;

	if (ch->dmach < 0)
		return;

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	/*
	 * Hold dg00x->lock while disabling DMA and draining queues.
	 * This serialises with dg00x_streaming_refill_tx() which also
	 * holds dg00x->lock across the fill+enable sequence, preventing
	 * concurrent manipulation of xferq queues and preventing a
	 * use-after-free of the DMA descriptor buffer (xferq->buf)
	 * that could otherwise occur when refill calls itx_enable
	 * while stop calls itx_disable.
	 */
	mtx_lock(&dg00x->lock);

	/* Disable DMA first.  After itx_disable returns, the OHCI
	 * context is quiesced — no new TX completion interrupts. */
	fc->itx_disable(fc, ch->dmach);

	/*
	 * Hold FW_GLOCK while draining stdma/stvalid back to stfree.
	 * An in-flight TX completion interrupt may still be running
	 * on another CPU moving stdma → stfree concurrently.  The
	 * lock serialises our cleanup against that last interrupt.
	 */
	FW_GLOCK(fc);

	while ((bx = STAILQ_FIRST(&xferq->stdma)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stdma, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}

	FW_GUNLOCK(fc);

	mtx_unlock(&dg00x->lock);
}

void
dg00x_streaming_stop_rx(struct snd_dg00x *dg00x)
{
	if (dg00x->iso_rx.dmach >= 0)
		ISO_FC(&dg00x->iso_rx)->irx_disable(
		    ISO_FC(&dg00x->iso_rx), dg00x->iso_rx.dmach);
}

/*
 * Refill TX chunks: move completed chunks from stfree to stvalid
 * (after filling with fresh audio data) and restart the DMA if
 * it has stalled.  Called from the PCM callout at ~1 ms intervals.
 *
 * Holds dg00x->lock across the entire operation, serialising with
 * dg00x_streaming_stop_tx() which also holds dg00x->lock while
 * calling itx_disable.  This prevents the callout's itx_enable from
 * racing with the stop path's itx_disable — the mutual exclusion
 * guarantees that only one thread manipulates the xferq queues and
 * the DMA descriptor buffer at any time.
 *
 * FW_GLOCK(fc) is held while touching the xferq STAILQ queues
 * (stfree, stvalid, stdma) to serialise with the fwohci TX completion
 * interrupt handler which also modifies stfree under FW_GLOCK.
 *
 * Lock ordering: dg00x->lock → FW_GLOCK.  Both the refill and stop
 * paths follow this order, preventing deadlock.
 */
void
dg00x_streaming_refill_tx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_tx;
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int refilled = 0;

	if (ch->dmach < 0)
		return;

	/* Hold dg00x->lock across the ENTIRE refill+enable sequence.
	 * This prevents the stop path (which also holds dg00x->lock
	 * across itx_disable) from interleaving between our queue
	 * manipulation and itx_enable call.  Without this, a race
	 * can occur where itx_enable re-allocates the DMA descriptor
	 * buffer after itx_disable has freed it, or where both threads
	 * manipulate the STAILQ queues concurrently → list corruption
	 * → kernel panic. */
	mtx_lock(&dg00x->lock);

	if (!ps->active)
		goto out_unlock;

	if (ps->substream == NULL ||
	    ps->substream->runtime == NULL ||
	    ps->substream->runtime->dma_area == NULL)
		goto out_unlock;

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	/* Lock the firewire xferq to prevent the TX completion
	 * interrupt handler from concurrently modifying stfree.
	 * fwohci_txbuf_update runs under FW_GLOCK and moves
	 * chunks from stdma → stfree. */
	FW_GLOCK(fc);

	while ((bx = STAILQ_FIRST(&xferq->stfree)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);

		dg00x_fill_tx_chunk(dg00x, xferq, bx);

		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
		refilled++;
	}

	FW_GUNLOCK(fc);

	/*
	 * Feed the TX DMA context on EVERY refill that moved chunks to
	 * stvalid, not just when FWXFERQ_RUNNING is clear.
	 *
	 * fwohci only clears FWXFERQ_RUNNING in fwohci_itx_disable().
	 * It does NOT clear it when the isochronous transmit chain runs
	 * to its end and the context goes idle.  Gating on the flag
	 * therefore meant: after the initial 32 chunks were transmitted,
	 * the callout kept refilling stfree → stvalid but never called
	 * itx_enable again, the DMA context was never restarted, no new
	 * packets were sent, hwptr stopped advancing, and playback
	 * froze after a few milliseconds.
	 *
	 * fwohci_itxbuf_enable() is designed to be called repeatedly
	 * (see fwdev.c fw_write(), which calls it on every completed
	 * packet batch).  It takes splfw() + FW_GLOCK internally, moves
	 * stvalid → stdma, chains the new descriptors onto the running
	 * context, and if the context has stopped it kicks it (start
	 * with a CYCLE_DELAY match when all chunks are buffered, or
	 * writes DMA_WAKE on underrun).  Calling it when there is
	 * nothing to submit is a no-op, so the refilled > 0 check is
	 * sufficient.
	 *
	 * dg00x->lock is still held here, preventing the stop path from
	 * concurrently calling itx_disable — so there is no race on
	 * xferq->buf and no possibility of re-enabling a freed
	 * descriptor buffer.
	 */
	if (refilled > 0)
		fc->itx_enable(fc, ch->dmach);

out_unlock:
	mtx_unlock(&dg00x->lock);
}
