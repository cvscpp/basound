/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dice_bsd.h - shared definitions for the FreeBSD DICE FireWire glue
 *
 * The DICE (TC Applied Technologies Digital Interface Communications
 * Engine) family covers many FireWire audio interfaces.  The FreeBSD
 * port keeps the device-specific parts in per-family files so they
 * cannot interfere with each other (or with the digi00x driver, which
 * lives in its own directory and is never touched here):
 *
 *   dice_bsd.c          - generic DICE core: device discovery, FireWire
 *                         transactions, PCM/MIDI glue, generic format
 *                         detection
 *   dice_alesis_bsd.c   - Alesis iO14/iO26 and MultiMix 12/16 FireWire
 *   dice_maudio_bsd.c   - M-Audio ProFire 2626
 *
 * Model tables from each family file are combined by the generic
 * matcher in dice_bsd.c; each entry carries its own detect callback.
 */

#ifndef _BASOUND_DICE_BSD_H_
#define _BASOUND_DICE_BSD_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/types.h>

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>
#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>

/* ------------------------------------------------------------------ */
/* Forward declaration for streaming                                    */
/* ------------------------------------------------------------------ */

struct dice_streaming;

/* ------------------------------------------------------------------ */
/* DICE vendor OUIs (EUI-64 prefixes)                                  */
/* ------------------------------------------------------------------ */

#define OUI_WEISS		0x001c6a
#define OUI_LOUD		0x000ff2
#define OUI_FOCUSRITE		0x00130e
#define OUI_TCELECTRONIC	0x000166
#define OUI_ALESIS		0x000595
#define OUI_MAUDIO		0x000d6c
#define OUI_MYTEK		0x001ee8
#define OUI_SSL			0x0050c2	/* Actually ID reserved by IEEE. */
#define OUI_PRESONUS		0x000a92
#define OUI_HARMAN		0x000fd7
#define OUI_AVID		0x00a07e

/* GUID category byte used by DICE devices (see check_dice_category). */
#define DICE_CATEGORY_ID	0x04
#define WEISS_CATEGORY_ID	0x00
#define LOUD_CATEGORY_ID	0x10
#define HARMAN_CATEGORY_ID	0x20

/* Unit-directory version of the DICE driver interface. */
#define DICE_INTERFACE		0x000001

/* ------------------------------------------------------------------ */
/* DICE register map (subset needed for format detection)              */
/* ------------------------------------------------------------------ */

#define DICE_PRIVATE_SPACE		0xffffe0000000ull

/* Section pointer table at the top of the private space (quadlets). */
#define DICE_GLOBAL_OFFSET		0x00
#define DICE_GLOBAL_SIZE		0x04
#define DICE_TX_OFFSET			0x08
#define DICE_TX_SIZE			0x0c
#define DICE_RX_OFFSET			0x10
#define DICE_RX_SIZE			0x14
#define DICE_EXT_SYNC_OFFSET		0x18
#define DICE_EXT_SYNC_SIZE		0x1c

/* Global section. */
#define GLOBAL_CLOCK_SELECT		0x04c
#define  CLOCK_RATE_MASK		0x0000ff00
#define  CLOCK_RATE_SHIFT		8
#define  CLOCK_SOURCE_MASK		0x000000ff
#define GLOBAL_CLOCK_CAPABILITIES	0x064
#define  CLOCK_CAP_RATE_32000		0x00000001
#define  CLOCK_CAP_RATE_44100		0x00000002
#define  CLOCK_CAP_RATE_48000		0x00000004
#define  CLOCK_CAP_RATE_88200		0x00000008
#define  CLOCK_CAP_RATE_96000		0x00000010
#define  CLOCK_CAP_RATE_176400		0x00000020
#define  CLOCK_CAP_RATE_192000		0x00000040

/* TX (device -> host, i.e. capture) and RX (host -> device, i.e. playback). */
#define TX_NUMBER			0x000
#define TX_SIZE				0x004
#define TX_NUMBER_AUDIO			0x00c
#define RX_NUMBER			0x000
#define RX_SIZE				0x004
#define RX_NUMBER_AUDIO			0x00c

/* TCAT application-protocol extension (TCD2210/2220), used by the
 * M-Audio ProFire 2626. */
#define DICE_EXT_APP_SPACE		0xffffe0200000ull
#define EXT_APP_STREAM_OFFSET		0x30	/* index into pointer table */
#define EXT_APP_STREAM_TX_NUMBER	0x0000
#define EXT_APP_STREAM_RX_NUMBER	0x0004
#define EXT_APP_STREAM_ENTRIES		0x0008
#define EXT_APP_STREAM_ENTRY_SIZE	0x010c
#define EXT_APP_NUMBER_AUDIO		0x0000
#define EXT_APP_NUMBER_MIDI		0x0004

/* ------------------------------------------------------------------ */
/* Streams and sampling-rate modes                                     */
/* ------------------------------------------------------------------ */

#define MAX_DICE_STREAMS	2

enum snd_dice_rate_mode {
	SND_DICE_RATE_MODE_LOW = 0,	/* 32.0/44.1/48.0 kHz */
	SND_DICE_RATE_MODE_MIDDLE,	/* 88.2/96.0 kHz */
	SND_DICE_RATE_MODE_HIGH,	/* 176.4/192.0 kHz */
	SND_DICE_RATE_MODE_COUNT,
};

