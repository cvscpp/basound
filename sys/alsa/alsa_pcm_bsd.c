// SPDX-License-Identifier: GPL-3.0-or-later
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <dev/sound/pcm/sound.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "alsa_pcm_bsd.h"
#include "channel_if.h"
#include "feeder_if.h"
#include "hdsp.h"

MALLOC_DECLARE(M_ALSA);

static uint32_t basound_fmtlist[] = {
	SND_FORMAT(AFMT_S32_LE, 2, 0),
	SND_FORMAT(AFMT_S16_LE, 2, 0),
	SND_FORMAT(AFMT_S32_LE, 1, 0),
	SND_FORMAT(AFMT_S16_LE, 1, 0),
	SND_FORMAT(AFMT_S32_LE, 8, 0),
	SND_FORMAT(AFMT_S16_LE, 8, 0),
	SND_FORMAT(AFMT_S32_LE, 18, 0),
	SND_FORMAT(AFMT_S16_LE, 18, 0),
	SND_FORMAT(AFMT_S32_LE, 26, 0),
	SND_FORMAT(AFMT_S16_LE, 26, 0),
	SND_FORMAT(AFMT_S32_LE, 32, 0),
	SND_FORMAT(AFMT_S16_LE, 32, 0),
	0
};

/*
 * digi00x-only format list: adds 32-bit float (AFMT_FLOAT), which is
 * Audacious' default output format (its config "output_bit_depth=0"
 * resolves to FMT_FLOAT).  Without a FLOAT entry the OSS layer's
 * feeder_chain() finds no hardware format for a FLOAT request,
 * SNDCTL_DSP_SETFMT fails and echoes the old format, and Audacious
 * reports "Selected audio format is not supported by the device."
 *
 * FLOAT is only advertised for digi00x because its sample path
 * (digi00x-streaming.c) converts float → 24-bit int before DOT
 * encoding.  HDSP/DICE are bitperfect and consume raw S16/S32 from
 * the DMA buffer; exposing FLOAT to them would let apps "succeed"
 * while playing garbage.
 */
static uint32_t basound_fmtlist_dg00x[] = {
	SND_FORMAT(AFMT_S32_LE, 2, 0),
	SND_FORMAT(AFMT_S16_LE, 2, 0),
	SND_FORMAT(AFMT_FLOAT, 2, 0),
	SND_FORMAT(AFMT_S32_LE, 1, 0),
	SND_FORMAT(AFMT_S16_LE, 1, 0),
	SND_FORMAT(AFMT_S32_LE, 8, 0),
	SND_FORMAT(AFMT_S16_LE, 8, 0),
	SND_FORMAT(AFMT_FLOAT, 8, 0),
	SND_FORMAT(AFMT_S32_LE, 18, 0),
	SND_FORMAT(AFMT_S16_LE, 18, 0),
	SND_FORMAT(AFMT_FLOAT, 18, 0),
	SND_FORMAT(AFMT_S32_LE, 26, 0),
	SND_FORMAT(AFMT_S16_LE, 26, 0),
	SND_FORMAT(AFMT_S32_LE, 32, 0),
	SND_FORMAT(AFMT_S16_LE, 32, 0),
	0
};

static uint32_t basound_line6_fmtlist[] = {
	SND_FORMAT(AFMT_S16_LE, 2, 0),
	0
};

/* FreeBSD Channel Methods */

