// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Hor_Slider.H>
#include <vector>
#include <string>

#include "pf2626_device.h"
#include "vu_meter.h"

/*
 * MixerWindow — top-level FLTK window for bsm_pf2626.
 *
 * Tabs
 * ----
 *   "Meters"  — VU meter strips for all inputs and outputs
 *   "Mixer"   — matrix mixer coefficient grid
 *   "Router"  — destination -> source crossbar
 *   "Clock"   — sample rate and clock source
 *
 * Menu
 * ----
 *   File   > Load Config / Save Config / Quit
 *   Device > (scanned device list)
 */

class MixerWindow : public Fl_Double_Window {
public:
	MixerWindow(int w, int h, const char *title = "bsm_pf2626");
	~MixerWindow();

	void connect(const char *path);
	void disconnect();

private:
	/* Device */
	Pf2626Device dev_;
	bool connected_;

	/* UI widgets */
	Fl_Menu_Bar *menu_;
	Fl_Tabs     *tabs_;

	/* Meters tab (main - opened by default) */
	Fl_Group *tab_meters_;
	Fl_Scroll *meter_scroll_;
	std::vector<VuMeter*> vu_in_;
	std::vector<VuMeter*> vu_out_;
	std::vector<VuMeter*> vu_phones_;
	struct MeterSlider {
		int            channel;
		bool           is_input;
		bool           is_headphone;
		Fl_Slider     *slider;
	};
	std::vector<MeterSlider> meter_sliders_;
	static void meter_slider_cb(Fl_Widget *w, void *data);
	void update_meter_levels();

	/* Mixer tab */
	Fl_Group  *tab_mixer_;
	Fl_Scroll *mixer_scroll_;
	Fl_Button *mixer_apply_;
	Fl_Box    *mixer_status_;
	struct MixerCell {
		int            out;
		int            in;
		Fl_Hor_Slider *slider;
	};
	std::vector<MixerCell> cells_;
	std::vector<uint32_t> mixer_coeff_;

	/* Router tab */
	Fl_Group   *tab_router_;
	Fl_Scroll  *router_scroll_;
	Fl_Choice  *router_mode_;
	Fl_Button  *router_apply_;
	Fl_Button  *router_monitor_;
	struct RouterDest {
		std::string label;
		uint8_t     block;
		uint8_t     ch;
		Fl_Choice  *choice;
	};
	std::vector<RouterDest> dests_;
	std::vector<std::string> src_labels_;
	std::vector<std::pair<uint8_t, uint8_t>> src_ids_;
	std::vector<uint32_t> router_entries_;
	unsigned int router_rate_mode_;

	/* Clock tab */
	Fl_Group  *tab_clock_;
	Fl_Choice *clock_rate_;
	Fl_Choice *clock_source_;
	Fl_Button *clock_apply_;
	Fl_Box    *clock_status_;

	/* Status bar */
	Fl_Box *status_bar_;

	/* Build/destroy UI sections */
	void build_ui();
	void build_meter_tab();
	void build_mixer_tab();
	void build_router_tab();
	void build_clock_tab();
	void build_device_menu();

	void clear_meters();

	/* Meter timer callback (25 Hz) */
	static void meter_timer_cb(void *userdata);
	void update_meters();

	/* Menu callback */
	static void menu_cb(Fl_Widget *w, void *data);
	void load_config();
	void save_config();

	/* Router callbacks */
	static void router_apply_cb(Fl_Widget *w, void *data);
	static void router_monitor_cb(Fl_Widget *w, void *data);
	void load_router();
	void init_default_router();

	/* Mixer callbacks */
	static void mixer_apply_cb(Fl_Widget *w, void *data);
	static void mixer_cell_cb(Fl_Widget *w, void *data);
	void load_mixer();
	void init_default_mixer();

	/* Clock callbacks */
	static void clock_apply_cb(Fl_Widget *w, void *data);
	void load_clock();

	/* Status line */
	void set_status(const char *msg);
};
