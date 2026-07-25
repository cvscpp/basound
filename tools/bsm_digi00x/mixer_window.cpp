// SPDX-License-Identifier: GPL-3.0-or-later
#include "mixer_window.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Button.H>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <list>
#include <sys/soundcard.h>

/* Layout constants */
static constexpr int kMenuH    = 25;
static constexpr int kStatH    = 20;
static constexpr int kMargin   = 10;
static constexpr int kSecGap   = 2;
static constexpr double kTimerInterval = 0.04; /* 25 Hz */

/* Clock section */
static constexpr int kClockSecH = 180;
static constexpr int kBtnW     = 150;
static constexpr int kBtnH     = 22;

/* Input level section */
static constexpr int kLevelVUH = 120;
static constexpr int kLevelDbW = 60;

/* ------------------------------------------------------------------ */
/* Menu action helper                                                    */
/* ------------------------------------------------------------------ */
struct MenuAction { MixerWindow *win; std::string arg; };
static std::list<MenuAction> g_menu_actions;

/* ------------------------------------------------------------------ */
/* Construction                                                         */
/* ------------------------------------------------------------------ */

MixerWindow::MixerWindow(int win_w, int win_h, const char *title)
    : Fl_Double_Window(win_w, win_h, title),
      connected_(false),
      menu_(nullptr),
      clock_group_(nullptr),
      rate_label_(nullptr),
      ext_label_(nullptr),
      level_group_(nullptr),
      vu_l_(nullptr), vu_r_(nullptr),
      db_l_(nullptr), db_r_(nullptr),
      status_bar_(nullptr)
{
	for (auto &b : clock_src_btn_) b = nullptr;
	for (auto &b : opt_mode_btn_) b = nullptr;

	color(fl_rgb_color(35, 35, 35));
	begin();

	menu_ = new Fl_Menu_Bar(0, 0, win_w, kMenuH);
	menu_->color(fl_rgb_color(45, 45, 45));
	menu_->textcolor(FL_WHITE);

	/* ---- Clock Configuration section ---- */
	clock_group_ = new Fl_Group(kMargin, kMenuH + kMargin,
	    win_w - 2 * kMargin, kClockSecH,
	    "  Clock Configuration  ");
	clock_group_->box(FL_FLAT_BOX);
	clock_group_->color(fl_rgb_color(40, 40, 42));
	clock_group_->labelcolor(fl_rgb_color(160, 200, 160));
	clock_group_->labelfont(FL_HELVETICA_BOLD);
	clock_group_->labelsize(12);
	clock_group_->align(FL_ALIGN_TOP_LEFT);
	clock_group_->begin();
	{
		int gx = clock_group_->x() + 16;
		int gy = clock_group_->y() + 24;

		/* ---- Clock Source column ---- */
		Fl_Box *clk_lbl = new Fl_Box(gx, gy, 120, 16, "Clock Source:");
		clk_lbl->labelcolor(fl_rgb_color(180, 180, 180));
		clk_lbl->labelsize(11);
		clk_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

		static const char *clk_names[] = {
			"Internal", "S/PDIF", "ADAT", "Word Clock"
		};
		int cy = gy + 20;
		for (int i = 0; i < 4; ++i) {
			clock_src_btn_[i] = new Fl_Round_Button(
			    gx, cy, kBtnW, kBtnH, clk_names[i]);
			clock_src_btn_[i]->type(FL_RADIO_BUTTON);
			clock_src_btn_[i]->color(fl_rgb_color(50, 50, 50));
			clock_src_btn_[i]->selection_color(fl_rgb_color(80, 180, 80));
			clock_src_btn_[i]->labelcolor(FL_WHITE);
			clock_src_btn_[i]->labelsize(11);
			clock_src_btn_[i]->callback(clock_src_cb, this);
			clock_src_btn_[i]->argument(i);
			cy += kBtnH + 2;
		}

		/* ---- Optical Port column ---- */
		int ox = gx + kBtnW + 40;
		Fl_Box *opt_lbl = new Fl_Box(ox, gy, 120, 16, "Optical Port:");
		opt_lbl->labelcolor(fl_rgb_color(180, 180, 180));
		opt_lbl->labelsize(11);
		opt_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

		static const char *opt_names[] = { "ADAT", "S/PDIF" };
		int oy = gy + 20;
		for (int i = 0; i < 2; ++i) {
			opt_mode_btn_[i] = new Fl_Round_Button(
			    ox, oy, kBtnW, kBtnH, opt_names[i]);
			opt_mode_btn_[i]->type(FL_RADIO_BUTTON);
			opt_mode_btn_[i]->color(fl_rgb_color(50, 50, 50));
			opt_mode_btn_[i]->selection_color(fl_rgb_color(80, 180, 80));
			opt_mode_btn_[i]->labelcolor(FL_WHITE);
			opt_mode_btn_[i]->labelsize(11);
			opt_mode_btn_[i]->callback(opt_mode_cb, this);
			opt_mode_btn_[i]->argument(i);
			oy += kBtnH + 2;
		}

		/* ---- Status info row ---- */
		int sy = clock_group_->y() + clock_group_->h() - 48;
		rate_label_ = new Fl_Box(gx, sy, 250, 16, "Rate: --- Hz");
		rate_label_->labelcolor(fl_rgb_color(140, 200, 140));
		rate_label_->labelsize(11);
		rate_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

		ext_label_ = new Fl_Box(gx, sy + 18, 350, 16, "External: ---");
		ext_label_->labelcolor(fl_rgb_color(140, 140, 200));
		ext_label_->labelsize(11);
		ext_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	}
	clock_group_->end();

	/* ---- Input Level section ---- */
	{
		int ly = clock_group_->y() + clock_group_->h() + kSecGap;
		level_group_ = new Fl_Group(kMargin, ly,
		    win_w - 2 * kMargin, kLevelVUH + 40,
		    "  Input Level  ");
		level_group_->box(FL_FLAT_BOX);
		level_group_->color(fl_rgb_color(40, 40, 42));
		level_group_->labelcolor(fl_rgb_color(160, 160, 200));
		level_group_->labelfont(FL_HELVETICA_BOLD);
		level_group_->labelsize(12);
		level_group_->align(FL_ALIGN_TOP_LEFT);
		level_group_->begin();
		{
			int gx = level_group_->x() + 20;
			int gy = level_group_->y() + 28;

			/* Left channel VU */
			Fl_Box *lbl_l = new Fl_Box(gx, gy, 20, kLevelVUH, "L");
			lbl_l->labelcolor(fl_rgb_color(180, 180, 180));
			lbl_l->labelsize(11);
			lbl_l->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

			vu_l_ = new VuMeter(gx + 20, gy, 22, kLevelVUH, "");
			db_l_ = new Fl_Box(gx + 48, gy + kLevelVUH / 2 - 8,
					   kLevelDbW, 16);
			db_l_->labelcolor(fl_rgb_color(140, 140, 140));
			db_l_->labelsize(10);
			db_l_->copy_label("--- dB");

			/* Right channel VU */
			int rx = gx + 130;
			Fl_Box *lbl_r = new Fl_Box(rx, gy, 20, kLevelVUH, "R");
			lbl_r->labelcolor(fl_rgb_color(180, 180, 180));
			lbl_r->labelsize(11);
			lbl_r->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

			vu_r_ = new VuMeter(rx + 20, gy, 22, kLevelVUH, "");
			db_r_ = new Fl_Box(rx + 48, gy + kLevelVUH / 2 - 8,
					   kLevelDbW, 16);
			db_r_->labelcolor(fl_rgb_color(140, 140, 140));
			db_r_->labelsize(10);
			db_r_->copy_label("--- dB");
		}
		level_group_->end();
	}

	/* ---- Status bar ---- */
	status_bar_ = new Fl_Box(0, win_h - kStatH, win_w, kStatH,
	    "Not connected");
	status_bar_->box(FL_FLAT_BOX);
	status_bar_->color(fl_rgb_color(25, 25, 25));
	status_bar_->labelcolor(fl_rgb_color(160, 160, 160));
	status_bar_->labelsize(10);
	status_bar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	end();
	resizable(this);
	size_range(520, 400);

	build_device_menu();
}