static void *
basound_chan_init(kobj_t obj, void *devinfo, struct snd_dbuf *b, struct pcm_channel *c, int dir)
{
	struct snd_pcm *pcm = devinfo;
	struct snd_pcm_str *pstr;
	struct snd_pcm_substream *substream;
	struct basound_chan *ch;
	int stream = (dir == PCMDIR_PLAY) ? SNDRV_PCM_STREAM_PLAYBACK : SNDRV_PCM_STREAM_CAPTURE;

	pstr = &pcm->streams[stream];
	if (pstr->substream_count <= 0)
		return NULL;

	substream = &pstr->substream[0];

	ch = malloc(sizeof(*ch), M_ALSA, M_WAITOK | M_ZERO);
	ch->substream = substream;
	ch->channel = c;
	ch->buffer = b;
	substream->private_data = ch;

	ch->runtime = malloc(sizeof(*ch->runtime), M_ALSA, M_WAITOK | M_ZERO);
	substream->runtime = ch->runtime;

	/* Allocate DMA-capable hardware buffer using the card-level DMA tag.
	 * bus_dmamem_alloc uses dmatag->maxsize as the allocation size,
	 * which matches the size we pass to sndbuf_alloc. */
	if (pcm->card->dmatag == NULL) {
		free(ch->runtime, M_ALSA);
		free(ch, M_ALSA);
		return NULL;
	}
	if (sndbuf_alloc(b, pcm->card->dmatag, 0,
	    BASOUND_DMA_BUFSIZE) != 0) {
		free(ch->runtime, M_ALSA);
		free(ch, M_ALSA);
		return NULL;
	}

	/* Make the DMA address visible to the ALSA runtime and the
	 * hardware trigger path (hdsp_main.c reads these via
	 * hdsp->playback_substream->runtime->dma_addr). */
	substream->runtime->dma_area  = b->buf;
	substream->runtime->dma_addr  = b->buf_addr;
	substream->runtime->dma_bytes = b->bufsize;

	/* Initialize hardware format from ALSA driver constraints.
	 *
	 * HDSP stores a struct hdsp * in pcm->private_data and reads the
	 * exact channel count from hdsp->ss_{out,in}_channels.
	 *
	 * Other drivers (digi00x, DICE) store their own private data in
	 * pcm->private_data but set hw constraints via their .open callback,
	 * so we defer channel/format selection until after ops->open(). */
	if (strcmp(pcm->card->driver, "hdsp") == 0) {
		struct hdsp *hdsp = pcm->private_data;
		int hw_channels = (dir == PCMDIR_PLAY) ?
		    hdsp->ss_out_channels : hdsp->ss_in_channels;
		ch->format = SND_FORMAT(AFMT_S32_LE, hw_channels, 0);
		c->format  = SND_FORMAT(AFMT_S32_LE, hw_channels, 0);
	} else if (pcm->private_data != NULL) {
		/* Non-HDSP driver with private data (e.g. digi00x):
		 * set a temporary format; ops->open() below will set
		 * runtime hw constraints that we re-read afterward. */
		ch->format = SND_FORMAT(AFMT_S32_LE, 18, 0);
		c->format  = SND_FORMAT(AFMT_S32_LE, 18, 0);
	} else {
		/* No private data (e.g. USB Line6): stereo S16_LE */
		ch->format = SND_FORMAT(AFMT_S16_LE, 2, 0);
		c->format  = SND_FORMAT(AFMT_S16_LE, 2, 0);
	}
	ch->speed = 48000;
	ch->blocksize = 4096;

	ch->caps.caps = DSP_CAP_DUPLEX;
	ch->caps.minspeed = 32000;
	ch->caps.maxspeed = 192000;

	if (substream->pstr->ops && substream->pstr->ops->open) {
		substream->pstr->ops->open(substream);
		if (ch->runtime->hw.channels_max > 0) {
			/* Use the runtime hw constraints set by the
			 * driver's .open callback (digi00x, DICE, etc.). */
			uint32_t fmt = (ch->runtime->hw.formats &
					SNDRV_PCM_FMTBIT_S32_LE) ?
			    AFMT_S32_LE : AFMT_S16_LE;
			ch->format = SND_FORMAT(fmt,
			    ch->runtime->hw.channels_max, 0);
			c->format  = SND_FORMAT(fmt,
			    ch->runtime->hw.channels_max, 0);
		}
		if (ch->runtime->hw.rate_min > 0) {
			ch->caps.minspeed = ch->runtime->hw.rate_min;
			ch->caps.maxspeed = ch->runtime->hw.rate_max;
			/*
			 * Only clamp ch->speed into the driver's supported
			 * range; don't unconditionally force it down to
			 * minspeed.  Doing so silently dropped the sane
			 * 48000 Hz default (set above) to 44100 Hz on
			 * devices like the Digi 002/003 whose minspeed is
			 * 44100 — causing the hardware to stream at a rate
			 * that didn't match what JACK (or any app assuming
			 * its requested rate was honored without querying
			 * back) believed it was using, leading to clock
			 * drift, xruns, and audible glitching.
			 */
			if (ch->speed < ch->caps.minspeed)
				ch->speed = ch->caps.minspeed;
			else if (ch->speed > ch->caps.maxspeed)
				ch->speed = ch->caps.maxspeed;
		}
	}

	/* Select the format capability list based on the actual driver
	 * constraints, NOT the driver name.
	 *
	 * Drivers that register PCM ops with an .open callback (digi00x,
	 * DICE, HDSP) advertise multi-channel capabilities via runtime
	 * constraints and should use the full basound_fmtlist so JACK,
	 * Audacious and other apps discover the full channel range.
	 *
	 * digi00x and DICE additionally advertise AFMT_FLOAT (Audacious'
	 * default output format) because their sample paths convert float
	 * → 24-bit int before encoding.  HDSP is bitperfect and consumes
	 * raw S16/S32 only — exposing FLOAT to it would play garbage.
	 *
	 * Drivers without an .open callback (e.g. USB Line6) are simple
	 * stereo-only devices that use basound_line6_fmtlist.
	 *
	 * NOTE: This must run AFTER ops->open() so that ch->runtime->hw
	 * is populated with the real constraints before we inspect it. */
	if (substream->pstr->ops && substream->pstr->ops->open &&
	    ch->runtime != NULL) {
		if (strcmp(pcm->card->driver, "basound_dice") == 0) {
			/* DICE advertises a stereo playback stream even when
			 * its RX_NUMBER_AUDIO is 0 (channels_min/max fall
			 * back to 2), so it must still get the float list.
			 * Audacious defaults to FLOAT output. */
			ch->caps.fmtlist = basound_fmtlist_dg00x;
		} else if (ch->runtime->hw.channels_max > 2) {
			if (strcmp(pcm->card->driver, "Digi00x") == 0)
				ch->caps.fmtlist = basound_fmtlist_dg00x;
			else
				ch->caps.fmtlist = basound_fmtlist;
		} else {
			ch->caps.fmtlist = basound_line6_fmtlist;
		}
	} else {
		ch->caps.fmtlist = basound_line6_fmtlist;
	}

	return ch;
}

