// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dice_bsd.c - FreeBSD glue for the ALSA DICE FireWire driver family
 *
 * Bridges FreeBSD firewire devices to the DICE (Digital Interface
 * Communications Engine) audio interface family.  Generic DICE handling
 * lives here; the device-specific parts (channel maps, MIDI port counts,
 * quirks) live in per-family files that register themselves through
 * dice_model_entry tables:
 *
 *   dice_alesis_bsd.c - Alesis iO14/iO26 and MultiMix 12/16 FireWire
 *   dice_maudio_bsd.c - M-Audio ProFire 2626
 *
 * The Digidesign Digi 002/003 driver (digi00x) is a separate driver in
 * its own directory and is not touched here.  DICE matching additionally
 * validates the DICE GUID category so digi00x devices are never claimed
 * by this driver.
 *
 * The FreeBSD firewire bus requires each child driver to implement a
 * device_identify method to create its child device (same pattern as
 * digi00x_bsd.c).
 *
 * Copyright (c) Clemens Ladisch
 * Copyright (c) 2014 Takashi Sakamoto
 */

#include <sys/param.h>
#include <machine/atomic.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>

#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>

#include "dice_bsd.h"
#include "dice_streaming.h"
#include "alsa_pcm_bsd.h"

MALLOC_DECLARE(M_ALSA);
MALLOC_DEFINE(M_DICE_BSD, "dice_bsd", "DICE BSD softc");

/* External declarations for fwmem transaction helpers (from fwmem.c). */
extern struct fw_xfer *fwmem_read_quad(struct fw_device *, caddr_t, uint8_t,
				       uint16_t, uint32_t, void *,
				       void (*)(struct fw_xfer *));
extern struct fw_xfer *fwmem_write_quad(struct fw_device *, caddr_t, uint8_t,
					uint16_t, uint32_t, void *,
					void (*)(struct fw_xfer *));
extern struct fw_xfer *fwmem_read_block(struct fw_device *, caddr_t, uint8_t,
					uint16_t, uint32_t, int, void *,
					void (*)(struct fw_xfer *));
extern struct fw_xfer *fwmem_write_block(struct fw_device *, caddr_t, uint8_t,
					 uint16_t, uint32_t, int, void *,
					 void (*)(struct fw_xfer *));

/* Forward declarations */
static void dice_identify(driver_t *, device_t);
static int  dice_probe(device_t);
static int  dice_attach(device_t);
static int  dice_detach(device_t);
static void dice_discover(void *);
static void dice_post_busreset(void *);
static void dice_post_explore(void *);
static int  dice_init_card(struct dice_bsd_softc *);

/* ------------------------------------------------------------------ */
/* FireWire transaction helpers                                        */
/*                                                                     */
/* Synchronous quadlet/block reads and quadlet writes with a 5-second  */
/* timeout, using the same pattern as digi00x-stream.c (fwmem helpers  */
/* + tsleep instead of fw_xferwait, which hangs forever when the       */
/* device does not respond).                                           */
/* ------------------------------------------------------------------ */

struct dice_txn {
	struct fw_xfer *xfer;
	int done;
	int error;
};

static void
dice_txn_callback(struct fw_xfer *xfer)
{
	struct dice_txn *txn = (struct dice_txn *)xfer->sc;

	txn->done = 1;
	txn->error = (xfer->flag != FWXF_RCVD) ? EIO : 0;
	wakeup(txn);
}

static int
dice_wait_txn(struct dice_txn *txn)
{
	int ret;

	while (!txn->done) {
		ret = tsleep(txn, 0, "dicetxn", 5 * hz);
		if (ret == EWOULDBLOCK) {
			txn->error = ETIMEDOUT;
			break;
		}
		if (txn->done)
			break;
		txn->error = EIO;
		break;
	}

	/* On timeout the xfer may still be pending; freeing it risks a
	 * use-after-free if the callback fires later, so we accept the
	 * small leak instead of a permanently hung process. */
	if (txn->xfer != NULL)
		fw_xfer_free(txn->xfer);

	return (txn->error);
}

