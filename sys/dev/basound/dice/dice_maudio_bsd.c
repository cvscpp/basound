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
#include "dice_ioctl.h"

#define MODEL_PROFIRE_2626	0x000010
#define MODEL_PROFIRE_610	0x000011

/* Extension application space layout (offsets within the pointer table). */
#define EXT_APP_STREAM_OFFSET_IDX	6	/* pair index 6 -> quadlet 12 */

/*
 * TCAT EAP (extension application protocol) router programming.
 *
 * The M-Audio ProFire 2626 is a DICE Jr. with the TCAT EAP extension.
 * Its physical outputs (line/headphone, ADAT, SPDIF) are fed by an
 * internal crossbar router whose default power-on state does NOT route
 * the FireWire playback streams (ARX0/ARX1) to those outputs.  FFADO
 * programs a default router config for exactly this device on connect;
 * the FreeBSD port must do the same, otherwise Audacious can stream for
 * hours with no signal on any output.
 *
 * The EAP space (DICE_EXT_APP_SPACE) starts with a pointer table of 9
 * (offset,size) pairs, in quadlets:
 *
 *   0 caps, 1 command, 2 mixer, 3 peak, 4 router, 5 stream,
 *   6 current-config, 7 standalone, 8 application
 *
 * A router config is written to the "router" section (index 4), then
 * activated with the LoadRouter command (index 1).  Layout of the router
 * section: quadlet[0] = number of entries, then one quadlet per route
 * encoded as (src << 8) | dst, where src/dst are (block_id << 4) | ch.
 *
 * Block IDs follow FFADO's eRouteSource/eRouteDestination, which match
 * Takashi Sakamoto's snd-firewire-ctl-services tcd22xx_spec.
 */

#define EAP_PTR_COUNT		9
#define EAP_PTR_CMD_IDX		1
#define EAP_PTR_ROUTER_IDX	4

#define EAP_CMD_OPCODE_OFF	0x00
#define EAP_CMD_RETVAL_OFF	0x04
#define EAP_CMD_LD_ROUTER	0x0001
#define EAP_CMD_FLAG_LOW	(1U << 16)
#define EAP_CMD_FLAG_MID	(1U << 17)
#define EAP_CMD_FLAG_HIGH	(1U << 18)
#define EAP_CMD_FLAG_EXECUTE	(1U << 31)

/* Router source block IDs. */
#define EAP_SRC_AES		0
#define EAP_SRC_ADAT		1
#define EAP_SRC_MIXER		2
#define EAP_SRC_INS0		4
#define EAP_SRC_INS1		5
#define EAP_SRC_ARM		10
#define EAP_SRC_ARX0		11	/* 1394 RX stream 0 (host -> device) */
#define EAP_SRC_ARX1		12	/* 1394 RX stream 1 (host -> device) */
#define EAP_SRC_MUTED		15

/* Router destination block IDs. */
#define EAP_DST_AES		0
#define EAP_DST_ADAT		1
#define EAP_DST_MIXER0		2
#define EAP_DST_MIXER1		3
#define EAP_DST_INS0		4
#define EAP_DST_INS1		5	/* analog line/headphone outputs */
#define EAP_DST_ARM		10
#define EAP_DST_ATX0		11	/* 1394 TX stream 0 (device -> host) */
#define EAP_DST_ATX1		12	/* 1394 TX stream 1 (device -> host) */
#define EAP_DST_MUTED		15

#define EAP_MAX_ROUTES		128

static uint32_t
eap_route(unsigned int src_blk, unsigned int src_ch,
	  unsigned int dst_blk, unsigned int dst_ch)
{
	uint32_t src = (uint32_t)((src_blk << 4) | (src_ch & 0x0f));
	uint32_t dst = (uint32_t)((dst_blk << 4) | (dst_ch & 0x0f));

	return ((src << 8) | dst);
}