static int
basound_chan_free(kobj_t obj, void *data)
{
	struct basound_chan *ch = data;

	if (ch->runtime != NULL) {
		free(ch->runtime, M_ALSA);
		ch->runtime = NULL;
	}
	free(ch, M_ALSA);
	return 0;
}

static int
basound_chan_setformat(kobj_t obj, void *data, uint32_t format)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;

	ch->format = format;
	sndbuf_setfmt(ch->buffer, format);
	if (substream->runtime != NULL) {
		if (ops && ops->hw_params)
			ops->hw_params(substream, NULL);
	}
	return 0;
}

static uint32_t
basound_chan_setspeed(kobj_t obj, void *data, uint32_t speed)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;
	uint32_t actual;

	/* Enforce hardware limits (Line6 is fixed at 44.1kHz). */
	actual = speed;
	if (ch->caps.minspeed != 0 && actual < ch->caps.minspeed)
		actual = ch->caps.minspeed;
	if (ch->caps.maxspeed != 0 && actual > ch->caps.maxspeed)
		actual = ch->caps.maxspeed;

	ch->speed = actual;
	if (substream->runtime != NULL) {
		if (ops && ops->prepare) {
			/*
			 * Release CHN_LOCK before calling into the ALSA
			 * prepare op.  digi00x's pcm_prepare() may need to
			 * write the sample-rate register over FireWire — a
			 * transaction that can tsleep for up to 5 seconds on
			 * an unresponsive bus.  Holding CHN_LOCK across that
			 * blocks chn_intr() (called from the DMA callout via
			 * snd_pcm_period_elapsed), starving the realtime
			 * audio thread.  See the identical rationale in
			 * basound_chan_trigger() for ops->trigger().
			 */
			struct pcm_channel *pc = ch->channel;

			if (pc != NULL) {
				CHN_UNLOCK(pc);
				ops->prepare(substream);
				CHN_LOCK(pc);
			} else {
				ops->prepare(substream);
			}
		}
	}
	return actual;
}
static uint32_t
basound_chan_setblocksize(kobj_t obj, void *data, uint32_t blocksize)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;
	uint32_t channels = AFMT_CHANNEL(ch->format);
	uint32_t bps = AFMT_BPS(ch->format);
	uint32_t frames;

	/*
	 * Pick up the OSS channel's CURRENT format.  The channel count
	 * here determines the frame size, and ch->format can lag behind
	 * ch->channel->format when the OSS layer reconfigures the format
	 * without routing it through channel_setformat().
	 */
	if (ch->channel != NULL && ch->channel->format != 0)
		ch->format = ch->channel->format;
	channels = AFMT_CHANNEL(ch->format);
	/* AFMT_BPS handles S16 (2), S32 (4) and FLOAT (4). */
	bps = AFMT_BPS(ch->format);
	if (bps != 2 && bps != 4)
		bps = 2;

	if (channels == 0) channels = 2;

	/* 1. Calculate how many frames this blocksize represents */
	frames = blocksize / (channels * bps);

	/* 2. Round up to nearest supported latency.
	 * HDSP hardware strictly requires power-of-two (64 to 8192).
	 * digi00x, DICE and other FireWire devices have no such
	 * constraint — forcing p2 fragments creates misalignment
	 * between JACK's period and the actual DMA fragment size,
	 * producing chronic xruns and buffer-balancing oscillation.
	 * USB devices (no private_data) also benefit from bypassing
	 * the p2 rounding. */
	if (substream->pcm->private_data != NULL &&
	    strcmp(substream->pcm->card->driver, "hdsp") == 0) {
		if (frames < 64) frames = 64;
		if (frames > 8192) frames = 8192;
		
		uint32_t p2frames = 64;
		while (p2frames < frames) p2frames <<= 1;
		frames = p2frames;

		/*
		 * The HDSP hardware is a strict 2-period double buffer:
		 * the driver's hw pointer returns only 0 or period_bytes
		 * (HDSP_BufferID bit), the ISR deinterleaves ring blocks
		 * 0/1 at fixed offsets, and the DSP interrupt interval is
		 * set from the negotiated period.  The sndbuf must therefore
		 * be exactly 2 blocks of `blocksize' — if the ring has more
		 * blocks, chn_dmaupdate() computes delta = B, (bufsize-B),
		 * B, ... from the alternating pointer and the PCM layer
		 * believes 2× (or more) data was consumed, producing
		 * "double tempo, unchanged pitch" playback (chunks skipped).
		 *
		 * Cap frames so that 2 × blocksize fits the 256 KB DMA
		 * allocation; round DOWN to a power of two.
		 */
		uint32_t maxframes = (BASOUND_DMA_BUFSIZE / 2) /
		    (channels * bps);
		if (maxframes < 64)
			maxframes = 64;
		while (frames > maxframes)
			frames >>= 1;
	} else {
		/* Keep period above tiny defaults that add jitter/distortion. */
		if (frames < 64)
			frames = 64;
	}

	/* 3. Recalculate actual blocksize */
	blocksize = frames * channels * bps;
	ch->blocksize = blocksize;

	if (substream->runtime != NULL) {
		substream->runtime->period_bytes = blocksize;
		if (ops && ops->hw_params)
			ops->hw_params(substream, NULL);
	}

	/*
	 * Size the sndbuf staging buffer to exactly match the negotiated
	 * block size, using as many blocks as fit in the DMA allocation.
	 *
	 * CRITICAL: a fixed 32-block count silently FAILS (EINVAL) when
	 * 32 * blocksize exceeds BASOUND_DMA_BUFSIZE (256 KB).  The
	 * boundary is exactly 8 channels at 512 frames S16_LE
	 * (32 * 8192 = 262144 fits; 32 * 9216 = 294912 does not) —
	 * matching the observed "≤8 ch clean, >8 ch broken" split.
	 * When the resize fails, ch->buffer keeps its default 2×131072
	 * geometry while the OSS layer, JACK and this driver all assume
	 * blocksize-sized blocks: hwptr wraps at the wrong boundary,
	 * the zero-fill heuristic misfires, and playback glitches.
	 *
	 * Compute blkcnt from the available budget so the resize always
	 * succeeds and blkcnt * blksz == bufsize exactly.
	 *
	 * HDSP is the exception: the hardware is a strict 2-period
	 * double buffer and the driver's pointer / ISR / deinterleave
	 * logic all assume exactly 2 blocks (see the frames cap above).
	 * Forcing blkcnt=2 makes chn_dmaupdate() compute a constant
	 * delta = blocksize per interrupt instead of the alternating
	 * B / (bufsize-B) deltas produced when the alternating
	 * 0/period_bytes pointer is folded into a larger ring — the
	 * latter makes the PCM layer consume data at 2× (or more) the
	 * real-time rate, heard as "faster tempo, unchanged pitch".
	 */
	uint32_t blkcnt;
	if (substream->pcm->private_data != NULL &&
	    strcmp(substream->pcm->card->driver, "hdsp") == 0) {
		blkcnt = 2;
	} else {
		blkcnt = BASOUND_DMA_BUFSIZE / blocksize;
		if (blkcnt < 2)
			blkcnt = 2;
	}
	sndbuf_resize(ch->buffer, blkcnt, blocksize);

	/* Keep runtime in sync with the logical buffer size so that
	 * the USB ring-buffer math (st->end = start + dma_bytes) agrees
	 * with what the PCM layer thinks the buffer size is. */
	if (ch->runtime != NULL) {
		/*
		 * sndbuf_resize() can remap/reallocate the backing buffer.
		 * Refresh runtime DMA pointers so drivers (e.g. Line6) read
		 * from the current hardware ring, not a stale old mapping.
		 */
		ch->runtime->dma_area = ch->buffer->buf;
		ch->runtime->dma_addr = ch->buffer->buf_addr;
		ch->runtime->dma_bytes = ch->buffer->bufsize;
	}

	return blocksize;
}

