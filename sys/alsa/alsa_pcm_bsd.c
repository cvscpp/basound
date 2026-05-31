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
	 * bus_dmamem_alloc uses dmatag->maxsize (4 MB) as the allocation size,
	 * which matches the size we pass to sndbuf_alloc. */
	if (pcm->card->dmatag == NULL) {
		free(ch->runtime, M_ALSA);
		free(ch, M_ALSA);
		return NULL;
	}
	if (sndbuf_alloc(b, pcm->card->dmatag, 0,
	    4 * 1024 * 1024) != 0) {
		free(ch->runtime, M_ALSA);
		free(ch, M_ALSA);
		return NULL;
	}

	/* Make the DMA address visible to the ALSA runtime and the
	 * hardware trigger path (hdsp_main.c reads these via
	 * hdsp->playback_substream->runtime->dma_addr). */
	substream->runtime->dma_area  = sndbuf_getbuf(b);
	substream->runtime->dma_addr  = sndbuf_getbufaddr(b);
	substream->runtime->dma_bytes = sndbuf_getsize(b);

	/* Initialize hardware format.  HDSP stores a struct hdsp * in
	 * pcm->private_data with the exact channel count; other drivers
	 * (e.g. USB Line6) leave private_data NULL and default to stereo. */
	if (pcm->private_data != NULL) {
		struct hdsp *hdsp = pcm->private_data;
		int hw_channels = (dir == PCMDIR_PLAY) ?
		    hdsp->ss_out_channels : hdsp->ss_in_channels;
		ch->format = SND_FORMAT(AFMT_S32_LE, hw_channels, 0);
		c->format  = SND_FORMAT(AFMT_S32_LE, hw_channels, 0);
	} else {
		/* Non-HDSP device (e.g. USB audio): stereo S16_LE */
		ch->format = SND_FORMAT(AFMT_S16_LE, 2, 0);
		c->format  = SND_FORMAT(AFMT_S16_LE, 2, 0);
	}
	ch->speed = 48000;
	ch->blocksize = 4096;

	/* Initialize per-channel capabilities from ALSA constraints. */
	ch->caps.fmtlist = basound_fmtlist;
	ch->caps.caps = DSP_CAP_DUPLEX;
	ch->caps.minspeed = 32000;
	ch->caps.maxspeed = 192000;

	if (substream->pstr->ops && substream->pstr->ops->open) {
		substream->pstr->ops->open(substream);
		if (ch->runtime->hw.rate_min > 0) {
			ch->caps.minspeed = ch->runtime->hw.rate_min;
			ch->caps.maxspeed = ch->runtime->hw.rate_max;
			ch->speed = ch->caps.minspeed;
		}
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

	ch->speed = speed;
	if (substream->runtime != NULL) {
		if (ops && ops->prepare)
			ops->prepare(substream);
	}
	return speed;
}
static uint32_t
basound_chan_setblocksize(kobj_t obj, void *data, uint32_t blocksize)
{
	struct basound_chan *ch = data;
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;
	uint32_t channels = AFMT_CHANNEL(ch->format);
	uint32_t bps = (ch->format & AFMT_S32_LE) ? 4 : 2;
	uint32_t frames;

	if (channels == 0) channels = 2;

	/* 1. Calculate how many frames this blocksize represents */
	frames = blocksize / (channels * bps);

	/* 2. Round up to nearest supported latency.
	 * HDSP hardware strictly requires power-of-two (64 to 8192).
	 * For other devices (USB), we allow the requested size to improve sync. */
	if (substream->pcm->private_data != NULL) {
		if (frames < 64) frames = 64;
		if (frames > 8192) frames = 8192;
		
		uint32_t p2frames = 64;
		while (p2frames < frames) p2frames <<= 1;
		frames = p2frames;
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
	 * HDSP uses a hardware double-buffer (exactly 2 periods): HDSP_BufferID
	 * only ever reports position 0 or period_bytes.  Using blkcnt > 2 causes
	 * the PCM layer's write pointer to drift ahead of the hardware read
	 * pointer, eventually filling all blocks and stopping writes → silence.
	 *
	 * For non-HDSP devices (e.g. USB audio) keep a larger ring to absorb
	 * ISO transfer jitter.
	 */
	uint32_t blkcnt;
	if (ch->substream->pcm->private_data != NULL) {
		blkcnt = 2;
	} else {
		blkcnt = 32768 / blocksize;
		if (blkcnt < 4)
			blkcnt = 4;
	}
	sndbuf_resize(ch->buffer, blkcnt, blocksize);

	/* Keep runtime in sync with the logical buffer size so that
	 * the USB ring-buffer math (st->end = start + dma_bytes) agrees
	 * with what the PCM layer thinks the buffer size is. */
	if (ch->runtime != NULL)
		ch->runtime->dma_bytes = sndbuf_getsize(ch->buffer);

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

	if (channels == 18)
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
	struct snd_pcm_substream *substream = ch->substream;
	const struct snd_pcm_ops *ops = substream->pstr->ops;
	int alsa_cmd;

	if (go == PCMTRIG_EMLDMAWR || go == PCMTRIG_EMLDMARD)
		return 0;

	switch (go) {
	case PCMTRIG_START:
		if (ops && ops->prepare)
			ops->prepare(substream);
		alsa_cmd = SNDRV_PCM_TRIGGER_START;
		break;
	case PCMTRIG_STOP:
	case PCMTRIG_ABORT:
		alsa_cmd = SNDRV_PCM_TRIGGER_STOP;
		break;
	default:
		return 0;
	}

	if (ops && ops->trigger) {
		/* ALSA ops return Linux-style negative errno; FreeBSD channel
		 * methods must return 0 on success or a positive errno. */
		int err = ops->trigger(substream, alsa_cmd);
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
	unsigned long frames;
	uint32_t ptr;

	if (ops && ops->pointer) {
		frames = ops->pointer(substream);
		uint32_t frame_bytes = AFMT_BPS(ch->format) * AFMT_CHANNEL(ch->format);
		ptr = (uint32_t)(frames * frame_bytes);
		return ptr;
	}

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

	/* Set description here because basound_pcm_probe is bypassed
	 * (we use device_set_driver + device_attach directly). */
	device_set_desc_copy(dev, pcm->id[0] ? pcm->id : "HDSP PCM");

	/* dev's softc is PCM_SOFTC_SIZE bytes — safe for snddev_info */
	pcm_init(dev, pcm);

	/*
	 * Only enable bitperfect mode for multi-channel devices (e.g. HDSP
	 * with 18/26 channels).  For stereo USB devices like Line6, leave it
	 * off so the PCM layer handles normal format negotiation.
	 */
	if (pcm->private_data != NULL) {
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
	 * Pre-set vchan format/rate for non-bitperfect (e.g. Line6 USB)
	 * devices.  Without this, pvchanformat=0 causes vchan_create() to
	 * call chn_reset(parent, 0, 0), which skips feeder_chain().  The
	 * parent channel then keeps feeder_root (installed during chn_init
	 * before CHN_F_HAS_VCHAN was set), which reads from the empty parent
	 * bufsoft instead of mixing vchan children → silence.  With non-zero
	 * fmt+rate, feeder_chain() builds feeder_mixer on the parent, which
	 * correctly reads from the active vchan children.
	 */
	if (pcm->private_data == NULL) {
		struct snddev_info *d = device_get_softc(dev);
		d->pvchanformat = SND_FORMAT(AFMT_S16_LE, 2, 0);
		d->pvchanrate = 44100;
	}

	snprintf(status, sizeof(status), "at %s",
	    device_get_nameunit(device_get_parent(dev)));

	if (pcm_register(dev, status) != 0) {
		dev_err(card->dev, "pcm_register failed\n");
		return ENXIO;
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
DRIVER_MODULE(basound_pcm, basound_hdsp,  basound_pcm_driver, 0, 0);
DRIVER_MODULE(basound_pcm, basound_line6, basound_pcm_driver, 0, 0);

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

