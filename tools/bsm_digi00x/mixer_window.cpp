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
#include <vector>
#include <sys/soundcard.h>

/* Layout constants */
static constexpr int kMenuH    = 25;
static constexpr int kStatH    = 20;
static constexpr int kMargin   = 10;
static constexpr int kSecGap   = 2;
static constexpr double kTimerInterval = 0.04; /* 25 Hz */

/* Clock section */
static constexpr int kClockSecH = 150;
static constexpr int kBtnW     = 150;
static constexpr int kBtnH     = 22;

/* Level sections */
static constexpr int kLevelSecH = 175;
static constexpr int kLevelVUH  = 140;
static constexpr int kMeterW    = 22;
static constexpr int kMeterGap  = 26;

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
      in_group_(nullptr),
      out_group_(nullptr),
      in_peak_label_(nullptr),
      out_peak_label_(nullptr),
      in_meter_count_(0),
      out_meter_count_(0),
      status_bar_(nullptr)
{
	for (auto &b : clock_src_btn_) b = nullptr;
	for (auto &b : opt_mode_btn_) b = nullptr;
	for (auto &m : in_meter_)  m = nullptr;
	for (auto &m : out_meter_) m = nullptr;

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
		int gy = clock_group_->y() + 22;

		/* ---- Clock Source column ---- */
		Fl_Box *clk_lbl = new Fl_Box(gx, gy, 120, 16, "Clock Source:");
		clk_lbl->labelcolor(fl_rgb_color(180, 180, 180));
		clk_lbl->labelsize(11);
		clk_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

		static const char *clk_names[] = {
			"Internal", "S/PDIF", "ADAT", "Word Clock"
		};
		int cy = gy + 18;
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
		int oy = gy + 18;
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
		int sy = clock_group_->y() + clock_group_->h() - 42;
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

	/* ---- Level sections ---- */
	auto build_level_section = [&](int y, const char *title, Fl_Group *&group,
	    Fl_Box *&peak_label, VuMeter **meters) {
		group = new Fl_Group(kMargin, y, win_w - 2 * kMargin, kLevelSecH, title);
		group->box(FL_FLAT_BOX);
		group->color(fl_rgb_color(40, 40, 42));
		group->labelcolor(fl_rgb_color(160, 160, 200));
		group->labelfont(FL_HELVETICA_BOLD);
		group->labelsize(12);
		group->align(FL_ALIGN_TOP_LEFT);
		group->begin();
		{
			int gx = group->x() + 40;
			int gy = group->y() + 30;

			peak_label = new Fl_Box(group->x() + group->w() - 220, gy - 24,
			    200, 16, "peak: ---");
			peak_label->labelcolor(fl_rgb_color(200, 160, 160));
			peak_label->labelsize(10);
			peak_label->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

			for (int i = 0; i < DG00X_TOOL_MAX_CH; ++i) {
				char lbl[8];
				snprintf(lbl, sizeof(lbl), "%d", i + 1);
				meters[i] = new VuMeter(gx + i * kMeterGap, gy,
				    kMeterW, kLevelVUH, lbl);
			}
		}
		group->end();
	};

	build_level_section(clock_group_->y() + clock_group_->h() + kSecGap,
	    "  Input Levels (capture)  ", in_group_, in_peak_label_, in_meter_);
	build_level_section(in_group_->y() + in_group_->h() + kSecGap,
	    "  Output Levels (playback)  ", out_group_, out_peak_label_, out_meter_);

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
	size_range(760, 540);

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

	/* Meter geometry */
	apply_meter_counts(dev_.get_capture_channels(),
	    dev_.get_device_channels());
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

	for (int i = 0; i < DG00X_TOOL_MAX_CH; i++) {
		if (in_meter_[i])  in_meter_[i]->set_db(-60.0f);
		if (out_meter_[i]) out_meter_[i]->set_db(-60.0f);
	}
	in_peak_label_->copy_label("peak: ---");
	out_peak_label_->copy_label("peak: ---");
	in_meter_count_ = 0;
	out_meter_count_ = 0;
}

