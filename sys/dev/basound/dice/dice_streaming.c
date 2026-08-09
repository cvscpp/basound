/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dice_streaming.c - FreeBSD-native ISO DMA streaming for DICE devices
 *
 * DICE uses IEC 61883-6 AMDTP with AM824 data format (24-bit audio
 * in 32-bit big-endian containers).  MIDI rides in one extra quadlet
 * per data block after the PCM quadlets.
 *
 * Streaming state is allocated as sc->stream (pointer) to avoid
 * circular header dependencies between dice_bsd.h and this file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/libkern.h>
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

#include "dice_bsd.h"
#include "dice_streaming.h"
#include "alsa_pcm_bsd.h"

MALLOC_DECLARE(M_ALSA);
MALLOC_DEFINE(M_DICE_ISO, "dice_iso", "DICE ISO DMA buffers");

/* Accessor macros for dice_iso_channel void* fields */
#define ISO_XFERQ(ch)	((struct fw_xferq *)(ch)->xferq)
#define ISO_FC(ch)	((struct firewire_comm *)(ch)->fc)
#define ISO_BX(ch)	((struct fw_bulkxfer *)(ch)->bulkxfer)
#define ISO_MB(ch)	((struct mbuf **)(ch)->mbufs)
#define DICE_SC(ch)	((struct dice_bsd_softc *)(ch)->ctx)
#define MAX_DICE_PCM_CH	   32

#define DSTREAM(sc)	((sc)->stream)

/* ------------------------------------------------------------------ */
/* Rate → SFC conversion                                               */
/* ------------------------------------------------------------------ */

unsigned int
dice_rate_to_sfc(unsigned int rate)
{
	switch (rate) {
	case  32000: return (DICE_SFC_32000);
	case  44100: return (DICE_SFC_44100);
	case  48000: return (DICE_SFC_48000);
	case  88200: return (DICE_SFC_88200);
	case  96000: return (DICE_SFC_96000);
	case 176400: return (DICE_SFC_176400);
	case 192000: return (DICE_SFC_192000);
	default:     return (DICE_SFC_48000);
	}
}

/* ------------------------------------------------------------------ */
/* CIP header builder                                                  */
/* ------------------------------------------------------------------ */

void
dice_build_cip_header(uint32_t *hdr, unsigned int node_id,
		      unsigned int dbs, unsigned int dbc,
		      unsigned int fmt, unsigned int fdf, unsigned int syt)
{
	hdr[0] = htobe32(
	    ((node_id & 0x3f) << 24) | ((dbs & 0xff) << 16) | (dbc & 0xff));
	hdr[1] = htobe32(
	    (1u << 31) | (((unsigned int)fmt & 0x3f) << 24) |
	    ((fdf & 0xff) << 16) | (syt & 0xffff));
}

/* ------------------------------------------------------------------ */
/* AMDTP fractional framing                                            */
/* ------------------------------------------------------------------ */

unsigned int
dice_frames_this_packet(struct dice_pcm_stream *ps)
{
	unsigned int phase;

	switch (ps->rate) {
	case 44100:
		phase = ps->frame_cycle;
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 80) ps->frame_cycle = 0;
		return (5 + ((phase & 1) ^
		    ((phase == 0 || phase >= 40) ? 1 : 0)));
	case 88200:
		phase = ps->frame_cycle;
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 40) ps->frame_cycle = 0;
		return (11 + ((phase == 0) ? 1 : 0));
	default:
		return (ps->frames_per_packet);
	}
}

/* ------------------------------------------------------------------ */
/* ISO DMA channel open/close                                          */
/* ------------------------------------------------------------------ */