static int
basound_chan_setfragments(kobj_t obj, void *data, uint32_t blocksize, uint32_t blockcount)
{
	/* We force double-buffering for hardware compatibility */
	basound_chan_setblocksize(obj, data, blocksize);
	return 0;
}

static struct pcmchan_caps *
basound_chan_getcaps(kobj_t obj, void *data)
{
	struct basound_chan *ch = data;
	return &ch->caps;
}

static struct pcmchan_matrix basound_matrix_2 = {
	.id = 1,
	.channels = 2,
	.ext = 0,
	.map = {
		{ .type = 0,  .members = (1 << 0) },
		{ .type = 1,  .members = (1 << 1) },
		{ .type = 2,  .members = 0         }
	},
	.mask = 0x0003,
	.offset = { 0, 1 }
};

static struct pcmchan_matrix basound_matrix_18 = {
	.id = 100,
	.channels = 18,
	.ext = 0,
	.map = {
		{ .type = 0,  .members = (1 << 0)  },
		{ .type = 1,  .members = (1 << 1)  },
		{ .type = 2,  .members = (1 << 2)  },
		{ .type = 3,  .members = (1 << 3)  },
		{ .type = 4,  .members = (1 << 4)  },
		{ .type = 5,  .members = (1 << 5)  },
		{ .type = 6,  .members = (1 << 6)  },
		{ .type = 7,  .members = (1 << 7)  },
		{ .type = 8,  .members = (1 << 8)  },
		{ .type = 9,  .members = (1 << 9)  },
		{ .type = 10, .members = (1 << 10) },
		{ .type = 11, .members = (1 << 11) },
		{ .type = 12, .members = (1 << 12) },
		{ .type = 13, .members = (1 << 13) },
		{ .type = 14, .members = (1 << 14) },
		{ .type = 15, .members = (1 << 15) },
		{ .type = 16, .members = (1 << 16) },
		{ .type = 17, .members = (1 << 17) },
		{ .type = 18, .members = 0         }
	},
	.mask = 0x3ffff,
	.offset = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 }
};