/*
 * Fill `entries` with the FFADO default router configuration for the
 * ProFire 2626 at the given rate mode.  Returns the entry count.
 *
 * Low/mid mapping (ports follow FFADO profire_2626.cpp):
 *  - FireWire playback stream 0 (ARX0) 0..7  -> analog line out 1..8
 *  - FireWire playback stream 0 (ARX0) 8..15 -> ADAT A out
 *  - FireWire playback stream 1 (ARX1) 0..7  -> ADAT B out
 *  - FireWire playback stream 1 (ARX1) 8..9  -> SPDIF out
 *  - analog line in 1..8                   -> FireWire capture stream 0
 *  - ADAT A in / ADAT B in / SPDIF in      -> FireWire capture streams
 *  - mixer outputs                          -> muted
 */
static unsigned int
dice_maudio_build_routes(uint32_t *entries, unsigned int max,
			 unsigned int rate)
{
	unsigned int adat_ch, i, n = 0;

	if (rate <= 48000)
		adat_ch = 8;
	else if (rate <= 96000)
		adat_ch = 4;
	else
		adat_ch = 2;

#define ROUTE(sb, sc, db, dc)						\
	do {								\
		if (n >= max)						\
			return (n);					\
		entries[n++] = eap_route((sb), (sc), (db), (dc));	\
	} while (0)

	/* Playback: FireWire RX -> physical outputs. */
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_ARX0, i, EAP_DST_INS1, i);	/* line/phones */
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_ARX0, i + 8, EAP_DST_ADAT, i);	/* ADAT A */
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_ARX1, i, EAP_DST_ADAT, i + 8);	/* ADAT B */
	for (i = 0; i < 2; i++)
		ROUTE(EAP_SRC_ARX1, i + 8, EAP_DST_AES, i);	/* SPDIF */

	/* Capture: physical inputs -> FireWire TX. */
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_INS1, i, EAP_DST_ATX0, i);	/* line in */
	for (i = 0; i < adat_ch; i++)
		ROUTE(EAP_SRC_ADAT, i, EAP_DST_ATX0, i + 8);	/* ADAT A */
	for (i = 0; i < adat_ch; i++)
		ROUTE(EAP_SRC_ADAT, i + adat_ch, EAP_DST_ATX1, i);/* ADAT B */
	for (i = 0; i < 2; i++)
		ROUTE(EAP_SRC_AES, i, EAP_DST_ATX1, i + 8);	/* SPDIF */

	/* Mixer inputs (so the internal mixer sees the physical inputs). */
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_INS1, i, EAP_DST_MIXER0, i);
	for (i = 0; i < 8; i++)
		ROUTE(EAP_SRC_ADAT, i, EAP_DST_MIXER0, i + 8);
	for (i = 0; i < 2; i++)
		ROUTE(EAP_SRC_AES, i, EAP_DST_MIXER1, i);

	/* Mute the mixer outputs; playback is routed directly, not through
	 * the mixer, so this keeps the mixer from feeding stray audio into
	 * the outputs as well. */
	for (i = 0; i < 16; i++)
		ROUTE(EAP_SRC_MIXER, i, EAP_DST_MUTED, 0);

#undef ROUTE

	return (n);
}

/*
 * Program the TCAT EAP router for the ProFire 2626.
 *
 * Writes the route table into the "router" section and activates it with
 * the LoadRouter command for the matching rate mode.  Called at stream
 * start (via cfg->setup_router) so the FireWire playback channels reach
 * the analog/digital outputs.
 */
