// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bsm_pf2626 — FreeBSD-native GUI mixer for the M-Audio ProFire 2626.
 *
 * Usage:
 *   bsm_pf2626 [/dev/pf2626N]
 *
 * The optional device path is consumed before Fl_Window::show() so FLTK's
 * argument parser never sees it (Fl::args() would otherwise treat a
 * non-option argument as an unknown option and print its usage text).
 * When no path is given, the first ProFire device found by
 * Pf2626Device::scan() is connected automatically.
 */
#include <FL/Fl.H>
#include "mixer_window.h"

int main(int argc, char **argv) {
	const char *dev = (argc > 1) ? argv[1] : NULL;
	std::string auto_dev;

	Fl::scheme("gtk+");
	Fl::visual(FL_DOUBLE | FL_INDEX);

	MixerWindow win(1100, 720, "bsm_pf2626 — M-Audio ProFire 2626 Mixer");
	win.show();

	if (dev == NULL) {
		auto devs = Pf2626Device::scan();
		if (!devs.empty())
			auto_dev = devs[0];
		dev = auto_dev.empty() ? NULL : auto_dev.c_str();
	}

	if (dev != NULL)
		win.connect(dev);

	return Fl::run();
}