int
dice_read_quad(struct fw_device *fwdev, uint64_t addr, uint32_t *val)
{
	struct dice_txn txn;
	uint32_t offset_hi, offset_lo;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_read_quad(fwdev, (caddr_t)&txn, fwdev->speed,
				    (uint16_t)offset_hi, offset_lo, val,
				    dice_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	return (dice_wait_txn(&txn));
}

/*
 * Quadlet write.  The value must already be in big-endian (bus) order;
 * callers convert with htobe32(), matching the digi00x-stream helpers.
 * Reads (dice_read_quad/block) return raw big-endian data, hence the
 * be32toh() calls throughout the detect callbacks.
 */
int
dice_write_quad(struct fw_device *fwdev, uint64_t addr, uint32_t val)
{
	struct dice_txn txn;
	uint32_t offset_hi, offset_lo;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_write_quad(fwdev, (caddr_t)&txn, fwdev->speed,
				     (uint16_t)offset_hi, offset_lo, &val,
				     dice_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	return (dice_wait_txn(&txn));
}

int
dice_read_block(struct fw_device *fwdev, uint64_t addr, void *buf, size_t len)
{
	struct dice_txn txn;
	uint32_t offset_hi, offset_lo;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_read_block(fwdev, (caddr_t)&txn, fwdev->speed,
				     (uint16_t)offset_hi, offset_lo,
				     (int)len, buf, dice_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	return (dice_wait_txn(&txn));
}

/*
 * Block write.  Like dice_write_quad(), the buffer must already contain
 * big-endian quadlets in bus order.
 */
int
dice_write_block(struct fw_device *fwdev, uint64_t addr, void *buf, size_t len)
{
	struct dice_txn txn;
	uint32_t offset_hi, offset_lo;

	if (fwdev == NULL || fwdev->fc == NULL)
		return (EIO);

	offset_hi = (uint32_t)(addr >> 32);
	offset_lo = (uint32_t)(addr & 0xffffffff);

	txn.done = 0;
	txn.error = 0;
	txn.xfer = fwmem_write_block(fwdev, (caddr_t)&txn, fwdev->speed,
				     (uint16_t)offset_hi, offset_lo,
				     (int)len, buf, dice_txn_callback);
	if (txn.xfer == NULL)
		return (ENOMEM);

	return (dice_wait_txn(&txn));
}

/* ------------------------------------------------------------------ */
/* Section-relative register access                                    */
/* ------------------------------------------------------------------ */

static uint64_t
dice_subaddr(struct dice_bsd_softc *sc, unsigned int section,
	     unsigned int offset)
{
	uint64_t base;

	switch (section) {
	case 1:	/* TX */
		base = sc->tx_offset;
		break;
	case 2:	/* RX */
		base = sc->rx_offset;
		break;
	case 3:	/* EXT_SYNC */
		base = sc->sync_offset;
		break;
	default: /* GLOBAL */
		base = sc->global_offset;
		break;
	}
	return (DICE_PRIVATE_SPACE + base + offset);
}

int
dice_read_global(struct dice_bsd_softc *sc, unsigned int offset,
		 void *buf, unsigned int len)
{
	if (len == 4)
		return (dice_read_quad(sc->fwdev,
				       dice_subaddr(sc, 0, offset), buf));
	return (dice_read_block(sc->fwdev, dice_subaddr(sc, 0, offset),
				buf, len));
}

int
dice_read_tx(struct dice_bsd_softc *sc, unsigned int offset,
	     void *buf, unsigned int len)
{
	if (len == 4)
		return (dice_read_quad(sc->fwdev,
				       dice_subaddr(sc, 1, offset), buf));
	return (dice_read_block(sc->fwdev, dice_subaddr(sc, 1, offset),
				buf, len));
}

int
dice_read_rx(struct dice_bsd_softc *sc, unsigned int offset,
	     void *buf, unsigned int len)
{
	if (len == 4)
		return (dice_read_quad(sc->fwdev,
				       dice_subaddr(sc, 2, offset), buf));
	return (dice_read_block(sc->fwdev, dice_subaddr(sc, 2, offset),
				buf, len));
}

/*
 * Read the DICE section pointer table and validate the layout, following
 * ALSA's get_subaddrs().  Section offsets are stored in quadlets.
 */
static int
dice_get_subaddrs(struct dice_bsd_softc *sc)
{
	static const unsigned int min_values[8] = {
		10, 0x60 / 4,	/* global offset/size */
		10, 0x18 / 4,	/* tx offset/size */
		10, 0x18 / 4,	/* rx offset/size */
		0, 0,		/* ext sync offset/size */
	};
	uint32_t pointers[8];
	unsigned int i;
	int err;

	err = dice_read_block(sc->fwdev, DICE_PRIVATE_SPACE,
			      pointers, sizeof(pointers));
	if (err != 0)
		return (err);

	for (i = 0; i < 8; i++) {
		uint32_t data = be32toh(pointers[i]);

		if (data < min_values[i] || data >= 0x40000)
			return (ENODEV);
	}

	sc->global_offset = (uint64_t)be32toh(pointers[0]) * 4;
	sc->tx_offset = (uint64_t)be32toh(pointers[2]) * 4;
	sc->rx_offset = (uint64_t)be32toh(pointers[4]) * 4;
	sc->sync_offset = (uint64_t)be32toh(pointers[6]) * 4;

	return (0);
}

/* ------------------------------------------------------------------ */
/* CSR ROM helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Extract specifier_id / version / model from the unit directory.
 * csrrom[3..4] hold the EUI-64, csrrom[5] is the root directory header.
 * A unit-directory pointer has type D (3) and key 0x11; its value is the
 * byte offset of the unit directory.
 */
int
dice_read_unit_directory(struct fw_device *fwdev, uint32_t *specifier_id,
			 uint32_t *model, uint32_t *version)
{
	uint32_t *rom = fwdev->csrrom;
	uint32_t spec = 0, mod = 0, ver = 0;
	int i, ui, ud;

	for (i = 6; i < 256; i++) {
		uint32_t e = rom[i];
		int type = (e >> 30) & 0x3;
		int key = (e >> 24) & 0x3f;

		if (type == 3 && key == 0x11) {
			ui = ((e & 0xffffff) / 4) + 1;
			ud = 64;
			while (ui < 256 && ud-- > 0) {
				uint32_t ue = rom[ui];
				int utype = (ue >> 30) & 0x3;
				int ukey = (ue >> 24) & 0x3f;

				if (utype == 0) {
					switch (ukey) {
					case 0x12:	/* specifier id */
						spec = ue & 0xffffff;
						break;
					case 0x13:	/* version */
						ver = ue & 0xffffff;
						break;
					case 0x17:	/* model */
						mod = ue & 0xffffff;
						break;
					}
				}
				ui++;
			}
			break;
		}
	}

	if (specifier_id != NULL)
		*specifier_id = spec;
	if (model != NULL)
		*model = mod;
	if (version != NULL)
		*version = ver;

	return (0);
}

/*
 * Check that GUID and unit directory are constructed according to DICE
 * rules (ALSA check_dice_category): the GUID's OUI/category bytes and
 * its 10-bit product ID must match the unit directory's specifier and
 * model.
 */
int
dice_check_category(struct fw_device *fwdev, uint32_t vendor, uint32_t model)
{
	unsigned int category = DICE_CATEGORY_ID;

	if (vendor == OUI_WEISS)
		category = WEISS_CATEGORY_ID;
	else if (vendor == OUI_LOUD)
		category = LOUD_CATEGORY_ID;
	else if (vendor == OUI_HARMAN)
		category = HARMAN_CATEGORY_ID;

	if (fwdev->csrrom[3] != ((vendor << 8) | category))
		return (-1);
	if (fwdev->csrrom[4] >> 22 != model)
		return (-1);

	return (0);
}

/* ------------------------------------------------------------------ */
/* Device matching                                                     */
/* ------------------------------------------------------------------ */

static const struct dice_model_entry dice_generic_entry = {
	.vendor_id	= 0,	/* any known DICE vendor */
	.model_id	= DICE_MODEL_ANY,
	.detect		= dice_detect_current_formats,
	.desc		= "DICE FireWire",
};

static int
dice_known_vendor(uint32_t vendor)
{
	switch (vendor) {
	case OUI_WEISS:
	case OUI_LOUD:
	case OUI_FOCUSRITE:
	case OUI_TCELECTRONIC:
	case OUI_ALESIS:
	case OUI_MAUDIO:
	case OUI_MYTEK:
	case OUI_SSL:
	case OUI_PRESONUS:
	case OUI_HARMAN:
	case OUI_AVID:
		return (0);
	default:
		return (-1);
	}
}

/*
 * Match a fw_device against the family model tables, then the generic
 * DICE fallback.  Explicit model entries (like the M-Audio ProFire 2626,
 * whose unit directory version differs from DICE_INTERFACE) do not need
 * the category/version checks; catch-all entries and the generic fallback
 * do, which keeps non-DICE devices (e.g. Digidesign Digi 002/003, same
 * OUI as Avid) out of this driver.
 */
static const struct dice_model_entry *
dice_match_device(struct fw_device *fwdev, uint32_t vendor,
		  uint32_t model, uint32_t version)
{
	static const struct dice_model_entry *const tables[] = {
		dice_alesis_models,
		dice_maudio_models,
		NULL,
	};
	int t, i;

	/* 1. Explicit model entries. */
	for (t = 0; tables[t] != NULL; t++) {
		for (i = 0; tables[t][i].vendor_id != 0; i++) {
			const struct dice_model_entry *ent = &tables[t][i];

			if (ent->model_id == DICE_MODEL_ANY)
				continue;
			if (ent->vendor_id == vendor && ent->model_id == model)
				return (ent);
		}
	}

	/* 2. Catch-all entries (DICE category + interface version). */
	for (t = 0; tables[t] != NULL; t++) {
		for (i = 0; tables[t][i].vendor_id != 0; i++) {
			const struct dice_model_entry *ent = &tables[t][i];

			if (ent->model_id != DICE_MODEL_ANY)
				continue;
			if (ent->vendor_id != vendor)
				continue;
			if (model < 32 && (ent->not_model_mask & (1u << model)))
				continue;
			if (version != DICE_INTERFACE)
				continue;
			if (dice_check_category(fwdev, vendor, model) < 0)
				continue;
			return (ent);
		}
	}

	/* 3. Generic DICE fallback (ALSA's IEEE1394_MATCH_VERSION entry). */
	if (dice_known_vendor(vendor) == 0 &&
	    version == DICE_INTERFACE &&
	    dice_check_category(fwdev, vendor, model) == 0)
		return (&dice_generic_entry);

	return (NULL);
}

static const struct dice_model_entry *
dice_scan_bus(struct dice_bsd_softc *sc, struct fw_device **found)
{
	struct fw_device *fwdev;
	const struct dice_model_entry *model;
	uint32_t vendor, model_id, version;

	model = NULL;
	if (found != NULL)
		*found = NULL;

	/*
	 * Hold the firewire bus lock while iterating.  The body only
	 * reads fw_device memory (status/csrrom) - it never sleeps or
	 * issues transactions - so holding FW_GLOCK is safe.  Without
	 * it, fw_attach_dev() may free stale fw_device entries right
	 * out from under the scan after a bus reset.
	 */
	FW_GLOCK(sc->fc);
	STAILQ_FOREACH(fwdev, &sc->fc->devices, link) {
		/*
		 * Accept both FWDEVATTACHED (normal) and FWDEVINIT
		 * (bus reset in progress): crom_load() runs before the
		 * device transitions to FWDEVATTACHED, so FWDEVINIT
		 * devices may already have valid config ROM data.
		 */
		if (fwdev->status != FWDEVATTACHED &&
		    fwdev->status != FWDEVINIT)
			continue;

		/* EUI-64 OUI (first 3 bytes) = csrrom[3] >> 8. */
		vendor = fwdev->csrrom[3] >> 8;
		dice_read_unit_directory(fwdev, NULL, &model_id, &version);

		model = dice_match_device(fwdev, vendor, model_id, version);
		if (model != NULL) {
			if (found != NULL)
				*found = fwdev;
			break;
		}
	}
	FW_GUNLOCK(sc->fc);

	return (model);
}

/* ------------------------------------------------------------------ */
/* Rate / clock helpers                                                */
/* ------------------------------------------------------------------ */

/*
 * Build the SNDRV_PCM_RATE_* mask from the device's clock capabilities.
 * Very old firmware without GLOBAL_CLOCK_CAPABILITIES is assumed to
 * support 44.1/48 kHz only (ALSA check_clock_caps fallback).
 */
static void
dice_config_apply_rates(struct dice_device_config *cfg)
{
	unsigned int caps = cfg->clock_caps;
	unsigned int rates = 0, min = 0, max = 0;

	if (caps == 0)
		caps = CLOCK_CAP_RATE_44100 | CLOCK_CAP_RATE_48000;

	/* rate_min/rate_max carry the real sample rate in Hz (used for
	 * the channel caps and the runtime hw constraints), while
	 * `rates` stays the SNDRV_PCM_RATE_* bitmask. */
#define ADD_RATE(bit, hz, rate)						\
	do {								\
		if (caps & (bit)) {					\
			rates |= (rate);				\
			if (min == 0 || (hz) < min)			\
				min = (hz);				\
			if ((hz) > max)					\
				max = (hz);				\
		}							\
	} while (0)

	ADD_RATE(CLOCK_CAP_RATE_32000, 32000, SNDRV_PCM_RATE_32000);
	ADD_RATE(CLOCK_CAP_RATE_44100, 44100, SNDRV_PCM_RATE_44100);
	ADD_RATE(CLOCK_CAP_RATE_48000, 48000, SNDRV_PCM_RATE_48000);
	ADD_RATE(CLOCK_CAP_RATE_88200, 88200, SNDRV_PCM_RATE_88200);
	ADD_RATE(CLOCK_CAP_RATE_96000, 96000, SNDRV_PCM_RATE_96000);
	ADD_RATE(CLOCK_CAP_RATE_176400, 176400, SNDRV_PCM_RATE_176400);
	ADD_RATE(CLOCK_CAP_RATE_192000, 192000, SNDRV_PCM_RATE_192000);
#undef ADD_RATE

	if (rates == 0) {
		rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000;
		min = 44100;
		max = 48000;
	}

	cfg->rates = rates;
	cfg->rate_min = min;
	cfg->rate_max = max;
}

/* ------------------------------------------------------------------ */
/* Generic format detection                                            */
/*                                                                     */
/* Reads the currently configured TX/RX stream formats (ALSA            */
/* snd_dice_stream_detect_current_formats).  Used for devices without  */
/* a dedicated detect callback, e.g. the Alesis MultiMix 12/16.        */
/* ------------------------------------------------------------------ */

int
dice_detect_current_formats(struct dice_bsd_softc *sc,
			    struct dice_device_config *cfg)
{
	uint32_t reg[2];
	unsigned int tx_count, tx_size, rx_count, rx_size;
	unsigned int mode = SND_DICE_RATE_MODE_LOW;
	unsigned int i;
	int err;

	/* Assume the low rate mode until the streaming layer negotiates
	 * the actual mode; registers are read from the current state. */
	err = dice_read_tx(sc, TX_NUMBER, &reg[0], 4);
	if (err != 0)
		return (err);
	err = dice_read_tx(sc, TX_SIZE, &reg[1], 4);
	if (err != 0)
		return (err);
	tx_count = be32toh(reg[0]);
	tx_size = be32toh(reg[1]) * 4;

	err = dice_read_rx(sc, RX_NUMBER, &reg[0], 4);
	if (err != 0)
		return (err);
	err = dice_read_rx(sc, RX_SIZE, &reg[1], 4);
	if (err != 0)
		return (err);
	rx_count = be32toh(reg[0]);
	rx_size = be32toh(reg[1]) * 4;

	for (i = 0; i < tx_count && i < MAX_DICE_STREAMS; i++) {
		err = dice_read_tx(sc, tx_size * i + TX_NUMBER_AUDIO,
				   reg, sizeof(reg));
		if (err != 0)
			return (err);
		cfg->tx_pcm_chs[i][mode] = be32toh(reg[0]);
		if (be32toh(reg[1]) > cfg->tx_midi_ports[i])
			cfg->tx_midi_ports[i] = be32toh(reg[1]);
	}

	for (i = 0; i < rx_count && i < MAX_DICE_STREAMS; i++) {
		err = dice_read_rx(sc, rx_size * i + RX_NUMBER_AUDIO,
				   reg, sizeof(reg));
		if (err != 0)
			return (err);
		cfg->rx_pcm_chs[i][mode] = be32toh(reg[0]);
		if (be32toh(reg[1]) > cfg->rx_midi_ports[i])
			cfg->rx_midi_ports[i] = be32toh(reg[1]);
	}

	return (0);
}

/* ------------------------------------------------------------------ */
/* PCM callback implementations                                        */
/* ------------------------------------------------------------------ */

/*
 * Total PCM channels for one direction in one rate mode (sum over
 * streams, matching what a single ALSA PCM device exposes).
 */
static void
dice_config_channel_range(struct dice_device_config *cfg, int capture,
			  unsigned int *min_ch, unsigned int *max_ch)
{
	unsigned int mode, i;
	unsigned int mn = UINT_MAX, mx = 0;

	for (mode = 0; mode < SND_DICE_RATE_MODE_COUNT; mode++) {
		unsigned int total = 0;

		for (i = 0; i < MAX_DICE_STREAMS; i++) {
			if (capture)
				total += cfg->tx_pcm_chs[i][mode];
			else
				total += cfg->rx_pcm_chs[i][mode];
		}
		if (total == 0)
			continue;
		if (total < mn)
			mn = total;
		if (total > mx)
			mx = total;
	}

	if (mx == 0) {
		/* Detection did not provide anything; keep 2 channels. */
		mn = 2;
		mx = 2;
	}

	*min_ch = mn;
	*max_ch = mx;
}

/*
 * Pick the stream pointer for this substream's direction.
 */
static struct dice_pcm_stream *
dice_stream_for_substream(struct snd_pcm_substream *substream)
{
	struct snd_card *card = substream->pcm->card;
	struct dice_bsd_softc *sc = device_get_softc(card->dev->bsddev);

	if (sc->stream == NULL)
		return (NULL);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return (&sc->stream->playback);
	else
		return (&sc->stream->capture);
}

static int
dice_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_card *card = substream->pcm->card;
	struct dice_bsd_softc *sc = device_get_softc(card->dev->bsddev);
	struct dice_pcm_stream *ps;
	unsigned int min_ch, max_ch;

	if (runtime == NULL)
		return (-ENOMEM);

	ps = dice_stream_for_substream(substream);
	if (ps == NULL)
		return (-ENODEV);

	/* One open at a time per direction. */
	if (ps->substream != NULL)
		return (-EBUSY);

	/* Set hardware constraints for this device. */
	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_MMAP_VALID |
	    SNDRV_PCM_INFO_INTERLEAVED;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S24_3LE |
	    SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.rates = sc->cfg.rates;
	runtime->hw.rate_min = sc->cfg.rate_min;
	runtime->hw.rate_max = sc->cfg.rate_max;

	dice_config_channel_range(&sc->cfg,
	    (substream->stream == SNDRV_PCM_STREAM_CAPTURE),
	    &min_ch, &max_ch);
	runtime->hw.channels_min = min_ch;
	runtime->hw.channels_max = max_ch;

	runtime->hw.buffer_bytes_max = 1 << 24; /* 16MB */
	runtime->hw.period_bytes_min = 512;
	runtime->hw.period_bytes_max = 1 << 16; /* 64KB */
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 1024;

	ps->substream = substream;
	runtime->private_data = ps;
	return (0);
}

static int
dice_pcm_close(struct snd_pcm_substream *substream)
{
	struct dice_pcm_stream *ps = dice_stream_for_substream(substream);

	if (ps == NULL)
		return (0);

	/* Stop streaming if still active. */
	if (ps->active) {
		struct snd_card *card = substream->pcm->card;
		struct dice_bsd_softc *sc = device_get_softc(card->dev->bsddev);

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dice_streaming_stop_playback(sc);
		else
			dice_streaming_stop_capture(sc);
		ps->active = false;
	}

	ps->substream = NULL;
	if (substream->runtime != NULL)
		substream->runtime->private_data = NULL;
	return (0);
}

/*
 * Re-derive the stream's rate/channel geometry from the OSS channel's
 * CURRENT speed and format.
 *
 * basound_chan_setformat() runs ops->hw_params before the OSS
 * SNDCTL_DSP_SPEED ioctl is applied, so at hw_params time ch->speed is
 * still the 48000 Hz default; the rate the app actually negotiated (e.g.
 * 44100 Hz for a CD track in Audacious) only lands in ch->speed later.
 * If the streaming layer kept the stale 48000 the CIP FDF and the device
 * clock would say 48 kHz while the app fed 44.1 kHz audio — the device
 * stays at its own (48 kHz) rate, the stream never locks and the meter
 * LEDs stay dark.  Re-syncing from the channel at prepare/start time
 * makes ps->rate the real rate, which is also what dice_set_rate()
 * programs the device clock to.
 */
static void
dice_stream_sync_from_channel(struct dice_bsd_softc *sc,
			      struct dice_pcm_stream *ps,
			      struct basound_chan *ch)
{
	unsigned int mode_idx, device_ch = 0, midi = 0, rate, pcm_ch, i;
	int capture = (ps == &sc->stream->capture);

	if (ch == NULL)
		return;

	rate = (ch->speed > 0) ? ch->speed :
	    ((ps->rate != 0) ? ps->rate : 48000);
	pcm_ch = AFMT_CHANNEL(ch->format);
	if (pcm_ch == 0)
		pcm_ch = 2;

	/* Determine rate mode and get channel+MIDI counts from config. */
	if (rate <= 48000)
		mode_idx = SND_DICE_RATE_MODE_LOW;
	else if (rate <= 96000)
		mode_idx = SND_DICE_RATE_MODE_MIDDLE;
	else
		mode_idx = SND_DICE_RATE_MODE_HIGH;

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		if (capture) {
			device_ch += sc->cfg.tx_pcm_chs[i][mode_idx];
			midi += sc->cfg.tx_midi_ports[i];
		} else {
			device_ch += sc->cfg.rx_pcm_chs[i][mode_idx];
			midi += sc->cfg.rx_midi_ports[i];
		}
	}

	if (device_ch == 0)
		device_ch = pcm_ch;
	if (pcm_ch > device_ch)
		pcm_ch = device_ch;

	ps->rate = rate;
	ps->pcm_channels = pcm_ch;
	ps->device_channels = device_ch;
	ps->midi_ports = midi;
	ps->double_pcm_frames = !sc->cfg.disable_double_pcm_frames;

	/* AM824 data block quadlets: one per PCM channel + one for MIDI. */
	ps->data_block_quadlets = device_ch + (midi > 0 ? 1 : 0);

	/* Dual-wire at >96 kHz doubles the data block. */
	if (ps->double_pcm_frames && rate > 96000)
		ps->data_block_quadlets *= 2;
}

static int
dice_pcm_hw_params(struct snd_pcm_substream *substream, void *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct dice_pcm_stream *ps = runtime->private_data;
	struct dice_bsd_softc *sc = device_get_softc(substream->pcm->card->dev->bsddev);
	struct basound_chan *ch = substream->private_data;
	int err;

	if (runtime == NULL || ps == NULL)
		return (-EINVAL);

	err = snd_pcm_lib_malloc_pages(substream,
				       runtime->hw.buffer_bytes_max);
	if (err < 0)
		return (err);
	if (runtime->dma_area == NULL)
		return (-ENOMEM);

	runtime->dma_bytes = runtime->hw.buffer_bytes_max;

	/* Extract rate and channel count from basound channel.  Note that
	 * basound_chan_setformat() runs ops->hw_params BEFORE the OSS
	 * SNDCTL_DSP_SPEED ioctl has been applied, so ch->speed is still
	 * the 48000 Hz default here.  dice_pcm_prepare() re-syncs with
	 * the real speed at stream start. */
	dice_stream_sync_from_channel(sc, ps, ch);

	ps->period_bytes = (ch != NULL) ? ch->blocksize : 512;
	ps->buffer_bytes = runtime->dma_bytes;

	return (0);
}

static int
dice_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return (snd_pcm_lib_free_pages(substream));
}