int
dice_maudio_setup_router(struct dice_bsd_softc *sc, unsigned int rate)
{
	uint32_t pointers[EAP_PTR_COUNT * 2];
	uint32_t entries[EAP_MAX_ROUTES];
	uint32_t cmd, retval, count;
	uint64_t router_addr, cmd_addr;
	unsigned int i, n, cmd_flag;
	int err;

	err = dice_read_block(sc->fwdev, DICE_EXT_APP_SPACE,
			      pointers, sizeof(pointers));
	if (err != 0) {
		device_printf(sc->dev, "dice: EAP pointer read failed (%d)\n",
		    err);
		return (err);
	}

	router_addr = DICE_EXT_APP_SPACE +
	    (uint64_t)be32toh(pointers[EAP_PTR_ROUTER_IDX * 2]) * 4;
	cmd_addr = DICE_EXT_APP_SPACE +
	    (uint64_t)be32toh(pointers[EAP_PTR_CMD_IDX * 2]) * 4;

	n = dice_maudio_build_routes(entries, EAP_MAX_ROUTES, rate);
	if (n == 0 || n >= EAP_MAX_ROUTES) {
		device_printf(sc->dev, "dice: invalid router table (%u routes)\n",
		    n);
		return (EINVAL);
	}

	/* Write count first as zero so a partial write never leaves a bogus
	 * active table, then the entries, then the real count last. */
	count = 0;
	err = dice_write_quad(sc->fwdev, router_addr, htobe32(count));
	if (err != 0)
		goto out;

	for (i = 0; i < n; i++) {
		err = dice_write_quad(sc->fwdev,
		    router_addr + 4 + (uint64_t)i * 4, htobe32(entries[i]));
		if (err != 0)
			goto out;
	}

	count = htobe32(n);
	err = dice_write_quad(sc->fwdev, router_addr, count);
	if (err != 0)
		goto out;

	/* Activate the router config for the current rate mode. */
	if (rate <= 48000)
		cmd_flag = EAP_CMD_FLAG_LOW;
	else if (rate <= 96000)
		cmd_flag = EAP_CMD_FLAG_MID;
	else
		cmd_flag = EAP_CMD_FLAG_HIGH;

	cmd = htobe32(EAP_CMD_LD_ROUTER | cmd_flag | EAP_CMD_FLAG_EXECUTE);
	err = dice_write_quad(sc->fwdev,
	    cmd_addr + EAP_CMD_OPCODE_OFF, cmd);
	if (err != 0)
		goto out;

	/* Wait for the firmware to clear the EXECUTE flag. */
	for (i = 0; i < 100; i++) {
		err = dice_read_quad(sc->fwdev,
		    cmd_addr + EAP_CMD_OPCODE_OFF, &cmd);
		if (err != 0)
			goto out;
		if ((be32toh(cmd) & EAP_CMD_FLAG_EXECUTE) == 0)
			break;
		DELAY(1000);	/* 1 ms */
	}
	if ((be32toh(cmd) & EAP_CMD_FLAG_EXECUTE) != 0) {
		device_printf(sc->dev,
		    "dice: router load timed out (cmd=0x%08x)\n", be32toh(cmd));
		err = ETIMEDOUT;
		goto out;
	}

	err = dice_read_quad(sc->fwdev,
	    cmd_addr + EAP_CMD_RETVAL_OFF, &retval);
	if (err != 0)
		goto out;
	if (be32toh(retval) != 0) {
		device_printf(sc->dev,
		    "dice: router load failed (retval=0x%08x)\n",
		    be32toh(retval));
		err = EIO;
		goto out;
	}

	device_printf(sc->dev, "dice: programmed router (%u routes, "
	    "rate %u)\n", n, rate);
	return (0);

out:
	device_printf(sc->dev, "dice: router programming failed (%d)\n", err);
	return (err);
}

/* ------------------------------------------------------------------ */
/* EAP section helpers used by the /dev/pf2626N mixer ioctls           */
/* ------------------------------------------------------------------ */

#define EAP_PTR_CAPS_IDX	0
#define EAP_PTR_MIXER_IDX	2
#define EAP_PTR_PEAK_IDX	3
#define EAP_PTR_CURRENT_IDX	6

/* Router capability register bits (FFADO dice_eap.h). */
#define EAP_CAP_ROUTER_EXPOSED		0x00000001
#define EAP_CAP_ROUTER_READONLY	0x00000002
#define EAP_CAP_ROUTER_FLASHSTORED	0x00000004
#define EAP_CAP_ROUTER_MAXROUTES	0xffff0000

