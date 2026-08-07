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
/* Callout-based period signalling and TX refill                       */
/*                                                                     */
/* ISO DMA handlers advance hwptr and accumulate progress into         *
 * period_accum.  A shared callout fires at 1 ms intervals to:        *
 *   1. Refill TX ISO chunks from stfree → stvalid (keeping the       *
 *      FireWire ISO pipeline fed)                                     *
 *   2. Signal snd_pcm_period_elapsed when a full period of bytes     *
 *      has been accumulated                                           *
 *                                                                     *
 * This avoids calling chn_intr (which acquires CHN_LOCK) from the     *
 * fwohci interrupt context, preventing a lock-ordering conflict       *
 * with the FireWire bus.                                              *
 *                                                                     *
 * NOTE: fwohci does NOT call a handler for TX (only RX).  All TX      *
 * refilling is driven from this callout.                              *
 * ------------------------------------------------------------------ */

#define DG00X_REFILL_TICKS	1	/* 1 ms at hz=1000 */

/*
 * CIP sampling-frequency code, used in the CIP FDF field.
 * Matches the Linux amdtp-stream.c convention:
 *   fdf = AMDTP_FDF_AM824 | sfc  where sfc is the CIP_SFC_ code.
 */
static unsigned int
dg00x_rate_to_fdf(unsigned int rate)
{
	switch (rate) {
	case 44100:	return (1);	/* CIP_SFC_44100 */
	case 48000:	return (2);	/* CIP_SFC_48000 */
	case 88200:	return (3);	/* CIP_SFC_88200 */
	case 96000:	return (4);	/* CIP_SFC_96000 */
	default:	return (2);
	}
}

/*
 * Rate index into snd_dg00x_stream_rates / snd_dg00x_stream_pcm_channels.
 * Used to look up the full channel complement the Digi 002/003 always
 * carries per data block (18 at 44.1/48 kHz, 10 at 88.2/96 kHz).
 */
static unsigned int
dg00x_rate_index(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < SND_DG00X_RATE_COUNT; i++) {
		if (snd_dg00x_stream_rates[i] == rate)
			return (i);
	}
	return (SND_DG00X_RATE_48000);
}

void
dg00x_pcm_update_position(struct dg00x_pcm_stream *ps, unsigned int bytes)
{

	if (ps->active)
		ps->period_accum += bytes;
}

static void
dg00x_pcm_stream_cb(void *arg)
{
	struct snd_dg00x *dg00x = (struct snd_dg00x *)arg;
	struct dg00x_pcm_stream *ps_pb = &dg00x->pcm_playback;
	struct dg00x_pcm_stream *ps_cap = &dg00x->pcm_capture;

	/* Refill TX ISO chunks to keep the FireWire pipeline fed.
	 * The fwohci does not call a handler for TX; refilling must
	 * be driven externally. */
	dg00x_streaming_refill_tx(dg00x);

	/* Signal period_elapsed for active streams that have
	 * accumulated at least one full period of data.  The PCM
	 * layer will check hwptr via pcm_pointer. */
	if (ps_pb->active && ps_pb->substream != NULL &&
	    ps_pb->period_accum >= ps_pb->period_bytes) {
		ps_pb->period_accum -= ps_pb->period_bytes;
		snd_pcm_period_elapsed(ps_pb->substream);
	}
	if (ps_cap->active && ps_cap->substream != NULL &&
	    ps_cap->period_accum >= ps_cap->period_bytes) {
		ps_cap->period_accum -= ps_cap->period_bytes;
		snd_pcm_period_elapsed(ps_cap->substream);
	}

	/* Stop callout if neither stream is active */
	if (!ps_pb->active && !ps_cap->active)
		return;

	/* Reschedule at the TX refill interval (1 ms).  This is fast
	 * enough to keep the ISO pipeline fed (8000 cycles/sec means
	 * we need to refill at least every DG00X_ISO_NCHUNKS/8 ms). */
	callout_reset(&dg00x->callout, DG00X_REFILL_TICKS,
	    dg00x_pcm_stream_cb, dg00x);
}

/* ------------------------------------------------------------------ */
/* Stream start / stop helpers                                         */
/* ------------------------------------------------------------------ */