MixerWindow::~MixerWindow() {
	Fl::remove_timeout(timer_cb, this);
}

/* ------------------------------------------------------------------ */
/* Menu                                                                  */
/* ------------------------------------------------------------------ */

void MixerWindow::menu_cb(Fl_Widget *, void *data) {
	auto *a = static_cast<MenuAction *>(data);
	if (a->arg == "__quit__")
		exit(0);
	else
		a->win->connect(a->arg.c_str());
}

void MixerWindow::build_device_menu() {
	g_menu_actions.clear();
	menu_->clear();

	/* File menu */
	g_menu_actions.push_back({this, "__quit__"});
	menu_->add("File/Quit", FL_CTRL + 'q', menu_cb, &g_menu_actions.back());

	/* Device menu */
	auto devices = Digi00xDevice::scan();
	if (devices.empty()) {
		menu_->add("Device/(no Digi 00x devices found)", 0, nullptr, nullptr,
		    FL_MENU_INACTIVE);
	} else {
		for (auto &path : devices) {
			g_menu_actions.push_back({this, path});
			menu_->add(("Device/" + path).c_str(), 0,
			    menu_cb, &g_menu_actions.back());
		}
	}
}

/* ------------------------------------------------------------------ */
/* Connect / Disconnect                                                 */
/* ------------------------------------------------------------------ */

