// SPDX-License-Identifier: GPL-3.0-or-later
#include "mixer_window.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_File_Chooser.H>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <list>

/* Router block IDs (must match dice_ioctl.h). */
#define SRC_AES    0
#define SRC_ADAT   1
#define SRC_MIXER  2
#define SRC_INS1   5
#define SRC_ARX0   11
#define SRC_ARX1   12
#define SRC_MUTED  15

#define DST_AES    0
#define DST_ADAT   1
#define DST_INS1   5

#define UNITY 16384

/* Layout constants */
static constexpr int kMenuH    = 25;
static constexpr int kStatH    = 20;
static constexpr int kSliderW  = 45;   /* vertical slider width (3/4 of previous) */
static constexpr int kMeterW   = 11;   /* meter width (1/4 of slider) */
static constexpr double kTimerInterval = 0.04; /* 25 Hz */

static uint8_t make_blk(uint8_t id, uint8_t ch) {
	return (uint8_t)(((id & 0x0f) << 4) | (ch & 0x0f));
}

static uint32_t make_entry(uint8_t src_blk, uint8_t src_ch,
			   uint8_t dst_blk, uint8_t dst_ch) {
	uint32_t src = make_blk(src_blk, src_ch);
	uint32_t dst = make_blk(dst_blk, dst_ch);
	return (src << 8) | dst;
}

/* ------------------------------------------------------------------ */
/* Construction                                                         */
/* ------------------------------------------------------------------ */

