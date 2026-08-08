/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-streaming.c - FreeBSD-native ISO DMA for Digi 002/003
 *
 * Implements isochronous streaming using FreeBSD firewire stack's
 * native DMA API (fw_open_isodma, xferq, irx_enable/itx_enable).
 *
 * DOT encoding: each 32-bit PCM sample has byte 2 XOR-scrambled using
 * a stateful table-driven transformation discovered by Robin Gareus
 * and Damien Zammit in 2012.  Each data block starts with one MIDI
 * quadlet, followed by pcm_channels quadlets of
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

#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include "digi00x.h"
#include "alsa_pcm_bsd.h"
#include "basound_debug.h"

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

/*
 * IEC 61883-6 (AMDTP/CIP) isochronous streams use TAG=1 in the
 * isochronous packet header ("data with CIP header").  In the fwohci
 * xferq flag's low byte (FWXFERQ_CHTAGMASK), bits 6-7 carry the tag,
 * bits 0-5 the channel — same layout as FreeBSD's fwdev.c ISO API
 * (it->flag |= (tag << 6)).
 */
#define DG00X_ISO_TAG_CIP	(1 << 6)

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
	/*
	 * Match Linux generate_cip_header() exactly for Digi 00x.
	 *
	 * The Digi 002/003 uses SPH=0, so there is no extra source packet
	 * header quadlet between the CIP header and the data blocks.
	 *
	 *   q0: SID=(node_id<<24) (bits 29-24),
	 *       DBS=(dbs<<16)     (bits 23-16), DBC (bits 7-0)
	 *   q1: EOH=0x80000000    (bit 31),
	 *       FMT=(fmt<<24)     (bits 29-24),
	 *       FDF=(fdf<<16)     (bits 23-16), SYT (bits 15-0)
	 */
	hdr[0] = htobe32(
	    ((node_id & 0x3f) << 24) |		/* SID */
	    ((dbs & 0xff) << 16) |		/* DBS */
	    (dbc & 0xff));			/* DBC */
	hdr[1] = htobe32(
	    (1u << 31) |			/* EOH */
	    (((unsigned int)fmt & 0x3f) << 24) |	/* FMT */
	    ((fdf & 0xff) << 16) |		/* FDF */
	    (syt & 0xffff));			/* SYT */
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
 *
 * This mirrors the Linux amdtp-stream.c pool_ideal_nonblocking_data_blocks()
 * exactly: for 44.1 kHz-based rates the fractional remainder is distributed
 * so that packets with a rounded-up block count occur as EARLY as possible
 * in the sequence.  The Digi 002/003 recovers its media clock from this
 * exact sequence ("The sequence of the number of data blocks per packet is
 * important for media clock recovery"), so a plain modulo accumulator
 * (which starts 5,6,5,6,... instead of 6,6,5,6,...) may prevent lock.
 *
 *   44100: 6 6 5 6 5 6 5 ...  (80-packet period)
 *   48000: 6 6 6 ...
 *   88200: 12 11 11 11 ...    (40-packet period)
 *   96000: 12 12 12 ...
 *
 * ps->frame_cycle is reused as the pattern phase counter.
 *
 * Called once per ISO packet; advances the phase state.
 */
static unsigned int
dg00x_frames_this_packet(struct dg00x_pcm_stream *ps)
{
	unsigned int phase;

	switch (ps->rate) {
	case 44100:
		phase = ps->frame_cycle;
		/* 5 + ((phase & 1) ^ (phase == 0 || phase >= 40)) */
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 80)
			ps->frame_cycle = 0;
		return (5 + ((phase & 1) ^
		    ((phase == 0 || phase >= 40) ? 1 : 0)));
	case 88200:
		phase = ps->frame_cycle;
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 40)
			ps->frame_cycle = 0;
		return (11 + ((phase == 0) ? 1 : 0));
	default:
		/* 48000/96000: rate/8000 is an integer. */
		return (ps->frames_per_packet);
	}
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
 *   [4..7]:   CIP header quadlet 0 (SID, DBS, DBC)
 *   [8..11]:  CIP header quadlet 1 (EOH, FMT, FDF, SYT)
 *   [12..]:   Data blocks: [MIDI] [PCM×nch] [MIDI] [PCM×nch] …
 *             where MIDI = 0x00000080 (memory order → 0x80 on the wire,
 *             the "no MIDI data" marker used by the Linux driver)
 *
 * - Calls dg00x_frames_this_packet() to determine how many audio frames
 *   this packet carries (rate/8000, with fractional distribution).
 * - Sets DBS = device_channels + 1 in the CIP header (the Digi 002/003
 *   always carries its full channel complement: 18 at 44.1/48 kHz,
 *   10 at 88.2/96 kHz, plus one MIDI quadlet per data block).
 * - Uses the current tx_dbc as the CIP DBC and advances it by the
 *   frame count (DBC counts data blocks, wrapping at 256).
 * - Advances hwptr and calls dg00x_pcm_update_position().
 */