void MixerWindow::connect(const char *pcm_path) {
	Fl::remove_timeout(timer_cb, this);
	disconnect();

	if (!dev_.open(pcm_path)) {
		fl_alert("Cannot connect to Digi 002/003 device.\n"
		    "Is the basound kernel module loaded,\n"
		    "firewire stack running, and a Digi 002/003\n"
		    "device attached?");
		return;
	}

	connected_ = true;
	build_ui();
	redraw();

	char buf[128];
	snprintf(buf, sizeof(buf), "Connected to %s — %s",
	    dev_.pcm_path(), dev_.device_name());
	set_status(buf);

	Fl::add_timeout(kTimerInterval, timer_cb, this);
}

void MixerWindow::disconnect() {
	Fl::remove_timeout(timer_cb, this);
	clear_ui();
	dev_.close();
	connected_ = false;
	set_status("Not connected");
}

/* ------------------------------------------------------------------ */
/* Build / clear UI                                                     */
/* ------------------------------------------------------------------ */

void MixerWindow::build_ui() {
	/* Clock source */
	int src = dev_.get_clock_source();
	if (src >= 0 && src < 4)
		clock_src_btn_[src]->setonly();

	/* Optical mode */
	int mode = dev_.get_optical_mode();
	if (mode >= 0 && mode < 2)
		opt_mode_btn_[mode]->setonly();

	/* Rate */
	int rate = dev_.get_rate();
	if (rate > 0) {
		char buf[32];
		snprintf(buf, sizeof(buf), "Rate: %d Hz", rate);
		rate_label_->copy_label(buf);
	} else {
		rate_label_->copy_label("Rate: (unavailable)");
	}

	/* External status */
	bool ext_detect = dev_.get_external_detect();
	int ext_rate = dev_.get_external_rate();
	if (ext_detect && ext_rate > 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "External: %s, %d Hz",
		    Digi00xDevice::clock_source_name(
			    (ext_rate == 44100) ? 0 :
			    (ext_rate == 48000) ? 1 :
			    (ext_rate == 88200) ? 2 : 3),
		    ext_rate);
		ext_label_->copy_label(buf);
	} else if (ext_detect) {
		ext_label_->copy_label("External: detected (rate unknown)");
	} else {
		ext_label_->copy_label("External: not detected");
	}
}

void MixerWindow::clear_ui() {
	for (auto &b : clock_src_btn_) {
		if (b) b->clear();
	}
	for (auto &b : opt_mode_btn_) {
		if (b) b->clear();
	}
	rate_label_->copy_label("Rate: --- Hz");
	ext_label_->copy_label("External: ---");
	vu_l_->set_db(-60.0f);
	vu_r_->set_db(-60.0f);
	db_l_->copy_label("--- dB");
	db_r_->copy_label("--- dB");
}

