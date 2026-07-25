// SPDX-License-Identifier: GPL-3.0-or-later
#include "line6_device.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

Line6Device::Line6Device()
    : opened_(false),
      pcm_unit_(-1),
      vu_fd_(-1)
{
}

Line6Device::~Line6Device() {
	close();
}

/* ------------------------------------------------------------------ */
/* Sysctl helpers                                                       */
/* ------------------------------------------------------------------ */

std::string Line6Device::sysctl_str(const char *name) {
	char buf[256] = {};
	size_t len = sizeof(buf) - 1;
	if (sysctlbyname(name, buf, &len, nullptr, 0) != 0)
		return {};
	return std::string(buf, strnlen(buf, len));
}

int Line6Device::sysctl_int(const char *name, int fallback) {
	int val = fallback;
	size_t len = sizeof(val);
	if (sysctlbyname(name, &val, &len, nullptr, 0) != 0)
		return fallback;
	return val;
}

bool Line6Device::sysctl_set_int(const char *name, int val) {
	return sysctlbyname(name, nullptr, nullptr, &val, sizeof(val)) == 0;
}

/* ------------------------------------------------------------------ */
/* Capture source names                                                 */
/* ------------------------------------------------------------------ */

const char *Line6Device::capture_source_name(int src) {
	switch (src) {
	case 0:  return "Microphone";
	case 1:  return "Line";
	case 2:  return "Instrument";
	case 3:  return "Inst + Mic";
	default: return "Unknown";
	}
}

/* ------------------------------------------------------------------ */
/* Device discovery                                                     */
/* ------------------------------------------------------------------ */

std::vector<std::string> Line6Device::scan() {
	std::vector<std::string> result;

	/* Line6 card_id prefixes we look for in dev.pcm.%d.%desc */
	static const char *line6_prefixes[] = {
		"Line6TonePort", "Line6PODStudio", "Line6POD",
		"Line6BassPOD", "Line6Variax",
		nullptr
	};

	for (int unit = 0; unit < 32; ++unit) {
		char mib[64];
		snprintf(mib, sizeof(mib), "dev.pcm.%d.%%desc", unit);
		std::string desc = sysctl_str(mib);
		if (desc.empty())
			continue;

		/* Check description against Line6 identifiers.
		 * Expected format: "Line6TonePortUX1 pcm" */
		bool match = false;
		for (const char **p = line6_prefixes; *p; ++p) {
			if (desc.find(*p) != std::string::npos) {
				match = true;
				break;
			}
		}
		if (!match)
			continue;

		char path[32];
		snprintf(path, sizeof(path), "/dev/dsp%d", unit);
		result.push_back(path);
	}

	return result;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                         */
/* ------------------------------------------------------------------ */

bool Line6Device::open(const char *pcm_path) {
	close();

	/* Verify the Line6 sysctl node exists */
	int vol = sysctl_int("hw.basound.line6.monitor_volume", -1);
	if (vol < 0) {
		/* Maybe driver loaded but no device attached? */
		return false;
	}

	if (pcm_path != nullptr) {
		pcm_path_ = pcm_path;
		/* Parse unit number from path like "/dev/dsp0" */
		if (sscanf(pcm_path, "/dev/dsp%d", &pcm_unit_) != 1)
			pcm_unit_ = -1;
	} else {
		/* Auto-detect: pick the first Line6 PCM device */
		auto devices = scan();
		if (devices.empty()) {
			pcm_path_ = "(none)";
			pcm_unit_ = -1;
		} else {
			pcm_path_ = devices[0];
			sscanf(devices[0].c_str(), "/dev/dsp%d", &pcm_unit_);
		}
	}

	/* Try to read device name from sysctl */
	if (pcm_unit_ >= 0) {
		char mib[64];
		snprintf(mib, sizeof(mib), "dev.pcm.%d.%%desc", pcm_unit_);
		name_ = sysctl_str(mib);
	} else {
		name_ = "Line6 Device";
	}

	opened_ = true;
	return true;
}

void Line6Device::close() {
	if (vu_fd_ >= 0) {
		::close(vu_fd_);
		vu_fd_ = -1;
	}
	opened_ = false;
	pcm_unit_ = -1;
	pcm_path_.clear();
	name_.clear();
}

/* ------------------------------------------------------------------ */
/* Controls                                                             */
/* ------------------------------------------------------------------ */

int Line6Device::get_monitor_volume() {
	if (!opened_) return -1;
	return sysctl_int("hw.basound.line6.monitor_volume", -1);
}

bool Line6Device::set_monitor_volume(int vol) {
	if (!opened_) return false;
	if (vol < 0) vol = 0;
	if (vol > 256) vol = 256;
	return sysctl_set_int("hw.basound.line6.monitor_volume", vol);
}

int Line6Device::get_capture_source() {
	if (!opened_) return -1;
	return sysctl_int("hw.basound.line6.capture_source", -1);
}

bool Line6Device::set_capture_source(int src) {
	if (!opened_) return false;
	if (src < 0 || src > 3) return false;
	return sysctl_set_int("hw.basound.line6.capture_source", src);
}

/* ------------------------------------------------------------------ */
/* VU polling                                                           */
/* ------------------------------------------------------------------ */

bool Line6Device::poll_capture_levels(uint16_t &peak_l, uint16_t &peak_r) {
	peak_l = peak_r = 0;

	if (!opened_ || pcm_unit_ < 0)
		return false;

	/* Try to open the PCM capture device in non-blocking mode.
	 * We cache the fd so we don't open/close on every poll cycle. */
	if (vu_fd_ < 0) {
		vu_fd_ = ::open(pcm_path_.c_str(), O_RDONLY | O_NONBLOCK);
		if (vu_fd_ < 0)
			return false; /* device busy or not available */

		/* Set 16-bit stereo, 44100 Hz via ioctl */
		int fmt = AFMT_S16_LE;
		if (::ioctl(vu_fd_, SNDCTL_DSP_SETFMT, &fmt) < 0) {
			::close(vu_fd_);
			vu_fd_ = -1;
			return false;
		}
		int ch = 2;
		if (::ioctl(vu_fd_, SNDCTL_DSP_CHANNELS, &ch) < 0 || ch != 2) {
			::close(vu_fd_);
			vu_fd_ = -1;
			return false;
		}
		/* Set read buffer size to a tiny chunk (64 frames = 256 bytes) */
		int frag = 0x00060004; /* 2^6 = 64 bytes, 4 fragments */
		::ioctl(vu_fd_, SNDCTL_DSP_SETFRAGMENT, &frag);

		int speed = 44100;
		::ioctl(vu_fd_, SNDCTL_DSP_SPEED, &speed);
	}

	/* Read a chunk of samples */
	int16_t buf[256]; /* 128 stereo frames */
	ssize_t nread = ::read(vu_fd_, buf, sizeof(buf));
	if (nread <= 0)
		return false; /* no data available (or error) */

	int nsamples = (int)(nread / sizeof(int16_t));
	uint16_t max_l = 0, max_r = 0;

	for (int i = 0; i + 1 < nsamples; i += 2) {
		uint16_t abs_l = (uint16_t)(buf[i] < 0 ? -buf[i] : buf[i]);
		uint16_t abs_r = (uint16_t)(buf[i+1] < 0 ? -buf[i+1] : buf[i+1]);
		if (abs_l > max_l) max_l = abs_l;
		if (abs_r > max_r) max_r = abs_r;
	}

	peak_l = max_l;
	peak_r = max_r;
	return true;
}