static int
dice_iso_open(struct firewire_comm *fc, struct dice_iso_channel *ch, int is_tx)
{
	struct fw_xferq *xferq;
	int i;

	ch->dmach = fw_open_isodma(fc, is_tx);
	if (ch->dmach < 0) return (-ENOMEM);

	ch->fc = (void *)fc;
	xferq = is_tx ? fc->it[ch->dmach] : fc->ir[ch->dmach];
	ch->xferq = (void *)xferq;

	xferq->flag &= ~(FWXFERQ_MODEMASK | FWXFERQ_OPEN |
			 FWXFERQ_STREAM | FWXFERQ_CHTAGMASK);
	xferq->flag |= FWXFERQ_OPEN | FWXFERQ_STREAM |
		       FWXFERQ_HANDLER | FWXFERQ_EXTBUF;
	xferq->psize = DICE_ISO_PACKET_SIZE;
	xferq->bnchunk = DICE_ISO_NCHUNKS;
	xferq->bnpacket = 1;
	xferq->queued = 0;
	xferq->sc = (caddr_t)ch;

	ch->bulkxfer = malloc(sizeof(struct fw_bulkxfer) * DICE_ISO_NCHUNKS,
	    M_DICE_ISO, M_NOWAIT | M_ZERO);
	if (ch->bulkxfer == NULL) return (-ENOMEM);
	ch->mbufs = malloc(sizeof(struct mbuf *) * DICE_ISO_NCHUNKS,
	    M_DICE_ISO, M_NOWAIT | M_ZERO);
	if (ch->mbufs == NULL) {
		free(ch->bulkxfer, M_DICE_ISO);
		ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	STAILQ_INIT(&xferq->stfree);
	STAILQ_INIT(&xferq->stdma);
	STAILQ_INIT(&xferq->stvalid);

	xferq->buf = fwdma_malloc_multiseg(fc,
	    DICE_ISO_PACKET_SIZE, DICE_ISO_PACKET_SIZE,
	    DICE_ISO_NCHUNKS, M_NOWAIT);
	if (xferq->buf == NULL) {
		free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
		free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		struct mbuf *m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);

		if (m == NULL) {
			int j;
			for (j = 0; j < i; j++) m_freem(ISO_MB(ch)[j]);
			free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
			free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL;
			fwdma_free_multiseg(xferq->buf); xferq->buf = NULL;
			return (-ENOMEM);
		}
		m->m_len = DICE_ISO_PACKET_SIZE;
		ISO_MB(ch)[i] = m;
		ISO_BX(ch)[i].mbuf = m;
		ISO_BX(ch)[i].start = mtod(m, caddr_t);
		ISO_BX(ch)[i].end = mtod(m, caddr_t) + DICE_ISO_PACKET_SIZE;
		ISO_BX(ch)[i].poffset = i;
		STAILQ_INSERT_TAIL(&xferq->stfree, &ISO_BX(ch)[i], link);
	}
	xferq->bulkxfer = ISO_BX(ch);
	return (0);
}

static void
dice_iso_close(struct dice_iso_channel *ch)
{
	struct fw_xferq *xferq = ISO_XFERQ(ch);
	int i;

	ch->dmach = -1;
	if (ch->mbufs) {
		for (i = 0; i < DICE_ISO_NCHUNKS; i++)
			if (ISO_MB(ch)[i]) m_freem(ISO_MB(ch)[i]);
		free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
	}
	if (ch->bulkxfer) { free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL; }
	if (xferq) {
		if (xferq->buf) { fwdma_free_multiseg(xferq->buf); xferq->buf = NULL; }
		xferq->flag &= ~(FWXFERQ_OPEN | FWXFERQ_HANDLER |
				 FWXFERQ_STREAM | FWXFERQ_EXTBUF);
		xferq->sc = NULL; xferq->hand = NULL;
	}
	ch->xferq = NULL; ch->fc = NULL;
}

/* ------------------------------------------------------------------ */
/* AM824 encode/decode                                                  */
/* ------------------------------------------------------------------ */

static void
dice_encode_am824(uint32_t *dest, const int32_t *src, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++)
		dest[c] = htobe32((uint32_t)src[c] & 0xffffff00);
}