static int
dg00x_pcm_stream_start(struct snd_dg00x *dg00x, int direction,
		       struct snd_pcm_substream *substream)
{
	struct dg00x_pcm_stream *ps;
	struct basound_chan *ch;
	unsigned int channels;

	ps = (direction == SNDRV_PCM_STREAM_PLAYBACK) ?
	     &dg00x->pcm_playback : &dg00x->pcm_capture;

	if (ps->active)
		return (0);

	ch = substream->private_data;
	if (ch == NULL || ch->buffer == NULL)
		return (-EINVAL);

	/*
	 * Read the negotiated channel count from the OSS channel's
	 * format field, which is always up to date.  ch->format is
	 * our local copy that may lag behind when the OSS framework
	 * reconstructs the format without calling chn_setformat
	 * (e.g. during SNDCTL_DSP_CHANNELS ioctl processing).
	 */
	if (ch->channel != NULL && ch->channel->format != 0)
		ch->format = ch->channel->format;

	channels = AFMT_CHANNEL(ch->format);
	if (channels == 0)
		channels = 2;

	ps->direction = direction;
	ps->pcm_channels = channels;
	ps->rate = ch->speed > 0 ? ch->speed : 48000;
	ps->fdf = dg00x_rate_to_fdf(ps->rate);

	/* Set sample rate on device before starting stream (no-op if
	 * already programmed to this rate — see dg00x_ensure_local_rate). */
	dg00x_ensure_local_rate(dg00x, ps->rate);
	/*
	 * The device always carries its full channel complement per data
	 * block (18 at 44.1/48 kHz, 10 at 88.2/96 kHz), independent of
	 * the channel count the PCM application opened.  The TX fill and
	 * RX handlers use this stride; the app's channels are mapped to
	 * the first channels of the block.
	 */
	ps->device_channels =
	    snd_dg00x_stream_pcm_channels[dg00x_rate_index(ps->rate)];
	ps->period_bytes = ch->blocksize;
	/*
	 * Use runtime->dma_bytes for buffer bounds, not ch->buffer->bufsize.
	 * The sndbuf staging buffer and the runtime DMA area are the same
	 * underlying memory (dma_area = b->buf, set in basound_chan_init),
	 * but using runtime->dma_bytes keeps the wrap check consistent with
	 * whatever ALSA hw_params may have negotiated.
	 */
	ps->buffer_bytes = substream->runtime->dma_bytes;
	ps->hwptr = 0;
	ps->period_accum = 0;
	ps->tx_dbc = 0;
	ps->active = true;

	/*
	 * Sync runtime->dma_area from ch->buffer->buf.  The OSS layer
	 * may have reallocated the buffer via chn_resizebuf() after
	 * the ALSA runtime was first set up, making the old dma_area
	 * pointer stale.  Without this sync, the driver reads from the
	 * old (zero-filled) buffer while the OSS app writes audio to
	 * the new buffer.
	 */
	substream->runtime->dma_area = ch->buffer->buf;
	substream->runtime->dma_addr = ch->buffer->buf_addr;
	substream->runtime->dma_bytes = ch->buffer->bufsize;
	ps->buffer_bytes = ch->buffer->bufsize;

	/*
	 * Clear CHN_F_MMAP on the OSS channel.  When set (e.g.
	 * by OSS mmap syscall), chn_wrfeed calls sndbuf_acquire
	 * on bufsoft — marking all of it "ready" with zeros —
	 * then feeds those zeros into bufhard, destroying audio.
	 * Our fill_chunk reads from bufhard directly via sndbuf
	 * API, and the OSS write path (bufsoft→feeder→bufhard)
	 * handles the data flow.  CHN_F_MMAP is destructive here.
	 */
	ch->channel->flags &= ~CHN_F_MMAP;

	/* AMDTP fractional framing: FireWire runs at 8000 ISO cycles/sec.
	 * Each packet must carry rate/8000 frames on average.  The
	 * remainder is distributed via a modulo accumulator so that
	 * the long-term average matches the sample rate exactly. */
	ps->frames_per_packet = ps->rate / 8000;
	ps->frame_remainder   = ps->rate % 8000;
	ps->frame_cycle       = 0;

