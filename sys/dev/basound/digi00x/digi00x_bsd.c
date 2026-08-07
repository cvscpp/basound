/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x_bsd.c - FreeBSD bridge for Digidesign Digi 002/003 family
 *
 * Bridges FreeBSD firewire devices to the snd_dg00x ALSA driver.
 * The FreeBSD firewire bus requires each child driver to implement
 * a device_identify method to create its child device.
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/sysctl.h>

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>
#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>

#include "../audio_stream.h"
#include "digi00x.h"

MALLOC_DECLARE(M_ALSA);

#define VENDOR_DIGIDESIGN	0x00a07e
#define MODEL_CONSOLE		0x000001
#define MODEL_RACK		0x000002

/* No CSR ROM parsing macros needed - we match by EUI64 OUI */

struct digi00x_softc {
	device_t dev;
	struct device alsa_dev;
	struct firewire_comm *fc;
	struct fw_device *fwdev;
	struct snd_dg00x *dg00x;
	int unit;
	bool attached;
	int discover_retries;
	struct callout discover_callout;
};

/* Forward declarations */
static void dg00x_identify(driver_t *, device_t);
static int  dg00x_probe(device_t);
static int  dg00x_attach(device_t);
static int  dg00x_detach(device_t);
static void dg00x_discover(void *);
static int  dg00x_init_card(struct digi00x_softc *);

/* ------------------------------------------------------------------ */
/* CSR ROM helpers                                                     */
/* ------------------------------------------------------------------ */

/* Check if a fw_device belongs to Digidesign by matching EUI64 OUI.
 * The first 3 bytes of the 8-byte EUI64 contain the vendor OUI.
 * fw_device.eui = { .hi = bytes[0..3], .lo = bytes[4..7] } */
#define EUI64_OUI(eui)	(((eui)->hi >> 8) & 0xffffff)

static int
match_digidesign(struct fw_device *fwdev)
{
	if (fwdev == NULL)
		return (-1);

	/* Check OUI (first 3 bytes of EUI-64) */
	if (EUI64_OUI(&fwdev->eui) != VENDOR_DIGIDESIGN)
		return (-1);

	return (0);
}

/* Get model ID from a fw_device's CSR ROM Unit directory (Model key = 0x17).
 * The root directory starts at csrrom[5] (byte offset 0x14 from ROM base).
 * csrrom[5] = directory header, csrrom[6]... = entries.
 * A Unit directory entry has key=0x11, type=D (directory, type=3).
 * Its value is the byte offset into ROM where the unit directory lives.
 */