/* ------------------------------------------------------------------ */
/* Channel geometry                                                     */
/* ------------------------------------------------------------------ */

void MixerWindow::apply_meter_counts(int in_n, int out_n) {
	if (in_n < 0)  in_n = 0;
	if (out_n < 0) out_n = 0;
	if (in_n > DG00X_TOOL_MAX_CH)  in_n = DG00X_TOOL_MAX_CH;
	if (out_n > DG00X_TOOL_MAX_CH) out_n = DG00X_TOOL_MAX_CH;

	in_meter_count_ = in_n;
	out_meter_count_ = out_n;

	char buf[64];
	snprintf(buf, sizeof(buf), "  Input Levels (%d ch)  ", in_n);
	in_group_->label(buf);
	snprintf(buf, sizeof(buf), "  Output Levels (%d ch)  ", out_n);
	out_group_->label(buf);

	for (int i = 0; i < DG00X_TOOL_MAX_CH; i++) {
		if (in_meter_[i]) {
			if (i < in_n)  in_meter_[i]->show();
			else           in_meter_[i]->hide();
		}
		if (out_meter_[i]) {
			if (i < out_n) out_meter_[i]->show();
			else           out_meter_[i]->hide();
		}
	}
}

/* ------------------------------------------------------------------ */
/* 25 Hz timer: poll controls and VU                                    */
/* ------------------------------------------------------------------ */

void MixerWindow::timer_cb(void *userdata) {
	auto *self = static_cast<MixerWindow *>(userdata);
	self->update_meters();
	Fl::repeat_timeout(kTimerInterval, timer_cb, userdata);
}

static float
peak_to_db(uint32_t peak)
{
	return VuMeter::peak_to_db(peak, 0x7fffff);
}

void MixerWindow::update_meters() {
	if (!connected_) return;

	/* ---- Track rate and channel geometry ---- */
	int rate = dev_.get_rate();
	int in_n = dev_.get_capture_channels();
	int out_n = dev_.get_device_channels();
	if (in_n != in_meter_count_ || out_n != out_meter_count_)
		apply_meter_counts(in_n, out_n);

	/* ---- Input (capture) levels: decode from /dev/dspN ---- */
	std::vector<uint32_t> cap_peaks;
	bool got_cap = dev_.poll_capture_levels(cap_peaks);
	float in_peak = -144.0f;
	for (int i = 0; i < in_meter_count_; i++) {
		float db = -60.0f;
		if (got_cap && i < (int)cap_peaks.size()) {
			db = peak_to_db(cap_peaks[i]);
			if (db > in_peak)
				in_peak = db;
		}
		in_meter_[i]->set_db(db);
		in_meter_[i]->tick();
	}
	if (in_peak > -143.0f) {
		char buf[32];
		snprintf(buf, sizeof(buf), "peak: %.1f dB", (double)in_peak);
		in_peak_label_->copy_label(buf);
	}

	/* ---- Output (playback) levels: tx_peaks sysctl ---- */
	std::vector<uint32_t> pb_peaks;
	int pb_nch = 0;
	bool got_pb = dev_.get_playback_levels(pb_peaks, pb_nch);
	float out_peak = -144.0f;
	for (int i = 0; i < out_meter_count_; i++) {
		float db = -60.0f;
		if (got_pb && i < (int)pb_peaks.size()) {
			db = peak_to_db(pb_peaks[i]);
			if (db > out_peak)
				out_peak = db;
		}
		out_meter_[i]->set_db(db);
		out_meter_[i]->tick();
	}
	if (out_peak > -143.0f) {
		char buf[32];
		snprintf(buf, sizeof(buf), "peak: %.1f dB", (double)out_peak);
		out_peak_label_->copy_label(buf);
	}

	/* ---- Poll status info (in case another app changed it) ---- */
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