static void
dg00x_fill_tx_chunk(struct snd_dg00x *dg00x, struct fw_xferq *xferq,
		    struct fw_bulkxfer *bx)
{
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	struct basound_chan *txch = ps->substream != NULL ?
	    ps->substream->private_data : NULL;
	unsigned int dbs = ps->device_channels + 1;
	unsigned int nch, frames, dbc;
	unsigned int sample_bytes;
	unsigned int bytes, pkt_len;
	struct fw_pkt *fp;
	uint32_t *payload;
	unsigned int i;

	/*
	 * Re-sync with the OSS layer's CURRENT buffer and format on
	 * every fill.  chn_resizebuf() can reallocate/remap the hardware
	 * buffer mid-stream (fragment renegotiation, overrun recovery,
	 * feeder reconfig) — the log shows repeated
	 * "chn_resizebuf(): PCMDIR_PLAY (hardware) timeout=37" during
	 * JACK operation.  Each resize can move the DMA buffer, leaving
	 * our cached dma_area/hwptr/buffer_bytes pointing at stale
	 * memory: the fill then reads garbage or the zero-fill heuristic
	 * misfires, producing periodic dropouts that sound like a slow
	 * (~1 Hz) volume modulation.  This is far more likely to trip at
	 * 18 channels where the buffers and per-packet byte counts are
	 * 9x larger than at 2 channels.
	 *
	 * Re-reading the current pointers on every fill is cheap (plain
	 * memory loads) and keeps hwptr, the zero-fill heuristic and
	 * pcm_pointer() consistent with the buffer the OSS layer is
	 * actually feeding.
	 */
	if (txch != NULL && txch->channel != NULL && txch->buffer != NULL) {
		/* Pick up any late format renegotiation (SNDCTL_DSP_*). */
		if (txch->channel->format != 0)
			txch->format = txch->channel->format;

		if (ps->substream->runtime != NULL) {
			ps->substream->runtime->dma_area =
			    txch->buffer->buf;
			ps->substream->runtime->dma_addr =
			    txch->buffer->buf_addr;
			ps->substream->runtime->dma_bytes =
			    txch->buffer->bufsize;
		}
		ps->buffer_bytes = txch->buffer->bufsize;
		ps->period_bytes = txch->blocksize;
		if (ps->buffer_bytes > 0)
			ps->hwptr %= ps->buffer_bytes;
	}

	/*
	 * Number of app channels to consume from the interleaved runtime
	 * buffer.  Never more than the device channel complement.
	 */
	nch = ps->pcm_channels;
	if (txch != NULL) {
		unsigned int fmt_ch = AFMT_CHANNEL(txch->format);

		if (fmt_ch != 0 && fmt_ch <= ps->device_channels) {
			nch = fmt_ch;
			ps->pcm_channels = nch;
		}
	}
	if (nch > ps->device_channels)
		nch = ps->device_channels;

	frames = dg00x_frames_this_packet(ps);
	dbc = ps->tx_dbc;
	ps->tx_dbc = (dbc + frames) & 0xff;

	sample_bytes = (txch != NULL) ? AFMT_BPS(txch->format) : 4;
	if (sample_bytes != 2 && sample_bytes != 4)
		sample_bytes = 4;