static int
get_model_id(struct fw_device *fwdev)
{
	uint32_t *rom = fwdev->csrrom;
	int i;

	/* Root directory entries start at csrrom[6] (after header at [5]) */
	for (i = 6; i < 256; i++) {
		uint32_t e = rom[i];
		int type = (e >> 30) & 0x3;
		int key = (e >> 24) & 0x3f;
		int val = e & 0xffffff;

		if (type == 3 && key == 0x11) {
			/* Unit directory at byte offset = val.
			 * Its entries start at csrrom[val/4 + 1] (after its own header). */
			int ui = (val / 4) + 1;
			int ud = 64;
			while (ui < 256 && ud-- > 0) {
				uint32_t ue = rom[ui];
				int utype = (ue >> 30) & 0x3;
				int ukey = (ue >> 24) & 0x3f;
				int uval = ue & 0xffffff;

				if (utype == 0 && ukey == 0x17)
					return (uval);
				ui++;
			}
			break;
		}
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* FreeBSD device interface                                            */
/* ------------------------------------------------------------------ */

/*
 * Identify method: create a child device on the firewire bus.
 * Without this, the system never probes our driver.
 */
static void
dg00x_identify(driver_t *driver, device_t parent)
{
	/* Only create one child */
	if (device_find_child(parent, "basound_digi00x", DEVICE_UNIT_ANY) == NULL)
		BUS_ADD_CHILD(parent, 0, "basound_digi00x", DEVICE_UNIT_ANY);
}

/*
 * Probe method: claim our child device.
 * The ivar from firewire bus is struct firewire_comm *.
 * Real device matching is deferred to attach where we scan fc->devices.
 */
static int
dg00x_probe(device_t dev)
{
	device_set_desc(dev, "Digidesign Digi 002/003 FireWire Audio");
	return (BUS_PROBE_DEFAULT);
}

static int
dg00x_attach(device_t dev)
{
	struct digi00x_softc *sc = device_get_softc(dev);
	struct firewire_comm *fc;
	struct fw_device *fwdev;

	/* The ivar on firewire bus children is struct firewire_comm * */
	fc = (struct firewire_comm *)device_get_ivars(dev);
	if (fc == NULL)
		return (ENXIO);

	sc->dev = dev;
	sc->fc = fc;
	sc->fwdev = NULL;
	sc->dg00x = NULL;
	sc->attached = false;
	sc->alsa_dev.bsddev = dev;

	/* Initialize callout for deferred discovery */
	callout_init(&sc->discover_callout, 1);

	/* Try to find device immediately, defer if bus not yet explored */
	fwdev = NULL;
	STAILQ_FOREACH(fwdev, &fc->devices, link) {
		if (fwdev->status != FWDEVATTACHED)
			continue;
		if (match_digidesign(fwdev) == 0)
			break;
	}

	if (fwdev != NULL) {
		/* Found immediately - set up card */
		sc->fwdev = fwdev;
		sc->unit = get_model_id(fwdev);
		return (dg00x_init_card(sc));
	}

	sc->discover_retries = 0;
	device_printf(dev, "Digidesign device not found yet, "
		      "scheduling deferred discovery\n");
	callout_reset(&sc->discover_callout, hz, dg00x_discover, sc);
	return (0);
}

/*
 * Deferred discovery callout.
 * FireWire bus exploration may complete after our attach,
 * so we retry finding the device after a delay.
 */
static void
dg00x_discover(void *arg)
{
	struct digi00x_softc *sc = (struct digi00x_softc *)arg;
	struct fw_device *fwdev;
	int err;

	if (sc->attached || sc->fwdev != NULL)
		return;

	fwdev = NULL;
	STAILQ_FOREACH(fwdev, &sc->fc->devices, link) {
		/*
		 * Accept both FWDEVATTACHED (normal) and FWDEVINIT
		 * (bus reset in progress).  crom_load(), which populates
		 * csrrom, runs before the device transitions to
		 * FWDEVATTACHED — so FWDEVINIT devices may already have
		 * valid config ROM data to read.
		 */
		if (fwdev->status != FWDEVATTACHED &&
		    fwdev->status != FWDEVINIT)
			continue;
		if (match_digidesign(fwdev) == 0)
			break;
	}

	if (fwdev == NULL) {
		sc->discover_retries++;
		if (sc->discover_retries == 5)
			device_printf(sc->dev,
			    "still waiting for Digidesign device "
			    "(retry %d)\n", sc->discover_retries);
		/* Retry indefinitely — the device will appear after a
		 * bus reset completes. */
		callout_reset(&sc->discover_callout, hz, dg00x_discover, sc);
		return;
	}

	device_printf(sc->dev, "Digidesign device found on firewire bus\n");
	sc->fwdev = fwdev;
	sc->unit = get_model_id(fwdev);
	err = dg00x_init_card(sc);
	if (err != 0)
		device_printf(sc->dev, "Failed to init card: %d\n", err);
}

/* ------------------------------------------------------------------ */
/* Sysctl handlers                                                    */
/* ------------------------------------------------------------------ */

/*
 * sysctl_dg00x_clock_source - RW: 0=internal, 1=SPDIF, 2=ADAT, 3=Word clock
 */
static int
sysctl_dg00x_clock_source(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	enum snd_dg00x_clock clock;
	uint32_t val;
	int err;

	err = dg00x_get_clock(dg00x, &clock);
	if (err != 0)
		return (err);
	val = (uint32_t)clock;

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	if (val >= SND_DG00X_CLOCK_COUNT)
		return (EINVAL);

	clock = (enum snd_dg00x_clock)val;
	return dg00x_write_quad(dg00x->fwdev,
		 DG00X_ADDR_BASE + DG00X_OFFSET_CLOCK_SOURCE, (uint32_t)clock);
}

/*
 * sysctl_dg00x_optical_mode - RW: 0=ADAT, 1=SPDIF
 */
static int
sysctl_dg00x_optical_mode(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	uint32_t val;
	int err;

	err = dg00x_read_quad(dg00x->fwdev,
		 DG00X_ADDR_BASE + DG00X_OFFSET_OPT_IFACE_MODE, &val);
	if (err != 0)
		return (err);

	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	if (val >= SND_DG00X_OPT_IFACE_MODE_COUNT)
		return (EINVAL);

	return dg00x_write_quad(dg00x->fwdev,
		 DG00X_ADDR_BASE + DG00X_OFFSET_OPT_IFACE_MODE, val);
}

/*
 * sysctl_dg00x_rate - RW: current sample rate
 */
static int
sysctl_dg00x_rate(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	unsigned int rate;
	int err;

	err = dg00x_get_local_rate(dg00x, &rate);
	if (err != 0)
		return (err);

	err = sysctl_handle_int(oidp, &rate, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	err = dg00x_set_local_rate(dg00x, rate);
	if (err != 0)
		return (err);

	return (0);
}

/*
 * sysctl_dg00x_external_rate - RO: detected external clock rate
 */
static int
sysctl_dg00x_external_rate(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	unsigned int rate;
	int err;

	err = dg00x_get_external_rate(dg00x, &rate);
	if (err != 0)
		return (err);

	return (sysctl_handle_int(oidp, &rate, 0, req));
}

/*
 * sysctl_dg00x_external_detect - RO: external clock presence (0/1)
 */
static int
sysctl_dg00x_external_detect(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	bool detect;
	uint32_t val;
	int err;

	err = dg00x_check_external(dg00x, &detect);
	if (err != 0)
		return (err);
	val = detect ? 1 : 0;

	return (sysctl_handle_int(oidp, &val, 0, req));
}

/*
 * sysctl_dg00x_tx_peaks - RD: per-channel playback (TX) peak levels.
 *
 * Returns a string "N p0 p1 ... pN-1" where N is the number of device
 * channels and pX is the peak (24-bit scale, 0..0x7fffff) of the data
 * currently being sent to the device on that channel, computed in the
 * TX fill path.  Padded channels that carry silence read 0.
 *
 * Peak-and-clear: each read returns the maximum level since the
 * previous read and resets the meter, so a slow UI can track true
 * peaks.  All zeros when no playback stream is active.
 */
static int
sysctl_dg00x_tx_peaks(SYSCTL_HANDLER_ARGS)
{
	struct snd_dg00x *dg00x = arg1;
	char buf[160];
	unsigned int n, i;
	int len;

	mtx_lock(&dg00x->lock);
	n = dg00x->pcm_playback.device_channels;
	if (n == 0 || n > DG00X_MAX_PCM_CHANNELS)
		n = 2;
	len = snprintf(buf, sizeof(buf), "%u", n);
	for (i = 0; i < n && len < (int)sizeof(buf) - 10; i++) {
		uint32_t pk = atomic_load_acq_32(&dg00x->pcm_playback.tx_peak[i]);
		atomic_store_rel_32(&dg00x->pcm_playback.tx_peak[i], 0);
		len += snprintf(buf + len, sizeof(buf) - len, " %u", pk);
	}
	mtx_unlock(&dg00x->lock);

	return (sysctl_handle_string(oidp, buf, len + 1, req));
}

/*
 * Initialize the ALSA card after discovering a Digidesign device.
 */
static int
dg00x_init_card(struct digi00x_softc *sc)
{
	struct snd_card *card;
	struct snd_dg00x *dg00x;
	struct fw_device *fwdev = sc->fwdev;
	int err;

	err = snd_card_new(&sc->alsa_dev, device_get_unit(sc->dev), "Digi00x",
			   NULL, sizeof(struct snd_dg00x), &card);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create ALSA card: %d\n", err);
		return (err);
	}

	dg00x = card->private_data;
	dg00x->card = card;
	dg00x->fwdev = fwdev;
	dg00x->dev = sc->dev;
	sc->dg00x = dg00x;

	mtx_init(&dg00x->lock, "dg00x_lock", NULL, MTX_DEF);
	mtx_init(&dg00x->mutex, "dg00x_mtx", NULL, MTX_DEF);

	dg00x->is_console = (sc->unit == MODEL_CONSOLE);
	dg00x->tx_resources.channel = -1;
	dg00x->rx_resources.channel = -1;
	dg00x->iso_tx.dmach = -1;
	dg00x->iso_rx.dmach = -1;

	strcpy(card->driver, "Digi00x");
	switch (sc->unit) {
	case MODEL_CONSOLE:
		strcpy(card->shortname, "Digi 003 Console");
		break;
	case MODEL_RACK:
		strcpy(card->shortname, "Digi 002/003 Rack");
		break;
	default:
		strcpy(card->shortname, "Digidesign Digi 002/003");
		break;
	}
	snprintf(card->longname, sizeof(card->longname),
		 "Digidesign %s, GUID %08x%08x at S%d",
		 card->shortname, fwdev->csrrom[3], fwdev->csrrom[4],
		 100 << fwdev->speed);

	dg00x_proc_init(dg00x);

	/* Register per-instance sysctl controls */
	{
		struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
		struct sysctl_oid *tree = device_get_sysctl_tree(sc->dev);

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "clock_source",
		    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_clock_source, "I",
		    "Clock source: 0=internal, 1=SPDIF, 2=ADAT, 3=Word");

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "optical_mode",
		    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_optical_mode, "I",
		    "Optical port mode: 0=ADAT, 1=SPDIF");

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "rate",
		    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_rate, "IU",
		    "Sample rate: 44100, 48000, 88200, or 96000");

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "external_rate",
		    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_external_rate, "IU",
		    "Detected external clock rate");

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "external_detect",
		    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_external_detect, "I",
		    "External clock present (0=no, 1=yes)");

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "tx_peaks",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    dg00x, 0, sysctl_dg00x_tx_peaks, "A",
		    "Playback output peaks per channel "
		    "(N p0 p1 ... pN-1, 24-bit scale, peak-and-clear)");
	}

	err = dg00x_create_pcm(dg00x);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create PCM: %d\n", err);
		goto error;
	}

	err = dg00x_create_midi(dg00x);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create MIDI: %d\n", err);
		goto error;
	}

	err = dg00x_create_hwdep(dg00x);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create HWDEP: %d\n", err);
		goto error;
	}

	err = snd_card_register(card);
	if (err != 0) {
		device_printf(sc->dev, "Failed to register ALSA card: %d\n", err);
		goto error;
	}

	sc->attached = true;
	device_printf(sc->dev, "Digi 00x attached (%s, model=0x%06x)\n",
		      dg00x->is_console ? "console" : "rack", sc->unit);
	return (0);

