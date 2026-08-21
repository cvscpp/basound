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
 *   Router  — destination -> source crossbar
 *   Mixer   — matrix mixer coefficient grid
 *   Clock   — sample rate and clock source
 */
class MixerWindow : public Fl_Double_Window {
public:
	MixerWindow(int w, int h, const char *title = "bsm_pf2626");
	~MixerWindow();

	void connect(const char *path);
	void disconnect();

private:
    // Meter tab
    Fl_Group *tab_meters_;
    Fl_Scroll *meter_scroll_;
    std::vector<VuMeter*> vu_in_;   /* PF2626_MAX_INPUTS meters, built once */
    std::vector<VuMeter*> vu_out_;  /* PF2626_MAX_OUTPUTS meters, built once */
    void build_meter_tab();
    void update_meters();
    static void meter_timer_cb(void *userdata);
    void clear_meters();            /* reset levels to floor, keep widgets */
	Pf2626Device dev_;
	bool connected_;

	Fl_Menu_Bar *menu_;
	Fl_Tabs     *tabs_;

	/* Router tab */
	Fl_Group   *tab_router_;
	Fl_Scroll  *router_scroll_;
	Fl_Choice  *router_mode_;   /* low/mid/high */
	Fl_Button  *router_apply_;
	struct RouterDest {
		std::string label;
		uint8_t     block;
		uint8_t     ch;
		Fl_Choice  *choice;
	};
	std::vector<RouterDest> dests_;
	std::vector<std::string> src_labels_;
	std::vector<std::pair<uint8_t, uint8_t>> src_ids_; /* (block, ch) */
	std::vector<uint32_t> router_entries_;
	unsigned int router_rate_mode_;

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

	/* Clock tab */
	Fl_Group  *tab_clock_;
	Fl_Choice *clock_rate_;
	Fl_Choice *clock_source_;
	Fl_Button *clock_apply_;
	Fl_Box    *clock_status_;

	/* Status bar */
	Fl_Box *status_bar_;

	void build_device_menu();
	void build_router_tab();
	void build_mixer_tab();
	void build_clock_tab();

	static void menu_cb(Fl_Widget *w, void *data);
	static void router_apply_cb(Fl_Widget *w, void *data);
	static void mixer_apply_cb(Fl_Widget *w, void *data);
	static void mixer_cell_cb(Fl_Widget *w, void *data);
	static void clock_apply_cb(Fl_Widget *w, void *data);

	void load_router();
	void load_mixer();
	void load_clock();
	void set_status(const char *msg);
};