static struct pcmchan_matrix basound_matrix_26 = {
	.id = 101,
	.channels = 26,
	.ext = 0,
	.map = {
		{ .type = 0,  .members = (1 << 0)  },
		{ .type = 1,  .members = (1 << 1)  },
		{ .type = 2,  .members = (1 << 2)  },
		{ .type = 3,  .members = (1 << 3)  },
		{ .type = 4,  .members = (1 << 4)  },
		{ .type = 5,  .members = (1 << 5)  },
		{ .type = 6,  .members = (1 << 6)  },
		{ .type = 7,  .members = (1 << 7)  },
		{ .type = 8,  .members = (1 << 8)  },
		{ .type = 9,  .members = (1 << 9)  },
		{ .type = 10, .members = (1 << 10) },
		{ .type = 11, .members = (1 << 11) },
		{ .type = 12, .members = (1 << 12) },
		{ .type = 13, .members = (1 << 13) },
		{ .type = 14, .members = (1 << 14) },
		{ .type = 15, .members = (1 << 15) },
		{ .type = 16, .members = (1 << 16) },
		{ .type = 17, .members = (1 << 17) },
		{ .type = 18, .members = 0         }
	},
	.mask = 0x3ffff,
	.offset = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 }
};

static struct pcmchan_matrix *
basound_chan_getmatrix(kobj_t obj, void *data, uint32_t format)
{
	uint32_t channels = AFMT_CHANNEL(format);
	struct pcmchan_matrix *m;

	if (channels == 2)
		m = &basound_matrix_2;
	else if (channels == 18)
		m = &basound_matrix_18;
	else if (channels == 26)
		m = &basound_matrix_26;
	else
		m = NULL;

	return m;
}

