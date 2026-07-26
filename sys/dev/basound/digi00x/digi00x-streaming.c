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
#define DG00X_ISO_NCHUNKS	4

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

	/* Configure xferq for ISO streaming with handler callbacks */
	xferq->flag &= ~(FWXFERQ_MODEMASK | FWXFERQ_OPEN |
			 FWXFERQ_STREAM | FWXFERQ_CHTAGMASK);
	xferq->flag |= FWXFERQ_OPEN | FWXFERQ_STREAM |
		       FWXFERQ_HANDLER | FWXFERQ_EXTBUF;
	xferq->psize = DG00X_ISO_PACKET_SIZE;
	xferq->bnchunk = DG00X_ISO_NCHUNKS;
	xferq->bnpacket = 1;
	xferq->queued = 0;
	xferq->sc = (caddr_t)ch;

	/* Allocate bulkxfer array and mbufs */
	ch->bulkxfer = (void *)malloc(
	    sizeof(struct fw_bulkxfer) * DG00X_ISO_NCHUNKS,
	    M_DG00X_ISO, M_WAITOK | M_ZERO);
	ch->mbufs = (void *)malloc(
	    sizeof(struct mbuf *) * DG00X_ISO_NCHUNKS,
	    M_DG00X_ISO, M_WAITOK | M_ZERO);

	STAILQ_INIT(&xferq->stfree);
	STAILQ_INIT(&xferq->stdma);
	STAILQ_INIT(&xferq->stvalid);

	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		struct mbuf *m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL)
			break;
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
/* DMA handlers — called from fwohci interrupt context                 */
/* ------------------------------------------------------------------ */

static void
dg00x_rx_handler(struct fw_xferq *xferq)
{
	struct dg00x_iso_channel *ch = (struct dg00x_iso_channel *)xferq->sc;
	struct snd_dg00x *dg00x = (struct snd_dg00x *)ch->ctx;
	struct dg00x_pcm_stream *ps = &dg00x->pcm_capture;
	struct firewire_comm *fc = ISO_FC(ch);

	if (ps->active && ps->substream != NULL && xferq->stproc != NULL) {
		struct fw_bulkxfer *bx = xferq->stproc;
		uint32_t *payload = mtod(bx->mbuf, uint32_t *);

		dot_read_pcm(&ps->dot,
			     (int32_t *)ps->substream->runtime->dma_area +
			     (ps->hwptr / 4),
			     payload + CIP_HEADER_QUADLETS + 1,
			     ps->pcm_channels, 1,
			     ps->pcm_channels + 1);

		ps->hwptr += ps->pcm_channels * 4;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;
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

static void
dg00x_tx_handler(struct fw_xferq *xferq)
{
	struct dg00x_iso_channel *ch = (struct dg00x_iso_channel *)xferq->sc;
	struct snd_dg00x *dg00x = (struct snd_dg00x *)ch->ctx;
	struct dg00x_pcm_stream *ps = &dg00x->pcm_playback;
	struct firewire_comm *fc = ISO_FC(ch);
	struct fw_bulkxfer *bx;

	/* Dequeue completed chunk */
	bx = STAILQ_FIRST(&xferq->stdma);
	if (bx != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stdma, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}

	if (!ps->active || ps->substream == NULL)
		goto restart;

	/* Take next chunk and fill with audio */
	bx = STAILQ_FIRST(&xferq->stfree);
	if (bx == NULL)
		return;

	STAILQ_REMOVE_HEAD(&xferq->stfree, link);
	{
		uint32_t *payload = mtod(bx->mbuf, uint32_t *);

		dg00x_build_cip_header(&payload[0],
		    dg00x->fwdev->fc->nodeid,
		    ps->pcm_channels + 1, 0,
		    CIP_FMT_AM, AMDTP_FDF_AM824, 0xffff);

		payload[CIP_HEADER_QUADLETS] = 0x80000000; /* MIDI: none */

		dot_write_pcm(&ps->dot,
		    &payload[CIP_HEADER_QUADLETS + 1],
		    (const int32_t *)ps->substream->runtime->dma_area +
		    (ps->hwptr / 4),
		    ps->pcm_channels, 1,
		    ps->pcm_channels + 1);

		ps->hwptr += ps->pcm_channels * 4;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;

		STAILQ_INSERT_TAIL(&xferq->stdma, bx, link);
	}

restart:
	if ((xferq->flag & FWXFERQ_RUNNING) == 0)
		fc->itx_enable(fc, ch->dmach);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
dg00x_streaming_init(struct snd_dg00x *dg00x)
{
	struct firewire_comm *fc = dg00x->fwdev->fc;
	int err;

	err = dg00x_iso_open(fc, &dg00x->iso_tx, 1);
	if (err < 0)
		return (err);
	dg00x->iso_tx.ctx = dg00x;
	dg00x->iso_tx.direction = SNDRV_PCM_STREAM_PLAYBACK;
	ISO_XFERQ(&dg00x->iso_tx)->hand = dg00x_tx_handler;

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
	int i;

	if (ch->dmach < 0 || !ps->active)
		return (0);

	STAILQ_INIT(&xferq->stfree);
	STAILQ_INIT(&xferq->stdma);
	dot_reset_state(&ps->dot);

	for (i = 0; i < DG00X_ISO_NCHUNKS; i++) {
		struct fw_bulkxfer *bx = &ISO_BX(ch)[i];
		uint32_t *payload = mtod(ISO_MB(ch)[i], uint32_t *);

		dg00x_build_cip_header(&payload[0],
		    dg00x->fwdev->fc->nodeid,
		    ps->pcm_channels + 1, i,
		    CIP_FMT_AM, AMDTP_FDF_AM824, 0xffff);

		payload[CIP_HEADER_QUADLETS] = 0x80000000;

		dot_write_pcm(&ps->dot,
		    &payload[CIP_HEADER_QUADLETS + 1],
		    (const int32_t *)ps->substream->runtime->dma_area,
		    ps->pcm_channels, 1,
		    ps->pcm_channels + 1);

		ps->hwptr += ps->pcm_channels * 4;
		if (ps->hwptr >= ps->buffer_bytes)
			ps->hwptr = 0;

		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
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
	if (dg00x->iso_tx.dmach >= 0)
		ISO_FC(&dg00x->iso_tx)->itx_disable(
		    ISO_FC(&dg00x->iso_tx), dg00x->iso_tx.dmach);
}

void
dg00x_streaming_stop_rx(struct snd_dg00x *dg00x)
{
	if (dg00x->iso_rx.dmach >= 0)
		ISO_FC(&dg00x->iso_rx)->irx_disable(
		    ISO_FC(&dg00x->iso_rx), dg00x->iso_rx.dmach);
}