error:
	snd_card_free(card);
	sc->dg00x = NULL;
	return (err);
}

static int
dg00x_detach(device_t dev)
{
	struct digi00x_softc *sc = device_get_softc(dev);

	if (sc == NULL)
		return (0);

	callout_drain(&sc->discover_callout);

	if (sc->dg00x != NULL) {
		callout_drain(&sc->dg00x->callout);
		snd_card_free(sc->dg00x->card);
		sc->dg00x = NULL;
	}

	sc->attached = false;
	return (0);
}

static device_method_t digi00x_methods[] = {
	DEVMETHOD(device_identify,	dg00x_identify),
	DEVMETHOD(device_probe,		dg00x_probe),
	DEVMETHOD(device_attach,	dg00x_attach),
	DEVMETHOD(device_detach,	dg00x_detach),
	DEVMETHOD_END
};

static driver_t digi00x_driver = {
	"basound_digi00x",
	digi00x_methods,
	sizeof(struct digi00x_softc),
};

DRIVER_MODULE(basound_digi00x, firewire, digi00x_driver, 0, 0);
MODULE_DEPEND(basound_digi00x, basound, 1, 1, 1);
MODULE_DEPEND(basound_digi00x, firewire, 1, 1, 1);
MODULE_DEPEND(basound_digi00x, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