static int
dice_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct dice_pcm_stream *ps = dice_stream_for_substream(substream);
	struct dice_bsd_softc *sc;
	struct basound_chan *ch;

	if (ps == NULL)
		return (-EINVAL);

	/*
	 * The OSS layer runs prepare at every trigger START, by which
	 * time SNDCTL_DSP_SPEED has applied the app's real rate — re-sync
	 * the stream geometry from the channel so the device clock, the
	 * CIP FDF and the frame rate all agree with what the app feeds
	 * (hw_params alone is not enough: it ran before the speed ioctl).
	 */
	sc = device_get_softc(substream->pcm->card->dev->bsddev);
	ch = substream->private_data;
	dice_stream_sync_from_channel(sc, ps, ch);
	if (ch != NULL) {
		if (ch->buffer != NULL)
			ps->buffer_bytes = ch->buffer->bufsize;
		ps->period_bytes = ch->blocksize;
	}

	ps->hwptr = 0;
	ps->period_accum = 0;
	ps->tx_dbc = 0;
	ps->frame_cycle = 0;
	return (0);
}

static int
dice_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_card *card = substream->pcm->card;
	struct dice_bsd_softc *sc = device_get_softc(card->dev->bsddev);
	int err = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			err = dice_streaming_start_playback(sc);
		else
			err = dice_streaming_start_capture(sc);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dice_streaming_stop_playback(sc);
		else
			dice_streaming_stop_capture(sc);
		break;
	default:
		return (-EINVAL);
	}

	return (err);
}