	bytes = frames * nch * sample_bytes;

	/*
	 * Total payload after the 8-byte isochronous header:
	 *   CIP header (8) + frames * dbs * 4
	 */
	pkt_len = 8 + frames * dbs * 4;

	/* Get pointer to the DMA buffer segment for this chunk */
	fp = (struct fw_pkt *)fwdma_v_addr(xferq->buf, bx->poffset);
	if (fp == NULL)
		return;

	fp->mode.stream.len = pkt_len;
	payload = (uint32_t *)fp->mode.stream.payload;

	dg00x_build_cip_header(&payload[0],
	    dg00x->fwdev->fc->nodeid,
	    dbs, dbc,
	    CIP_FMT_AM, ps->fdf, 0xffff);

	for (i = 0; i < frames; i++)
		payload[CIP_HEADER_QUADLETS + i * dbs] = 0x00000080;

	/*
	 * Read PCM data from the DMA ring at ps->hwptr, but do not call
	 * sndbuf_dispose() here.  The FreeBSD PCM layer consumes bufhard
	 * in chn_dmaupdate() after pcm_pointer() reports hwptr progress.
	 * Disposing here would consume the same bytes twice.
	 */
	{
		struct snd_dbuf *sb = txch ? txch->buffer : NULL;
		uint8_t srcbuf[18 * 12 * 4];
		int32_t tmpbuf[18 * 12];
		const int32_t *sp;
		unsigned int source_off = ps->hwptr;
		unsigned int ready_bytes = 0;
		unsigned int pending_bytes = 0;
		unsigned int read_bytes = 0;
		bool underrun = false;
		bool shortfall = false;
		static int zero_dbg;
		static int data_dbg;

		if (sb != NULL) {
			unsigned int rp = sndbuf_getreadyptr(sb);

			ready_bytes = sndbuf_getready(sb);
			if (source_off >= rp)
				pending_bytes = source_off - rp;
			else
				pending_bytes = ps->buffer_bytes - rp + source_off;

			if (pending_bytes > ready_bytes) {
				/* True underrun: hwptr has advanced past the
				 * last byte the OSS layer wrote.  Nothing to
				 * play — silence the whole packet. */
				underrun = true;
				read_bytes = 0;
			} else {
				/* Data ahead of hwptr.  If it is less than a
				 * full packet (the app wrote slightly late),
				 * read only what is available and zero-pad the
				 * tail instead of dropping the whole packet —
				 * a full-packet silence is an audible click,
				 * a few zero samples at a packet edge is not. */
				read_bytes = ready_bytes - pending_bytes;
				if (read_bytes > bytes)
					read_bytes = bytes;
				if (read_bytes < bytes)
					shortfall = true;
			}
		} else {
			underrun = true;
		}
		/* Round to whole samples so the conversion loops below never
		 * see a partial sample. */
		if (sample_bytes > 1)
			read_bytes -= read_bytes % sample_bytes;

		/*
		 * Debug test tone: synthesize audio directly instead of
		 * reading the DMA ring, so wire-level output can be
		 * verified independently of whatever data (or silence,
		 * e.g. `dd if=/dev/zero`) userspace happens to be writing.
		 * See hw.basound.debug.test_tone sysctl.
		 */
		if (basound_debug_tone_enabled()) {
			basound_debug_tone_fill_s32le(tmpbuf, frames, nch,
			    ps->rate);
			sp = tmpbuf;
		} else if (underrun) {
			if (zero_dbg < 8) {
				printf("digi00x: tx underrun fmt=0x%08x bps=%u "
				    "ready=%u pending=%u source_off=%u bytes=%u\n",
				    txch ? txch->format : 0, sample_bytes,
				    ready_bytes, pending_bytes, source_off, bytes);
				zero_dbg++;
			}
			memset(tmpbuf, 0, sizeof(tmpbuf));
			sp = tmpbuf;
		} else if (read_bytes == bytes && sample_bytes == 4 &&
		    source_off + bytes <= ps->buffer_bytes) {
			/* Fast path: full packet available, S32, no wrap. */
			sp = (const int32_t *)((const uint8_t *)
			    ps->substream->runtime->dma_area + source_off);
		} else {
			unsigned int samples = frames * nch;

			if (shortfall && zero_dbg < 8) {
				printf("digi00x: tx shortfall ready=%u pending=%u "
				    "source_off=%u bytes=%u read=%u\n",
				    ready_bytes, pending_bytes,
				    source_off, bytes, read_bytes);
				zero_dbg++;
			}

			/*
			 * Read read_bytes (possibly less than the full packet
			 * when the app is momentarily behind) from the ring,
			 * zero-padding the remainder of the sample buffer.
			 * Zeroing first means the tail samples are silence;
			 * only the available prefix carries real audio.
			 */
			if (sample_bytes == 4) {
				memset(tmpbuf, 0, sizeof(tmpbuf));
				if (read_bytes > 0) {
					if (source_off + read_bytes <= ps->buffer_bytes) {
						memcpy(tmpbuf,
						    (const uint8_t *)
						    ps->substream->runtime->dma_area +
						    source_off, read_bytes);
					} else {
						unsigned int first =
						    ps->buffer_bytes - source_off;
						memcpy(tmpbuf,
						    (const uint8_t *)
						    ps->substream->runtime->dma_area +
						    source_off, first);
						memcpy((uint8_t *)tmpbuf + first,
						    ps->substream->runtime->dma_area,
						    read_bytes - first);
					}
				}
				sp = tmpbuf;
			} else {
				unsigned int rd_smps = read_bytes / 2;

				/*
				 * Convert S16_LE → 24-bit top-justified in a
				 * 32-bit container.  dot_write_pcm_padded()
				 * extracts the wire 24-bit value with >> 8,
				 * so the 16-bit sample must be shifted by 16
				 * here to end up at full scale on the wire
				 * (sample << 16 → >> 8 → sample << 8).
				 *
				 * Shifting by only 8 (as was done before)
				 * produced a wire value of the unscaled
				 * 16-bit sample in the low 24 bits —
				 * ~48 dB too quiet, with the DOT encoder
				 * state machine keying off a byte-2 pattern
				 * that no longer matches full-scale data.
				 */
				for (i = 0; i < samples; i++)
					tmpbuf[i] = 0;
				if (rd_smps > 0) {
					if (source_off + read_bytes <= ps->buffer_bytes) {
						memcpy(srcbuf,
						    (const uint8_t *)
						    ps->substream->runtime->dma_area +
						    source_off, read_bytes);
					} else {
						unsigned int first =
						    ps->buffer_bytes - source_off;
						memcpy(srcbuf,
						    (const uint8_t *)
						    ps->substream->runtime->dma_area +
						    source_off, first);
						memcpy(srcbuf + first,
						    ps->substream->runtime->dma_area,
						    read_bytes - first);
					}
					const int16_t *src16 = (const int16_t *)srcbuf;

					for (i = 0; i < rd_smps; i++)
						tmpbuf[i] = ((int32_t)src16[i]) << 16;
				}
				sp = tmpbuf;
			}
		}

		if (!underrun && data_dbg < 8) {
			printf("digi00x: tx source fmt=0x%08x bps=%u ready=%u "
			    "pending=%u s0=0x%08x s1=0x%08x s2=0x%08x s3=0x%08x\n",
			    txch ? txch->format : 0, sample_bytes,
			    ready_bytes, pending_bytes,
			    (unsigned int)sp[0],
			    (unsigned int)((frames * nch) > 1 ? sp[1] : 0),
			    (unsigned int)((frames * nch) > 2 ? sp[2] : 0),
			    (unsigned int)((frames * nch) > 3 ? sp[3] : 0));
			data_dbg++;
		}

		/* Update peak meter from the source data */
		{
			unsigned int c, f;
			for (c = 0; c < ps->device_channels; c++) {
				uint32_t pk = 0;
				if (c < nch) {
					for (f = 0; f < frames; f++) {
						int32_t v = sp[f * nch + c] >> 8;
						uint32_t a = (v < 0) ?
						    (uint32_t)(-v) : (uint32_t)v;
						if (a > pk) pk = a;
					}
				}
				if (pk > atomic_load_acq_32(&ps->tx_peak[c]))
					atomic_store_rel_32(&ps->tx_peak[c], pk);
			}
		}

		/* DOT-encode into the FireWire DMA segment */
		dot_write_pcm_padded(&ps->dot,
		    &payload[CIP_HEADER_QUADLETS + 1],
		    sp, nch, ps->device_channels, frames, dbs);
	}