	/* Reset DOT state at stream start */
	dot_reset_state(&ps->dot);

	dg00x->active_streams++;

	printf("digi00x: stream_start — dir=%s rate=%u pcm_ch=%u dev_ch=%u "
	    "period_bytes=%u buffer_bytes=%u format=0x%08x bps=%u ready=%d\n",
	    direction == SNDRV_PCM_STREAM_PLAYBACK ? "PB" : "CAP",
	    ps->rate, ps->pcm_channels, ps->device_channels,
	    ps->period_bytes, ps->buffer_bytes, ch->format,
	    AFMT_BPS(ch->format), ch->buffer ? sndbuf_getready(ch->buffer) : -1);
	printf("digi00x: stream_start — dma_area=%p dma_bytes=%zu "
	    "ch->buffer->buf=%p ch->buffer->bufsize=%u\n",
	    substream->runtime->dma_area, substream->runtime->dma_bytes,
	    ch->buffer->buf, ch->buffer->bufsize);

	/*
	 * When playback starts, clone its rate and channel geometry
	 * into the capture stream so the RX DMA handler (which we
	 * start alongside TX for the bidirectional session the Digi
	 * 002/003 requires) has valid framing parameters even when
	 * no capture app is running.  The handler won't write to the
	 * capture DMA buffer unless capture is independently active.
	 */
	if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
		struct dg00x_pcm_stream *cap = &dg00x->pcm_capture;
		cap->rate = ps->rate;
		cap->fdf = ps->fdf;
		cap->device_channels = ps->device_channels;
		cap->frames_per_packet = ps->frames_per_packet;
		cap->frame_remainder = ps->frame_remainder;
		cap->frame_cycle = 0;
	} else {
		/*
		 * Mirror image of the above: when capture starts on its
		 * own (no playback app running), clone the geometry into
		 * the playback stream so dg00x_fill_tx_chunk() can build
		 * correctly shaped (silent) TX packets.  The device will
		 * not transmit its capture audio unless the host is also
		 * transmitting to it, so TX must run with valid framing
		 * even though nothing has opened the playback substream.
		 */
		struct dg00x_pcm_stream *pb = &dg00x->pcm_playback;
		pb->rate = ps->rate;
		pb->fdf = ps->fdf;
		pb->device_channels = ps->device_channels;
		pb->frames_per_packet = ps->frames_per_packet;
		pb->frame_remainder = ps->frame_remainder;
		pb->frame_cycle = 0;
	}

	return (0);
}

static void
dg00x_pcm_stream_stop(struct snd_dg00x *dg00x, int direction)
{
	struct dg00x_pcm_stream *ps;

	ps = (direction == SNDRV_PCM_STREAM_PLAYBACK) ?
	     &dg00x->pcm_playback : &dg00x->pcm_capture;

	if (!ps->active)
		return;

	/*
	 * Clear active under dg00x->lock to serialise with the callout
	 * refill path.  The refill function reads active under the same
	 * lock and will bail out if it sees active==false — preventing
	 * it from calling itx_enable after we call itx_disable below.
	 */
	mtx_lock(&dg00x->lock);
	ps->active = false;
	ps->period_accum = 0;
	if (dg00x->active_streams > 0)
		dg00x->active_streams--;

	/* Stop the shared callout when both streams are inactive.
	 * callout_stop only cancels a pending callout; a currently
	 * running callout will see active==false under dg00x->lock
	 * and bail out before touching xferq queues. */
	if (dg00x->active_streams == 0)
		callout_stop(&dg00x->callout);
	mtx_unlock(&dg00x->lock);
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
		return (-EINVAL);

	/* Set sample rate on device (no-op if already at this rate) */
	dg00x_ensure_local_rate(dg00x, rate);

	return 0;
}

static int
pcm_hw_free(struct snd_pcm_substream *substream)
{
	/* Buffer is managed by basound_chan_init — nothing to free. */
	return 0;
}

