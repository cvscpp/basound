// SPDX-License-Identifier: GPL-3.0-or-later
#include "digi00x_device.h"

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

Digi00xDevice::Digi00xDevice()
    : opened_(false),
      pcm_unit_(-1),
      dg00x_unit_(-1),
      vu_fd_(-1),
      vu_nch_(0),
      vu_rate_(0)
{
}

Digi00xDevice::~Digi00xDevice() {
	close();
}

/* ------------------------------------------------------------------ */
/* Sysctl helpers                                                       */
/* ------------------------------------------------------------------ */

/* Construct a full sysctl path for the digi00x instance, e.g.
 * "dev.basound_digi00x.0.clock_source" */
std::string Digi00xDevice::sysctl_path(const char *leaf) const {
	char buf[64];
	snprintf(buf, sizeof(buf), "dev.basound_digi00x.%d.%s",
		 dg00x_unit_, leaf);
	return std::string(buf);
}

std::string Digi00xDevice::sysctl_str(const char *name) {
	char buf[256] = {};
	size_t len = sizeof(buf) - 1;
	if (sysctlbyname(name, buf, &len, nullptr, 0) != 0)
		return {};
	return std::string(buf, strnlen(buf, len));
}

int Digi00xDevice::sysctl_int(const char *name, int fallback) {
	int val = fallback;
	size_t len = sizeof(val);
	if (sysctlbyname(name, &val, &len, nullptr, 0) != 0)
		return fallback;
	return val;
}

bool Digi00xDevice::sysctl_set_int(const char *name, int val) {
	return sysctlbyname(name, nullptr, nullptr, &val, sizeof(val)) == 0;
}

/* ------------------------------------------------------------------ */
/* Clock / mode name helpers                                            */
/* ------------------------------------------------------------------ */

const char *Digi00xDevice::clock_source_name(int src) {
	switch (src) {
	case 0:  return "Internal";
	case 1:  return "S/PDIF";
	case 2:  return "ADAT";
	case 3:  return "Word Clock";
	default: return "Unknown";
	}
}

const char *Digi00xDevice::optical_mode_name(int mode) {
	switch (mode) {
	case 0:  return "ADAT";
	case 1:  return "S/PDIF";
	default: return "Unknown";
	}
}

/* ------------------------------------------------------------------ */
/* Device discovery                                                     */
/* ------------------------------------------------------------------ */

