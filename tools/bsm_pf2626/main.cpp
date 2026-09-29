// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bsm_pf2626 — FreeBSD-native GUI mixer for the M-Audio ProFire 2626.
 *
 * Usage:
 *   bsm_pf2626 [/dev/pf2626N]
 *
 * If no device is given the tool starts with no card selected; use
 * Device > /dev/pf2626N to connect.
 */
#include <FL/Fl.H>
#include "mixer_window.h"

int main(int argc, char **argv) {
	Fl::scheme("gtk+");
	Fl::visual(FL_DOUBLE | FL_INDEX);

	MixerWindow win(1280, 780, "bsm_pf2626 — M-Audio ProFire 2626 Mixer");
	win.show(argc, argv);

	/* Auto-connect if a device path was given on the command line */
	if (argc > 1)
		win.connect(argv[1]);

	return Fl::run();
}
