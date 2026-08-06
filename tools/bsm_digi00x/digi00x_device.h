// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>
#include <stdint.h>

/*
 * Digi00xDevice — sysctl-based control interface for the basound Digi00x driver.
 *
 * The Digi 002/003 driver exposes per-instance sysctl controls at
 * dev.basound_digi00x.X.*:
 *
 *   clock_source    RW   0=internal, 1=SPDIF, 2=ADAT, 3=Word
 *   optical_mode    RW   0=ADAT, 1=SPDIF
 *   rate            RW   current sample rate (Hz)
 *   external_rate   RO   detected external clock rate (Hz)
 *   external_detect RO   1 = external clock present
 *   tx_peaks        RO   playback output peaks per channel, formatted as
 *                        "N p0 p1 ... pN-1" (24-bit scale, peak-and-clear)
 *
 * Capture (input) level metering reads the decoded multichannel stream
 * from /dev/dspN directly; playback (output) level metering reads the
 * tx_peaks sysctl (the TX data lives in the player's DMA buffer and is
 * not visible to userspace).
 */
class Digi00xDevice {
public:
	Digi00xDevice();
	~Digi00xDevice();

	/* "Open" — scan for the device and optionally open a PCM path for VU.
	 * If pcm_path is nullptr, auto-detect. Returns true if found. */
	bool open(const char *pcm_path = nullptr);
	void close();
	bool is_open() const { return opened_; }

	/* ---- Clock Configuration (via sysctl) ---- */

	/* Clock source: 0=internal, 1=SPDIF, 2=ADAT, 3=Word.
	 * Returns current value, or -1 on error. */
	int  get_clock_source();
	bool set_clock_source(int src);

	/* Optical port mode: 0=ADAT, 1=SPDIF.
	 * Returns current value, or -1 on error. */
	int  get_optical_mode();
	bool set_optical_mode(int mode);

	static const char *clock_source_name(int src);
	static const char *optical_mode_name(int mode);

	/* ---- Status ---- */

	/* Current sample rate in Hz, or 0 on error. */
	int  get_rate();
	bool set_rate(int rate);

	/* Detected external clock rate in Hz, or 0 on error. */
	int  get_external_rate();

	/* External clock present? */
	bool get_external_detect();

	/* ---- Channel geometry ---- */

	/* Full channel complement the device carries on the bus at the
	 * current rate: 18 at 44.1/48 kHz, 10 at 88.2/96 kHz. */
	int  get_device_channels();

	/* Channel count the OSS capture stream can negotiate: 18 at
	 * 44.1/48 kHz, 8 at 88.2/96 kHz (the driver fmtlist has no
	 * 10-channel entry; the 8 analog channels cover the DAW path
	 * at high rates). */
	int  get_capture_channels();

	/* ---- Device info ---- */

	const char *pcm_path() const { return pcm_path_.c_str(); }
	const char *device_name() const { return name_.c_str(); }
	int         pcm_unit() const { return pcm_unit_; }
	int         digi00x_unit() const { return dg00x_unit_; }

	/* ---- Level polling ---- */

	/* Poll capture (input) levels by reading the decoded multichannel
	 * stream from /dev/dspN.  Returns per-channel peaks (24-bit scale,
	 * 0..0x7fffff) in channel order; true if fresh data was read. */
	bool poll_capture_levels(std::vector<uint32_t> &peaks);

	/* Read the driver's playback (output) peak sysctl.  Fills
	 * per-channel 24-bit peaks and the channel count; true on success. */
	bool get_playback_levels(std::vector<uint32_t> &peaks, int &nch);

	/* ---- Device discovery ---- */

	/* Scan dev.pcm.* for Digi 002/003 devices.
	 * Returns a list of PCM paths like "/dev/dspN". */
	static std::vector<std::string> scan();

private:
	bool    opened_;
	int     pcm_unit_;
	int     dg00x_unit_;
	std::string name_;
	std::string pcm_path_;
	int     vu_fd_;  /* cached capture fd for VU polling, or -1 */
	int     vu_nch_; /* channel count the capture fd was opened with */
	int     vu_rate_;/* sample rate the capture fd was opened with */

	/* Helpers */
	std::string sysctl_path(const char *leaf) const;
	static std::string sysctl_str(const char *name);
	static int         sysctl_int(const char *name, int fallback);
	static bool        sysctl_set_int(const char *name, int val);
};