std::vector<std::string> Digi00xDevice::scan() {
	std::vector<std::string> result;

	/* Look for digi00x PCM devices by scanning dev.pcm.%d.%%desc */
	for (int unit = 0; unit < 32; ++unit) {
		char mib[64];
		snprintf(mib, sizeof(mib), "dev.pcm.%d.%%desc", unit);
		std::string desc = sysctl_str(mib);
		if (desc.empty())
			continue;

		/* Check for Digi 00x identifiers in the description */
		if (desc.find("Digi 00x") == std::string::npos &&
		    desc.find("Digi00x") == std::string::npos &&
		    desc.find("Digidesign") == std::string::npos)
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

bool Digi00xDevice::open(const char *pcm_path) {
	close();

	/* Auto-detect the digi00x sysctl unit.
	 * We scan dev.basound_digi00x.X.rate — if it returns > 0, the
	 * device exists and is responsive. */
	dg00x_unit_ = -1;
	for (int u = 0; u < 16; ++u) {
		char mib[64];
		snprintf(mib, sizeof(mib), "dev.basound_digi00x.%d.rate", u);
		int r = sysctl_int(mib, -1);
		if (r > 0) {
			dg00x_unit_ = u;
			break;
		}
	}

	if (dg00x_unit_ < 0)
		return false;

	if (pcm_path != nullptr) {
		pcm_path_ = pcm_path;
		if (sscanf(pcm_path, "/dev/dsp%d", &pcm_unit_) != 1)
			pcm_unit_ = -1;
	} else {
		/* Auto-detect: pick the first digi00x PCM device */
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
		name_ = "Digi 002/003";
	}

	opened_ = true;
	return true;
}

void Digi00xDevice::close() {
	if (vu_fd_ >= 0) {
		::close(vu_fd_);
		vu_fd_ = -1;
	}
	opened_ = false;
	pcm_unit_ = -1;
	dg00x_unit_ = -1;
	pcm_path_.clear();
	name_.clear();
}

/* ------------------------------------------------------------------ */
/* Controls                                                             */
/* ------------------------------------------------------------------ */

int Digi00xDevice::get_clock_source() {
	if (!opened_ || dg00x_unit_ < 0) return -1;
	return sysctl_int(sysctl_path("clock_source").c_str(), -1);
}

bool Digi00xDevice::set_clock_source(int src) {
	if (!opened_ || dg00x_unit_ < 0) return false;
	if (src < 0 || src > 3) return false;
	return sysctl_set_int(sysctl_path("clock_source").c_str(), src);
}

int Digi00xDevice::get_optical_mode() {
	if (!opened_ || dg00x_unit_ < 0) return -1;
	return sysctl_int(sysctl_path("optical_mode").c_str(), -1);
}

bool Digi00xDevice::set_optical_mode(int mode) {
	if (!opened_ || dg00x_unit_ < 0) return false;
	if (mode < 0 || mode > 1) return false;
	return sysctl_set_int(sysctl_path("optical_mode").c_str(), mode);
}

int Digi00xDevice::get_rate() {
	if (!opened_ || dg00x_unit_ < 0) return 0;
	return sysctl_int(sysctl_path("rate").c_str(), 0);
}

int Digi00xDevice::get_external_rate() {
	if (!opened_ || dg00x_unit_ < 0) return 0;
	return sysctl_int(sysctl_path("external_rate").c_str(), 0);
}

bool Digi00xDevice::get_external_detect() {
	if (!opened_ || dg00x_unit_ < 0) return false;
	int val = sysctl_int(sysctl_path("external_detect").c_str(), -1);
	return val == 1;
}

/* ------------------------------------------------------------------ */
/* Channel geometry                                                     */
/* ------------------------------------------------------------------ */

int Digi00xDevice::get_device_channels() {
	int rate = get_rate();
	return (rate > 48000) ? 10 : 18;
}

int Digi00xDevice::get_capture_channels() {
	int rate = get_rate();
	return (rate > 48000) ? 8 : 18;
}

/* ------------------------------------------------------------------ */
/* VU polling                                                           */
/* ------------------------------------------------------------------ */

bool Digi00xDevice::poll_capture_levels(std::vector<uint32_t> &peaks) {
	peaks.clear();
	if (!opened_ || pcm_unit_ < 0)
		return false;

	int rate = get_rate();
	if (rate <= 0)
		rate = 48000;
	int nch = get_capture_channels();
	int frames_per_block = 512;

	/* Re-open the VU fd when the negotiated geometry changes
	 * (rate change → channel count change). */
	if (vu_fd_ < 0 || vu_nch_ != nch || vu_rate_ != rate) {
		if (vu_fd_ >= 0) {
			::close(vu_fd_);
			vu_fd_ = -1;
		}
		vu_fd_ = ::open(pcm_path_.c_str(), O_RDONLY | O_NONBLOCK);
		if (vu_fd_ < 0)
			return false;

		/* Native S32_LE multichannel — the driver's natural format. */
		int fmt = AFMT_S32_LE;
		if (::ioctl(vu_fd_, SNDCTL_DSP_SETFMT, &fmt) < 0) {
			::close(vu_fd_);
			vu_fd_ = -1;
			return false;
		}
		int ch = nch;
		if (::ioctl(vu_fd_, SNDCTL_DSP_CHANNELS, &ch) < 0 || ch != nch) {
			::close(vu_fd_);
			vu_fd_ = -1;
			return false;
		}
		int frag = (12 << 16) | 0x0006; /* 6 fragments of 4096 bytes */
		::ioctl(vu_fd_, SNDCTL_DSP_SETFRAGMENT, &frag);

		/* Match the device rate; setspeed only runs ALSA prepare,
		 * it never rewrites the device rate register, so this is
		 * safe even while another app (e.g. Audacious) is playing. */
		int speed = rate;
		::ioctl(vu_fd_, SNDCTL_DSP_SPEED, &speed);

		vu_nch_ = nch;
		vu_rate_ = rate;
	}

	std::vector<int32_t> buf(nch * frames_per_block);
	ssize_t nread = ::read(vu_fd_, buf.data(), buf.size() * sizeof(int32_t));
	if (nread <= 0)
		return false;

	int frames = (int)(nread / (nch * (int)sizeof(int32_t)));
	if (frames <= 0)
		return false;

	/* Per-channel peak in 24-bit scale (the driver delivers 24-bit
	 * audio in the top bits of each 32-bit sample). */
	peaks.assign(nch, 0);
	for (int f = 0; f < frames; f++) {
		for (int c = 0; c < nch; c++) {
			int32_t v = buf[f * nch + c] >> 8;
			uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
			if (a > peaks[c])
				peaks[c] = a;
		}
	}
	return true;
}

bool Digi00xDevice::get_playback_levels(std::vector<uint32_t> &peaks, int &nch) {
	peaks.clear();
	nch = 0;
	if (!opened_ || dg00x_unit_ < 0)
		return false;

	std::string s = sysctl_str(sysctl_path("tx_peaks").c_str());
	if (s.empty())
		return false;

	/* Format: "N p0 p1 ... pN-1" */
	char *end = nullptr;
	long n = strtol(s.c_str(), &end, 10);
	if (n <= 0 || end == nullptr || *end == '\0')
		return false;

	/* Parse the peak values; tolerate a truncated string. */
	long max_n = (n > 18) ? 18 : n;
	peaks.reserve(max_n);
	char *p = end;
	for (long i = 0; i < max_n && p != nullptr && *p != '\0'; i++) {
		long v = strtol(p, &end, 10);
		if (end == p)
			break;	/* parse stalled — stop, keep what we have */
		peaks.push_back((uint32_t)v);
		p = end;
	}

	if (peaks.empty())
		return false;
	nch = (int)peaks.size();
	return true;
}