static void
dice_encode_am824_padded(uint32_t *dest, const int32_t *src,
			 unsigned int src_channels, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++) {
		uint32_t s = (c < src_channels) ? ((uint32_t)src[c] & 0xffffff00) : 0;
		dest[c] = htobe32(s);
	}
}

static void
dice_decode_am824(int32_t *dest, const uint32_t *src, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++)
		dest[c] = (int32_t)(be32toh(src[c]) & 0xffffff00);
}

/* ------------------------------------------------------------------ */
/* DICE register helpers — use the proper dice_write_quad from dice_bsd.c */
/* ------------------------------------------------------------------ */

static int
dice_reg_write(struct dice_bsd_softc *sc, uint64_t addr, uint32_t val)
{
	return (dice_write_quad(sc->fwdev, addr, htobe32(val)));
}

static int
dice_program_iso(struct dice_bsd_softc *sc, int is_tx, unsigned int idx, int ch)
{
	uint64_t addr = DICE_PRIVATE_SPACE;
	uint32_t val = htobe32(ch >= 0 ? (uint32_t)ch : 0xffffffff);

	if (is_tx)
		addr += sc->tx_offset + idx * 0x20 + 0x008; /* TX_ISOCHRONOUS */
	else
		addr += sc->rx_offset + idx * 0x20 + 0x008; /* RX_ISOCHRONOUS */

	return (dice_write_quad(sc->fwdev, addr, val));
}

static int
dice_enable(struct dice_bsd_softc *sc, bool en)
{
	uint64_t addr = DICE_PRIVATE_SPACE + sc->global_offset + 0x050;
	uint32_t val = htobe32(en ? 1 : 0);
	return (dice_write_quad(sc->fwdev, addr, val));
}

/* ------------------------------------------------------------------ */
/* TX fill — encode PCM into one ISO chunk (playback direction)         */
/* ------------------------------------------------------------------ */