/* Mixer capability register bits (FFADO dice_eap.h). */
#define EAP_CAP_MIXER_EXPOSED		0x00000001
#define EAP_CAP_MIXER_READONLY		0x00000002
#define EAP_CAP_MIXER_FLASHSTORED	0x00000004
#define EAP_CAP_MIXER_INPUTS		0x00ff0000
#define EAP_CAP_MIXER_OUTPUTS		0xff000000

/* Peak meter encoding: each quadlet holds the 12-bit linear signal level
 * in bits 16..27 and the route's destination (block/channel) in the low
 * byte. */
#define EAP_PEAK_MASK			0x0fff0000
#define EAP_PEAK_SHIFT			16

static int
dice_maudio_eap_section(struct dice_bsd_softc *sc, unsigned int idx,
			uint64_t *addr_out)
{
	uint32_t pointers[EAP_PTR_COUNT * 2];
	int err;

	err = dice_read_block(sc->fwdev, DICE_EXT_APP_SPACE,
			      pointers, sizeof(pointers));
	if (err != 0)
		return (err);

	*addr_out = DICE_EXT_APP_SPACE +
	    (uint64_t)be32toh(pointers[idx * 2]) * 4;
	return (0);
}

/*
 * Fill the router/mixer capability fields used by GET_CONFIG.
 */
int
dice_maudio_read_caps(struct dice_bsd_softc *sc,
		      uint16_t *router_max, uint8_t *router_exposed,
		      uint8_t *router_readonly,
		      uint16_t *mixer_in, uint16_t *mixer_out,
		      uint8_t *mixer_exposed, uint8_t *mixer_readonly)
{
	uint64_t caps_addr;
	uint32_t rcap, mcap;
	int err;

	err = dice_maudio_eap_section(sc, EAP_PTR_CAPS_IDX, &caps_addr);
	if (err != 0)
		return (err);

	err = dice_read_quad(sc->fwdev, caps_addr + 0x00, &rcap);
	if (err != 0)
		return (err);
	err = dice_read_quad(sc->fwdev, caps_addr + 0x04, &mcap);
	if (err != 0)
		return (err);

	rcap = be32toh(rcap);
	mcap = be32toh(mcap);

	if (router_max != NULL)
		*router_max = (uint16_t)((rcap & EAP_CAP_ROUTER_MAXROUTES) >> 16);
	if (router_exposed != NULL)
		*router_exposed = (rcap & EAP_CAP_ROUTER_EXPOSED) != 0;
	if (router_readonly != NULL)
		*router_readonly = (rcap & EAP_CAP_ROUTER_READONLY) != 0;

	if (mixer_in != NULL)
		*mixer_in = (uint16_t)((mcap & EAP_CAP_MIXER_INPUTS) >> 16);
	if (mixer_out != NULL)
		*mixer_out = (uint16_t)((mcap & EAP_CAP_MIXER_OUTPUTS) >> 24);
	if (mixer_exposed != NULL)
		*mixer_exposed = (mcap & EAP_CAP_MIXER_EXPOSED) != 0;
	if (mixer_readonly != NULL)
		*mixer_readonly = (mcap & EAP_CAP_MIXER_READONLY) != 0;

	return (0);
}

static unsigned int
dice_maudio_rate_mode_offset(unsigned int rate_mode)
{
	if (rate_mode == SND_DICE_RATE_MODE_HIGH)
		return (0x4000);
	if (rate_mode == SND_DICE_RATE_MODE_MIDDLE)
		return (0x2000);
	return (0x0000);
}

/*
 * Read the currently-active router table for `rate_mode` from the
 * current-configuration section (EAP pointer index 6).
 */
