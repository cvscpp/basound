/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dice_ioctl.h — M-Audio ProFire 2626 (DICE EAP) mixer ioctl interface
 *
 * This header is safe to include from both kernel modules and userspace
 * programs.  A FreeBSD-native mixer tool should include this file and
 * open /dev/pf2626N to configure the card's internal crossbar router,
 * matrix mixer and sample clock.
 *
 * The ProFire 2626 is a TCAT DICE Jr. that exposes the "extension
 * application protocol" (EAP) at 0xffffe0200000.  Inside that space the
 * router, mixer and stream-configuration sections are referenced by a
 * pointer table (see dice_maudio_bsd.c for the on-wire details).
 *
 * Router entry encoding
 * ---------------------
 *   entries[i] = (src << 8) | dst
 *   src        = (source_block << 4) | source_channel
 *   dst        = (dest_block   << 4) | dest_channel
 *
 * Source block IDs
 *   AES 0, ADAT 1, Mixer 2, InS0 4, InS1 5, ARM 10,
 *   ARX0 11 (1394 RX stream 0), ARX1 12 (1394 RX stream 1), Muted 15
 *
 * Destination block IDs
 *   AES 0, ADAT 1, Mixer0 2, Mixer1 3, InS0 4, InS1 5 (analog out),
 *   ARM 10, ATX0 11 (1394 TX stream 0), ATX1 12, Muted 15
 *
 * Mixer coefficient encoding
 * --------------------------
 *   0        = mute (-inf)
 *   16384    = unity (0 dB)  [FFADO c2p14]
 *   dB       = 20 * log10(value / 16384)
 */

#ifndef _DICE_IOCTL_H_
#define _DICE_IOCTL_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define PF2626_MAX_ROUTES	128
#define PF2626_MAX_MIXER_IN	18
#define PF2626_MAX_MIXER_OUT	16
#define PF2626_MIXER_COEFFS	(PF2626_MAX_MIXER_IN * PF2626_MAX_MIXER_OUT)

/*
 * Device-level I/O: the ProFire 2626 has 26 capture and 26 playback
 * channels (8 analog + 8+8 ADAT + 2 S/PDIF on each side).  This is
 * independent of the internal matrix-mixer geometry above (18x16).
 */
#define PF2626_MAX_INPUTS	26
#define PF2626_MAX_OUTPUTS	26

/*
 * PF2626_IOCTL_GET_CONFIG
 *
 * Returns static device capabilities plus the live clock state.  Channel
 * counts are the per-stream PCM complements at each of the three DICE
 * rate modes (0 = up to 48 kHz, 1 = up to 96 kHz, 2 = up to 192 kHz).
 */
struct pf2626_config {
	uint16_t router_max_entries;	/* max route entries */
	uint8_t  router_exposed;	/* router is exposed */
	uint8_t  router_readonly;	/* router is read-only */
	uint16_t mixer_inputs;		/* matrix mixer input count */
	uint16_t mixer_outputs;		/* matrix mixer output count */
	uint8_t  mixer_exposed;		/* mixer is exposed */
	uint8_t  mixer_readonly;	/* mixer is read-only */
	uint32_t clock_caps;		/* GLOBAL_CLOCK_CAPABILITIES */
	uint32_t clock_select;		/* raw GLOBAL_CLOCK_SELECT */
	uint32_t clock_status;		/* raw GLOBAL_STATUS */
	uint16_t rx_pcm_chs[2][3];	/* playback channels, [stream][mode] */
	uint16_t tx_pcm_chs[2][3];	/* capture channels, [stream][mode] */
	uint8_t  rx_midi[2];		/* playback MIDI ports per stream */
	uint8_t  tx_midi[2];		/* capture MIDI ports per stream */
	uint8_t  _pad[4];
};

