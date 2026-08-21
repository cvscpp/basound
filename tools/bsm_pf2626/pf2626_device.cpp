// SPDX-License-Identifier: GPL-3.0-or-later
#include "pf2626_device.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <cstring>
#include <cmath>
#include <cstdio>

Pf2626Device::Pf2626Device() : fd_(-1) {
	memset(&cfg_, 0, sizeof(cfg_));
}

Pf2626Device::~Pf2626Device() {
	close();
}

bool Pf2626Device::open(const char *path) {
	close();
	fd_ = ::open(path, O_RDONLY);
	if (fd_ < 0)
		return false;
	path_ = path;
	return get_config(cfg_);
}

void Pf2626Device::close() {
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
	path_.clear();
	memset(&cfg_, 0, sizeof(cfg_));
}

bool Pf2626Device::get_config(struct pf2626_config &out) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_GET_CONFIG, &out) == 0;
}

bool Pf2626Device::get_router(struct pf2626_router &out) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_GET_ROUTER, &out) == 0;
}

bool Pf2626Device::set_router(const struct pf2626_router &rt) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_SET_ROUTER,
	    const_cast<struct pf2626_router *>(&rt)) == 0;
}

bool Pf2626Device::get_mixer(struct pf2626_mixer &out) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_GET_MIXER, &out) == 0;
}

bool Pf2626Device::set_mixer(const struct pf2626_mixer &mx) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_SET_MIXER,
	    const_cast<struct pf2626_mixer *>(&mx)) == 0;
}

bool Pf2626Device::get_clock(struct pf2626_clock &out) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_GET_CLOCK, &out) == 0;
}

bool Pf2626Device::set_clock(const struct pf2626_clock &clk) {
	return is_open() && ::ioctl(fd_, PF2626_IOCTL_SET_CLOCK,
	    const_cast<struct pf2626_clock *>(&clk)) == 0;
}

std::vector<std::string> Pf2626Device::scan() {
	std::vector<std::string> paths;
	char path[64];

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/pf2626%d", i);
		int fd = ::open(path, O_RDONLY);
		if (fd >= 0) {
			::close(fd);
			paths.push_back(path);
		}
	}
	return paths;
}

float Pf2626Device::coeff_to_db(uint32_t v) {
	if (v == 0)
		return -120.0f;
	return 20.0f * log10f((float)(v & 0xffff) / 16384.0f);
}

float Pf2626Device::peak_to_db(uint32_t v) {
	if (v == 0)
		return -120.0f;
	/* DICE EAP peaks are 12-bit linear levels, 4095 = full scale. */
	return 20.0f * log10f((float)(v & 0x0fff) / 4095.0f);
}