static void
dice_fill_tx_chunk(struct dice_bsd_softc *sc, struct fw_xferq *xferq,
		   struct fw_bulkxfer *bx)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	unsigned int dbs = ps->data_block_quadlets;
	unsigned int nch, frames, dbc, bytes, pkt_len;
	struct fw_pkt *fp;
	uint32_t *payload;
	unsigned int i;
	struct basound_chan *txch = ps->substream ?
	    ps->substream->private_data : NULL;
	unsigned int sample_bytes;

	if (txch != NULL && txch->channel != NULL && txch->buffer != NULL) {
		if (txch->channel->format != 0)
			txch->format = txch->channel->format;
		if (ps->substream->runtime != NULL) {
			ps->substream->runtime->dma_area = txch->buffer->buf;
			ps->substream->runtime->dma_addr = txch->buffer->buf_addr;
			ps->substream->runtime->dma_bytes = txch->buffer->bufsize;
		}
		ps->buffer_bytes = txch->buffer->bufsize;
		ps->period_bytes = txch->blocksize;
		if (ps->buffer_bytes > 0) ps->hwptr %= ps->buffer_bytes;
	}

	nch = ps->pcm_channels;
	if (txch != NULL) {
		unsigned int fmt_ch = AFMT_CHANNEL(txch->format);
		if (fmt_ch != 0 && fmt_ch <= ps->device_channels) {
			nch = fmt_ch; ps->pcm_channels = nch;
		}
	}
	if (nch > ps->device_channels) nch = ps->device_channels;

	frames = dice_frames_this_packet(ps);
	dbc = ps->tx_dbc; ps->tx_dbc = (dbc + frames) & 0xff;

	sample_bytes = (txch != NULL) ? AFMT_BPS(txch->format) : 4;
	if (sample_bytes != 2 && sample_bytes != 4) sample_bytes = 4;

	bytes = frames * nch * sample_bytes;
	pkt_len = 8 + frames * dbs * 4;

	fp = (struct fw_pkt *)fwdma_v_addr(xferq->buf, bx->poffset);
	if (fp == NULL) return;

	fp->mode.stream.len = pkt_len;
	payload = (uint32_t *)fp->mode.stream.payload;

	dice_build_cip_header(&payload[0], sc->fwdev->fc->nodeid,
	    dbs, dbc, CIP_FMT_AM, ps->fdf, 0xffff);

	/* Fill data blocks */
	{
		struct snd_dbuf *sb = txch ? txch->buffer : NULL;
		int32_t tmpbuf[MAX_DICE_PCM_CH * 12];
		const int32_t *sp;
		unsigned int source_off = ps->hwptr;
		unsigned int ready_bytes = 0, pending_bytes = 0, read_bytes = 0;
		bool underrun = false;

		if (sb != NULL) {
			unsigned int rp = sndbuf_getreadyptr(sb);
			ready_bytes = sndbuf_getready(sb);
			if (source_off >= rp)
				pending_bytes = source_off - rp;
			else
				pending_bytes = ps->buffer_bytes - rp + source_off;

			if (pending_bytes > ready_bytes) {
				underrun = true; read_bytes = 0; ps->tx_underruns++;
			} else {
				read_bytes = ready_bytes - pending_bytes;
				if (read_bytes > bytes) read_bytes = bytes;
				if (read_bytes < bytes) ps->tx_shortfalls++;
			}
		} else {
			underrun = true; ps->tx_underruns++;
		}
		if (sample_bytes > 1) read_bytes -= read_bytes % sample_bytes;

		for (i = 0; i < frames; i++) {
			uint32_t *blk = &payload[CIP_HEADER_QUADLETS + i * dbs];
			unsigned int midi_off = ps->midi_ports > 0 ? 1 : 0;

			if (ps->midi_ports > 0) blk[0] = 0x00000080; /* no MIDI */

			if (underrun) {
				memset(tmpbuf, 0, frames * nch * sizeof(int32_t));
				sp = tmpbuf;
			} else if (read_bytes >= bytes && sample_bytes == 4 &&
			    source_off + bytes <= ps->buffer_bytes) {
				sp = (const int32_t *)((const uint8_t *)
				    ps->substream->runtime->dma_area + source_off);
			} else {
				unsigned int samples = frames * nch;
				for (i = 0; i < samples; i++) tmpbuf[i] = 0;
				if (read_bytes > 0 && sample_bytes == 4) {
					unsigned int rd = read_bytes / 4;
					if (source_off + read_bytes <= ps->buffer_bytes)
						memcpy(tmpbuf,
						    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
						    read_bytes);
					else {
						unsigned int first = ps->buffer_bytes - source_off;
						memcpy(tmpbuf,
						    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
						    first);
						memcpy((uint8_t *)tmpbuf + first,
						    ps->substream->runtime->dma_area,
						    read_bytes - first);
					}
				} else if (read_bytes > 0 && sample_bytes == 2) {
					unsigned int rd = read_bytes / 2;
					const int16_t *s16;
					uint8_t sbuf[18*12*2];
					if (source_off + read_bytes <= ps->buffer_bytes)
						s16 = (const int16_t *)((const uint8_t *)
						    ps->substream->runtime->dma_area + source_off);
					else {
						unsigned int first = ps->buffer_bytes - source_off;
						memcpy(sbuf,
						    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
						    first);
						memcpy(sbuf + first,
						    ps->substream->runtime->dma_area,
						    read_bytes - first);
						s16 = (const int16_t *)sbuf;
					}
					for (i = 0; i < rd; i++)
						tmpbuf[i] = ((int32_t)s16[i]) << 16;
				}
				sp = tmpbuf;
			}
			dice_encode_am824_padded(&blk[midi_off],
			    &sp[i * nch], nch, ps->device_channels);
		}
	}

	ps->hwptr += bytes;
	if (ps->hwptr >= ps->buffer_bytes) ps->hwptr = 0;
	ps->period_accum += bytes;
}

/* ------------------------------------------------------------------ */
/* RX handler — decode ISO packets into PCM DMA buffer                  */
/* ------------------------------------------------------------------ */