static unsigned long
dice_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct dice_pcm_stream *ps = dice_stream_for_substream(substream);

	if (ps == NULL || ps->buffer_bytes == 0)
		return (0);
	return (ps->hwptr % ps->buffer_bytes);
}

static const struct snd_pcm_ops dice_pcm_ops = {
	.open		= dice_pcm_open,
	.close		= dice_pcm_close,
	.hw_params	= dice_pcm_hw_params,
	.hw_free	= dice_pcm_hw_free,
	.prepare	= dice_pcm_prepare,
	.trigger	= dice_pcm_trigger,
	.pointer	= dice_pcm_pointer,
};

/* ------------------------------------------------------------------ */
/* FreeBSD device interface                                            */
/* ------------------------------------------------------------------ */

static void
dice_identify(driver_t *driver, device_t parent)
{
	if (device_find_child(parent, "basound_dice", DEVICE_UNIT_ANY) == NULL)
		BUS_ADD_CHILD(parent, 0, "basound_dice", DEVICE_UNIT_ANY);
}

static int
dice_probe(device_t dev)
{
	/* Real device matching happens in attach/discover, where the
	 * firewire bus has been explored. */
	device_set_desc(dev, "DICE FireWire Audio Device");
	return (BUS_PROBE_DEFAULT);
}