int
dice_maudio_get_router(struct dice_bsd_softc *sc, unsigned int rate_mode,
		       uint32_t *entries, unsigned int max_entries,
		       unsigned int *count_out)
{
	uint64_t cur_addr;
	uint32_t count;
	unsigned int i, n, off;
	int err;

	err = dice_maudio_eap_section(sc, EAP_PTR_CURRENT_IDX, &cur_addr);
	if (err != 0)
		return (err);

	off = dice_maudio_rate_mode_offset(rate_mode);
	err = dice_read_quad(sc->fwdev, cur_addr + off, &count);
	if (err != 0)
		return (err);
	n = be32toh(count);
	if (n > max_entries)
		n = max_entries;

	for (i = 0; i < n; i++) {
		err = dice_read_quad(sc->fwdev,
		    cur_addr + off + 4 + (uint64_t)i * 4, &entries[i]);
		if (err != 0)
			return (err);
		entries[i] = be32toh(entries[i]);
	}

	if (count_out != NULL)
		*count_out = n;
	return (0);
}

/*
 * Program a new router table and activate it with LoadRouter for
 * `rate_mode`.
 */
int
dice_maudio_set_router(struct dice_bsd_softc *sc, unsigned int rate_mode,
		       const uint32_t *entries, unsigned int count)
{
	uint64_t router_addr, cmd_addr;
	uint32_t cmd, retval;
	unsigned int i, cmd_flag;
	int err;

	if (count == 0 || count > EAP_MAX_ROUTES)
		return (EINVAL);

	err = dice_maudio_eap_section(sc, EAP_PTR_ROUTER_IDX, &router_addr);
	if (err != 0)
		return (err);
	err = dice_maudio_eap_section(sc, EAP_PTR_CMD_IDX, &cmd_addr);
	if (err != 0)
		return (err);

	/* Zero the count first, then entries, then the real count last. */
	err = dice_write_quad(sc->fwdev, router_addr, htobe32(0));
	if (err != 0)
		return (err);
	for (i = 0; i < count; i++) {
		err = dice_write_quad(sc->fwdev,
		    router_addr + 4 + (uint64_t)i * 4, htobe32(entries[i]));
		if (err != 0)
			return (err);
	}
	err = dice_write_quad(sc->fwdev, router_addr, htobe32(count));
	if (err != 0)
		return (err);

	if (rate_mode == SND_DICE_RATE_MODE_HIGH)
		cmd_flag = EAP_CMD_FLAG_HIGH;
	else if (rate_mode == SND_DICE_RATE_MODE_MIDDLE)
		cmd_flag = EAP_CMD_FLAG_MID;
	else
		cmd_flag = EAP_CMD_FLAG_LOW;

	cmd = htobe32(EAP_CMD_LD_ROUTER | cmd_flag | EAP_CMD_FLAG_EXECUTE);
	err = dice_write_quad(sc->fwdev, cmd_addr + EAP_CMD_OPCODE_OFF, cmd);
	if (err != 0)
		return (err);

	for (i = 0; i < 100; i++) {
		err = dice_read_quad(sc->fwdev,
		    cmd_addr + EAP_CMD_OPCODE_OFF, &cmd);
		if (err != 0)
			return (err);
		if ((be32toh(cmd) & EAP_CMD_FLAG_EXECUTE) == 0)
			break;
		DELAY(1000);
	}
	if ((be32toh(cmd) & EAP_CMD_FLAG_EXECUTE) != 0)
		return (ETIMEDOUT);

	err = dice_read_quad(sc->fwdev, cmd_addr + EAP_CMD_RETVAL_OFF, &retval);
	if (err != 0)
		return (err);
	if (be32toh(retval) != 0)
		return (EIO);

	return (0);
}

/*
 * Read the matrix mixer coefficients from the mixer section.
 * Coeff layout is output-major: index = output * inputs + input.
 */