static void
dice_rx_handler(struct fw_xferq *xferq)
{
	struct dice_iso_channel *ch;
	struct dice_bsd_softc *sc;
	struct dice_pcm_stream *ps;
	struct fw_bulkxfer *bx;
	unsigned int frames, dbs, bytes;
	int recycled = 0;

	if (xferq == NULL || xferq->sc == NULL) return;
	ch = (struct dice_iso_channel *)xferq->sc;
	if (ch->ctx == NULL) return;
	sc = DICE_SC(ch);
	ps = &DSTREAM(sc)->capture;

	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);

		if (ps->active && ps->substream != NULL &&
		    ps->substream->runtime != NULL &&
		    ps->substream->runtime->dma_area != NULL &&
		    xferq->buf != NULL) {
			struct basound_chan *rxch = ps->substream->private_data;
			unsigned int sample_bytes = (rxch) ? AFMT_BPS(rxch->format) : 4;
			uint32_t *payload;
			unsigned int nch;

			if (sample_bytes != 2 && sample_bytes != 4) sample_bytes = 4;

			payload = (uint32_t *)fwdma_v_addr(xferq->buf, bx->poffset);
			frames = dice_frames_this_packet(ps);
			dbs = ps->data_block_quadlets;
			nch = ps->pcm_channels;
			if (nch > ps->device_channels) nch = ps->device_channels;
			bytes = frames * nch * sample_bytes;

			{
				int32_t tmpbuf[MAX_DICE_PCM_CH * 12];
				unsigned int fi;

				for (fi = 0; fi < frames; fi++) {
					const uint32_t *blk = &payload[CIP_HEADER_QUADLETS + fi * dbs];
					unsigned int mo = ps->midi_ports > 0 ? 1 : 0;
					dice_decode_am824(&tmpbuf[fi * nch], &blk[mo], nch);
				}

				if (sample_bytes == 4 &&
				    ps->hwptr + bytes <= ps->buffer_bytes) {
					memcpy((uint8_t *)ps->substream->runtime->dma_area + ps->hwptr,
					    tmpbuf, bytes);
				} else if (sample_bytes == 4) {
					unsigned int first = ps->buffer_bytes - ps->hwptr;
					uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
					if (first >= bytes)
						memcpy(dst, tmpbuf, bytes);
					else {
						memcpy(dst, tmpbuf, first);
						memcpy(ps->substream->runtime->dma_area,
						    (uint8_t *)tmpbuf + first, bytes - first);
					}
				} else {
					int16_t tmp16[MAX_DICE_PCM_CH * 12];
					unsigned int s;
					for (s = 0; s < frames * nch; s++)
						tmp16[s] = (int16_t)(tmpbuf[s] >> 16);
					{
						unsigned int first = ps->buffer_bytes - ps->hwptr;
						uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
						if (first >= bytes)
							memcpy(dst, tmp16, bytes);
						else {
							memcpy(dst, tmp16, first);
							memcpy(ps->substream->runtime->dma_area,
							    (uint8_t *)tmp16 + first, bytes - first);
						}
					}
				}
			}
			ps->hwptr += bytes;
			if (ps->hwptr >= ps->buffer_bytes) ps->hwptr = 0;
			ps->period_accum += bytes;
		}
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
		recycled++;
	}
	if (recycled > 0)
		ISO_FC(ch)->irx_enable(ISO_FC(ch), ch->dmach);
}

/* ------------------------------------------------------------------ */
/* TX refill + period signalling callout                                */
/* ------------------------------------------------------------------ */