/* ------------------------------------------------------------------ */
/* 25 Hz timer: poll controls and VU                                    */
/* ------------------------------------------------------------------ */

void MixerWindow::timer_cb(void *userdata) {
	auto *self = static_cast<MixerWindow *>(userdata);
	self->update_meters();
	Fl::repeat_timeout(kTimerInterval, timer_cb, userdata);
}

void MixerWindow::update_meters() {
	if (!connected_) return;

	/* ---- VU: poll capture levels from /dev/dspN ---- */
	uint16_t peak_l = 0, peak_r = 0;
	bool got_levels = dev_.poll_capture_levels(peak_l, peak_r);

	if (got_levels) {
		vu_l_->set_db(VuMeter::peak_i16_to_db(peak_l));
		vu_r_->set_db(VuMeter::peak_i16_to_db(peak_r));

		char buf[16];
		snprintf(buf, sizeof(buf), "%.0f dB",
			 (double)VuMeter::peak_i16_to_db(peak_l));
		db_l_->copy_label(buf);
		snprintf(buf, sizeof(buf), "%.0f dB",
			 (double)VuMeter::peak_i16_to_db(peak_r));
		db_r_->copy_label(buf);
	}

	vu_l_->tick();
	vu_r_->tick();

	/* ---- Poll status info (in case another app changed it) ---- */
	int rate = dev_.get_rate();
	if (rate > 0) {
		char buf[32];
		snprintf(buf, sizeof(buf), "Rate: %d Hz", rate);
		rate_label_->copy_label(buf);
	}

	bool ext_detect = dev_.get_external_detect();
	int ext_rate = dev_.get_external_rate();
	if (ext_detect && ext_rate > 0) {
		char buf[64];
		const char *ext_name =
		    (ext_rate == 44100) ? "44.1 kHz" :
		    (ext_rate == 48000) ? "48 kHz" :
		    (ext_rate == 88200) ? "88.2 kHz" :
		    (ext_rate == 96000) ? "96 kHz" : "unknown";
		snprintf(buf, sizeof(buf), "External: %s", ext_name);
		if (ext_rate > 0)
			snprintf(buf, sizeof(buf), "External: %s, %d Hz",
				 ext_name, ext_rate);
		ext_label_->copy_label(buf);
	} else if (ext_detect) {
		ext_label_->copy_label("External: detected (rate unknown)");
	} else {
		ext_label_->copy_label("External: not detected");
	}

	redraw();
}

/* ------------------------------------------------------------------ */
/* Clock source callback                                                 */
/* ------------------------------------------------------------------ */

void MixerWindow::clock_src_cb(Fl_Widget *w, void *data) {
	intptr_t src = (intptr_t)data;
	static_cast<MixerWindow *>(w->parent()->parent()->user_data())
	    ->on_clock_source_changed((int)src);
}

void MixerWindow::on_clock_source_changed(int src) {
	if (!connected_) return;
	if (dev_.set_clock_source(src)) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Clock source set to %s",
			 Digi00xDevice::clock_source_name(src));
		set_status(buf);
	} else {
		set_status("Failed to set clock source");
	}
}

/* ------------------------------------------------------------------ */
/* Optical mode callback                                                 */
/* ------------------------------------------------------------------ */

void MixerWindow::opt_mode_cb(Fl_Widget *w, void *data) {
	intptr_t mode = (intptr_t)data;
	static_cast<MixerWindow *>(w->parent()->parent()->user_data())
	    ->on_optical_mode_changed((int)mode);
}

void MixerWindow::on_optical_mode_changed(int mode) {
	if (!connected_) return;
	if (dev_.set_optical_mode(mode)) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Optical port set to %s",
			 Digi00xDevice::optical_mode_name(mode));
		set_status(buf);
	} else {
		set_status("Failed to set optical mode");
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

void MixerWindow::set_status(const char *msg) {
	status_bar_->copy_label(msg);
	status_bar_->redraw();
}