/*
 * PF2626_IOCTL_GET_ROUTER / PF2626_IOCTL_SET_ROUTER
 *
 * Reads the currently-active router table for `rate_mode`, or programs a
 * new table and activates it with the LoadRouter command.
 *
 * rate_mode: 0 = low (<=48 kHz), 1 = mid (<=96 kHz), 2 = high (<=192 kHz).
 * For SET, `count` entries in `entries[]` are written and activated.
 */
struct pf2626_router {
	uint16_t count;
	uint16_t rate_mode;
	uint32_t entries[PF2626_MAX_ROUTES];
};

/*
 * PF2626_IOCTL_GET_MIXER / PF2626_IOCTL_SET_MIXER
 *
 * Reads/writes the matrix mixer coefficients.  The matrix is stored
 * output-major: coefficient for (input j -> output i) is at index
 * `i * inputs + j`, where inputs/outputs are the counts from
 * pf2626_config.  Only the low 16 bits of each quadlet carry gain.
 *
 * GET_MIXER additionally returns the DICE EAP peak meters for the
 * currently-active router configuration.  Each value is a 12-bit linear
 * signal level (0..4095, 4095 = full scale).  Channels follow the
 * device's 26 in / 26 out complement (PF2626_MAX_INPUTS/_OUTPUTS):
 *
 *   input_peak[0..7]     = capture 1..8       (analog line in)
 *   input_peak[8..15]    = capture 9..16      (ADAT A in)
 *   input_peak[16..23]   = capture 17..24     (ADAT B in)
 *   input_peak[24..25]   = capture 25..26     (S/PDIF in)
 *
 *   output_peak[0..7]    = playback 1..8      (analog line/phone out)
 *   output_peak[8..15]   = playback 9..16     (ADAT A out)
 *   output_peak[16..23]  = playback 17..24    (ADAT B out)
 *   output_peak[24..25]  = playback 25..26    (S/PDIF out)
 *
 * Peaks are taken from the router destination levels of the active
 * configuration: capture channels are the 1394 TX stream destinations
 * (ATX0/ATX1), playback channels the physical output destinations
 * (InS1/ADAT/AES).  SET_MIXER ignores the peak arrays.
 */
struct pf2626_mixer {
	uint16_t inputs;
	uint16_t outputs;
	uint16_t _pad[2];
	uint32_t coeff[PF2626_MIXER_COEFFS];
	uint32_t input_peak[PF2626_MAX_INPUTS];
	uint32_t output_peak[PF2626_MAX_OUTPUTS];
};

/*
 * PF2626_IOCTL_GET_CLOCK / PF2626_IOCTL_SET_CLOCK
 *
 * The clock source/rate selection.  `source` uses the CLOCK_SOURCE_*
 * values (0x0c = internal), `rate` uses CLOCK_RATE_* >> CLOCK_RATE_SHIFT
 * (1 = 44.1 kHz, 2 = 48 kHz, ...).  `select`/`status` mirror the raw
 * GLOBAL_CLOCK_SELECT and GLOBAL_STATUS registers.
 */
struct pf2626_clock {
	uint8_t  source;
	uint8_t  rate;
	uint8_t  _pad[2];
	uint32_t select;
	uint32_t status;
};

#define PF2626_IOCTL_GET_CONFIG  _IOR('P', 1, struct pf2626_config)
#define PF2626_IOCTL_GET_ROUTER  _IOWR('P', 2, struct pf2626_router)
#define PF2626_IOCTL_SET_ROUTER  _IOW('P', 3, struct pf2626_router)
#define PF2626_IOCTL_GET_MIXER   _IOWR('P', 4, struct pf2626_mixer)
#define PF2626_IOCTL_SET_MIXER   _IOW('P', 5, struct pf2626_mixer)
#define PF2626_IOCTL_GET_CLOCK   _IOR('P', 6, struct pf2626_clock)
#define PF2626_IOCTL_SET_CLOCK   _IOW('P', 7, struct pf2626_clock)

#endif /* _DICE_IOCTL_H_ */
