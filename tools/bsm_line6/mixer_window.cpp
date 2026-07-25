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
static constexpr int kSecH     = 150;    /* section group height */
static constexpr int kSecGap   = 2;      /* gap between section groups */
static constexpr double kTimerInterval = 0.04; /* 25 Hz */

/* Monitor section */
static constexpr int kFaderW  = 280;
static constexpr int kMonVUW  = 30;
static constexpr int kMonVUH  = 100;
static constexpr int kDbLabelH = 18;

/* Source selector */
static constexpr int kBtnH   = 22;

/* Input level section */
static constexpr int kLevelVUH  = 120;
static constexpr int kLevelDbW  = 60;

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
      mon_group_(nullptr), mon_fader_(nullptr),
      mon_db_label_(nullptr), mon_vu_(nullptr),
      src_group_(),
      level_group_(nullptr),
      vu_l_(nullptr), vu_r_(nullptr),
      db_l_(nullptr), db_r_(nullptr),
      status_bar_(nullptr)
{
	for (auto &b : src_btn_) b = nullptr;

	color(fl_rgb_color(35, 35, 35));
	begin();

	menu_ = new Fl_Menu_Bar(0, 0, win_w, kMenuH);
	menu_->color(fl_rgb_color(45, 45, 45));
	menu_->textcolor(FL_WHITE);

	/* ---- Monitor Volume section ---- */
	mon_group_ = new Fl_Group(kMargin, kMenuH + kMargin,
	    win_w - 2 * kMargin, kSecH,
	    "  Monitor Mix  ");
	mon_group_->box(FL_FLAT_BOX);
	mon_group_->color(fl_rgb_color(40, 40, 42));
	mon_group_->labelcolor(fl_rgb_color(160, 200, 160));
	mon_group_->labelfont(FL_HELVETICA_BOLD);
	mon_group_->labelsize(12);
	mon_group_->align(FL_ALIGN_TOP_LEFT);
	mon_group_->begin();
	{
		int gx = mon_group_->x() + 8;
		int gy = mon_group_->y() + 22;
		int gh = mon_group_->h() - 30;

		/* Fader */
		mon_fader_ = new Fl_Slider(gx, gy, kFaderW, gh);
		mon_fader_->type(FL_HOR_NICE_SLIDER);
		mon_fader_->bounds(0.0, 1.0);
		mon_fader_->value(0.0);
		mon_fader_->color(fl_rgb_color(55, 55, 60));
		mon_fader_->selection_color(fl_rgb_color(80, 180, 80));
		mon_fader_->callback(mon_fader_cb, this);

		/* Monitor VU meter (right of fader) */
		int vux = gx + kFaderW + 15;
		int vuy = gy + (gh - kMonVUH) / 2;
		mon_vu_ = new VuMeter(vux, vuy, kMonVUW, kMonVUH, "Mix");

		/* dB readout below fader */
		mon_db_label_ = new Fl_Box(gx, gy + gh + 2, kFaderW, kDbLabelH);
		mon_db_label_->labelcolor(fl_rgb_color(150, 150, 150));
		mon_db_label_->labelsize(10);
		mon_db_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		mon_db_label_->copy_label("-inf dB  (monitor mix)");
	}
	mon_group_->end();

	/* ---- Capture Source section ---- */
	{
		int sy = mon_group_->y() + mon_group_->h() + kSecGap;
		src_group_ = new Fl_Group(kMargin, sy,
		    win_w - 2 * kMargin, 120,
		    "  Input Source  ");
		src_group_->box(FL_FLAT_BOX);
		src_group_->color(fl_rgb_color(40, 40, 42));
		src_group_->labelcolor(fl_rgb_color(200, 180, 120));
		src_group_->labelfont(FL_HELVETICA_BOLD);
		src_group_->labelsize(12);
		src_group_->align(FL_ALIGN_TOP_LEFT);
		src_group_->begin();
		{
			static const char *names[] = {
				"Microphone", "Line", "Instrument", "Inst + Mic"
			};
			int gx = src_group_->x() + 20;
			int gy = src_group_->y() + 28;
			int gw = 160;

			for (int i = 0; i < 4; ++i) {
				int row = i / 2;
				int col = i % 2;
				src_btn_[i] = new Fl_Round_Button(
				    gx + col * gw, gy + row * (kBtnH + 4),
				    gw, kBtnH, names[i]);
				src_btn_[i]->type(FL_RADIO_BUTTON);
				src_btn_[i]->color(fl_rgb_color(50, 50, 50));
				src_btn_[i]->selection_color(fl_rgb_color(80, 180, 80));
				src_btn_[i]->labelcolor(FL_WHITE);
				src_btn_[i]->labelsize(11);
				src_btn_[i]->callback(src_btn_cb, this);
				src_btn_[i]->argument(i);
			}
		}
		src_group_->end();
	}

	/* ---- Input Level section ---- */
	{
		int ly = (src_group_ ? src_group_->y() + src_group_->h() : kMenuH + 20) + kSecGap;
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
			db_l_ = new Fl_Box(gx + 48, gy + kLevelVUH / 2 - 8, kLevelDbW, 16);
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
			db_r_ = new Fl_Box(rx + 48, gy + kLevelVUH / 2 - 8, kLevelDbW, 16);
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
	size_range(520, 440);

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
	if (a->arg == "__quit__")    exit(0);
	else                          a->win->connect(a->arg.c_str());
}

void MixerWindow::build_device_menu() {
	g_menu_actions.clear();
	menu_->clear();

	/* File menu */
	g_menu_actions.push_back({this, "__quit__"});
	menu_->add("File/Quit", FL_CTRL + 'q', menu_cb, &g_menu_actions.back());

	/* Device menu */
	auto devices = Line6Device::scan();
	if (devices.empty()) {
		menu_->add("Device/(no Line6 devices found)", 0, nullptr, nullptr,
		    FL_MENU_INACTIVE);
	} else {
		for (auto &path : devices) {
			g_menu_actions.push_back({this, path});
			menu_->add(("Device/" + path).c_str(), 0,
			    menu_cb, &g_menu_actions.back());
		}
	}

	/* Auto-connect is handled in main() after win.show() */
}

/* ------------------------------------------------------------------ */
/* Connect / Disconnect                                                 */
/* ------------------------------------------------------------------ */

void MixerWindow::connect(const char *pcm_path) {
	Fl::remove_timeout(timer_cb, this);
	disconnect();

	if (!dev_.open(pcm_path)) {
		fl_alert("Cannot connect to Line6 device.\n"
		    "Is the basound_line6 kernel module loaded\n"
		    "and a Line6 device attached?");
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
	/* Restore current device state to UI widgets */

	/* Monitor volume */
	int vol = dev_.get_monitor_volume();
	if (vol >= 0) {
		double frac = (double)vol / 256.0;
		mon_fader_->value(frac);
		mon_fader_->activate();

		char buf[32];
		if (vol == 0)
			snprintf(buf, sizeof(buf), "-inf dB  (monitor off)");
		else {
			float db = 20.0f * log10f((float)vol / 256.0f);
			snprintf(buf, sizeof(buf), "%.1f dB  (monitor mix)", db);
		}
		mon_db_label_->copy_label(buf);

		/* VU: show monitor level as fraction */
		mon_vu_->set_db(vol > 0 ? 20.0f * log10f((float)vol / 256.0f) : -60.0f);
	} else {
		mon_fader_->value(0.0);
		mon_fader_->deactivate();
		mon_db_label_->copy_label("(driver not responding)");
	}

	/* Capture source */
	int src = dev_.get_capture_source();
	if (src >= 0 && src < 4) {
		src_btn_[src]->setonly();
	}
}

void MixerWindow::clear_ui() {
	/* Widget memory is owned by the window, so just reset state */
	for (auto &b : src_btn_) {
		if (b) b->clear();
	}
	mon_fader_->value(0.0);
	mon_fader_->deactivate();
	mon_db_label_->copy_label("-inf dB");
	mon_vu_->set_db(-60.0f);
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
		snprintf(buf, sizeof(buf), "%.0f dB", VuMeter::peak_i16_to_db(peak_l));
		db_l_->copy_label(buf);
		snprintf(buf, sizeof(buf), "%.0f dB", VuMeter::peak_i16_to_db(peak_r));
		db_r_->copy_label(buf);
	}

	vu_l_->tick();
	vu_r_->tick();

	/* ---- Poll sysctl monitor volume (in case another app changed it) ---- */
	int vol = dev_.get_monitor_volume();
	if (vol >= 0) {
		double frac = (double)vol / 256.0;
		/* Only update fader if not being dragged by user */
		if (!mon_fader_->active() || !Fl::pushed())
			mon_fader_->value(frac);

		/* Update monitor VU */
		if (vol > 0) {
			float db = 20.0f * log10f((float)vol / 256.0f);
			mon_vu_->set_db(db);
		} else {
			mon_vu_->set_db(-60.0f);
		}
		mon_vu_->tick();
	}

	/* ---- Redraw ---- */
	redraw();
}

/* ------------------------------------------------------------------ */
/* Monitor fader callback                                                */
/* ------------------------------------------------------------------ */

void MixerWindow::mon_fader_cb(Fl_Widget *, void *data) {
	static_cast<MixerWindow *>(data)->on_mon_fader_moved();
}

void MixerWindow::on_mon_fader_moved() {
	if (!connected_) return;

	double frac = mon_fader_->value();
	int vol = (int)(frac * 256.0 + 0.5);
	if (vol < 0) vol = 0;
	if (vol > 256) vol = 256;

	dev_.set_monitor_volume(vol);

	/* Update dB readout */
	char buf[32];
	if (vol == 0)
		snprintf(buf, sizeof(buf), "-inf dB  (monitor off)");
	else {
		float db = 20.0f * log10f((float)vol / 256.0f);
		snprintf(buf, sizeof(buf), "%.1f dB  (monitor mix)", db);
	}
	mon_db_label_->copy_label(buf);
}

/* ------------------------------------------------------------------ */
/* Capture source button callbacks                                      */
/* ------------------------------------------------------------------ */

void MixerWindow::src_btn_cb(Fl_Widget *w, void *data) {
	intptr_t src = (intptr_t)data;
	static_cast<MixerWindow *>(w->parent()->parent()->user_data())
	    ->on_source_changed((int)src);
}

void MixerWindow::on_source_changed(int src) {
	if (!connected_) return;
	dev_.set_capture_source(src);
	set_status("Input source changed");
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

void MixerWindow::set_status(const char *msg) {
	status_bar_->copy_label(msg);
	status_bar_->redraw();
}