MixerWindow::MixerWindow(int win_w, int win_h, const char *title)
    : Fl_Double_Window(win_w, win_h, title),
      connected_(false),
      menu_(nullptr), tabs_(nullptr),
      tab_meters_(nullptr), meter_scroll_(nullptr),
      tab_mixer_(nullptr), mixer_scroll_(nullptr),
      mixer_apply_(nullptr), mixer_status_(nullptr),
      tab_router_(nullptr), router_scroll_(nullptr),
      router_mode_(nullptr), router_apply_(nullptr),
      router_rate_mode_(0),
      tab_clock_(nullptr), clock_rate_(nullptr), clock_source_(nullptr),
      clock_apply_(nullptr), clock_status_(nullptr),
      status_bar_(nullptr)
{
	color(fl_rgb_color(35, 35, 35));
	begin();

	int body_h = win_h - kMenuH - kStatH;

	menu_ = new Fl_Menu_Bar(0, 0, win_w, kMenuH);
	menu_->color(fl_rgb_color(45, 45, 45));
	menu_->textcolor(FL_WHITE);

	tabs_ = new Fl_Tabs(0, kMenuH, win_w, body_h);
	tabs_->color(fl_rgb_color(40, 40, 40));
	tabs_->labelcolor(FL_WHITE);
	tabs_->selection_color(fl_rgb_color(60, 60, 100));

	{
		int tw = win_w, th = body_h - 25, ty = kMenuH + 25;

		/* --- Meters tab (main) --- */
		tab_meters_ = new Fl_Group(0, ty, tw, th, "  Meters  ");
		tab_meters_->color(fl_rgb_color(35, 35, 35));
		tab_meters_->labelcolor(FL_WHITE);
		tab_meters_->begin();
		{
			meter_scroll_ = new Fl_Scroll(0, ty, tw, th);
			meter_scroll_->type(Fl_Scroll::HORIZONTAL);
			meter_scroll_->color(fl_rgb_color(30, 30, 30));
		}
		tab_meters_->resizable(meter_scroll_);
		tab_meters_->end();

		/* --- Mixer tab --- */
		tab_mixer_ = new Fl_Group(0, ty, tw, th, "  Mixer  ");
		tab_mixer_->color(fl_rgb_color(35, 35, 35));
		tab_mixer_->labelcolor(FL_WHITE);
		tab_mixer_->begin();
		{
			mixer_apply_ = new Fl_Button(10, ty + 10, 90, 25, "Apply");
			mixer_apply_->callback(mixer_apply_cb, this);
			mixer_status_ = new Fl_Box(FL_NO_BOX, 110, ty + 10, 400, 25,
			    "Unity = 16384 (0 dB)");
			mixer_status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
			mixer_status_->labelcolor(fl_rgb_color(180, 180, 180));
			mixer_status_->labelsize(10);

			mixer_scroll_ = new Fl_Scroll(10, ty + 45, tw - 20, th - 55);
			mixer_scroll_->type(Fl_Scroll::BOTH);
			mixer_scroll_->color(fl_rgb_color(30, 30, 30));
		}
		tab_mixer_->resizable(mixer_scroll_);
		tab_mixer_->end();

		/* --- Router tab --- */
		tab_router_ = new Fl_Group(0, ty, tw, th, "  Router  ");
		tab_router_->color(fl_rgb_color(35, 35, 35));
		tab_router_->labelcolor(FL_WHITE);
		tab_router_->begin();
		{
			router_mode_ = new Fl_Choice(120, ty + 15, 140, 25, "Rate mode");
			router_mode_->color(fl_rgb_color(45, 45, 45));
			router_mode_->textcolor(FL_WHITE);
			router_mode_->add("Low (<=48k)");
			router_mode_->add("Mid (<=96k)");
			router_mode_->add("High (<=192k)");
			router_mode_->value(0);

			router_apply_ = new Fl_Button(270, ty + 15, 90, 25, "Apply");
			router_apply_->callback(router_apply_cb, this);

		router_monitor_ = new Fl_Button(370, ty + 15, 160, 25, "Enable Monitoring");
		router_monitor_->callback(router_monitor_cb, this);

			router_scroll_ = new Fl_Scroll(10, ty + 50, tw - 20, th - 60);
			router_scroll_->type(Fl_Scroll::VERTICAL);
			router_scroll_->color(fl_rgb_color(30, 30, 30));
		}
		tab_router_->resizable(router_scroll_);
		tab_router_->end();

		/* --- Clock tab --- */
		tab_clock_ = new Fl_Group(0, ty, tw, th, "  Clock  ");
		tab_clock_->color(fl_rgb_color(35, 35, 35));
		tab_clock_->labelcolor(FL_WHITE);
		tab_clock_->begin();
		{
			clock_rate_ = new Fl_Choice(150, ty + 20, 140, 25, "Sample rate");
			clock_rate_->color(fl_rgb_color(45, 45, 45));
			clock_rate_->textcolor(FL_WHITE);
			clock_rate_->add("44100 Hz");
			clock_rate_->add("48000 Hz");
			clock_rate_->add("88200 Hz");
			clock_rate_->add("96000 Hz");
			clock_rate_->add("176400 Hz");
			clock_rate_->add("192000 Hz");
			clock_rate_->value(1);

			clock_source_ = new Fl_Choice(150, ty + 55, 140, 25, "Clock source");
			clock_source_->color(fl_rgb_color(45, 45, 45));
			clock_source_->textcolor(FL_WHITE);
			clock_source_->add("Internal");
			clock_source_->add("ADAT");
			clock_source_->add("SPDIF");
			clock_source_->add("Word Clock");
			clock_source_->add("TDIF");
			clock_source_->value(0);

			clock_apply_ = new Fl_Button(300, ty + 55, 90, 25, "Apply");
			clock_apply_->callback(clock_apply_cb, this);

			clock_status_ = new Fl_Box(FL_NO_BOX, 20, ty + 100, 600, 25, "");
			clock_status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
			clock_status_->labelcolor(fl_rgb_color(180, 180, 180));
			clock_status_->labelsize(10);
		}
		tab_clock_->end();
	}

	tabs_->end();

	status_bar_ = new Fl_Box(0, win_h - kStatH, win_w, kStatH,
	    "Not connected");
	status_bar_->box(FL_FLAT_BOX);
	status_bar_->color(fl_rgb_color(25, 25, 25));
	status_bar_->labelcolor(fl_rgb_color(160, 160, 160));
	status_bar_->labelsize(10);
	status_bar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	end();
	resizable(tabs_);
	size_range(640, 480);

	/* Open on the Meters tab by default */
	tabs_->value(tab_meters_);

	build_device_menu();
}

