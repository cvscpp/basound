/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-stream.c - Digidesign Digi 002/003 stream management
 *
 * Uses FreeBSD native firewire transaction helpers (fwmem_read/write_quad).
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/bus.h>

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>

#include "digi00x.h"

/* External declarations for fwmem transaction helpers (from fwmem.c) */
extern struct fw_xfer *fwmem_read_quad(struct fw_device *, caddr_t, uint8_t,
				       uint16_t, uint32_t, void *,
				       void (*)(struct fw_xfer *));
extern struct fw_xfer *fwmem_write_quad(struct fw_device *, caddr_t, uint8_t,
					uint16_t, uint32_t, void *,
					void (*)(struct fw_xfer *));

/* ------------------------------------------------------------------ */
/* Transaction helpers using fwmem_read/write_quad + sync wrapper     */
/* ------------------------------------------------------------------ */

struct dg00x_txn {
	struct fw_xfer *xfer;
	int done;
	int error;
};

static void
dg00x_txn_callback(struct fw_xfer *xfer)
{
	struct dg00x_txn *txn = (struct dg00x_txn *)xfer->sc;
	txn->done = 1;
	txn->error = (xfer->flag != FWXF_RCVD) ? EIO : 0;
	wakeup(txn);
}

/*
 * Synchronous quadlet read using fwmem_read_quad + timeout-based wait.
 *
 * Uses tsleep() with a 5-second timeout instead of fw_xferwait() (which
 * blocks forever and makes the process unkillable in D state when the
 * FireWire device doesn't respond).
 */