int
dice_maudio_get_mixer(struct dice_bsd_softc *sc, unsigned int inputs,
		      unsigned int outputs, uint32_t *coeff)
{
	uint64_t mixer_addr;
	unsigned int i, n;
	int err;

	err = dice_maudio_eap_section(sc, EAP_PTR_MIXER_IDX, &mixer_addr);
	if (err != 0)
		return (err);

	n = inputs * outputs;
	for (i = 0; i < n; i++) {
		err = dice_read_quad(sc->fwdev, mixer_addr + 4 + (uint64_t)i * 4,
				     &coeff[i]);
		if (err != 0)
			return (err);
		coeff[i] = be32toh(coeff[i]);
	}
	return (0);
}

/*
 * Write the matrix mixer coefficients.  The mixer is applied live by the
 * device; no Load command is required.
 */
int
dice_maudio_set_mixer(struct dice_bsd_softc *sc, unsigned int inputs,
		      unsigned int outputs, const uint32_t *coeff)
{
	uint64_t mixer_addr;
	unsigned int i, n;
	int err;

	err = dice_maudio_eap_section(sc, EAP_PTR_MIXER_IDX, &mixer_addr);
	if (err != 0)
		return (err);

	n = inputs * outputs;
	for (i = 0; i < n; i++) {
		err = dice_write_quad(sc->fwdev,
		    mixer_addr + 4 + (uint64_t)i * 4, htobe32(coeff[i]));
		if (err != 0)
			return (err);
	}
	return (0);
}

/*
 * Read the DICE EAP peak meters and split them into the device's 26
 * capture / 26 playback channels.
 *
 * The peak section (EAP pointer index 3) mirrors the currently-active
 * router configuration: one quadlet per route, each carrying the 12-bit
 * linear signal level of that route in bits 16..27 and the route's
 * destination in the low byte.  The active configuration is the one for
 * the current sample rate, so the rate is derived from
 * GLOBAL_CLOCK_SELECT first.
 *
 * Channel mapping (destination-keyed, so it follows whatever the user
 * has routed; see dice_ioctl.h):
 *
 *   capture  (input_peak): 1394 TX stream destinations
 *     ATX0 ch 0..15 -> input_peak[0..15]   (analog in, ADAT A in)
 *     ATX1 ch 0..9  -> input_peak[16..25]  (ADAT B in, S/PDIF in)
 *
 *   playback (output_peak): physical output destinations
 *     InS1 ch 0..7  -> output_peak[0..7]   (analog line/phone out)
 *     ADAT ch 0..15 -> output_peak[8..23]  (ADAT A/B out)
 *     AES  ch 0..1  -> output_peak[24..25] (S/PDIF out)
 *
 * All values are zero when no routes are active (e.g. before the first
 * stream start programs the router).
 */