MixerWindow::~MixerWindow() {
	Fl::remove_timeout(meter_timer_cb, this);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                           */
/* ------------------------------------------------------------------ */

void MixerWindow::set_status(const char *msg) {
	status_bar_->copy_label(msg);
}

/* ------------------------------------------------------------------ */
/* Menu construction                                                    */
/* ------------------------------------------------------------------ */

struct MenuAction { MixerWindow *win; std::string arg; };
static std::list<MenuAction> g_menu_actions;

void MixerWindow::build_device_menu() {
	g_menu_actions.clear();
	menu_->clear();

	/* File menu */
	g_menu_actions.push_back({this, "__load__"});
	menu_->add("File/Load Config", 0, menu_cb, &g_menu_actions.back());
	g_menu_actions.push_back({this, "__save__"});
	menu_->add("File/Save Config", 0, menu_cb, &g_menu_actions.back());
	menu_->add("File/", 0, nullptr, nullptr, FL_MENU_DIVIDER | FL_MENU_INACTIVE);
	g_menu_actions.push_back({this, "__quit__"});
	menu_->add("File/Quit", FL_CTRL + 'q', menu_cb, &g_menu_actions.back());

	/* Device menu */
	auto devices = Pf2626Device::scan();
	if (devices.empty()) {
		menu_->add("Device/(no devices found)", 0, nullptr, nullptr,
		    FL_MENU_INACTIVE);
	} else {
		for (auto &path : devices) {
			g_menu_actions.push_back({this, path});
			menu_->add(("Device/" + path).c_str(), 0,
			    menu_cb, &g_menu_actions.back());
		}
	}
}

void MixerWindow::menu_cb(Fl_Widget *, void *data) {
	auto *a = static_cast<MenuAction *>(data);
	if (a->arg == "__load__")        a->win->load_config();
	else if (a->arg == "__save__")   a->win->save_config();
	else if (a->arg == "__quit__")   exit(0);
	else                             a->win->connect(a->arg.c_str());
}

void MixerWindow::load_config() {
	const char *filename = fl_file_chooser("Load Config", "*.txt", nullptr);
	if (!filename) return;
	fl_message("Load config from %s not yet implemented", filename);
}

void MixerWindow::save_config() {
	const char *filename = fl_file_chooser("Save Config", "*.txt", nullptr, 1);
	if (!filename) return;
	fl_message("Save config to %s not yet implemented", filename);
}

/* ------------------------------------------------------------------ */
/* Device connect / disconnect                                          */
/* ------------------------------------------------------------------ */

void MixerWindow::connect(const char *path) {
	Fl::remove_timeout(meter_timer_cb, this);
	disconnect();

	if (!dev_.open(path)) {
		fl_alert("Cannot open %s", path);
		return;
	}
	connected_ = true;

	build_ui();
	redraw();

	char buf[128];
	snprintf(buf, sizeof(buf), "Connected: %s", path);
	set_status(buf);

	Fl::add_timeout(kTimerInterval, meter_timer_cb, this);
}

void MixerWindow::disconnect() {
	if (connected_) {
		dev_.close();
		connected_ = false;
		Fl::remove_timeout(meter_timer_cb, this);
		clear_meters();
	}
	set_status("Not connected");
}

void MixerWindow::build_ui() {
	/* Clear and rebuild UI tabs */
	build_meter_tab();
	build_mixer_tab();
	build_router_tab();
	
	/* Load device state into tabs (read-only, passive mode) */
	load_router();
	load_mixer();
	load_clock();
}

/* ------------------------------------------------------------------ */
/* Meter tab                                                            */
/* ------------------------------------------------------------------ */

void MixerWindow::clear_meters() {
	for (auto *v : vu_in_)
		v->set_db(-60.0f);
	for (auto *v : vu_out_)
		v->set_db(-60.0f);
	if (meter_scroll_)
		meter_scroll_->redraw();
}

void MixerWindow::update_meters() {
	if (!connected_)
		return;

	struct pf2626_mixer mx;
	memset(&mx, 0, sizeof(mx));
	if (!dev_.get_mixer(mx))
		return;

	for (unsigned int i = 0; i < vu_in_.size(); ++i) {
		if (i < PF2626_MAX_INPUTS)
			vu_in_[i]->set_peak_raw(mx.input_peak[i]);
		vu_in_[i]->tick();
	}
	for (unsigned int i = 0; i < vu_out_.size(); ++i) {
		if (i < PF2626_MAX_OUTPUTS)
			vu_out_[i]->set_peak_raw(mx.output_peak[i]);
		vu_out_[i]->tick();
	}
	/* Update headphone meters (output channels 0-1) */
	for (unsigned int i = 0; i < vu_phones_.size(); ++i) {
		if (i < PF2626_MAX_OUTPUTS)
			vu_phones_[i]->set_peak_raw(mx.output_peak[i]);
		vu_phones_[i]->tick();
	}
}

/* ------------------------------------------------------------------ */
/* Meter tab                                                            */
/* ------------------------------------------------------------------ */

void MixerWindow::build_meter_tab() {
	/* Clear old meters */
	vu_in_.clear();
	vu_out_.clear();
	meter_sliders_.clear();
	meter_scroll_->clear();

	meter_scroll_->begin();

	int mx = 10, my = 10;
	static constexpr int kMeterH   = 200;  /* VU meter height */
	static constexpr int kChanSpacing = 70; /* Total width per channel */

	/* Input meters header */
	Fl_Box *in_hdr = new Fl_Box(FL_NO_BOX, mx, my, 900, 16,
	    "Inputs (capture): 1-8 analog, 9-16 ADAT A, 17-24 ADAT B, 25-26 S/PDIF");
	in_hdr->labelcolor(fl_rgb_color(180, 180, 180));
	in_hdr->labelsize(10);
	in_hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	my += 18;

	/* Input sliders with level meters (slider first, meter second) */
	for (int i = 0; i < PF2626_MAX_INPUTS; ++i) {
		int col_x = mx + i * kChanSpacing;
		
		/* Label for this channel */
		char *label_copy = new char[16];
		snprintf(label_copy, 16, "In %d", i + 1);
		
		/* Vertical level slider with label */
		Fl_Slider *slider = new Fl_Slider(col_x, my, kSliderW, kMeterH);
		slider->type(FL_VERT_NICE_SLIDER);
		slider->range(0, 32768);
		slider->step(1);
		slider->value(16384);  /* Unity gain */
		slider->color(fl_rgb_color(60, 60, 60));
		slider->selection_color(fl_rgb_color(80, 160, 80));
		slider->labelcolor(FL_WHITE);
		slider->copy_label(label_copy);
		slider->callback(meter_slider_cb, this);
		
		/* Meter next to slider without label */
		vu_in_.push_back(new VuMeter(col_x + kSliderW, my, kMeterW, kMeterH));
		
		MeterSlider ms;
		ms.channel = i;
		ms.is_input = true;
		ms.slider = slider;
		meter_sliders_.push_back(ms);
	}
	my += kMeterH + 20;

	/* Output meters header */
	Fl_Box *out_hdr = new Fl_Box(FL_NO_BOX, mx, my, 900, 16,
	    "Outputs (playback): 1-8 analog, 9-16 ADAT A, 17-24 ADAT B, 25-26 S/PDIF");
	out_hdr->labelcolor(fl_rgb_color(180, 180, 180));
	out_hdr->labelsize(10);
	out_hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	my += 18;

	/* Output sliders with level meters (slider first, meter second) */
	for (int i = 0; i < PF2626_MAX_OUTPUTS; ++i) {
		int col_x = mx + i * kChanSpacing;
		
		/* Label for this channel */
		char *label_copy = new char[16];
		snprintf(label_copy, 16, "Out %d", i + 1);
		
		/* Vertical level slider with label */
		Fl_Slider *slider = new Fl_Slider(col_x, my, kSliderW, kMeterH);
		slider->type(FL_VERT_NICE_SLIDER);
		slider->range(0, 32768);
		slider->step(1);
		slider->value(16384);  /* Unity gain */
		slider->color(fl_rgb_color(60, 60, 60));
		slider->selection_color(fl_rgb_color(80, 160, 80));
		slider->labelcolor(FL_WHITE);
		slider->copy_label(label_copy);
		slider->callback(meter_slider_cb, this);
		
		/* Meter next to slider without label */
		vu_out_.push_back(new VuMeter(col_x + kSliderW, my, kMeterW, kMeterH));
		
		MeterSlider ms;
		ms.channel = i;
		ms.is_input = false;
		ms.is_headphone = false;
		ms.slider = slider;
		meter_sliders_.push_back(ms);
	}
	my += kMeterH + 20;

	/* Headphones section (outputs 0-1) */
	vu_phones_.clear();
	Fl_Box *phones_hdr = new Fl_Box(FL_NO_BOX, mx, my, 900, 16, "Headphones");
	phones_hdr->labelcolor(fl_rgb_color(200, 180, 140));
	phones_hdr->labelsize(10);
	phones_hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	my += 18;

	/* Headphone sliders with level meters (outputs 0-1) */
	for (int i = 0; i < 2; ++i) {
		int col_x = mx + i * kChanSpacing;
		
		/* Label for this channel */
		char *label_copy = new char[16];
		snprintf(label_copy, 16, "HPH %d", i + 1);
		
		/* Vertical level slider with label */
		Fl_Slider *slider = new Fl_Slider(col_x, my, kSliderW, kMeterH);
		slider->type(FL_VERT_NICE_SLIDER);
		slider->range(0, 32768);
		slider->step(1);
		slider->value(16384);  /* Unity gain */
		slider->color(fl_rgb_color(60, 60, 60));
		slider->selection_color(fl_rgb_color(200, 160, 80));
		slider->labelcolor(FL_WHITE);
		slider->copy_label(label_copy);
		slider->callback(meter_slider_cb, this);
		
		/* Meter next to slider without label */
		vu_phones_.push_back(new VuMeter(col_x + kSliderW, my, kMeterW, kMeterH));
		
		MeterSlider ms;
		ms.channel = i;
		ms.is_input = false;
		ms.is_headphone = true;
		ms.slider = slider;
		meter_sliders_.push_back(ms);
	}

	meter_scroll_->end();
	meter_scroll_->redraw();
}

void MixerWindow::meter_slider_cb(Fl_Widget *, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	if (!win->connected_)
		return;
	win->update_meter_levels();
}

void MixerWindow::update_meter_levels() {
	/* Currently just read slider values. Could be extended to control
	 * actual device input/output levels if those controls exist. */
	for (auto &ms : meter_sliders_) {
		uint32_t val = (uint32_t)ms.slider->value();
		(void)val;  /* Suppress unused warning for now */
	}
}

void MixerWindow::meter_timer_cb(void *userdata) {
	MixerWindow *win = static_cast<MixerWindow *>(userdata);
	win->update_meters();
	Fl::repeat_timeout(kTimerInterval, meter_timer_cb, userdata);
}

/* ------------------------------------------------------------------ */
/* Mixer tab                                                            */
/* ------------------------------------------------------------------ */

void MixerWindow::build_mixer_tab() {
	mixer_scroll_->clear();
	cells_.clear();
	mixer_coeff_.clear();
}

void MixerWindow::mixer_cell_cb(Fl_Widget *w, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	if (!win->connected_)
		return;
	for (auto &c : win->cells_) {
		if (c.slider == w) {
			uint32_t val = (uint32_t)((Fl_Hor_Slider *)w)->value();
			unsigned int inputs = win->dev_.config().mixer_inputs;
			if (inputs == 0 || inputs > PF2626_MAX_MIXER_IN)
				inputs = PF2626_MAX_MIXER_IN;
			win->mixer_coeff_[c.out * inputs + c.in] = val;
			break;
		}
	}
}

void MixerWindow::load_mixer() {
	if (!connected_)
		return;

	struct pf2626_mixer mx;
	memset(&mx, 0, sizeof(mx));
	if (!dev_.get_mixer(mx)) {
		set_status("Failed to read mixer");
		return;
	}

	unsigned int inputs = mx.inputs;
	unsigned int outputs = mx.outputs;
	if (inputs == 0) inputs = 18;
	if (outputs == 0) outputs = 16;
	if (inputs > PF2626_MAX_MIXER_IN) inputs = PF2626_MAX_MIXER_IN;
	if (outputs > PF2626_MAX_MIXER_OUT) outputs = PF2626_MAX_MIXER_OUT;

	mixer_coeff_.assign(mx.coeff, mx.coeff + PF2626_MIXER_COEFFS);

	mixer_scroll_->clear();
	cells_.clear();

	mixer_scroll_->begin();

	int x_start = 160;  /* space for output labels */
	int y = 10;
	int row_h = 24;
	int col_w = 40;

	/* Title */
	Fl_Box *title = new Fl_Box(FL_NO_BOX, 10, y, 900, 20,
	    "Mixer Matrix: Drag sliders to mix inputs to outputs");
	title->labelcolor(fl_rgb_color(200, 200, 200));
	title->labelsize(11);
	title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	y += 25;

	/* Input source labels (column headers) */
	int col_y = y;
	for (unsigned int i = 0; i < inputs; i++) {
		char buf[16];
		if (i < 8)
			snprintf(buf, sizeof(buf), "An%d", i + 1);  /* Analog */
		else if (i < 16)
			snprintf(buf, sizeof(buf), "AD%d", i - 7);  /* ADAT */
		else
			snprintf(buf, sizeof(buf), "SP%d", i - 15); /* SPDIF */
		
		Fl_Box *col_lbl = new Fl_Box(x_start + (int)i * col_w, col_y, col_w - 2, 18);
		col_lbl->box(FL_NO_BOX);
		col_lbl->copy_label(buf);
		col_lbl->labelcolor(fl_rgb_color(180, 200, 180));
		col_lbl->labelsize(9);
		col_lbl->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
	}
	y += 22;

	/* Mixer rows (outputs) */
	for (unsigned int o = 0; o < outputs; o++) {
		char buf[20];
		if (o < 8)
			snprintf(buf, sizeof(buf), "Out An%d", o + 1);    /* Analog output */
		else if (o < 16)
			snprintf(buf, sizeof(buf), "Out AD%d", o - 7);    /* ADAT output */
		else
			snprintf(buf, sizeof(buf), "Out SP%d", o - 15);   /* SPDIF output */

		/* Output label (row header) */
		Fl_Box *row_lbl = new Fl_Box(10, y, 145, row_h);
		row_lbl->box(FL_NO_BOX);
		row_lbl->copy_label(buf);
		row_lbl->labelcolor(fl_rgb_color(200, 200, 200));
		row_lbl->labelsize(10);
		row_lbl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

		/* Sliders for this output row */
		for (unsigned int i = 0; i < inputs; i++) {
			MixerCell c;
			c.out = (int)o;
			c.in = (int)i;
			c.slider = new Fl_Hor_Slider(x_start + (int)i * col_w, y, col_w - 4, row_h - 4);
			c.slider->type(FL_HOR_NICE_SLIDER);
			c.slider->range(0, 32768);
			c.slider->step(1);
			c.slider->value(mixer_coeff_[o * inputs + i] & 0xffff);
			c.slider->color(fl_rgb_color(60, 60, 60));
			c.slider->selection_color(fl_rgb_color(80, 160, 80));
			c.slider->callback(mixer_cell_cb, this);
			cells_.push_back(c);
		}
		y += row_h + 2;
	}

	mixer_scroll_->end();
	mixer_scroll_->redraw();
}

void MixerWindow::mixer_apply_cb(Fl_Widget *, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	if (!win->connected_)
		return;

	unsigned int inputs = win->dev_.config().mixer_inputs;
	unsigned int outputs = win->dev_.config().mixer_outputs;
	if (inputs == 0 || inputs > PF2626_MAX_MIXER_IN)
		inputs = PF2626_MAX_MIXER_IN;
	if (outputs == 0 || outputs > PF2626_MAX_MIXER_OUT)
		outputs = PF2626_MAX_MIXER_OUT;

	struct pf2626_mixer mx;
	memset(&mx, 0, sizeof(mx));
	mx.inputs = (uint16_t)inputs;
	mx.outputs = (uint16_t)outputs;
	for (unsigned int i = 0; i < inputs * outputs; i++)
		mx.coeff[i] = win->mixer_coeff_[i];

	if (win->dev_.set_mixer(mx))
		win->set_status("Mixer programmed");
	else
		win->set_status("Mixer write failed");
}

void MixerWindow::init_default_mixer() {
	if (!connected_)
		return;

	struct pf2626_mixer mx;
	memset(&mx, 0, sizeof(mx));
	if (!dev_.get_mixer(mx)) {
		set_status("Failed to read mixer config");
		return;
	}

	unsigned int inputs = mx.inputs;
	unsigned int outputs = mx.outputs;
	if (inputs == 0) inputs = 18;
	if (outputs == 0) outputs = 16;
	if (inputs > PF2626_MAX_MIXER_IN) inputs = PF2626_MAX_MIXER_IN;
	if (outputs > PF2626_MAX_MIXER_OUT) outputs = PF2626_MAX_MIXER_OUT;

	/* Set diagonal matrix to unity gain (16384 = 0 dB).
	 * This creates a 1:1 pass-through for each channel that's configured.
	 * The actual routing is handled by the router, but the mixer matrix
	 * coefficients control the level (amplitude) of each signal path. */
	mx.inputs = (uint16_t)inputs;
	mx.outputs = (uint16_t)outputs;
	for (unsigned int i = 0; i < inputs * outputs; i++) {
		if ((i % (inputs + 1)) == 0)
			mx.coeff[i] = UNITY;  /* Diagonal: unity gain */
		else
			mx.coeff[i] = 0;      /* Off-diagonal: muted */
	}

	if (dev_.set_mixer(mx)) {
		set_status("Mixer matrix initialized to unity gain");
	} else {
		set_status("Failed to initialize mixer matrix");
	}
}/* ------------------------------------------------------------------ */
/* Router tab                                                           */
/* ------------------------------------------------------------------ */

static void add_src(std::vector<std::string> &labels,
		    std::vector<std::pair<uint8_t,uint8_t>> &ids,
		    uint8_t blk, uint8_t ch, const char *name) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s %d", name, ch + 1);
	labels.push_back(buf);
	ids.push_back(std::make_pair(blk, ch));
}