	/* hwptr tracks total bytes consumed for period_elapsed accounting */
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
	struct fw_bulkxfer *bx;
	unsigned int frames, dbs, bytes;
	int recycled = 0;

	if (xferq == NULL || xferq->sc == NULL)
		return;

	ch = (struct dg00x_iso_channel *)xferq->sc;
	if (ch->ctx == NULL)
		return;

	dg00x = (struct snd_dg00x *)ch->ctx;
	ps = &dg00x->pcm_capture;
	fc = ISO_FC(ch);

	/*
	 * fwohci_rbuf_update() moves each completed chunk from stdma to
	 * stvalid before calling ir->hand(), so the received packets are
	 * waiting in the stvalid queue.  The data lands in the EXTBUF DMA
	 * segments (fwdma_v_addr(xferq->buf, bx->poffset)), NOT in the
	 * mbuf — the mbuf is never populated in EXTBUF mode.
	 *
	 * The first descriptor quadlet of each RX chunk is a dummy that
	 * absorbs the isochronous packet header, so the segment starts at
	 * the CIP header: [CIP q0] [CIP q1] [MIDI] [PCM×nch] ...
	 *
	 * This handler runs with FW_GLOCK held (fwohci calls hand()
	 * while holding it), so the queue manipulation below is safe.
	 */
	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);

		if (ps->active && ps->substream != NULL &&
		    ps->substream->runtime != NULL &&
		    ps->substream->runtime->dma_area != NULL &&
		    xferq->buf != NULL) {
			struct basound_chan *rxch = ps->substream->private_data;
			unsigned int sample_bytes = (rxch != NULL) ?
			    AFMT_BPS(rxch->format) : 4;
			uint32_t *payload;
			unsigned int nch;

			if (sample_bytes != 2 && sample_bytes != 4)
				sample_bytes = 4;

			payload = (uint32_t *)fwdma_v_addr(xferq->buf,
							   bx->poffset);

			/*
			 * The device sends the same number of frames per
			 * ISO packet as we expect for TX (rate/8000
			 * average).  Use the same fractional framing.
			 *
			 * The device transmits its full channel complement
			 * per data block (18 at 44.1/48 kHz, 10 at 88.2/96
			 * kHz), so stride by device_channels + 1 and copy
			 * only the first nch channels into the app buffer.
			 *
			 * Skip CIP[2] + MIDI[1] = payload[2]; PCM starts
			 * at payload[3] (= CIP_HEADER_QUADLETS + 1).
			 */
			frames = dg00x_frames_this_packet(ps);
			dbs = ps->device_channels + 1;
			nch = ps->pcm_channels;
			if (nch > ps->device_channels)
				nch = ps->device_channels;

			bytes = frames * nch * sample_bytes;

			if (sample_bytes == 4 && ps->hwptr + bytes <= ps->buffer_bytes) {
				dot_read_pcm(&ps->dot,
				    (int32_t *)((uint8_t *)
					ps->substream->runtime->dma_area + ps->hwptr),
				    payload + CIP_HEADER_QUADLETS + 1,
				    nch, frames, dbs);
			} else {
				int32_t tmpbuf[18 * 12];
				unsigned int total_samples = frames * nch;
				unsigned int s;

				dot_read_pcm(&ps->dot, tmpbuf,
				    payload + CIP_HEADER_QUADLETS + 1,
				    nch, frames, dbs);

				if (sample_bytes == 4) {
					uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
					unsigned int first_bytes = ps->buffer_bytes - ps->hwptr;
					if (first_bytes >= bytes) {
						memcpy(dst, tmpbuf, bytes);
					} else {
						memcpy(dst, tmpbuf, first_bytes);
						memcpy(ps->substream->runtime->dma_area,
						    (uint8_t *)tmpbuf + first_bytes,
						    bytes - first_bytes);
					}
				} else {
					/*
					 * Convert 24-bit top-justified (in a
					 * 32-bit container, as produced by
					 * dot_read_pcm: wire << 8) back to
					 * S16_LE.  The wire 24-bit sample
					 * carries a full-scale 16-bit value
					 * in its top 16 bits (sample << 8),
					 * so after the << 8 from dot_read_pcm
					 * the sample sits at bit 24; >> 16
					 * recovers it exactly.
					 *
					 * Using >> 8 (as was done before)
					 * truncated the 24-bit wire value to
					 * its low 16 bits — for full-scale
					 * audio that is the sample's low
					 * byte plus noise, producing
					 * distorted capture.
					 */
					int16_t tmp16[18 * 12];
					for (s = 0; s < total_samples; s++)
						tmp16[s] = (int16_t)(tmpbuf[s] >> 16);

					uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
					unsigned int first_bytes = ps->buffer_bytes - ps->hwptr;
					if (first_bytes >= bytes) {
						memcpy(dst, tmp16, bytes);
					} else {
						memcpy(dst, tmp16, first_bytes);
						memcpy(ps->substream->runtime->dma_area,
						    (uint8_t *)tmp16 + first_bytes,
						    bytes - first_bytes);
					}
				}
			}

			ps->hwptr += bytes;
			if (ps->hwptr >= ps->buffer_bytes)
				ps->hwptr = 0;

			dg00x_pcm_update_position(ps, bytes);
		}

		/* Recycle the chunk for the next receive */
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
		recycled++;
	}

	/*
	 * Re-arm the RX context whenever chunks were recycled.
	 *
	 * fwohci only clears FWXFERQ_RUNNING in fwohci_irx_disable();
	 * it stays set when the receive chain runs to its end and the
	 * context goes idle.  Gating on the flag (as before) meant the
	 * context was never restarted and capture stalled after the
	 * first ring pass.  irx_enable() re-chains stfree chunks onto
	 * the context and restarts it if it stopped; with an empty
	 * stfree it is a no-op.
	 */
	if (recycled > 0)
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
	 * Program the isochronous channel numbers AND the CIP tag into
	 * the xferq flags.
	 */
	{
		uint32_t tx_flag = (DG00X_ISO_TAG_CIP |
		    (dg00x->tx_resources.channel & 0x3f));
		uint32_t rx_flag = (DG00X_ISO_TAG_CIP |
		    (dg00x->rx_resources.channel & 0x3f));

		printf("digi00x: streaming_init — TX flag=0x%02x (tag=%d ch=%d), "
		    "RX flag=0x%02x (tag=%d ch=%d), node_id=%d\n",
		    tx_flag, (tx_flag >> 6) & 3, tx_flag & 0x3f,
		    rx_flag, (rx_flag >> 6) & 3, rx_flag & 0x3f,
		    dg00x->fwdev->fc->nodeid);

		ISO_XFERQ(&dg00x->iso_tx)->flag =
		    (ISO_XFERQ(&dg00x->iso_tx)->flag & ~FWXFERQ_CHTAGMASK) | tx_flag;
		ISO_XFERQ(&dg00x->iso_rx)->flag =
		    (ISO_XFERQ(&dg00x->iso_rx)->flag & ~FWXFERQ_CHTAGMASK) | rx_flag;
	}

	return (0);
}

