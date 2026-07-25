// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bsm_digi00x — FreeBSD-native GUI control panel for Digidesign Digi 002/003
 * FireWire audio interfaces via the basound digi00x kernel driver.
 *
 * Controls:
 *   - Clock source selection (Internal / S/PDIF / ADAT / Word Clock)
 *   - Optical port mode (ADAT / S/PDIF)
 *   - Live status (sample rate, external clock presence and rate)
 *   - Input VU meters (L/R)
 *
 * Usage:
 *   bsm_digi00x                    # auto-detect first Digi 00x device
 *   bsm_digi00x /dev/dspN          # connect to specific PCM device
 *
 * Requires the basound kernel module to be loaded with digi00x driver.
 */
#include <FL/Fl.H>
#include "mixer_window.h"
#include "digi00x_device.h"

int main(int argc, char **argv) {
	Fl::scheme("gtk+");
	Fl::visual(FL_DOUBLE | FL_INDEX);

	MixerWindow win(600, 440, "bsm_digi00x — Digi 002/003 Control Panel");
	win.show(argc, argv);

	/* Auto-connect after the window is shown.
	 * Command-line argument takes priority; otherwise try auto-detect. */
	if (argc > 1) {
		win.connect(argv[1]);
	} else {
		auto devices = Digi00xDevice::scan();
		if (devices.size() == 1)
			win.connect(devices[0].c_str());
	}

	return Fl::run();
}