int
dg00x_read_quad(struct fw_device *fwdev, uint64_t addr, uint32_t *val)
{
	struct dg00x_txn txn;
	uint32_t offset_hi, offset_lo;
	int ret;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_read_quad(fwdev, (caddr_t)&txn, fwdev->speed,
				    (uint16_t)offset_hi, offset_lo, val,
				    dg00x_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	/* Wait for transaction completion with 5-second timeout.
	 * Do NOT use fw_xferwait() — it has no timeout and hangs forever
	 * in D state if the device is unresponsive. */
	while (!txn.done) {
		/*
		 * Use a non-interruptible sleep with a 5-second timeout.
		 * PCATCH is NOT used because signal interruption in the
		 * session control path (pcm_trigger → begin_session)
		 * can leave the device in an inconsistent state.
		 * The 5-second timeout prevents a permanent hang.
		 */
		ret = tsleep(&txn, 0, "dg00xrd", 5 * hz);
		if (ret == EWOULDBLOCK) {
			txn.error = ETIMEDOUT;
			break;
		}
		if (txn.done)
			break;
		txn.error = EIO;
		break;
	}

	/* If we timed out, the xfer may still be pending.
	 * Freeing it is risky (use-after-free if callback fires later),
	 * but the alternative is a permanently hung process.
	 * We accept the small leak on timeout. */
	if (txn.xfer != NULL)
		fw_xfer_free(txn.xfer);

	return (txn.error);
}

/*
 * Synchronous quadlet write using fwmem_write_quad + timeout-based wait.
 *
 * Uses tsleep() with a 5-second timeout instead of fw_xferwait() (which
 * blocks forever and makes the process unkillable in D state when the
 * FireWire device doesn't respond).
 */
int
dg00x_write_quad(struct fw_device *fwdev, uint64_t addr, uint32_t val)
{
	struct dg00x_txn txn;
	uint32_t offset_hi, offset_lo;
	int ret;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_write_quad(fwdev, (caddr_t)&txn, fwdev->speed,
				     (uint16_t)offset_hi, offset_lo, &val,
				     dg00x_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	/* Non-interruptible sleep with 5-second timeout.
	 * See dg00x_read_quad for rationale. */
	while (!txn.done) {
		ret = tsleep(&txn, 0, "dg00xwr", 5 * hz);
		if (ret == EWOULDBLOCK) {
			txn.error = ETIMEDOUT;
			break;
		}
		if (txn.done)
			break;
		txn.error = EIO;
		break;
	}

	/* If we timed out, the xfer may still be pending.
	 * Freeing it is risky (use-after-free if callback fires later),
	 * but the alternative is a permanently hung process.
	 * We accept the small leak on timeout. */
	if (txn.xfer != NULL)
		fw_xfer_free(txn.xfer);

	return (txn.error);
}

/* ------------------------------------------------------------------ */
/* Rate / Clock helpers                                                */
/* ------------------------------------------------------------------ */

const unsigned int snd_dg00x_stream_rates[SND_DG00X_RATE_COUNT] = {
	[SND_DG00X_RATE_44100] = 44100,
	[SND_DG00X_RATE_48000] = 48000,
	[SND_DG00X_RATE_88200] = 88200,
	[SND_DG00X_RATE_96000] = 96000,
};

const unsigned int
snd_dg00x_stream_pcm_channels[SND_DG00X_RATE_COUNT] = {
	[SND_DG00X_RATE_44100] = (8 + 8 + 2),
	[SND_DG00X_RATE_48000] = (8 + 8 + 2),
	[SND_DG00X_RATE_88200] = (8 + 2),
	[SND_DG00X_RATE_96000] = (8 + 2),
};

int
dg00x_get_local_rate(struct snd_dg00x *dg00x, unsigned int *rate)
{
	uint32_t reg;
	int err;

	err = dg00x_read_quad(dg00x->fwdev,
			      DG00X_ADDR_BASE + DG00X_OFFSET_LOCAL_RATE, &reg);
	if (err != 0)
		return (err);

	reg = be32toh(reg) & 0x0f;
	if (reg < SND_DG00X_RATE_COUNT)
		*rate = snd_dg00x_stream_rates[reg];
	else
		err = EIO;

	return (err);
}

int
dg00x_set_local_rate(struct snd_dg00x *dg00x, unsigned int rate)
{
	unsigned int i;
	int err;

	for (i = 0; i < SND_DG00X_RATE_COUNT; i++) {
		if (rate == snd_dg00x_stream_rates[i])
			break;
	}
	if (i == SND_DG00X_RATE_COUNT)
		return (EINVAL);

	err = dg00x_write_quad(dg00x->fwdev,
				DG00X_ADDR_BASE + DG00X_OFFSET_LOCAL_RATE,
				htobe32(i));
	if (err == 0) {
		dg00x->hw_rate_cached = rate;
		dg00x->hw_rate_valid = true;
	}
	return (err);
}

/*
 * dg00x_ensure_local_rate - like dg00x_set_local_rate(), but skips the
 * FireWire register write (a blocking transaction that can tsleep for
 * up to 5 seconds on an unresponsive bus) when the requested rate
 * matches the last rate this driver programmed.
 *
 * pcm_prepare() and dg00x_pcm_stream_start() are called with CHN_LOCK
 * held (see basound_chan_trigger()/basound_chan_setspeed() in
 * alsa_pcm_bsd.c, which only release CHN_LOCK around ops->trigger(),
 * not ops->prepare()).  JACK re-triggers prepare/start frequently
 * under its realtime audio thread; unconditionally writing the rate
 * register on every call reintroduces exactly the kind of
 * lock-held-across-firewire-sleep stall that ops->trigger() was
 * changed to avoid, causing xruns/timeouts under tight periods even
 * though relaxed playback-only apps (e.g. Audacious) don't notice.
 */
int
dg00x_ensure_local_rate(struct snd_dg00x *dg00x, unsigned int rate)
{
	if (dg00x->hw_rate_valid && dg00x->hw_rate_cached == rate)
		return (0);

	return dg00x_set_local_rate(dg00x, rate);
}

int
dg00x_get_clock(struct snd_dg00x *dg00x, enum snd_dg00x_clock *clock)
{
	uint32_t reg;
	int err;

	err = dg00x_read_quad(dg00x->fwdev,
			      DG00X_ADDR_BASE + DG00X_OFFSET_CLOCK_SOURCE, &reg);
	if (err != 0)
		return (err);

	*clock = (enum snd_dg00x_clock)(be32toh(reg) & 0x0f);
	if (*clock >= SND_DG00X_CLOCK_COUNT)
		err = EIO;

	return (err);
}

int
dg00x_check_external(struct snd_dg00x *dg00x, bool *detect)
{
	uint32_t reg;
	int err;

	err = dg00x_read_quad(dg00x->fwdev,
			      DG00X_ADDR_BASE + DG00X_OFFSET_DETECT_EXTERNAL,
			      &reg);
	if (err == 0)
		*detect = (be32toh(reg) > 0);

	return (err);
}

int
dg00x_get_external_rate(struct snd_dg00x *dg00x, unsigned int *rate)
{
	uint32_t reg;
	int err;

	err = dg00x_read_quad(dg00x->fwdev,
			      DG00X_ADDR_BASE + DG00X_OFFSET_EXTERNAL_RATE,
			      &reg);
	if (err != 0)
		return (err);

	reg = be32toh(reg) & 0x0f;
	if (reg < SND_DG00X_RATE_COUNT)
		*rate = snd_dg00x_stream_rates[reg];
	else
		err = EBUSY; /* desync */

	return (err);
}

/* ------------------------------------------------------------------ */
/* Session control                                                     */
/* ------------------------------------------------------------------ */

void
dg00x_finish_session(struct snd_dg00x *dg00x)
{
	/* Guard against close-without-open: fwdev must be valid */
	if (dg00x->fwdev == NULL)
		return;

	printf("digi00x: finish_session — stopping streaming\n");

	/* Write 0x00000003 to streaming set (stop) */
	dg00x_write_quad(dg00x->fwdev,
			 DG00X_ADDR_BASE + DG00X_OFFSET_STREAMING_SET,
			 htobe32(0x00000003));

	/* Unregister isochronous channels */
	dg00x_write_quad(dg00x->fwdev,
			 DG00X_ADDR_BASE + DG00X_OFFSET_ISOC_CHANNELS, 0);

	/* Allow hardware to quiesce after session end.
	 * pause() sleeps rather than busy-spinning. */
	pause("dg00xfe", hz / 20);	/* ~50 ms */

	printf("digi00x: finish_session — done\n");
}

int
dg00x_begin_session(struct snd_dg00x *dg00x, int tx_ch, int rx_ch)
{
	uint32_t reg;
	uint32_t curr;
	int err;

	/* Register isochronous channels for both directions */
	reg = htobe32(((uint32_t)tx_ch << 16) | (uint32_t)rx_ch);
	err = dg00x_write_quad(dg00x->fwdev,
			       DG00X_ADDR_BASE + DG00X_OFFSET_ISOC_CHANNELS,
			       reg);
	if (err != 0) {
		printf("digi00x: begin_session ISOC_CHANS failed: %d\n", err);
		return (err);
	}

	/* Read current streaming state */
	err = dg00x_read_quad(dg00x->fwdev,
			      DG00X_ADDR_BASE + DG00X_OFFSET_STREAMING_STATE,
			      &reg);
	if (err != 0) {
		printf("digi00x: begin_session STATE read failed: %d\n", err);
		return (err);
	}

	curr = be32toh(reg);
	if (curr == 0)
		curr = 2;

	curr--;
	while (curr > 0) {
		err = dg00x_write_quad(dg00x->fwdev,
				       DG00X_ADDR_BASE + DG00X_OFFSET_STREAMING_SET,
				       htobe32(curr));
		if (err != 0)
			break;
		pause("dg00xbs", hz / 50);	/* ~20 ms */
		curr--;
	}

	if (err == 0)
		printf("digi00x: begin_session OK (tx=%d rx=%d state=0x%08x)\n",
		    tx_ch, rx_ch, be32toh(reg));
	else
		printf("digi00x: begin_session failed: %d\n", err);

	return (err);
}

/* ------------------------------------------------------------------ */
/* ISOC resource allocation (placeholder - uses FreeBSD fw_iso API)    */
/* ------------------------------------------------------------------ */

int
dg00x_alloc_isoc_resources(struct snd_dg00x *dg00x)
{
	/*
	 * Hardcode isochronous channel numbers.
	 * Channel 0 is often reserved; use 2 and 3 to avoid
	 * potential bus conflicts.  The Digi 002/003 stores
	 * these via ISOC_CHANNELS register during begin_session.
	 */
	dg00x->tx_resources.channel = 2;
	dg00x->rx_resources.channel = 3;
	dg00x->tx_resources.generation = 0;
	dg00x->rx_resources.generation = 0;
	return (0);
}

void
dg00x_free_isoc_resources(struct snd_dg00x *dg00x)
{
	/*
	 * TODO: Free isochronous resources.
	 */
	dg00x->tx_resources.channel = -1;
	dg00x->rx_resources.channel = -1;
}