static int
dice_attach(device_t dev)
{
	struct dice_bsd_softc *sc = device_get_softc(dev);
	struct fw_device *fwdev;
	const struct dice_model_entry *model;

	/* The ivar on firewire bus children is struct firewire_comm *. */
	sc->fc = (struct firewire_comm *)device_get_ivars(dev);
	if (sc->fc == NULL)
		return (ENXIO);

	sc->dev = dev;
	sc->fwdev = NULL;
	sc->model = NULL;
	sc->attached = false;
	sc->discovering = 0;
	sc->alsa_dev.bsddev = dev;

	/*
	 * firewire bus dispatch contract: fw_busreset()/fw_attach_dev()
	 * cast our softc to struct firewire_dev_comm (hence the fwc
	 * first member) and call post_busreset/post_explore if set.
	 * post_busreset runs with FW_GLOCK held, so it must not sleep.
	 */
	sc->fwc.dev = dev;
	sc->fwc.fc = sc->fc;
	sc->fwc.post_busreset = dice_post_busreset;
	sc->fwc.post_explore = dice_post_explore;

	callout_init(&sc->discover_callout, 1);

	/* Try to find the device immediately. */
	model = dice_scan_bus(sc, &fwdev);
	if (model != NULL) {
		int err;

		sc->fwdev = fwdev;
		sc->model = model;
		err = dice_init_card(sc);
		if (err != 0) {
			/* Attach failed; detach() will not be called, so
			 * clean up the resources we took here. */
			sc->fwdev = NULL;
			sc->model = NULL;
			dice_streaming_fini(sc);
			return (err);
		}
		return (0);
	}

	sc->discover_retries = 0;
	device_printf(dev, "No DICE device found yet, "
	    "scheduling deferred discovery\n");
	callout_reset(&sc->discover_callout, hz, dice_discover, sc);
	return (0);
}

