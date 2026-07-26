/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-pcm.c - PCM device for Digidesign Digi 002/003
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/callout.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "digi00x.h"
#include "alsa_pcm_bsd.h"

/* The Digi 002/003 provides 18 channels at 44.1/48 kHz (8 analog + 8 ADAT + 2 SPDIF)
 * and 10 channels at 88.2/96 kHz (8 analog + 2 SPDIF). */

/* ------------------------------------------------------------------ */
/* Single shared callout for PCM timing engine                         *
 *                                                                     *
 * Uses a single callout per device to drive both playback and capture *
 * period advancement.  A counter (active_streams) tracks how many     *
 * directions are active.  The callout is stopped only when both       *
 * streams become inactive.                                            *
 *                                                                     *
 * When the session is fully active and ISO DMA is working, this       *
 * callout provides the PCM timing signal (snd_pcm_period_elapsed)     *
 * so that the PCM layer advances its pointer correctly.               *
 * ------------------------------------------------------------------ */

static void
dg00x_pcm_stream_cb(void *arg)
{
	struct snd_dg00x *dg00x = (struct snd_dg00x *)arg;
	struct dg00x_pcm_stream *ps_pb = &dg00x->pcm_playback;
	struct dg00x_pcm_stream *ps_cap = &dg00x->pcm_capture;
	unsigned int frames;
	int ticks;

	/* Advance playback position and signal period */
	if (ps_pb->active) {
		ps_pb->hwptr += ps_pb->period_bytes;
		if (ps_pb->hwptr >= ps_pb->buffer_bytes)
			ps_pb->hwptr -= ps_pb->buffer_bytes;
		if (ps_pb->substream)
			snd_pcm_period_elapsed(ps_pb->substream);
	}

	/* Advance capture position and signal period */
	if (ps_cap->active) {
		ps_cap->hwptr += ps_cap->period_bytes;
		if (ps_cap->hwptr >= ps_cap->buffer_bytes)
			ps_cap->hwptr -= ps_cap->buffer_bytes;
		if (ps_cap->substream)
			snd_pcm_period_elapsed(ps_cap->substream);
	}

	/* If neither stream is active, stop the callout */
	if (!ps_pb->active && !ps_cap->active)
		return;

	/* Reschedule at the period interval of whichever stream is active.
	 * Use the playback period if both are active. */
	if (ps_pb->active) {
		frames = ps_pb->period_bytes / (ps_pb->pcm_channels * 4);
		ticks = frames * hz / ps_pb->rate;
	} else {
		frames = ps_cap->period_bytes / (ps_cap->pcm_channels * 4);
		ticks = frames * hz / ps_cap->rate;
	}
	if (ticks < 1)
		ticks = 1;
	callout_reset(&dg00x->callout, ticks, dg00x_pcm_stream_cb, dg00x);
}

static int
dg00x_pcm_stream_start(struct snd_dg00x *dg00x, int direction,
		       struct snd_pcm_substream *substream)
{
	struct dg00x_pcm_stream *ps;
	struct basound_chan *ch;
	unsigned int channels, bps;
	int ticks;

	ps = (direction == SNDRV_PCM_STREAM_PLAYBACK) ?
	     &dg00x->pcm_playback : &dg00x->pcm_capture;

	if (ps->active)
		return 0;

	ch = substream->private_data;
	if (ch == NULL || ch->buffer == NULL)
		return -EINVAL;

	channels = AFMT_CHANNEL(ch->format);
	if (channels == 0)
		channels = 2;
	bps = (ch->format & AFMT_S32_LE) ? 4 : 2;

	ps->direction = direction;
	ps->pcm_channels = channels;
	ps->rate = ch->speed > 0 ? ch->speed : 48000;
	ps->period_bytes = ch->blocksize;
	ps->buffer_bytes = ch->buffer->bufsize;
	ps->hwptr = 0;
	ps->active = true;

	/* Reset DOT state at stream start */
	dot_reset_state(&ps->dot);

	dg00x->active_streams++;

