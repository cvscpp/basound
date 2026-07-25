/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-pcm.c - PCM device for Digidesign Digi 002/003
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "digi00x.h"

/* The Digi 002/003 provides 18 channels at 44.1/48 kHz (8 analog + 8 ADAT + 2 SPDIF)
 * and 10 channels at 88.2/96 kHz (8 analog + 2 SPDIF). */

static int
pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	(void)substream;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;

	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;

	runtime->hw.channels_min = 10;
	runtime->hw.channels_max = 18;

	runtime->hw.rates = SNDRV_PCM_RATE_44100 |
			    SNDRV_PCM_RATE_48000 |
			    SNDRV_PCM_RATE_88200 |
			    SNDRV_PCM_RATE_96000;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 96000;

	runtime->hw.buffer_bytes_max = 1 << 24;
	runtime->hw.period_bytes_min = 256;
	runtime->hw.period_bytes_max = 1 << 16;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 1024;

	return (0);
}

static int
pcm_close(struct snd_pcm_substream *substream)
{
	return (0);
}

static int
pcm_hw_params(struct snd_pcm_substream *substream,
	      void *hw_params)
{
	struct snd_dg00x *dg00x = substream->pcm->private_data;
	unsigned int rate;
	bool rate_ok = false;
	int i;

	/*
	 * basound_chan_setformat() and basound_chan_setblocksize() call
	 * ops->hw_params(substream, NULL) to re-apply hardware settings
	 * after a format or blocksize change.  When hw_params is NULL
	 * we skip rate/channel validation and just return success.
	 */
	if (hw_params == NULL)
		return (0);

	rate = params_rate(hw_params);

	/* Find supported channels for this rate */
	for (i = 0; i < SND_DG00X_RATE_COUNT; i++) {
		if (snd_dg00x_stream_rates[i] == rate) {
			rate_ok = true;
			break;
		}
	}
	if (!rate_ok)
		return -EINVAL;

	/* Set sample rate on device */
	dg00x_set_local_rate(dg00x, rate);

	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int
pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int
pcm_prepare(struct snd_pcm_substream *substream)
{
	return (0);
}

static int
pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_STOP:
		return (0);
	default:
		return -EINVAL;
	}
}

static unsigned long
pcm_pointer(struct snd_pcm_substream *substream)
{
	return (0);
}

static const struct snd_pcm_ops dg00x_pcm_ops = {
	.open		= pcm_open,
	.close		= pcm_close,
	.hw_params	= pcm_hw_params,
	.hw_free	= pcm_hw_free,
	.prepare	= pcm_prepare,
	.trigger	= pcm_trigger,
	.pointer	= pcm_pointer,
};

int
dg00x_create_pcm(struct snd_dg00x *dg00x)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(dg00x->card, "Digi00x", 0, 1, 1, &pcm);
	if (err != 0)
		return (err);

	pcm->private_data = dg00x;
	snprintf(pcm->name, sizeof(pcm->name), "%s PCM",
		 dg00x->card->shortname);

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dg00x_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &dg00x_pcm_ops);

	return (0);
}