static int
basound_chan_trigger(kobj_t obj, void *data, int go)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream;
	const struct snd_pcm_ops *ops;
	int alsa_cmd;

	if (ch == NULL)
		return (EINVAL);
	substream = ch->substream;
	if (substream == NULL || substream->pstr == NULL)
		return (EINVAL);
	ops = substream->pstr->ops;

	if (go == PCMTRIG_EMLDMAWR || go == PCMTRIG_EMLDMARD)
		return 0;

	switch (go) {
	case PCMTRIG_START:
		alsa_cmd = SNDRV_PCM_TRIGGER_START;
		break;
	case PCMTRIG_STOP:
	case PCMTRIG_ABORT:
		alsa_cmd = SNDRV_PCM_TRIGGER_STOP;
		break;
	default:
		return 0;
	}

	if (ops && (ops->trigger || (go == PCMTRIG_START && ops->prepare))) {
		/*
		 * Release CHN_LOCK before calling the ALSA prepare/trigger
		 * ops.
		 *
		 * digi00x's trigger does FireWire transactions (tsleep with
		 * 5-second timeout) and itx_disable (pause 1s).  Its
		 * prepare() can likewise write the sample-rate register
		 * over FireWire (tsleep, 5-second timeout) when the rate
		 * actually changes.  Holding CHN_LOCK across those sleeps
		 * blocks chn_intr (called from the DMA callout via
		 * snd_pcm_period_elapsed), creating a deadlock during STOP:
		 * the callout blocks on CHN_LOCK in chn_intr while the
		 * trigger sleeps in pause(), and the callout is then in the
		 * middle of refill when the trigger continues with
		 * itx_disable + db_free + queue drain — a corrupted DMA
		 * queue causes a panic/reboot.  For START, calling prepare()
		 * under CHN_LOCK similarly stalls the realtime audio thread
		 * (observed as JACK read/write timeouts) even though relaxed
		 * playback-only apps don't notice.
		 *
		 * chn_trigger() calls us with CHN_LOCK held and drops it
		 * after we return on success.  On error it returns with
		 * CHN_LOCK held — we must re-acquire before returning
		 * failure so the caller's invariants stay intact.
		 */
		struct pcm_channel *pc = ch->channel;
		int err = 0;

		CHN_UNLOCK(pc);
		if (go == PCMTRIG_START && ops->prepare)
			ops->prepare(substream);
		if (ops->trigger)
			err = ops->trigger(substream, alsa_cmd);
		CHN_LOCK(pc);

		/* ALSA ops return Linux-style negative errno; FreeBSD
		 * channel methods must return 0 on success or a positive
		 * errno. */
		return (err < 0) ? -err : err;
	}

	return 0;
}

static uint32_t
basound_chan_getptr(kobj_t obj, void *data)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;

	if (ops && ops->pointer)
		return (uint32_t)ops->pointer(substream);

	return 0;
}