void MixerWindow::build_router_tab() {
	/* Sources selectable for any destination. */
	src_labels_.clear();
	src_ids_.clear();
	add_src(src_labels_, src_ids_, SRC_MUTED, 0, "Mute");
	for (int i = 0; i < 8; i++)
		add_src(src_labels_, src_ids_, SRC_INS1, i, "Analog In");
	for (int i = 0; i < 8; i++)
		add_src(src_labels_, src_ids_, SRC_ADAT, i, "ADAT A In");
	for (int i = 0; i < 8; i++)
		add_src(src_labels_, src_ids_, SRC_ADAT, i + 8, "ADAT B In");
	for (int i = 0; i < 2; i++)
		add_src(src_labels_, src_ids_, SRC_AES, i, "SPDIF In");
	for (int i = 0; i < 16; i++)
		add_src(src_labels_, src_ids_, SRC_MIXER, i, "Mixer Out");
	for (int i = 0; i < 16; i++)
		add_src(src_labels_, src_ids_, SRC_ARX0, i, "1394 Play A");
	for (int i = 0; i < 16; i++)
		add_src(src_labels_, src_ids_, SRC_ARX1, i, "1394 Play B");

	/* Destinations the tool exposes. */
	dests_.clear();
	auto add_dst = [&](uint8_t blk, uint8_t ch, const char *name) {
		RouterDest d;
		char buf[64];
		snprintf(buf, sizeof(buf), "%s %d", name, ch + 1);
		d.label = buf;
		d.block = blk;
		d.ch = ch;
		d.choice = nullptr;
		dests_.push_back(d);
	};

	for (int i = 0; i < 8; i++)
		add_dst(DST_INS1, i, "Analog Out");
	for (int i = 0; i < 8; i++)
		add_dst(DST_ADAT, i, "ADAT A Out");
	for (int i = 0; i < 8; i++)
		add_dst(DST_ADAT, i + 8, "ADAT B Out");
	for (int i = 0; i < 2; i++)
		add_dst(DST_AES, i, "SPDIF Out");

	router_scroll_->clear();
	router_scroll_->begin();
	int y = 0;
	for (auto &d : dests_) {
		Fl_Box *label = new Fl_Box(FL_NO_BOX, 20, y, 180, 25, d.label.c_str());
		label->labelcolor(fl_rgb_color(200, 200, 200));
		label->labelsize(11);
		label->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
		d.choice = new Fl_Choice(210, y, 260, 25);
		d.choice->color(fl_rgb_color(45, 45, 45));
		d.choice->textcolor(FL_WHITE);
		for (const auto &s : src_labels_)
			d.choice->add(s.c_str());
		d.choice->value(0);
		y += 30;
	}
	router_scroll_->end();
}

