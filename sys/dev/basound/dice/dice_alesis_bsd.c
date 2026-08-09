// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dice_alesis_bsd.c - Alesis DICE FireWire devices (FreeBSD glue)
 *
 * Device-specific parts for:
 *   - Alesis iO14 / iO26      (unit-directory model 0x000001)
 *   - Alesis MultiMix 12/16 FireWire (standard DICE devices)
 *
 * The iO14 and iO26 share one model ID; the hardware is told apart by
 * the TX_NUMBER_AUDIO register (4/6 = iO14, otherwise iO26).  The
 * MultiMix consoles are plain DICE II devices with no dedicated ALSA
 * entry - they use the generic current-format detection.  This file
 * mirrors ALSA's dice-alesis.c plus the generic fallback behaviour.
 *
 * The tables below are combined with the other family tables by the
 * generic matcher in dice_bsd.c; nothing here touches digi00x.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include "dice_bsd.h"

#define MODEL_ALESIS_IO_BOTH	0x000001	/* iO14 + iO26 */
#define MODEL_ALESIS_MASTERCTL	0x000002

/*
 * TX (device -> host) PCM channels per rate mode:
 *   mode 0 = up to 48 kHz, mode 1 = up to 96 kHz, mode 2 = up to 192 kHz.
 * Stream 0 is analog + S/PDIF, stream 1 is the ADAT ports.
 */
static const unsigned int
alesis_io14_tx_pcm_chs[MAX_DICE_STREAMS][SND_DICE_RATE_MODE_COUNT] = {
	{6, 6, 4},	/* Tx0 = Analog + S/PDIF. */
	{8, 4, 0},	/* Tx1 = ADAT1. */
};

static const unsigned int
alesis_io26_tx_pcm_chs[MAX_DICE_STREAMS][SND_DICE_RATE_MODE_COUNT] = {
	{10, 10, 4},	/* Tx0 = Analog + S/PDIF. */
	{16, 4, 0},	/* Tx1 = ADAT1 + ADAT2 (available at low rate). */
};

/*
 * Alesis iO14/iO26 format detection (ALSA snd_dice_detect_alesis_formats).
 * The model ID is shared, so we read TX_NUMBER_AUDIO and pick the
 * channel map accordingly.
 */
int
dice_alesis_detect_io_formats(struct dice_bsd_softc *sc,
			      struct dice_device_config *cfg)
{
	uint32_t reg;
	unsigned int i;
	int err;

	err = dice_read_tx(sc, TX_NUMBER_AUDIO, &reg, 4);
	if (err != 0)
		return (err);

	if (be32toh(reg) == 4 || be32toh(reg) == 6)
		memcpy(cfg->tx_pcm_chs, alesis_io14_tx_pcm_chs,
		       sizeof(alesis_io14_tx_pcm_chs));
	else
		memcpy(cfg->tx_pcm_chs, alesis_io26_tx_pcm_chs,
		       sizeof(alesis_io26_tx_pcm_chs));

	/* RX (host -> device) is 8 channels at every rate mode. */
	for (i = 0; i < SND_DICE_RATE_MODE_COUNT; i++)
		cfg->rx_pcm_chs[0][i] = 8;

	cfg->tx_midi_ports[0] = 1;
	cfg->rx_midi_ports[0] = 1;

	return (0);
}

/*
 * Alesis MultiMix 12/16 FireWire format detection.
 *
 * The MultiMix consoles are DICE II devices without a dedicated ALSA
 * entry; they are matched by the generic fallback and report their
 * current stream configuration through the standard TX/RX registers
 * (18-in / 2-out at up to 48 kHz on the MultiMix 16).  We reuse the
 * generic current-format detection from the core.
 */
int
dice_alesis_detect_multimix_formats(struct dice_bsd_softc *sc,
				    struct dice_device_config *cfg)
{
	return (dice_detect_current_formats(sc, cfg));
}

/*
 * Alesis model table.  Order matters: the explicit iO entry must come
 * before the catch-all MultiMix entry, and the MasterControl (0x000002)
 * is excluded from the catch-all because it has its own protocol.
 */
const struct dice_model_entry dice_alesis_models[] = {
	{
		.vendor_id	= OUI_ALESIS,
		.model_id	= MODEL_ALESIS_IO_BOTH,
		.detect		= dice_alesis_detect_io_formats,
		.desc		= "Alesis iO14/iO26",
	},
	{
		/* Any other Alesis DICE device is a MultiMix console. */
		.vendor_id	= OUI_ALESIS,
		.model_id	= DICE_MODEL_ANY,
		.not_model_mask	= (1u << MODEL_ALESIS_IO_BOTH) |
				  (1u << MODEL_ALESIS_MASTERCTL),
		.detect		= dice_alesis_detect_multimix_formats,
		.desc		= "Alesis MultiMix 12/16 FireWire",
	},
	{ 0 },
};