static kobj_method_t basound_chan_methods[] = {
	KOBJMETHOD(channel_init,		basound_chan_init),
	KOBJMETHOD(channel_free,		basound_chan_free),
	KOBJMETHOD(channel_getcaps,		basound_chan_getcaps),
	KOBJMETHOD(channel_getmatrix,		basound_chan_getmatrix),
	KOBJMETHOD(channel_setformat,		basound_chan_setformat),
	KOBJMETHOD(channel_setspeed,		basound_chan_setspeed),
	KOBJMETHOD(channel_setblocksize,	basound_chan_setblocksize),
	KOBJMETHOD(channel_setfragments,	basound_chan_setfragments),
	KOBJMETHOD(channel_trigger,		basound_chan_trigger),
	KOBJMETHOD(channel_getptr,		basound_chan_getptr),
	KOBJMETHOD_END
};
DEFINE_CLASS(basound_chan, basound_chan_methods, 0);

/*
 * PCM child device driver.
 *
 * The parent PCI device (e.g. basound_hdsp) creates a "pcm" child device via
 * device_add_child(), stores a struct snd_pcm * in its ivars, and calls
 * device_probe_and_attach().  This driver is registered for the
 * "basound_hdsp" bus, so newbus finds it, allocates PCM_SOFTC_SIZE bytes
 * for the snddev_info softc, and calls basound_pcm_attach().
 */
static int
basound_pcm_probe(device_t dev)
{
	device_set_desc(dev, "PCM Audio");
	return BUS_PROBE_DEFAULT;
}

static int
basound_pcm_attach(device_t dev)
{
	struct snd_pcm *pcm = device_get_ivars(dev);
	struct snd_card *card = pcm->card;
	struct snd_pcm_str *pstr_p = &pcm->streams[SNDRV_PCM_STREAM_PLAYBACK];
	struct snd_pcm_str *pstr_c = &pcm->streams[SNDRV_PCM_STREAM_CAPTURE];
	char status[SND_STATUSLEN];
	int is_line6 = (strcmp(card->driver, "line6_bsd") == 0);

	/* Set description here because basound_pcm_probe is bypassed
	 * (we use device_set_driver + device_attach directly). */
	device_set_desc_copy(dev, pcm->id[0] ? pcm->id : "HDSP PCM");

	/* dev's softc is PCM_SOFTC_SIZE bytes — safe for snddev_info */
	pcm_init(dev, pcm);

	/*
	 * Enable bitperfect mode for HDSP and digi00x so the app's audio
	 * flows directly to the hardware channel without feeder matrix
	 * conversion (which cannot handle the 18-channel format).
	 *
	 * The HDSP interrupt handler (hdsp_deinterleave_to_planar) reads
	 * the actual negotiated channel count from ch->format and copies
	 * only the channels the app is actually using into the planar DMA
	 * buffer — unused channels stay at zero.  This is safe.
	 */
	if (pcm->private_data != NULL || is_line6) {
		pcm_setflags(dev, pcm_getflags(dev) | SD_F_BITPERFECT);
	}

	/*
	 * Add channels before pcm_register().  pcm_register() inspects
	 * playcount/reccount and sets SD_F_SIMPLEX when either is zero,
	 * which prevents a second open() on the same device in the opposite
	 * direction (errno EOPNOTSUPP).  All reference drivers follow the
	 * same pcm_init → pcm_addchan → pcm_register ordering.
	 */
	if (pstr_p->substream_count > 0)
		pcm_addchan(dev, PCMDIR_PLAY, &basound_chan_class, pcm);
	if (pstr_c->substream_count > 0)
		pcm_addchan(dev, PCMDIR_REC, &basound_chan_class, pcm);

	/*
	 * Pre-set vchan format/rate for non-bitperfect devices.
	 * With SD_F_BITPERFECT set (HDSP, digi00x), no vchan is
	 * created and these fields are unused.
	 *
	 * Without this, {p,r}vchanformat=0 causes vchan_create() to
	 * call chn_reset(parent, 0, 0), which skips feeder_chain().  The
	 * parent channel then keeps feeder_root (installed during chn_init
	 * before CHN_F_HAS_VCHAN was set), so playback won't mix children
	 * and capture won't distribute to children.
	 */
	if (pcm->private_data == NULL && !is_line6) {
		struct snddev_info *d = device_get_softc(dev);
		d->pvchanformat = SND_FORMAT(AFMT_S16_LE, 2, 0);
		d->pvchanrate = 44100;
		d->rvchanformat = SND_FORMAT(AFMT_S16_LE, 2, 0);
		d->rvchanrate = 44100;
	}

	snprintf(status, sizeof(status), "at %s",
	    device_get_nameunit(device_get_parent(dev)));

	if (pcm_register(dev, status) != 0) {
		dev_err(card->dev, "pcm_register failed\n");
		return ENXIO;
	}

	/*
	 * pcm_register() unconditionally sets SD_F_PVCHANS | SD_F_RVCHANS
	 * when playcount/reccount > 0, which causes dsp_open() to call
	 * vchan_create() on every open — even for bitperfect devices.
	 *
	 * For HDSP/digi00x/DICE/Line6, the app must talk directly to
	 * the hardware channel.  Clear the vchan flags and counters
	 * AFTER pcm_register() so dsp_open() skips vchan_create and
	 * opens the real hardware channel instead.
	 */
	if (pcm->private_data != NULL || is_line6) {
		struct snddev_info *d = device_get_softc(dev);
		d->flags &= ~(SD_F_PVCHANS | SD_F_RVCHANS);
		d->pvchancount = 0;
		d->rvchancount = 0;
	}

	return 0;
}