static void
dice_pcm_stream_cb(void *arg)
{
	struct dice_bsd_softc *sc = arg;
	struct dice_pcm_stream *pb = &DSTREAM(sc)->playback;
	struct dice_pcm_stream *cap = &DSTREAM(sc)->capture;

	dice_streaming_refill_tx(sc);

	if (pb->active && pb->substream != NULL)
		snd_pcm_period_elapsed(pb->substream);
	if (cap->active && cap->substream != NULL)
		snd_pcm_period_elapsed(cap->substream);

	if (!pb->active && !cap->active) return;
	callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
dice_streaming_init(struct dice_bsd_softc *sc)
{
	struct firewire_comm *fc;
	int err;

	if (sc->fwdev == NULL || sc->fwdev->fc == NULL) return (-ENODEV);
	if (sc->stream != NULL) return (0);

	sc->stream = malloc(sizeof(struct dice_streaming), M_DICE_ISO,
	    M_WAITOK | M_ZERO);
	if (sc->stream == NULL) return (-ENOMEM);

	fc = sc->fwdev->fc;

	err = dice_iso_open(fc, &DSTREAM(sc)->iso_tx, 1);
	if (err < 0) { free(sc->stream, M_DICE_ISO); sc->stream = NULL; return (err); }
	DSTREAM(sc)->iso_tx.ctx = sc;

	err = dice_iso_open(fc, &DSTREAM(sc)->iso_rx, 0);
	if (err < 0) {
		dice_iso_close(&DSTREAM(sc)->iso_tx);
		free(sc->stream, M_DICE_ISO); sc->stream = NULL;
		return (err);
	}
	DSTREAM(sc)->iso_rx.ctx = sc;
	ISO_XFERQ(&DSTREAM(sc)->iso_rx)->hand = dice_rx_handler;

	callout_init(&DSTREAM(sc)->callout, 1);
	mtx_init(&DSTREAM(sc)->playback_lock, "dice_playback", NULL, MTX_DEF);
	mtx_init(&DSTREAM(sc)->capture_lock, "dice_capture", NULL, MTX_DEF);
	DSTREAM(sc)->tx_channel = -1;
	DSTREAM(sc)->rx_channel = -1;
	return (0);
}

void
dice_streaming_fini(struct dice_bsd_softc *sc)
{
	if (sc->stream == NULL) return;

	callout_drain(&DSTREAM(sc)->callout);

	if (DSTREAM(sc)->iso_tx.dmach >= 0) {
		ISO_FC(&DSTREAM(sc)->iso_tx)->itx_disable(
		    ISO_FC(&DSTREAM(sc)->iso_tx), DSTREAM(sc)->iso_tx.dmach);
		dice_iso_close(&DSTREAM(sc)->iso_tx);
	}
	if (DSTREAM(sc)->iso_rx.dmach >= 0) {
		ISO_FC(&DSTREAM(sc)->iso_rx)->irx_disable(
		    ISO_FC(&DSTREAM(sc)->iso_rx), DSTREAM(sc)->iso_rx.dmach);
		dice_iso_close(&DSTREAM(sc)->iso_rx);
	}

	mtx_destroy(&DSTREAM(sc)->playback_lock);
	mtx_destroy(&DSTREAM(sc)->capture_lock);
	free(sc->stream, M_DICE_ISO);
	sc->stream = NULL;
}

static void
dice_stream_configure(struct dice_pcm_stream *ps, int dir, unsigned int rate)
{
	ps->direction = dir;
	ps->sfc = dice_rate_to_sfc(rate);
	ps->fdf = CIP_FMT_AM | ps->sfc;
	ps->tx_dbc = 0;
	ps->frame_cycle = 0;
	ps->frames_per_packet = rate / 8000;
	ps->frame_remainder = rate % 8000;
}

int
dice_streaming_start_playback(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int i;

	if (sc->stream == NULL || ch->dmach < 0) return (0);
	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);
	if (fc == NULL || xferq == NULL) return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	if (DSTREAM(sc)->rx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->playback_lock); return (0);
	}
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	dice_stream_configure(ps, SNDRV_PCM_STREAM_PLAYBACK, ps->rate);

	/* Program RX isochronous channel, speed, then enable */
	dice_program_iso(sc, 0, 0, 0);
	dice_enable(sc, true);

	FW_GLOCK(fc);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stdma);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stvalid);
	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL) break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);
		dice_fill_tx_chunk(sc, xferq, bx);
		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
	}
	FW_GUNLOCK(fc);

	{
		uint32_t fv = DICE_ISO_TAG_CIP |
		    (DSTREAM(sc)->rx_channel >= 0 ? (DSTREAM(sc)->rx_channel & 0x3f) : 0);
		xferq->flag = (xferq->flag & ~FWXFERQ_CHTAGMASK) | fv;
		fc->itx_enable(fc, ch->dmach);
	}

	if (DSTREAM(sc)->active_streams == 0)
		callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
	DSTREAM(sc)->active_streams++;
	ps->active = true;
	return (0);
}