void MixerWindow::load_router() {
	if (!connected_)
		return;

	struct pf2626_router rt;
	memset(&rt, 0, sizeof(rt));
	rt.rate_mode = (uint16_t)router_rate_mode_;
	if (!dev_.get_router(rt)) {
		set_status("Failed to read router");
		return;
	}

	router_entries_.assign(rt.entries, rt.entries + rt.count);

	for (auto &d : dests_) {
		uint8_t dst = make_blk(d.block, d.ch);
		int found = -1;
		for (unsigned int i = 0; i < router_entries_.size(); i++) {
			uint32_t e = router_entries_[i];
			if ((e & 0xff) == dst) {
				uint8_t src = (uint8_t)((e >> 8) & 0xff);
				uint8_t sblk = (uint8_t)(src >> 4);
				uint8_t sch = (uint8_t)(src & 0x0f);
				for (unsigned int s = 0; s < src_ids_.size(); s++) {
					if (src_ids_[s].first == sblk &&
					    src_ids_[s].second == sch) {
						found = (int)s;
						break;
					}
				}
				break;
			}
		}
		d.choice->value(found >= 0 ? found : 0);
	}
	router_mode_->value((int)router_rate_mode_);
}

void MixerWindow::init_default_router() {
	if (!connected_)
		return;

	struct pf2626_router rt;
	memset(&rt, 0, sizeof(rt));
	rt.rate_mode = 0;
	
	std::vector<uint32_t> entries;
	
	/* Route analog inputs to analog outputs (1:1 bypass) */
	for (int i = 0; i < 8; i++) {
		entries.push_back(make_entry(SRC_INS1, i, DST_INS1, i));
	}
	
	/* Route all analog inputs to both headphone channels (stereo duplicate mix) */
	for (int i = 0; i < 8; i++) {
		entries.push_back(make_entry(SRC_INS1, i, DST_INS1, 0));  // All inputs → HPH 1
		entries.push_back(make_entry(SRC_INS1, i, DST_INS1, 1));  // All inputs → HPH 2
	}
	
	/* Route ADAT A inputs to ADAT A outputs (1:1 bypass) */
	for (int i = 0; i < 8; i++) {
		entries.push_back(make_entry(SRC_ADAT, i, DST_ADAT, i));
	}
	
	/* Route ADAT B inputs to ADAT B outputs (1:1 bypass) */
	for (int i = 0; i < 8; i++) {
		entries.push_back(make_entry(SRC_ADAT, i + 8, DST_ADAT, i + 8));
	}
	
	/* Route SPDIF inputs to SPDIF outputs (1:1 bypass) */
	for (int i = 0; i < 2; i++) {
		entries.push_back(make_entry(SRC_AES, i, DST_AES, i));
	}
	
	rt.count = (uint16_t)entries.size();
	for (unsigned int i = 0; i < entries.size() && i < PF2626_MAX_ROUTES; i++) {
		rt.entries[i] = entries[i];
	}
	
	if (dev_.set_router(rt)) {
		router_entries_ = entries;
		set_status("Default routing configured");
	} else {
		set_status("Failed to configure default routing");
	}
}

