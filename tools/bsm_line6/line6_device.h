// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>
#include <stdint.h>

/*
 * Line6Device — sysctl-based control interface for the basound Line6 driver.
 *
 * The Line6 UX1/TonePort/POD Studio driver exposes two sysctl controls:
 *
 *   hw.basound.line6.monitor_volume  0 = off, 256 = unity gain
 *   hw.basound.line6.capture_source  0 = mic, 1 = line, 2 = instrument,
 *                                    3 = instrument + mic
 *
 * Unlike HdspDevice there is no /dev/line6 char device — all interaction
 * happens via sysctl(3) and the standard FreeBSD sound(4) PCM interface.
 */
class Line6Device {
public:
	Line6Device();
	~Line6Device();

	/* "Open" — verify the Line6 driver is loaded and a device is attached.
	 * Optionally supply a PCM device path (e.g. "/dev/dsp0") for VU polling.
	 * Returns true if at least one Line6 device is present. */
	bool open(const char *pcm_path = nullptr);
	void close();
	bool is_open() const { return opened_; }

	/* ---- Controls (via sysctl) ---- */

	/* Monitor volume: 0 (off) … 256 (unity gain).
	 * Returns the current value, or -1 on error. */
	int  get_monitor_volume();
	bool set_monitor_volume(int vol);

	/* Capture source: 0=Mic  1=Line  2=Instrument  3=Inst+Mic.
	 * Returns the current value, or -1 on error. */
	int  get_capture_source();
	bool set_capture_source(int src);

	static const char *capture_source_name(int src);

	/* ---- Device info ---- */

	const char *pcm_path() const { return pcm_path_.c_str(); }
	const char *device_name() const { return name_.c_str(); }
	int         pcm_unit() const { return pcm_unit_; }

	/* ---- VU polling via /dev/dspN ---- */

	/* Poll capture levels by reading a short sample block from /dev/dspN.
	 * Returns peak 16-bit sample values for left/right channels.
	 * Returns 0 for both channels on error or if device is busy. */
	bool poll_capture_levels(uint16_t &peak_l, uint16_t &peak_r);

	/* ---- Device discovery ---- */

	/* Scan sysctl dev.pcm.* for Line6 audio devices.
	 * Returns a list of PCM paths like "/dev/dsp0". */
	static std::vector<std::string> scan();

private:
	bool    opened_;
	int     pcm_unit_;
	std::string name_;
	std::string pcm_path_;
	int     vu_fd_;  /* cached fd for VU polling, or -1 */

	/* Helpers */
	static std::string sysctl_str(const char *name);
	static int         sysctl_int(const char *name, int fallback);
	static bool        sysctl_set_int(const char *name, int val);
};
