/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * hdsp_ioctl.h — RME HDSP mixer ioctl interface
 *
 * This header is safe to include from both kernel modules and userspace
 * programs.  A FreeBSD-native hdspmixer equivalent tool should include
 * this file and open /dev/hdspN to access the card.
 *
 * Gain value encoding
 * -------------------
 *   HDSP_GAIN_UNITY    = 32768 (0x8000) =  0 dB
 *   HDSP_GAIN_SILENCE  =     0 (0x0000) = −∞ (muted)
 *
 * Matrix address encoding  (firmware_rev != 0x0a)
 * ------------------------------------------------
 *   Playback channel `p' routed to output `o':
 *       addr = (52 * o) + (26 + p)
 *
 *   Capture (hardware input) channel `c' monitored on output `o':
 *       addr = (52 * o) + c
 *
 *   For the very old firmware_rev == 0x0a variant use row-width 64
 *   instead of 52 and column-offset 32 instead of 26 — in practice
 *   no card in service has this revision.
 *
 * Helper macros (safe to use from userspace)
 * ------------------------------------------
 *   HDSP_PLAYBACK_ADDR(o, p)  addr for playback p → output o
 *   HDSP_CAPTURE_ADDR(o, c)   addr for capture  c → output o (monitoring)
 */

#ifndef _HDSP_IOCTL_H_
#define _HDSP_IOCTL_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define HDSP_MATRIX_MIXER_SIZE  2048

#define HDSP_GAIN_UNITY         32768   /* 0x8000 =  0 dB */
#define HDSP_GAIN_SILENCE       0       /* 0x0000 = muted */

/* I/O box types — matches enum HDSP_IO_Type in the kernel driver */
#define HDSP_IO_DIGIFACE        0
#define HDSP_IO_MULTIFACE       1
#define HDSP_IO_H9652           2
#define HDSP_IO_H9632           3
#define HDSP_IO_RPM             4
#define HDSP_IO_UNKNOWN         5

/* Matrix address helpers (firmware_rev != 0x0a) */
#define HDSP_PLAYBACK_ADDR(out, in)  ((52 * (out)) + (26 + (in)))
#define HDSP_CAPTURE_ADDR(out, in)   ((52 * (out)) + (in))

/*
 * HDSP_IOCTL_GET_CONFIG
 *
 * Returns static card information.  All fields are valid after the
 * firmware has been uploaded (i.e. as soon as /dev/hdspN appears).
 */
struct hdsp_config_info {
	uint8_t  io_type;               /* HDSP_IO_* constant above */
	uint8_t  firmware_rev;          /* PCI revision byte */
	uint8_t  max_channels;          /* physical I/O channel count */
	uint8_t  ss_in_channels;        /* capture channels (single-speed) */
	uint8_t  ss_out_channels;       /* playback channels (single-speed) */
	uint8_t  _pad[3];
	uint32_t system_sample_rate;    /* current sample rate in Hz */
};

/*
 * HDSP_IOCTL_GET_MIXER
 *
 * Returns a snapshot of all 2048 gain values from the software-mirror
 * of the hardware matrix mixer.  The mirror is always kept in sync with
 * the hardware by hdsp_write_gain().
 *
 * matrix[addr] = gain (0 = silence, 32768 = 0 dB unity)
 */
struct hdsp_mixer_ioctl {
	uint16_t matrix[HDSP_MATRIX_MIXER_SIZE]; /* 4096 bytes */
};

/*
 * HDSP_IOCTL_SET_ENTRY
 *
 * Writes a single gain value to both the hardware mixer RAM and the
 * software mirror.  Use this for incremental updates (e.g. dragging a
 * fader) to avoid the overhead of a full matrix upload.
 */
struct hdsp_mixer_entry {
	uint32_t addr;          /* 0 .. HDSP_MATRIX_MIXER_SIZE - 1 */
	uint16_t gain;          /* HDSP_GAIN_SILENCE .. HDSP_GAIN_UNITY */
	uint16_t _pad;
};

/*
 * HDSP_IOCTL_SET_MIXER
 *
 * Atomically replaces all 2048 hardware gain values.  Useful for
 * restoring a saved mixer state on startup.  The full matrix write
 * takes a few milliseconds (2048 FIFO writes); the ioctl blocks until
 * all writes complete.
 */

/*
 * HDSP_IOCTL_GET_LEVELS
 *
 * Returns instantaneous peak and RMS levels read directly from the
 * hardware BAR registers (no FIFO involved — safe to call at any time).
 * The hardware maintains peak-hold accumulators that are reset on each
 * read; call at least 25 Hz for smooth VU meter display.
 *
 * Peak value encoding:
 *   0           = silence
 *   0x7FFFFFFF  = 0 dBFS (full scale)
 *   dBFS = 20 * log10(value / 2147483647.0)
 *
 * RMS values are 64-bit accumulators.  Convert to dBFS:
 *   dBFS = 10 * log10(rms / (2^63))   (approx; hardware dependent)
 *
 * output_peaks has 28 entries: channels 0..25 are the normal outputs,
 * 26..27 are the phones (headphone) outputs present on all card
 * variants (on a Multiface these are the headphone jack; SPDIF is
 * 24/25).
 */
struct hdsp_peak_levels {
	uint32_t input_peaks[26];
	uint32_t playback_peaks[26];
	uint32_t output_peaks[28];
	uint64_t input_rms[26];
	uint64_t playback_rms[26];
};

#define HDSP_IOCTL_GET_CONFIG   _IOR('H', 1, struct hdsp_config_info)
#define HDSP_IOCTL_GET_MIXER    _IOR('H', 2, struct hdsp_mixer_ioctl)
#define HDSP_IOCTL_SET_ENTRY    _IOW('H', 3, struct hdsp_mixer_entry)
#define HDSP_IOCTL_SET_MIXER    _IOW('H', 4, struct hdsp_mixer_ioctl)
#define HDSP_IOCTL_GET_LEVELS   _IOR('H', 5, struct hdsp_peak_levels)

#endif /* _HDSP_IOCTL_H_ */