void
dg00x_streaming_fini(struct snd_dg00x *dg00x)
{
	dg00x->tx_use_count = 0;
	dg00x->rx_use_count = 0;

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

	if (ch->dmach < 0)
		return (0);

	/*
	 * TX must run even when no playback app has opened the
	 * substream: the Digi 002/003 will not transmit its capture
	 * audio unless the host is simultaneously transmitting to it
	 * (silence is fine).  dg00x_fill_tx_chunk() already handles
	 * ps->substream->runtime being NULL (or ps->active being
	 * false) by generating silent, correctly framed packets, so
	 * we must not gate starting TX on either condition here.
	 */

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	if (fc == NULL || xferq == NULL)
		return (-ENODEV);

	mtx_lock(&dg00x->lock);
	if (dg00x->tx_use_count++ > 0) {
		mtx_unlock(&dg00x->lock);
		return (0);
	}
	mtx_unlock(&dg00x->lock);

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

	printf("digi00x: start_tx — rate=%u, pcm_ch=%u, dev_ch=%u, "
	    "dbs=%u, dmach=%d, filling %d chunks\n",
	    ps->rate, ps->pcm_channels, ps->device_channels,
	    ps->device_channels + 1, ch->dmach, DG00X_ISO_NCHUNKS);

	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL)
			break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);

		dg00x_fill_tx_chunk(dg00x, xferq, bx);

		/* Dump first packet's full payload for verification */
		if (i == 0) {
			uint32_t *pl = (uint32_t *)
			    ((struct fw_pkt *)fwdma_v_addr(xferq->buf,
				bx->poffset))->mode.stream.payload;
			printf("digi00x: start_tx CIP q0=0x%08x q1=0x%08x\n",
			    be32toh(pl[0]), be32toh(pl[1]));
			printf("digi00x: start_tx MIDI[2]=0x%08x PCM[3]=0x%08x "
			    "[4]=0x%08x [5]=0x%08x\n",
			    be32toh(pl[2]), be32toh(pl[3]),
			    be32toh(pl[4]), be32toh(pl[5]));
		}

		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
	}
	FW_GUNLOCK(fc);

	/* Single itx_enable: allocates descriptors + starts DMA */
	{
		int ret = fc->itx_enable(fc, ch->dmach);
		printf("digi00x: start_tx — itx_enable returned %d, "
		    "xferq flag=0x%08x\n", ret, xferq->flag);
		if (ret != 0) {
			mtx_lock(&dg00x->lock);
			dg00x->tx_use_count = 0;
			mtx_unlock(&dg00x->lock);
		}
		return (ret);
	}
}

