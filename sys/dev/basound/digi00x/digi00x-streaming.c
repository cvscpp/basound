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

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>

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
			return (-ENOMEM);
		}
		m->m_len = DG00X_ISO_PACKET_SIZE;
		ISO_MB(ch)[i] = m;
		ISO_BX(ch)[i].mbuf = m;
		ISO_BX(ch)[i].start = mtod(m, caddr_t);
		ISO_BX(ch)[i].end = mtod(m, caddr_t) + DG00X_ISO_PACKET_SIZE;
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
		xferq->flag &= ~(FWXFERQ_OPEN | FWXFERQ_HANDLER |
				 FWXFERQ_STREAM | FWXFERQ_EXTBUF);
		xferq->sc = NULL;
		xferq->hand = NULL;
	}
	ch->xferq = NULL;
	ch->fc = NULL;
}

/* ------------------------------------------------------------------ */
/* Fill one ISO chunk with audio data from the PCM DMA buffer          */
/* ------------------------------------------------------------------ */

static void
dg00x_fill_tx_chunk(struct snd_dg00x *dg00x, struct fw_bulkxfer *bx,
		    unsigned int dbc)
{
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	uint32_t *payload = mtod(bx->mbuf, uint32_t *);

	dg00x_build_cip_header(&payload[0],
	    dg00x->fwdev->fc->nodeid,
	    ps->pcm_channels + 1, dbc,
	    CIP_FMT_AM, AMDTP_FDF_AM824, 0xffff);

	payload[CIP_HEADER_QUADLETS] = 0x80000000; /* MIDI: none */

	dot_write_pcm(&ps->dot,
	    &payload[CIP_HEADER_QUADLETS + 1],
	    (const int32_t *)ps->substream->runtime->dma_area +
	    (ps->hwptr / 4),
	    ps->pcm_channels, 1,
	    ps->pcm_channels + 1);
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
	unsigned int bytes;

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

		dot_read_pcm(&ps->dot,
			     (int32_t *)ps->substream->runtime->dma_area +
			     (ps->hwptr / 4),
			     payload + CIP_HEADER_QUADLETS + 1,
			     ps->pcm_channels, 1,
			     ps->pcm_channels + 1);

		bytes = ps->pcm_channels * 4;
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
	struct fw_xferq *xferq = ISO_XFERQ(ch);
	struct firewire_comm *fc = ISO_FC(ch);
	struct fw_bulkxfer *bx;
	int i;

	if (ch->dmach < 0 || !ps->active)
		return (0);

	/* Safety: don't start if the PCM DMA buffer isn't yet mapped */
	if (ps->substream == NULL ||
	    ps->substream->runtime == NULL ||
	    ps->substream->runtime->dma_area == NULL)
		return (-EINVAL);

	/*
	 * Drain all chunks back to stfree for a clean starting state.
	 * Use STAILQ_CONCAT to move the entire list atomically — no
	 * per-element iteration that could corrupt link pointers.
	 * Don't use STAILQ_INIT on stfree — that would orphan elements
	 * that the fwohci may still be DMAing from.
	 */
	STAILQ_CONCAT(&xferq->stfree, &xferq->stdma);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stvalid);

	dot_reset_state(&ps->dot);

	/* Pre-fill all chunks to seed the ISO pipeline.  hwptr and
	 * dot state advance through the buffer so that the callout-
	 * driven refill path (dg00x_streaming_refill_tx) continues
	 * from where we leave off. */
	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL)
			break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);

		dg00x_fill_tx_chunk(dg00x, bx, i);

		ps->hwptr += ps->pcm_channels * 4;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;

		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
	}

	return fc->itx_enable(fc, ch->dmach);
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
	struct fw_bulkxfer *bx;

	if (ch->dmach < 0)
		return;

	xferq = ISO_XFERQ(ch);
	ISO_FC(ch)->itx_disable(ISO_FC(ch), ch->dmach);

	/* Return any in-flight chunks to stfree so they can be reused
	 * on the next start. */
	while ((bx = STAILQ_FIRST(&xferq->stdma)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stdma, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
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
 */
void
dg00x_streaming_refill_tx(struct snd_dg00x *dg00x)
{
	struct dg00x_iso_channel *ch = &dg00x->iso_tx;
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	unsigned int bytes;
	int refilled = 0;

	if (ch->dmach < 0 || !ps->active)
		return;

	if (ps->substream == NULL ||
	    ps->substream->runtime == NULL ||
	    ps->substream->runtime->dma_area == NULL)
		return;

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	/* Move completed chunks from stfree to stvalid, filling each
	 * with fresh audio data. */
	while ((bx = STAILQ_FIRST(&xferq->stfree)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);

		dg00x_fill_tx_chunk(dg00x, bx, ps->tx_dbc++);

		bytes = ps->pcm_channels * 4;
		ps->hwptr += bytes;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;

		dg00x_pcm_update_position(ps, bytes);

		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
		refilled++;
	}

	/* If we refilled chunks and the DMA isn't running, restart it.
	 * itx_enable is safe to call even if DMA is already active
	 * (it returns 0 immediately in that case). */
	if (refilled > 0 && (xferq->flag & FWXFERQ_RUNNING) == 0)
		fc->itx_enable(fc, ch->dmach);
}