	/* Schedule first period callback only if this is the first stream. */
	if (dg00x->active_streams == 1) {
		ticks = (ps->period_bytes / (ps->pcm_channels * bps)) * hz / ps->rate;
		if (ticks < 1)
			ticks = 1;
		callout_reset(&dg00x->callout, ticks,
			      dg00x_pcm_stream_cb, dg00x);
	}

	return 0;
}

static void
dg00x_pcm_stream_stop(struct snd_dg00x *dg00x, int direction)
{
	struct dg00x_pcm_stream *ps;

	ps = (direction == SNDRV_PCM_STREAM_PLAYBACK) ?
	     &dg00x->pcm_playback : &dg00x->pcm_capture;

	if (!ps->active)
		return;

	ps->active = false;
	if (dg00x->active_streams > 0)
		dg00x->active_streams--;

	/* Only stop the shared callout when both streams are inactive. */
	if (dg00x->active_streams == 0)
		callout_stop(&dg00x->callout);
}

/* ------------------------------------------------------------------ */
/* PCM ops                                                             */
/* ------------------------------------------------------------------ */

static int
pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	(void)substream;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;

	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;

	runtime->hw.channels_min = 2;
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
	struct snd_dg00x *dg00x = (struct snd_dg00x *)substream->pcm->private_data;
	struct basound_chan *ch = (struct basound_chan *)substream->private_data;

	/* Reset position for the stream direction */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dg00x->pcm_playback.hwptr = 0;
		dg00x->pcm_playback.rate = ch->speed > 0 ? ch->speed : 48000;
	} else {
		dg00x->pcm_capture.hwptr = 0;
		dg00x->pcm_capture.rate = ch->speed > 0 ? ch->speed : 48000;
	}

	return (0);
}

static int
pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_dg00x *dg00x = (struct snd_dg00x *)substream->pcm->private_data;
	int err;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* Allocate isochronous resources if not already done */
		if (dg00x->tx_resources.channel < 0 &&
		    dg00x->rx_resources.channel < 0) {
			err = dg00x_alloc_isoc_resources(dg00x);
			if (err < 0)
				return (err);
		}

		/* Initialize streaming engine if not already done */
		if (dg00x->iso_tx.dmach < 0 &&
		    dg00x->iso_rx.dmach < 0) {
			err = dg00x_streaming_init(dg00x);
			if (err < 0)
				return (err);
		}

		/* Start the callout-based timing engine */
		err = dg00x_pcm_stream_start(dg00x,
			substream->stream, substream);
		if (err < 0)
			return (err);

		/* Start real ISO DMA streaming */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dg00x_streaming_start_tx(dg00x);
		else
			dg00x_streaming_start_rx(dg00x);

		/* Begin hardware session (only if both channels are ready).
		 * We always begin the session; finish_session is ref-counted. */
		dg00x_begin_session(dg00x,
		    dg00x->tx_resources.channel,
		    dg00x->rx_resources.channel);

		return (0);

	case SNDRV_PCM_TRIGGER_STOP:
		/* Stop ISO DMA first */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dg00x_streaming_stop_tx(dg00x);
		else
			dg00x_streaming_stop_rx(dg00x);

		/* Stop callout timing engine (ref-counted via active_streams) */
		dg00x_pcm_stream_stop(dg00x, substream->stream);

		/* Only finish the hardware session when ALL streams have
		 * stopped — the device cannot handle a partial session stop. */
		if (dg00x->active_streams == 0)
			dg00x_finish_session(dg00x);

		return (0);

	default:
		return -EINVAL;
	}
}

static unsigned long
pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_dg00x *dg00x = (struct snd_dg00x *)substream->pcm->private_data;
	struct dg00x_pcm_stream *ps;

	ps = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
	     &dg00x->pcm_playback : &dg00x->pcm_capture;

	return ps->hwptr;
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

	/* Initialize shared callout and active stream counter */
	callout_init(&dg00x->callout, 1);
	dg00x->active_streams = 0;

	/* Store substream references for the callout callback */
	dg00x->pcm_playback.substream =
	    pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	dg00x->pcm_capture.substream =
	    pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;

	return (0);
}