int
dice_maudio_get_peaks(struct dice_bsd_softc *sc, uint32_t *input_peak,
		      uint32_t *output_peak)
{
	uint32_t peaks[EAP_MAX_ROUTES];
	uint32_t sel, v;
	uint64_t cur_addr, peak_addr;
	unsigned int i, n, off, mode;
	int err;

	memset(input_peak, 0, PF2626_MAX_INPUTS * sizeof(*input_peak));
	memset(output_peak, 0, PF2626_MAX_OUTPUTS * sizeof(*output_peak));

	/* Active rate mode from GLOBAL_CLOCK_SELECT. */
	err = dice_read_global(sc, GLOBAL_CLOCK_SELECT, &v, 4);
	if (err != 0)
		return (err);
	sel = be32toh(v);
	switch ((sel & CLOCK_RATE_MASK) >> CLOCK_RATE_SHIFT) {
	case 3:		/* 88.2 kHz */
	case 4:		/* 96 kHz */
		mode = SND_DICE_RATE_MODE_MIDDLE;
		break;
	case 5:		/* 176.4 kHz */
	case 6:		/* 192 kHz */
		mode = SND_DICE_RATE_MODE_HIGH;
		break;
	default:	/* 32/44.1/48 kHz */
		mode = SND_DICE_RATE_MODE_LOW;
		break;
	}

	/* Route count for that mode from the current-configuration
	 * section; the peak section has exactly one entry per route. */
	err = dice_maudio_eap_section(sc, EAP_PTR_CURRENT_IDX, &cur_addr);
	if (err != 0)
		return (err);
	off = dice_maudio_rate_mode_offset(mode);
	err = dice_read_quad(sc->fwdev, cur_addr + off, &v);
	if (err != 0)
		return (err);
	n = be32toh(v);
	if (n > EAP_MAX_ROUTES)
		n = EAP_MAX_ROUTES;
	if (n == 0)
		return (0);

	err = dice_maudio_eap_section(sc, EAP_PTR_PEAK_IDX, &peak_addr);
	if (err != 0)
		return (err);
	err = dice_read_block(sc->fwdev, peak_addr, peaks,
			      n * sizeof(*peaks));
	if (err != 0)
		return (err);

	for (i = 0; i < n; i++) {
		uint32_t peak = be32toh(peaks[i]);
		uint8_t dst = (uint8_t)(peak & 0xff);
		unsigned int lvl = (peak & EAP_PEAK_MASK) >> EAP_PEAK_SHIFT;
		unsigned int blk = dst >> 4, ch = dst & 0x0f;

		switch (blk) {
		case EAP_DST_ATX0:	/* capture stream 0 */
			if (ch < 16 && lvl > input_peak[ch])
				input_peak[ch] = lvl;
			break;
		case EAP_DST_ATX1:	/* capture stream 1 */
			if (ch < 10 && lvl > input_peak[16 + ch])
				input_peak[16 + ch] = lvl;
			break;
		case EAP_DST_INS1:	/* analog line/phone out */
			if (ch < 8 && lvl > output_peak[ch])
				output_peak[ch] = lvl;
			break;
		case EAP_DST_ADAT:	/* ADAT A + ADAT B out */
			if (ch < 16 && lvl > output_peak[8 + ch])
				output_peak[8 + ch] = lvl;
			break;
		case EAP_DST_AES:	/* S/PDIF out */
			if (ch < 2 && lvl > output_peak[24 + ch])
				output_peak[24 + ch] = lvl;
			break;
		default:
			break;
		}
	}
	return (0);
}

/*
 * Read the current GLOBAL_CLOCK_SELECT / GLOBAL_STATUS registers.
 */
int
dice_maudio_get_clock(struct dice_bsd_softc *sc, uint32_t *select,
		      uint32_t *status)
{
	uint32_t v;
	int err;

	err = dice_read_global(sc, 0x04c /* GLOBAL_CLOCK_SELECT */, &v, 4);
	if (err != 0)
		return (err);
	if (select != NULL)
		*select = be32toh(v);

	err = dice_read_global(sc, 0x054 /* GLOBAL_STATUS */, &v, 4);
	if (err != 0)
		return (err);
	if (status != NULL)
		*status = be32toh(v);

	return (0);
}

/*
 * Program GLOBAL_CLOCK_SELECT with the requested source and rate.
 */
int
dice_maudio_set_clock(struct dice_bsd_softc *sc, unsigned int source,
		      unsigned int rate)
{
	uint32_t cur, v;
	int err;

	err = dice_read_global(sc, 0x04c /* GLOBAL_CLOCK_SELECT */, &v, 4);
	if (err != 0)
		return (err);
	cur = be32toh(v);
	cur = (cur & ~0x000000ff) | (source & 0xff);
	cur = (cur & ~0x0000ff00) | ((rate & 0xff) << 8);
	err = dice_write_quad(sc->fwdev,
	    DICE_PRIVATE_SPACE + sc->global_offset + 0x04c, htobe32(cur));
	return (err);
}

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

	/* The ProFire 2626 needs its internal crossbar router programmed
	 * before FireWire playback reaches the physical outputs. */
	cfg->setup_router = dice_maudio_setup_router;

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