static int
pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_dg00x *dg00x = (struct snd_dg00x *)substream->pcm->private_data;
	struct basound_chan *ch = (struct basound_chan *)substream->private_data;
	unsigned int rate;

	if (ch == NULL)
		return (-EINVAL);

	rate = ch->speed > 0 ? ch->speed : 48000;

	/* Reset position for the stream direction */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dg00x->pcm_playback.hwptr = 0;
		dg00x->pcm_playback.period_accum = 0;
		dg00x->pcm_playback.tx_dbc = 0;
		dg00x->pcm_playback.rate = rate;
		dg00x->pcm_playback.fdf =
		    dg00x_rate_to_fdf(dg00x->pcm_playback.rate);
		dg00x->pcm_playback.device_channels =
		    snd_dg00x_stream_pcm_channels[
			dg00x_rate_index(dg00x->pcm_playback.rate)];
		dg00x->pcm_playback.frames_per_packet =
		    dg00x->pcm_playback.rate / 8000;
		dg00x->pcm_playback.frame_remainder =
		    dg00x->pcm_playback.rate % 8000;
		dg00x->pcm_playback.frame_cycle = 0;
	} else {
		dg00x->pcm_capture.hwptr = 0;
		dg00x->pcm_capture.period_accum = 0;
		dg00x->pcm_capture.rate = rate;
		dg00x->pcm_capture.fdf =
		    dg00x_rate_to_fdf(dg00x->pcm_capture.rate);
		dg00x->pcm_capture.device_channels =
		    snd_dg00x_stream_pcm_channels[
			dg00x_rate_index(dg00x->pcm_capture.rate)];
		dg00x->pcm_capture.frames_per_packet =
		    dg00x->pcm_capture.rate / 8000;
		dg00x->pcm_capture.frame_remainder =
		    dg00x->pcm_capture.rate % 8000;
		dg00x->pcm_capture.frame_cycle = 0;
	}

	/*
	 * Program sample rate into Digi 00x hardware register — but only
	 * if it actually changed.  pcm_prepare() runs under CHN_LOCK (see
	 * basound_chan_trigger()/basound_chan_setspeed()), and the
	 * FireWire register write can tsleep for up to 5 seconds on a
	 * slow/unresponsive bus.  JACK calls prepare on every trigger
	 * START; unconditionally writing here reintroduces a CHN_LOCK-held
	 * stall on every such call, which starves realtime playback and
	 * capture and can miss periods (observed as JACK read timeouts),
	 * even though relaxed playback-only apps don't notice.
	 */
	dg00x_ensure_local_rate(dg00x, rate);

	return (0);
}