/*
 * Called by the firewire bus after every bus reset, with FW_GLOCK
 * held (fwohci_task_busreset -> fw_busreset).  We must not sleep or
 * take FW_GLOCK here - just re-arm the discovery callout so the new
 * topology is scanned shortly.
 */
static void
dice_post_busreset(void *arg)
{
	struct dice_bsd_softc *sc = (struct dice_bsd_softc *)arg;

	if (!sc->attached && sc->fwdev == NULL)
		callout_reset(&sc->discover_callout, hz, dice_discover, sc);
}

/*
 * Called by fw_attach_dev() after bus exploration completes, on the
 * firewire probe thread with no locks held.  This is the race-free
 * moment to scan the device list: list mutation (fw_attach_dev's
 * free of stale entries) happens on this same thread before us.
 */
static void
dice_post_explore(void *arg)
{
	struct dice_bsd_softc *sc = (struct dice_bsd_softc *)arg;

	dice_discover(sc);
}

static void
dice_discover(void *arg)
{
	struct dice_bsd_softc *sc = (struct dice_bsd_softc *)arg;
	struct fw_device *fwdev;
	const struct dice_model_entry *model;
	int err;

	if (sc->attached || sc->fwdev != NULL)
		return;

	/* Re-entry guard: post_explore (probe thread) and the callout
	 * (softclock) can race each other. */
	if (!atomic_cmpset_int(&sc->discovering, 0, 1))
		return;

	model = dice_scan_bus(sc, &fwdev);
	if (model == NULL) {
		sc->discover_retries++;
		if (sc->discover_retries == 5)
			device_printf(sc->dev,
			    "still waiting for DICE device (retry %d)\n",
			    sc->discover_retries);
		/* Retry indefinitely - the device will appear after a
		 * bus reset completes. */
		callout_reset(&sc->discover_callout, hz, dice_discover, sc);
		atomic_store_int(&sc->discovering, 0);
		return;
	}

	device_printf(sc->dev, "DICE device found on firewire bus (%s)\n",
		      model->desc);
	sc->fwdev = fwdev;
	sc->model = model;
	err = dice_init_card(sc);
	if (err != 0) {
		device_printf(sc->dev, "Failed to init card: %d\n", err);
		/* Drop the failed match and keep retrying; the device
		 * may not have been ready yet. */
		sc->fwdev = NULL;
		sc->model = NULL;
		sc->discover_retries = 0;
		callout_reset(&sc->discover_callout, hz, dice_discover, sc);
	}
	atomic_store_int(&sc->discovering, 0);
}