static int
basound_pcm_detach(device_t dev)
{
	return pcm_unregister(dev);
}

static device_method_t basound_pcm_methods[] = {
	DEVMETHOD(device_probe,		basound_pcm_probe),
	DEVMETHOD(device_attach,	basound_pcm_attach),
	DEVMETHOD(device_detach,	basound_pcm_detach),
	DEVMETHOD_END
};

static driver_t basound_pcm_driver = {
	"pcm",
	basound_pcm_methods,
	PCM_SOFTC_SIZE,
};

/*
 * Register the PCM sub-driver under every basound bus type so that
 * device_probe_and_attach() finds it when the parent creates a "pcm" child.
 */
DRIVER_MODULE(basound_pcm, basound_hdsp,    basound_pcm_driver, 0, 0);
DRIVER_MODULE(basound_pcm, basound_line6,   basound_pcm_driver, 0, 0);
DRIVER_MODULE(basound_pcm, basound_digi00x, basound_pcm_driver, 0, 0);

/*
 * basound_pcm_register — called from snd_card_register().
 *
 * Creates a "pcm" child device under the parent PCI device, stores the
 * snd_pcm pointer as ivars, and lets newbus probe/attach it via the
 * basound_pcm sub-driver above.  The child device gets PCM_SOFTC_SIZE bytes
 * for its softc, so pcm_init() writes into snddev_info, not our own softc.
 */
int
basound_pcm_register(struct snd_pcm *pcm)
{
	struct snd_card *card = pcm->card;
	device_t parent = card->dev->bsddev;
	device_t pcm_dev;
	int err;

	pcm_dev = device_add_child(parent, "pcm", -1);
	if (pcm_dev == NULL) {
		dev_err(card->dev, "device_add_child(pcm) failed\n");
		return -ENXIO;
	}

	device_set_ivars(pcm_dev, pcm);

	/*
	 * Explicitly set our driver so device_set_driver() allocates
	 * PCM_SOFTC_SIZE bytes for the softc.  We then call device_attach()
	 * directly to skip the devclass-based probe path (which would race
	 * against the DRIVER_MODULE SYSINIT if the PCI reprobe fires before
	 * our SYSINIT has run).
	 */
	err = device_set_driver(pcm_dev, &basound_pcm_driver);
	if (err != 0) {
		dev_err(card->dev, "device_set_driver failed: %d\n", err);
		device_delete_child(parent, pcm_dev);
		return -ENXIO;
	}

	card->pcm_dev = pcm_dev;

	err = device_attach(pcm_dev);
	if (err != 0) {
		dev_err(card->dev, "pcm device attach failed: %d\n", err);
		device_delete_child(parent, pcm_dev);
		card->pcm_dev = NULL;
		return -ENXIO;
	}

	return 0;
}
