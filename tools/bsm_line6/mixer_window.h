// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Value_Output.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Round_Button.H>
#include <memory>
#include "line6_device.h"
#include "vu_meter.h"

/*
 * MixerWindow — FLTK control panel for Line6 USB audio devices
 * (TonePort UX1/UX2/GX, POD Studio UX1/UX2).
 *
 * Layout (single tab, single window):
 *
 *   [Menu Bar]  File > Quit   Device > /dev/dsp0 ...
 *
 *   ┌─── "Monitor" ──────────────────────────────┐
 *   │   ┌─────────────────────────┬────┐          │
 *   │   │    (volume slider)      │ VU │          │
 *   │   └─────────────────────────┴────┘          │
 *   │          -12.5 dB         Monitor Mix       │
 *   └─────────────────────────────────────────────┘
 *
 *   ┌─── "Input Source" ─────────────────────────┐
 *   │   ○ Microphone    ○ Line                   │
 *   │   ○ Instrument    ○ Inst + Mic             │
 *   └─────────────────────────────────────────────┘
 *
 *   ┌─── "Input Level" ──────────────────────────┐
 *   │   L: [████████░░░░░░]  -12 dB              │
 *   │   R: [██████████░░░░]   -9 dB              │
 *   └─────────────────────────────────────────────┘
 *
 *   [Status Bar] Connected to /dev/dsp0 — TonePort UX1
 */

class MixerWindow : public Fl_Double_Window {
public:
	MixerWindow(int w, int h, const char *title = "bsm_line6");
	~MixerWindow();

	/* Connect to a PCM device path (e.g. "/dev/dsp0").
	 * Pass nullptr for auto-detect. */
	void connect(const char *pcm_path);

	/* Disconnect from current device. */
	void disconnect();

private:
	/* Device */
	Line6Device dev_;
	bool        connected_;

	/* UI widgets */
	Fl_Menu_Bar  *menu_;

	/* Monitor volume section */
	Fl_Group   *mon_group_;
	Fl_Slider  *mon_fader_;
	Fl_Box     *mon_db_label_;
	VuMeter    *mon_vu_;

	/* Capture source section */
	Fl_Group       *src_group_;
	Fl_Round_Button *src_btn_[4];

	/* Input level section */
	Fl_Group   *level_group_;
	VuMeter    *vu_l_;
	VuMeter    *vu_r_;
	Fl_Box     *db_l_;
	Fl_Box     *db_r_;

	/* Status bar */
	Fl_Box     *status_bar_;

	/* Build/destroy the device-specific UI */
	void build_ui();
	void clear_ui();

	/* Timer callback (25 Hz meter updates + polling) */
	static void timer_cb(void *userdata);
	void        update_meters();

	/* Menu callbacks */
	static void menu_cb(Fl_Widget *w, void *data);
	void        build_device_menu();

	/* Widget callbacks */
	static void mon_fader_cb(Fl_Widget *w, void *data);
	void        on_mon_fader_moved();
	static void src_btn_cb(Fl_Widget *w, void *data);
	void        on_source_changed(int src);

	/* Status line */
	void set_status(const char *msg);
};
