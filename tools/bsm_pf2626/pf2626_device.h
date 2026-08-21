// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>
#include <stdint.h>
#include "dice_ioctl.h"

/*
 * Pf2626Device — thin RAII wrapper around /dev/pf2626N.
 *
 * All ioctl calls return true on success and false on error (errno
 * preserved).  The device node is closed automatically on destruction.
 */
class Pf2626Device {
public:
	Pf2626Device();
	~Pf2626Device();

	bool open(const char *path);
	void close();
	bool is_open() const { return fd_ >= 0; }

	bool get_config(struct pf2626_config &out);
	bool get_router(struct pf2626_router &out);
	bool set_router(const struct pf2626_router &rt);
	bool get_mixer(struct pf2626_mixer &out);
	bool set_mixer(const struct pf2626_mixer &mx);
	bool get_clock(struct pf2626_clock &out);
	bool set_clock(const struct pf2626_clock &clk);

	const struct pf2626_config &config() const { return cfg_; }
	const char *path() const { return path_.c_str(); }

	static std::vector<std::string> scan();

	/* Convert a raw mixer coefficient to dBFS. */
	static float coeff_to_db(uint32_t v);

	/* Convert a raw DICE EAP peak-meter value (12-bit, 4095 = full
	 * scale) to dBFS. */
	static float peak_to_db(uint32_t v);

private:
	int fd_;
	std::string path_;
	struct pf2626_config cfg_;
};
