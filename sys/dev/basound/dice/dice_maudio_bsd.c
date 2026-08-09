// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dice_maudio_bsd.c - M-Audio DICE FireWire devices (FreeBSD glue)
 *
 * Device-specific parts for:
 *   - M-Audio ProFire 2626  (unit-directory model 0x000010)
 *
 * The ProFire 2626 is based on the TCD2210/2220 (Dice Mini/Jr.) and
 * implements the TCAT application-protocol extension: detailed stream
 * formats live in a separate address space (0xffffe0200000) instead of
 * the standard TX/RX section.  Its unit-directory version also differs
 * from DICE_INTERFACE, so it must be matched explicitly (vendor+model).
 *
 * Ported from ALSA's dice-extension.c.  M-Audio devices are compliant
 * to IEC 61883-1/6 and have no quirk at high sampling transfer
 * frequency, hence disable_double_pcm_frames (ALSA dice.c probe).
 *
 * The table below is combined with the other family tables by the
 * generic matcher in dice_bsd.c; nothing here touches digi00x.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include "dice_bsd.h"

#define MODEL_PROFIRE_2626	0x000010
#define MODEL_PROFIRE_610	0x000011

/* Extension application space layout (offsets within the pointer table). */
#define EXT_APP_STREAM_OFFSET_IDX	6	/* pair index 6 -> quadlet 12 */

static int
dice_maudio_read_stream_entries(struct dice_bsd_softc *sc, uint64_t section_addr,
				unsigned int base_offset,
				unsigned int stream_count, unsigned int mode,
				unsigned int pcm_chs[MAX_DICE_STREAMS]
						  [SND_DICE_RATE_MODE_COUNT],
				unsigned int midi_ports[MAX_DICE_STREAMS])
{
	uint32_t reg[2];
	unsigned int i;
	int err;

	for (i = 0; i < stream_count && i < MAX_DICE_STREAMS; i++) {
		uint64_t entry_addr = section_addr + base_offset +
		    (uint64_t)i * EXT_APP_STREAM_ENTRY_SIZE;

		err = dice_read_block(sc->fwdev,
				      entry_addr + EXT_APP_NUMBER_AUDIO,
				      reg, sizeof(reg));
		if (err != 0)
			return (err);

		pcm_chs[i][mode] = be32toh(reg[0]);
		if (be32toh(reg[1]) > midi_ports[i])
			midi_ports[i] = be32toh(reg[1]);
	}

	return (0);
}

static int
dice_maudio_detect_stream_formats(struct dice_bsd_softc *sc,
				  uint64_t section_addr,
				  struct dice_device_config *cfg)
{
	uint32_t reg[2];
	unsigned int base_offset, stream_count;
	unsigned int mode;
	int err = 0;

	for (mode = 0; mode < SND_DICE_RATE_MODE_COUNT; mode++) {
		unsigned int cap;

		if (mode == SND_DICE_RATE_MODE_HIGH)
			cap = CLOCK_CAP_RATE_176400 | CLOCK_CAP_RATE_192000;
		else if (mode == SND_DICE_RATE_MODE_MIDDLE)
			cap = CLOCK_CAP_RATE_88200 | CLOCK_CAP_RATE_96000;
		else
			cap = CLOCK_CAP_RATE_32000 | CLOCK_CAP_RATE_44100 |
			      CLOCK_CAP_RATE_48000;

		/* Some models report formats at the highest mode although
		 * they do not support it; check clock capabilities. */
		if (!(cap & cfg->clock_caps))
			continue;

		base_offset = 0x2000 * mode + 0x1000;
		err = dice_read_block(sc->fwdev,
				      section_addr + base_offset +
				      EXT_APP_STREAM_TX_NUMBER,
				      reg, sizeof(reg));
		if (err != 0)
			break;

		base_offset += EXT_APP_STREAM_ENTRIES;

		stream_count = be32toh(reg[0]);
		err = dice_maudio_read_stream_entries(sc, section_addr,
						      base_offset,
						      stream_count, mode,
						      cfg->tx_pcm_chs,
						      cfg->tx_midi_ports);
		if (err != 0)
			break;

		base_offset += stream_count * EXT_APP_STREAM_ENTRY_SIZE;

		stream_count = be32toh(reg[1]);
		err = dice_maudio_read_stream_entries(sc, section_addr,
						      base_offset,
						      stream_count, mode,
						      cfg->rx_pcm_chs,
						      cfg->rx_midi_ports);
		if (err != 0)
			break;
	}

	return (err);
}

/*
 * ProFire 2626 format detection (ALSA snd_dice_detect_extension_formats).
 * Reads the extension application space pointer table and, from the
 * stream section, the per-rate-mode TX/RX channel and MIDI counts.
 */
int
dice_maudio_detect_profire2626_formats(struct dice_bsd_softc *sc,
				       struct dice_device_config *cfg)
{
	uint32_t pointers[9 * 2];
	uint64_t section_addr;
	unsigned int i, j;
	int err;

	err = dice_read_block(sc->fwdev, DICE_EXT_APP_SPACE,
			      pointers, sizeof(pointers));
	if (err != 0)
		return (err);

	/* Check that the section offsets are all distinct. */
	for (i = 0; i < 9; i++) {
		for (j = i + 1; j < 9; j++) {
			if (pointers[i * 2] == pointers[j * 2])
				return (ENXIO);	/* fall back to limited func. */
		}
	}

	section_addr = DICE_EXT_APP_SPACE +
	    (uint64_t)be32toh(pointers[EXT_APP_STREAM_OFFSET_IDX * 2]) * 4;

	err = dice_maudio_detect_stream_formats(sc, section_addr, cfg);
	if (err != 0)
		return (err);

	/* M-Audio devices are IEC 61883-1/6 compliant. */
	cfg->disable_double_pcm_frames = true;

	return (0);
}

/*
 * M-Audio model table.  The ProFire 610 (0x000011) uses the same
 * extension protocol; add it here when hardware becomes available.
 */
const struct dice_model_entry dice_maudio_models[] = {
	{
		.vendor_id	= OUI_MAUDIO,
		.model_id	= MODEL_PROFIRE_2626,
		.detect		= dice_maudio_detect_profire2626_formats,
		.desc		= "M-Audio ProFire 2626",
	},
	{ 0 },
};