void MixerWindow::router_apply_cb(Fl_Widget *, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	if (!win->connected_)
		return;

	unsigned int mode = (unsigned int)win->router_mode_->value();
	win->router_rate_mode_ = mode;

	/* Remove any existing entries for the exposed destinations, then
	 * append the currently selected source for each. */
	std::vector<uint32_t> entries = win->router_entries_;
	for (auto &d : win->dests_) {
		uint8_t dst = make_blk(d.block, d.ch);
		entries.erase(std::remove_if(entries.begin(), entries.end(),
		    [&](uint32_t e){ return (e & 0xff) == dst; }),
		    entries.end());

		int sel = d.choice->value();
		if (sel > 0 && (unsigned)sel < win->src_ids_.size()) {
			uint8_t sblk = win->src_ids_[sel].first;
			uint8_t sch  = win->src_ids_[sel].second;
			entries.push_back(make_entry(sblk, sch, d.block, d.ch));
		}
	}

	struct pf2626_router rt;
	memset(&rt, 0, sizeof(rt));
	rt.rate_mode = (uint16_t)mode;
	rt.count = (uint16_t)entries.size();
	for (unsigned int i = 0; i < entries.size() && i < PF2626_MAX_ROUTES; i++)
		rt.entries[i] = entries[i];

	if (win->dev_.set_router(rt)) {
		win->router_entries_ = entries;
		win->set_status("Router programmed");
	} else {
		win->set_status("Router write failed");
	}
}

