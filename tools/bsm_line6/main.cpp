// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bsm_line6 — FreeBSD-native GUI mixer for the Line6 USB audio devices
 * (TonePort UX1/UX2/GX, POD Studio UX1/UX2).
 *
 * Controls:
 *   - Monitor mix volume (software monitoring via kernel driver)
 *   - Input source selection (Mic / Line / Instrument / Inst+Mic)
 *   - Live input VU meters (L/R)
 *
 * Usage:
 *   bsm_line6                    # auto-detect first Line6 device
 *   bsm_line6 /dev/dspN          # connect to specific PCM device
 *
 * Requires the basound_line6 kernel module to be loaded.
 */
#include <FL/Fl.H>
#include "mixer_window.h"
#include "line6_device.h"

int main(int argc, char **argv) {
	Fl::scheme("gtk+");
	Fl::visual(FL_DOUBLE | FL_INDEX);

	MixerWindow win(600, 480, "bsm_line6 — Line6 Audio Mixer");
	win.show(argc, argv);

	/* Auto-connect after the window is shown.
	 * Command-line argument takes priority; otherwise try auto-detect. */
	if (argc > 1) {
		win.connect(argv[1]);
	} else {
		auto devices = Line6Device::scan();
		if (devices.size() == 1)
			win.connect(devices[0].c_str());
	}

	return Fl::run();
}