int
dice_streaming_start_capture(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->capture;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	int err;

	if (sc->stream == NULL || ch->dmach < 0) return (0);
	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);
	if (fc == NULL || xferq == NULL) return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->capture_lock);
	if (DSTREAM(sc)->tx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->capture_lock); return (0);
	}
	mtx_unlock(&DSTREAM(sc)->capture_lock);

	dice_stream_configure(ps, SNDRV_PCM_STREAM_CAPTURE, ps->rate);
	dice_program_iso(sc, 1, 0, 0);

	FW_GLOCK(fc);
	err = fc->irx_enable(fc, ch->dmach);
	FW_GUNLOCK(fc);
	if (err != 0) {
		mtx_lock(&DSTREAM(sc)->capture_lock);
		DSTREAM(sc)->tx_use_count = 0;
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return (err);
	}

	if (DSTREAM(sc)->active_streams == 0)
		callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
	DSTREAM(sc)->active_streams++;
	ps->active = true;
	return (0);
}

void
dice_streaming_stop_playback(struct dice_bsd_softc *sc)
{
	if (sc->stream == NULL) return;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx;
	struct firewire_comm *fc = ISO_FC(ch);

	if (ch->dmach < 0) return;
	ps->active = false;

	mtx_lock(&DSTREAM(sc)->playback_lock);
	if (DSTREAM(sc)->rx_use_count == 0) { mtx_unlock(&DSTREAM(sc)->playback_lock); return; }
	if (--DSTREAM(sc)->rx_use_count > 0) { mtx_unlock(&DSTREAM(sc)->playback_lock); return; }
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	fc->itx_disable(fc, ch->dmach);
	if (DSTREAM(sc)->active_streams > 0) DSTREAM(sc)->active_streams--;
	dice_enable(sc, false);
}

void
dice_streaming_stop_capture(struct dice_bsd_softc *sc)
{
	if (sc->stream == NULL) return;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->capture;
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx;
	struct firewire_comm *fc = ISO_FC(ch);

	if (ch->dmach < 0) return;
	ps->active = false;

	mtx_lock(&DSTREAM(sc)->capture_lock);
	if (DSTREAM(sc)->tx_use_count == 0) { mtx_unlock(&DSTREAM(sc)->capture_lock); return; }
	if (--DSTREAM(sc)->tx_use_count > 0) { mtx_unlock(&DSTREAM(sc)->capture_lock); return; }
	mtx_unlock(&DSTREAM(sc)->capture_lock);

	fc->irx_disable(fc, ch->dmach);
	if (DSTREAM(sc)->active_streams > 0) DSTREAM(sc)->active_streams--;
}

void
dice_streaming_refill_tx(struct dice_bsd_softc *sc)
{
	if (sc->stream == NULL) return;
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int refilled = 0;

	if (ch->dmach < 0 || !ps->active) return;
	if (ps->substream == NULL || ps->substream->runtime == NULL ||
	    ps->substream->runtime->dma_area == NULL) return;

	xferq = ISO_XFERQ(ch); fc = ISO_FC(ch);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	FW_GLOCK(fc);
	while ((bx = STAILQ_FIRST(&xferq->stfree)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);
		dice_fill_tx_chunk(sc, xferq, bx);
		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
		refilled++;
	}
	FW_GUNLOCK(fc);
	if (refilled > 0) fc->itx_enable(fc, ch->dmach);
	mtx_unlock(&DSTREAM(sc)->playback_lock);
}