/* ------------------------------------------------------------------ */
/* Clock tab                                                            */
/* ------------------------------------------------------------------ */

static const uint8_t kClockRates[] = { 1, 2, 3, 4, 5, 6 }; /* CLOCK_RATE >> 8 */
static const uint8_t kClockSources[] = { 0x0c, 0x05, 0x00, 0x07, 0x06 };

void MixerWindow::load_clock() {
	if (!connected_)
		return;

	struct pf2626_clock clk;
	memset(&clk, 0, sizeof(clk));
	if (!dev_.get_clock(clk)) {
		set_status("Failed to read clock");
		return;
	}

	int rate = 1;
	for (int i = 0; i < 6; i++) {
		if (kClockRates[i] == clk.rate) { rate = i; break; }
	}
	clock_rate_->value(rate);

	int src = 0;
	for (int i = 0; i < 5; i++) {
		if (kClockSources[i] == clk.source) { src = i; break; }
	}
	clock_source_->value(src);

	char buf[128];
	snprintf(buf, sizeof(buf), "select=0x%08x status=0x%08x",
	    clk.select, clk.status);
	clock_status_->copy_label(buf);
}

void MixerWindow::clock_apply_cb(Fl_Widget *, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	if (!win->connected_)
		return;

	struct pf2626_clock clk;
	memset(&clk, 0, sizeof(clk));
	clk.rate = kClockRates[win->clock_rate_->value()];
	clk.source = kClockSources[win->clock_source_->value()];

	if (win->dev_.set_clock(clk))
		win->set_status("Clock programmed");
	else
		win->set_status("Clock write failed");
}

