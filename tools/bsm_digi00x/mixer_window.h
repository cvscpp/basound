// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Value_Output.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Choice.H>
#include <memory>
#include "digi00x_device.h"
#include "vu_meter.h"

/*
 * MixerWindow — FLTK control panel for Digidesign Digi 002/003.
 *
 * Layout:
 *
 *   [Menu Bar]  File > Quit   Device > /dev/dspN ...
 *
 *   ┌─── "Clock Configuration" ───────────────────────────────────────┐
 *   │   Clock Source:      Optical Port:      Sample Rate:            │
 *   │   ○ Internal          ○ ADAT             [ 44100 Hz ▾ ]         │
 *   │   ○ S/PDIF            ○ S/PDIF                                  │
 *   │   ○ ADAT                                                       │
 *   │   ○ Word Clock                                                 │
 *   │   External: not detected                                        │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 *   ┌─── "Input Levels (18 ch)"  peak: -3.2 dB ────────────┐
 *   │   ▐▐▐▐▐▐▐▐▐▐▐▐▐▐▐▐▐▐  per-channel capture meters     │
 *   │   1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18       │
 *   └───────────────────────────────────────────────────────┘
 *
 *   ┌─── "Output Levels (18 ch)"  peak: -6.0 dB ───────────┐
 *   │   per-channel playback (TX) meters                    │
 *   └───────────────────────────────────────────────────────┘
 *
 *   [Status Bar] Connected to /dev/dsp13 — Digi 003 Rack
 *
 * Input metering decodes the multichannel capture stream via /dev/dspN;
 * output metering reads the driver's tx_peaks sysctl (what is actually
 * being sent to the device per output channel).
 */

#define DG00X_TOOL_MAX_CH 18

class MixerWindow : public Fl_Double_Window {
public:
	MixerWindow(int w, int h, const char *title = "bsm_digi00x");
	~MixerWindow();

	/* Connect to a PCM device path (e.g. "/dev/dsp13").
	 * Pass nullptr for auto-detect. */
	void connect(const char *pcm_path);

	/* Disconnect from current device. */
	void disconnect();

private:
	/* Device */
	Digi00xDevice dev_;
	bool          connected_;

	/* UI widgets */
	Fl_Menu_Bar  *menu_;

	/* Clock Configuration section */
	Fl_Group       *clock_group_;
	Fl_Round_Button *clock_src_btn_[4];
	Fl_Round_Button *opt_mode_btn_[2];
	Fl_Choice      *rate_choice_;
	Fl_Box         *ext_label_;
	bool            polling_;

	/* Level sections */
	Fl_Group  *in_group_;
	Fl_Group  *out_group_;
	Fl_Box    *in_peak_label_;
	Fl_Box    *out_peak_label_;
	VuMeter   *in_meter_[DG00X_TOOL_MAX_CH];
	VuMeter   *out_meter_[DG00X_TOOL_MAX_CH];
	int        in_meter_count_;
	int        out_meter_count_;

	/* Status bar */
	Fl_Box     *status_bar_;

	/* Build/destroy UI */
	void build_ui();
	void clear_ui();

	/* Show/hide meters to match the current channel geometry. */
	void apply_meter_counts(int in_n, int out_n);

	/* Timer callback (25 Hz meter updates + polling) */
	static void timer_cb(void *userdata);
	void        update_meters();

	/* Menu callbacks */
	static void menu_cb(Fl_Widget *w, void *data);
	void        build_device_menu();

	/* Widget callbacks */
	static void clock_src_cb(Fl_Widget *w, void *data);
	void        on_clock_source_changed(int src);
	static void opt_mode_cb(Fl_Widget *w, void *data);
	void        on_optical_mode_changed(int mode);
	static void rate_cb(Fl_Widget *w, void *data);
	void        on_rate_changed();

	/* Status line */
	void set_status(const char *msg);
};
