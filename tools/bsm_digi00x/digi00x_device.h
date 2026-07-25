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
 *   rate            RO   current sample rate (Hz)
 *   external_rate   RO   detected external clock rate (Hz)
 *   external_detect RO   1 = external clock present
 *
 * All interaction happens via sysctl(3).  VU metering uses the standard
 * FreeBSD sound(4) /dev/dspN interface.
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

	/* ---- Status (read-only) ---- */

	/* Current sample rate in Hz, or 0 on error. */
	int  get_rate();

	/* Detected external clock rate in Hz, or 0 on error. */
	int  get_external_rate();

	/* External clock present? */
	bool get_external_detect();

	/* ---- Device info ---- */

	const char *pcm_path() const { return pcm_path_.c_str(); }
	const char *device_name() const { return name_.c_str(); }
	int         pcm_unit() const { return pcm_unit_; }
	int         digi00x_unit() const { return dg00x_unit_; }

	/* ---- VU polling via /dev/dspN ---- */

	/* Poll capture levels by reading a short sample block from /dev/dspN.
	 * Returns peak 16-bit sample values for left/right channels. */
	bool poll_capture_levels(uint16_t &peak_l, uint16_t &peak_r);

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
	int     vu_fd_;  /* cached fd for VU polling, or -1 */

	/* Helpers */
	std::string sysctl_path(const char *leaf) const;
	static std::string sysctl_str(const char *name);
	static int         sysctl_int(const char *name, int fallback);
	static bool        sysctl_set_int(const char *name, int val);
};