void MixerWindow::router_monitor_cb(Fl_Widget *, void *data) {
MixerWindow *win = (MixerWindow *)data;
if (!win->connected_)
return;

struct pf2626_router rt;
memset(&rt, 0, sizeof(rt));
rt.rate_mode = (uint16_t)win->router_rate_mode_;

/* Start with Jack's existing routes */
std::vector<uint32_t> entries = win->router_entries_;

/* Remove any routes to headphone outputs (DST_INS1 ch 0-1) */
std::vector<uint32_t> filtered;
for (const auto &e : entries) {
uint8_t dst = (uint8_t)(e & 0xff);
uint8_t dch = dst & 0x0f;
uint8_t dblk = dst >> 4;
/* Keep routes to non-headphone destinations */
if (!(dblk == DST_INS1 && dch < 2))
filtered.push_back(e);
}
entries = filtered;

/* Add monitoring: all analog inputs to both headphones */
for (int i = 0; i < 8; i++) {
entries.push_back(make_entry(SRC_INS1, i, DST_INS1, 0));
entries.push_back(make_entry(SRC_INS1, i, DST_INS1, 1));
}

rt.count = (uint16_t)entries.size();
for (unsigned int i = 0; i < entries.size() && i < PF2626_MAX_ROUTES; i++)
rt.entries[i] = entries[i];

if (win->dev_.set_router(rt)) {
win->router_entries_ = entries;
win->load_router();
win->set_status("Monitoring enabled");
} else {
win->set_status("Failed to enable monitoring");
}
}