static int
pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_dg00x *dg00x;
	int err;
	bool was_idle;

	if (substream == NULL || substream->pcm == NULL)
		return (-EINVAL);

	dg00x = (struct snd_dg00x *)substream->pcm->private_data;
	if (dg00x == NULL)
		return (-EINVAL);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		{
			enum snd_dg00x_clock clk;
			unsigned int rate;
			dg00x_get_clock(dg00x, &clk);
			dg00x_get_local_rate(dg00x, &rate);
			printf("digi00x: pcm_trigger START — stream=%s "
			    "clock=%d rate=%u\n",
			    substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			    "PLAYBACK" : "CAPTURE", (int)clk, rate);
		}
		was_idle = (dg00x->active_streams == 0);

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

		/* Mark the PCM stream active first so the ISO DMA
		 * start functions (which check ps->active) proceed. */
		err = dg00x_pcm_stream_start(dg00x,
		    substream->stream, substream);
		if (err < 0)
			return (err);

		/*
		 * Begin the hardware session BEFORE starting ISO DMA.
		 * The Digi hardware must be in session mode before it
		 * will accept isochronous packets.  Starting DMA first
		 * confuses the device and can leave it in an undefined
		 * state that triggers a bus reset or panic.
		 */
		if (was_idle) {
			/*
			 * dg00x_begin_session()'s (tx_ch, rx_ch) arguments
			 * follow the Linux driver's DEVICE-centric naming:
			 * tx_ch is the channel the DEVICE transmits on (i.e.
			 * the channel our host must receive/capture on), and
			 * rx_ch is the channel the DEVICE receives on (i.e.
			 * the channel our host must transmit/playback on).
			 *
			 * Our own dg00x->tx_resources / iso_tx are HOST-
			 * centric (tx = host transmits = playback), which is
			 * the opposite convention.  Passing them in that
			 * order swaps the two channel numbers in the
			 * DG00X_OFFSET_ISOC_CHANNELS register, telling the
			 * device to listen for playback on the channel our
			 * host actually uses for capture (and vice versa) —
			 * so the device never receives the playback stream
			 * it is being sent, producing silence even though
			 * well-formed audio is on the wire.  Swap here to
			 * match the register's expected field order.
			 */
			err = dg00x_begin_session(dg00x,
			    dg00x->rx_resources.channel,
			    dg00x->tx_resources.channel);
			if (err < 0) {
				dg00x_pcm_stream_stop(dg00x,
				    substream->stream);
				return (err);
			}
		}

		/*
		 * Start the ISO DMA.  The Digi 002/003 requires a
		 * bidirectional isochronous session — its audio
		 * pipeline (DAC routing) only activates when the
		 * device can both receive packets from the host AND
		 * have its own transmitter active.  The Linux driver
		 * always starts both RX and TX streams together via
		 * amdtp_domain_start(), and the device has a
		 * documented quirk: "No packets are transmitted
		 * without receiving packets".
		 *
		 * Regardless of which direction triggered this start,
		 * always start both RX and TX together: TX runs in
		 * silent/no-op mode when playback isn't active, and RX's
		 * handler is a no-op when capture isn't active.  Both
		 * dg00x_streaming_start_tx()/start_rx() are reference
		 * counted (tx_use_count/rx_use_count), so calling them
		 * from either direction's trigger is safe and idempotent.
		 */
		err = dg00x_streaming_start_rx(dg00x);
		if (err == 0)
			err = dg00x_streaming_start_tx(dg00x);
		if (err < 0) {
			dg00x_streaming_stop_tx(dg00x);
			dg00x_streaming_stop_rx(dg00x);
			if (was_idle)
				dg00x_finish_session(dg00x);
			dg00x_pcm_stream_stop(dg00x,
			    substream->stream);
			return (err);
		}

		/*
		 * Start the callout NOW — after the ISO DMA is fully
		 * set up and the hardware session is active.  Starting
		 * it earlier risks the callout calling itx_enable
		 * before dg00x_streaming_start_tx, which then destroys
		 * the active DMA queues with STAILQ_INIT → corruption
		 * → panic → reboot.
		 */
		if (dg00x->active_streams == 1) {
			callout_reset(&dg00x->callout, DG00X_REFILL_TICKS,
			    dg00x_pcm_stream_cb, dg00x);
		}

		return (0);

	case SNDRV_PCM_TRIGGER_STOP:
		/*
		 * Mark stream inactive FIRST (under dg00x->lock).
		 * This tells the callout refill path to stop touching
		 * the xferq queues.  If we called itx_disable first,
		 * the callout could still be mid-refill, and after
		 * db_free clears FWXFERQ_RUNNING, the callout would
		 * call itx_enable — re-initialising the freed DB and
		 * starting DMA just as we drain the queues below.
		 * Kernel panic.
		 */
		dg00x_pcm_stream_stop(dg00x, substream->stream);

		/*
		 * Now stop ISO DMA safely — the callout has bailed out.
		 * Always stop both TX and RX; each is reference counted
		 * (tx_use_count/rx_use_count) so the DMA context for the
		 * direction still in use keeps running, and the one
		 * whose last user just stopped is properly torn down.
		 */
		dg00x_streaming_stop_tx(dg00x);
		dg00x_streaming_stop_rx(dg00x);

		/*
		 * Only finish the hardware session when ALL streams have
		 * stopped.  The device cannot handle a partial session stop
		 * while the other direction is still running.
		 */
		if (dg00x->active_streams == 0)
			dg00x_finish_session(dg00x);

		return (0);

	default:
		return (-EINVAL);
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

	dg00x->active_streams = 0;

	/* Initialize shared callout for period_elapsed + TX refill */
	callout_init(&dg00x->callout, 1);

	/* Store substream references for the ISO DMA handlers */
	dg00x->pcm_playback.substream =
	    pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	dg00x->pcm_capture.substream =
	    pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;

	return (0);
}