/* ------------------------------------------------------------------ */
/* Card setup                                                          */
/* ------------------------------------------------------------------ */

static int
dice_init_card(struct dice_bsd_softc *sc)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_rawmidi *rmidi;
	struct dice_device_config *cfg = &sc->cfg;
	unsigned int tx_midi = 0, rx_midi = 0, i;
	uint32_t reg;
	int err;

	memset(cfg, 0, sizeof(*cfg));

	/* Locate the DICE register sections. */
	err = dice_get_subaddrs(sc);
	if (err != 0) {
		device_printf(sc->dev, "DICE subaddresses invalid (%d)\n", err);
		return (err);
	}

	/* Clock capabilities; very old firmware does not implement this
	 * register, in which case we keep the 44.1/48 kHz fallback. */
	err = dice_read_global(sc, GLOBAL_CLOCK_CAPABILITIES, &reg, 4);
	if (err == 0)
		cfg->clock_caps = be32toh(reg);

	/* Device-specific format detection. */
	err = sc->model->detect(sc, cfg);
	if (err != 0) {
		device_printf(sc->dev, "%s: format detection failed (%d)\n",
			      sc->model->desc, err);
		return (err);
	}

	dice_config_apply_rates(cfg);

	err = snd_card_new(&sc->alsa_dev, device_get_unit(sc->dev), "DICE",
			   NULL, 0, &card);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create ALSA card: %d\n", err);
		return (err);
	}

	strlcpy(card->driver, "basound_dice", sizeof(card->driver));
	strlcpy(card->shortname, sc->model->desc, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s, GUID %08x%08x at S%d",
		 sc->model->desc, sc->fwdev->csrrom[3], sc->fwdev->csrrom[4],
		 100 << sc->fwdev->speed);

	/* One PCM device covering playback (RX) and capture (TX). */
	err = snd_pcm_new(card, "DICE Audio", 0, 1, 1, &pcm);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create PCM device: %d\n", err);
		snd_card_free(card);
		return (err);
	}

	pcm->private_data = sc;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dice_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &dice_pcm_ops);

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		tx_midi += cfg->tx_midi_ports[i];
		rx_midi += cfg->rx_midi_ports[i];
	}
	err = snd_rawmidi_new(card, "DICE MIDI", 0, rx_midi, tx_midi, &rmidi);
	if (err != 0) {
		device_printf(sc->dev, "Failed to create MIDI device: %d\n",
			      err);
		/* MIDI is optional, continue without it. */
	}

	/*
	 * Initialise the ISO DMA streaming layer BEFORE snd_card_register
	 * (i.e. before the pcm channels are created and their .open ops
	 * run).  dice_pcm_open() resolves the per-stream state through
	 * sc->stream, so if streaming_init ran after the channels were
	 * added, open would fail (sc->stream == NULL), ps->substream
	 * would never be set, the 1 ms refill callout would bail out on
	 * every tick, hwptr would never advance and the app's write()
	 * would block forever — "transport starts, then stops, no
	 * audio" (observed on the Alesis MultiMix and iO26).
	 */
	err = dice_streaming_init(sc);
	if (err != 0) {
		device_printf(sc->dev, "Failed to init streaming: %d\n", err);
		snd_card_free(card);
		return (err);
	}

	err = snd_card_register(card);
	if (err != 0) {
		device_printf(sc->dev, "Failed to register ALSA card: %d\n",
			      err);
		dice_streaming_fini(sc);
		snd_card_free(card);
		return (err);
	}

	sc->attached = true;

	/* The /dev/pf2626N control interface only applies to EAP devices
	 * (ProFire 2626), which set cfg->setup_router during detection. */
	if (cfg->setup_router != NULL) {
		err = dice_cdev_create(sc, device_get_unit(sc->dev));
		if (err != 0)
			device_printf(sc->dev,
			    "Failed to create mixer device: %d\n", err);
	}

	device_printf(sc->dev, "%s attached (rates %u-%u, "
	    "capture %u/%u ch, playback %u/%u ch)\n",
	    sc->model->desc, cfg->rate_min, cfg->rate_max,
	    cfg->tx_pcm_chs[0][0] + cfg->tx_pcm_chs[1][0],
	    cfg->tx_pcm_chs[0][SND_DICE_RATE_MODE_HIGH] +
	    cfg->tx_pcm_chs[1][SND_DICE_RATE_MODE_HIGH],
	    cfg->rx_pcm_chs[0][0] + cfg->rx_pcm_chs[1][0],
	    cfg->rx_pcm_chs[0][SND_DICE_RATE_MODE_HIGH] +
	    cfg->rx_pcm_chs[1][SND_DICE_RATE_MODE_HIGH]);

	return (0);
}

static int
dice_detach(device_t dev)
{
	struct dice_bsd_softc *sc = device_get_softc(dev);

	if (sc == NULL)
		return (0);

	callout_drain(&sc->discover_callout);
	dice_cdev_destroy(sc);
	dice_streaming_fini(sc);
	sc->attached = false;

	return (0);
}

static device_method_t dice_bsd_methods[] = {
	DEVMETHOD(device_identify,	dice_identify),
	DEVMETHOD(device_probe,		dice_probe),
	DEVMETHOD(device_attach,	dice_attach),
	DEVMETHOD(device_detach,	dice_detach),
	DEVMETHOD_END
};

static driver_t dice_bsd_driver = {
	"basound_dice",
	dice_bsd_methods,
	sizeof(struct dice_bsd_softc),
};

DRIVER_MODULE(basound_dice, firewire, dice_bsd_driver, 0, 0);
MODULE_DEPEND(basound_dice, basound, 1, 1, 1);
MODULE_DEPEND(basound_dice, firewire, 1, 1, 1);
MODULE_DEPEND(basound_dice, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