struct dice_bsd_softc;

/* Per-device configuration filled in by the family detect callbacks. */
struct dice_device_config {
	unsigned int clock_caps;	/* GLOBAL_CLOCK_CAPABILITIES, 0 = unknown */
	unsigned int rates;		/* SNDRV_PCM_RATE_* mask */
	unsigned int rate_min, rate_max;
	unsigned int tx_pcm_chs[MAX_DICE_STREAMS][SND_DICE_RATE_MODE_COUNT];
	unsigned int rx_pcm_chs[MAX_DICE_STREAMS][SND_DICE_RATE_MODE_COUNT];
	unsigned int tx_midi_ports[MAX_DICE_STREAMS];
	unsigned int rx_midi_ports[MAX_DICE_STREAMS];
	bool disable_double_pcm_frames;	/* IEC 61883-1/6 compliant devices */
};

typedef int (*dice_detect_formats_t)(struct dice_bsd_softc *sc,
				     struct dice_device_config *cfg);

/* Sentinel for "match any unit-directory model". */
#define DICE_MODEL_ANY		0xffffffff

/*
 * One row of a family model table (see dice_alesis_models[] /
 * dice_maudio_models[]).  Entries with model_id == DICE_MODEL_ANY only
 * match devices that also pass the DICE category/version checks (they
 * behave like ALSA's generic fallback entry); not_model_mask excludes
 * known models (bit n = model n) from such catch-all entries.
 */
struct dice_model_entry {
	uint32_t vendor_id;
	uint32_t model_id;		/* DICE_MODEL_ANY = catch-all */
	uint32_t not_model_mask;	/* used when model_id == DICE_MODEL_ANY */
	dice_detect_formats_t detect;
	const char *desc;
};

/* Softc for the FreeBSD side of the DICE driver. */
struct dice_bsd_softc {
	/*
	 * MUST be the first member: the firewire bus dispatches
	 * post_busreset/post_explore by casting the child softc to
	 * struct firewire_dev_comm.  Without this the first fields of
	 * this softc would be interpreted as those function pointers
	 * and called as code - a guaranteed kernel panic on the first
	 * bus reset after attach (e.g. when a device is plugged in).
	 */
	struct firewire_dev_comm fwc;
	device_t dev;
	struct device alsa_dev;		/* wrapper so card->dev stays valid */
	struct firewire_comm *fc;
	struct fw_device *fwdev;
	const struct dice_model_entry *model;
	struct dice_device_config cfg;

	/* DICE section offsets in bytes, relative to DICE_PRIVATE_SPACE. */
	uint64_t global_offset;
	uint64_t tx_offset;
	uint64_t rx_offset;
	uint64_t sync_offset;

	/* Audio streaming framework (PCM plumbing). */
	struct dice_streaming *stream;

	/* Deferred discovery (FireWire bus may not be explored yet). */
	struct callout discover_callout;
	int discover_retries;
	volatile u_int discovering;	/* re-entry guard for dice_discover */
	bool attached;
};

/* ------------------------------------------------------------------ */
/* FireWire transaction helpers (fwmem + timeout, see digi00x-stream)  */
/* ------------------------------------------------------------------ */

int dice_read_quad(struct fw_device *fwdev, uint64_t addr, uint32_t *val);
int dice_write_quad(struct fw_device *fwdev, uint64_t addr, uint32_t val);
int dice_read_block(struct fw_device *fwdev, uint64_t addr,
		    void *buf, size_t len);

/* Section-relative reads used by the format detection callbacks. */
int dice_read_global(struct dice_bsd_softc *sc, unsigned int offset,
		     void *buf, unsigned int len);
int dice_read_tx(struct dice_bsd_softc *sc, unsigned int offset,
		 void *buf, unsigned int len);
int dice_read_rx(struct dice_bsd_softc *sc, unsigned int offset,
		 void *buf, unsigned int len);

/* ------------------------------------------------------------------ */
/* CSR ROM helpers                                                     */
/* ------------------------------------------------------------------ */

int dice_read_unit_directory(struct fw_device *fwdev, uint32_t *specifier_id,
			     uint32_t *model, uint32_t *version);
int dice_check_category(struct fw_device *fwdev, uint32_t vendor,
			uint32_t model);

/* ------------------------------------------------------------------ */
/* Detect callbacks (one per family file)                              */
/* ------------------------------------------------------------------ */

/* dice_bsd.c - generic current-format detection (ALSA
 * snd_dice_stream_detect_current_formats). */
int dice_detect_current_formats(struct dice_bsd_softc *sc,
				struct dice_device_config *cfg);

/* dice_alesis_bsd.c */
int dice_alesis_detect_io_formats(struct dice_bsd_softc *sc,
				  struct dice_device_config *cfg);
int dice_alesis_detect_multimix_formats(struct dice_bsd_softc *sc,
					struct dice_device_config *cfg);

/* dice_maudio_bsd.c */
int dice_maudio_detect_profire2626_formats(struct dice_bsd_softc *sc,
					   struct dice_device_config *cfg);

/* ------------------------------------------------------------------ */
/* Family model tables                                                 */
/* ------------------------------------------------------------------ */

extern const struct dice_model_entry dice_alesis_models[];
extern const struct dice_model_entry dice_maudio_models[];

#endif /* _BASOUND_DICE_BSD_H_ */