int
dg00x_streaming_start_rx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_rx;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int err;

	if (ch->dmach < 0) {
		printf("digi00x: start_rx — dmach < 0, bailing\n");
		return (-ENODEV);
	}

	dot_reset_state(&dg00x->pcm_capture.dot);
	fc = ISO_FC(ch);
	xferq = ISO_XFERQ(ch);

	mtx_lock(&dg00x->lock);
	if (dg00x->rx_use_count++ > 0) {
		mtx_unlock(&dg00x->lock);
		return (0);
	}
	mtx_unlock(&dg00x->lock);

	FW_GLOCK(fc);
	while ((bx = STAILQ_FIRST(&xferq->stdma)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stdma, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	err = fc->irx_enable(fc, ch->dmach);
	FW_GUNLOCK(fc);

	printf("digi00x: start_rx — irx_enable(dmach=%d) returned %d, "
	    "cap.rate=%u cap.dev_ch=%u\n",
	    ch->dmach, err,
	    dg00x->pcm_capture.rate, dg00x->pcm_capture.device_channels);

	if (err != 0) {
		mtx_lock(&dg00x->lock);
		dg00x->rx_use_count = 0;
		mtx_unlock(&dg00x->lock);
	}

	return (err);
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
	if (dg00x->tx_use_count == 0) {
		mtx_unlock(&dg00x->lock);
		return;
	}
	if (--dg00x->tx_use_count > 0) {
		mtx_unlock(&dg00x->lock);
		return;
	}

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
	struct dg00x_iso_channel *ch = &dg00x->iso_rx;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;

	if (dg00x->iso_rx.dmach < 0)
		return;

	mtx_lock(&dg00x->lock);
	if (dg00x->rx_use_count == 0) {
		mtx_unlock(&dg00x->lock);
		return;
	}
	if (--dg00x->rx_use_count > 0) {
		mtx_unlock(&dg00x->lock);
		return;
	}
	mtx_unlock(&dg00x->lock);

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	fc->irx_disable(fc, ch->dmach);

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
