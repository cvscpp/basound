// SPDX-License-Identifier: GPL-3.0-or-later
#include "mixer_window.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_File_Chooser.H>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

/* Layout constants */
static constexpr int kMenuH    = 25;
static constexpr int kStatH    = 20;
static constexpr int kFaderW   = 120;  /* matrix tab right panel width */
static constexpr int kMeterW   = 40;   /* per-channel VU width (Meters tab) */
static constexpr int kMeterH   = 200;  /* VU meter height (Meters tab) */
static constexpr double kTimerInterval = 0.04; /* 25 Hz */

/* Mixer tab channel strip constants */
static constexpr int kStripW  = 60;   /* channel strip total width */
static constexpr int kSLabelH = 14;   /* channel number label height */
static constexpr int kSVuW    = 22;   /* VU meter width (left side of slot) */
static constexpr int kSSlotH  = 100;  /* VU + fader slot height (same for both) */
static constexpr int kSDbH    = 14;   /* dB readout height below slot */
static constexpr int kSHdrH   = 22;   /* section header height */
static constexpr int kSGapH   = 30;   /* vertical gap between sections */

/* ------------------------------------------------------------------ */
/* Construction                                                         */
/* ------------------------------------------------------------------ */

MixerWindow::MixerWindow(int win_w, int win_h, const char *title)
    : Fl_Double_Window(win_w, win_h, title),
      connected_(false),
      menu_(nullptr), tabs_(nullptr),
      tab_mixer_(nullptr), mix_scroll_(nullptr),
      tab_matrix_(nullptr), matrix_(nullptr),
      cell_fader_(nullptr), cell_label_(nullptr), cell_db_(nullptr),
      tab_meters_(nullptr), meter_scroll_(nullptr),
      status_bar_(nullptr),
      selected_addr_(0)
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

		/* --- Mixer tab (main) --- */
		tab_mixer_ = new Fl_Group(0, kMenuH, win_w, body_h, "  Mixer  ");
		tab_mixer_->color(fl_rgb_color(35, 35, 35));
		tab_mixer_->labelcolor(FL_WHITE);
		tab_mixer_->begin();
		{
			mix_scroll_ = new Fl_Scroll(0, ty, tw, th);
			mix_scroll_->type(Fl_Scroll::BOTH);
			mix_scroll_->color(fl_rgb_color(30, 30, 30));
		}
		tab_mixer_->resizable(mix_scroll_);
		tab_mixer_->end();

		/* --- Matrix tab --- */
		tab_matrix_ = new Fl_Group(0, kMenuH, win_w, body_h, "  Matrix  ");
		tab_matrix_->color(fl_rgb_color(35, 35, 35));
		tab_matrix_->labelcolor(FL_WHITE);
		tab_matrix_->begin();
		{
			int mx_w = tw - kFaderW;
			matrix_ = new MatrixWidget(0, ty, mx_w, th);
			matrix_->on_select([this](uint32_t addr, uint16_t gain){
				on_cell_selected(addr, gain);
			});

			/* Right panel: selected cell fader */
			Fl_Group *panel = new Fl_Group(mx_w, ty, kFaderW, th);
			panel->box(FL_FLAT_BOX);
			panel->color(fl_rgb_color(45, 45, 45));
			panel->begin();
			{
				int px = mx_w + 10, pw = kFaderW - 20;
				int py = ty + 10;

				cell_label_ = new Fl_Box(px, py, pw, 30);
				cell_label_->labelcolor(FL_WHITE);
				cell_label_->labelfont(FL_HELVETICA_BOLD);
				cell_label_->labelsize(11);
				cell_label_->copy_label("—");
				py += 35;

				cell_fader_ = new Fl_Slider(px, py, pw, th - 120);
				cell_fader_->type(FL_VERT_NICE_SLIDER);
				cell_fader_->bounds(1.0, 0.0);  /* top=1.0=unity, bottom=0.0=silence */
				cell_fader_->value(1.0);
				cell_fader_->color(fl_rgb_color(60, 60, 60));
				cell_fader_->selection_color(fl_rgb_color(80, 160, 80));
				cell_fader_->callback(fader_cb, this);
				py += th - 115;

				cell_db_ = new Fl_Box(px, py, pw, 20);
				cell_db_->labelcolor(fl_rgb_color(180, 180, 180));
				cell_db_->labelsize(10);
				cell_db_->copy_label("-inf dB");
			}
			panel->end();
		}
		tab_matrix_->resizable(matrix_);
		tab_matrix_->end();

		/* --- Meters tab --- */
		tab_meters_ = new Fl_Group(0, kMenuH, win_w, body_h, "  Meters  ");
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

	build_device_menu();
}

MixerWindow::~MixerWindow() {
	Fl::remove_timeout(timer_cb, this);
}

/* ------------------------------------------------------------------ */
/* Menu construction                                                    */
/* ------------------------------------------------------------------ */

struct MenuAction { MixerWindow *win; std::string arg; };

void MixerWindow::menu_cb(Fl_Widget *, void *data) {
	auto *a = static_cast<MenuAction *>(data);
	if (a->arg == "__load__")        a->win->load_config();
	else if (a->arg == "__save__")   a->win->save_config();
	else if (a->arg == "__quit__")   exit(0);
	else                             a->win->connect(a->arg.c_str());
}

/* MenuAction objects live for the program lifetime */
static std::vector<MenuAction> g_menu_actions;

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
	auto devices = HdspDevice::scan();
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

/* ------------------------------------------------------------------ */
/* Device connect / disconnect                                          */
/* ------------------------------------------------------------------ */

static const char *io_type_name(uint8_t t) {
	switch (t) {
	case HDSP_IO_DIGIFACE:  return "Digiface";
	case HDSP_IO_MULTIFACE: return "Multiface";
	case HDSP_IO_H9652:     return "H9652";
	case HDSP_IO_H9632:     return "H9632";
	case HDSP_IO_RPM:       return "RPM";
	default:                return "Unknown";
	}
}

void MixerWindow::connect(const char *path) {
	Fl::remove_timeout(timer_cb, this);
	disconnect();

	if (!dev_.open(path)) {
		fl_alert("Cannot open %s", path);
		return;
	}
	connected_ = true;
	build_ui();
	redraw();   /* paint new widgets immediately */

	const auto &cfg = dev_.config();
	char buf[128];
	snprintf(buf, sizeof(buf), "%s  —  %s  |  %d ch  |  %d Hz  |  fw 0x%02x",
	    path,
	    io_type_name(cfg.io_type),
	    cfg.max_channels,
	    cfg.system_sample_rate,
	    cfg.firmware_rev);
	set_status(buf);

	Fl::add_timeout(kTimerInterval, timer_cb, this);
}

void MixerWindow::disconnect() {
	clear_mix_strips();
	clear_meters();
	if (matrix_) matrix_->init(0, nullptr);
	dev_.close();
	connected_ = false;
	set_status("Not connected");
}

/* ------------------------------------------------------------------ */
/* Build card-specific UI                                               */
/* ------------------------------------------------------------------ */

void MixerWindow::build_ui() {
	const auto &cfg = dev_.config();
	int nch = cfg.max_channels;

	/* Load current matrix state */
	struct hdsp_mixer_ioctl mx;
	if (!dev_.get_mixer(mx))
		memset(&mx, 0, sizeof(mx));

	matrix_->init(nch, mx.matrix);

	/* ----- Mixer tab -------------------------------------------------- */
	clear_mix_strips();
	mix_scroll_->scroll_to(0, 0);

	int mx0 = mix_scroll_->x() + 4;
	int y_cur = mix_scroll_->y() + 4;

	mix_scroll_->begin();
	/* Note: headers use full calculated width to ensure background fills horizontally */
	int section_w = std::max(mix_scroll_->w(), (nch + 2) * kStripW + 8);

	build_mix_section("PCM Outputs (playback)", nch,     0, mix_pcm_,
	    mx0, section_w, y_cur, fl_rgb_color(80, 160, 80));
	build_mix_section("Physical Inputs",         nch,     1, mix_in_,
	    mx0, section_w, y_cur, fl_rgb_color(180, 130, 60));
	build_mix_section("Physical Outputs",        nch + 2, 2, mix_out_,
	    mx0, section_w, y_cur, fl_rgb_color(80, 120, 200));

	mix_scroll_->end();
	mix_scroll_->init_sizes();
	mix_scroll_->redraw();

	/* ----- Meters tab ------------------------------------------------- */
	clear_meters();

	int my = meter_scroll_->y() + 5;
	int mx_x = meter_scroll_->x() + 5;

	meter_scroll_->begin();
	auto add_section = [&](const char *title, int count,
	    std::vector<VuMeter *> &vec, Fl_Color lbl_col)
	{
		/* Section label */
		Fl_Box *hdr = new Fl_Box(mx_x, my, count * kMeterW, 16, title);
		hdr->labelcolor(lbl_col);
		hdr->labelfont(FL_HELVETICA_BOLD);
		hdr->labelsize(10);
		hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
		my += 18;

		for (int i = 0; i < count; ++i) {
			char lbl[8];
			snprintf(lbl, sizeof(lbl), "%d", i + 1);
			VuMeter *vm = new VuMeter(mx_x + i * kMeterW, my,
			    kMeterW - 2, kMeterH, strdup(lbl));
			vec.push_back(vm);
		}
		my += kMeterH + 10;
	};

	add_section("Hardware Inputs",    nch, vu_in_,  fl_rgb_color(180, 130, 60));
	add_section("Playback (DAW out)", nch, vu_pb_,  fl_rgb_color(80, 160, 80));
	add_section("Hardware Outputs",   nch + 2, vu_out_, fl_rgb_color(80, 120, 200));

	meter_scroll_->end();
	meter_scroll_->init_sizes();
	meter_scroll_->redraw();
}

void MixerWindow::clear_meters() {
	vu_in_.clear();
	vu_pb_.clear();
	vu_out_.clear();
	if (meter_scroll_) {
		meter_scroll_->clear();
	}
}

void MixerWindow::clear_mix_strips() {
	/* Delete strip descriptors; widget memory owned by mix_scroll_ */
	for (auto *s : mix_pcm_) delete s;
	for (auto *s : mix_in_)  delete s;
	for (auto *s : mix_out_) delete s;
	mix_pcm_.clear();
	mix_in_.clear();
	mix_out_.clear();
	if (mix_scroll_) {
		mix_scroll_->clear();
	}
}

/* ------------------------------------------------------------------ */
/* 25 Hz meter timer                                                    */
/* ------------------------------------------------------------------ */

void MixerWindow::timer_cb(void *userdata) {
	auto *self = static_cast<MixerWindow *>(userdata);
	self->update_meters();
	Fl::repeat_timeout(kTimerInterval, timer_cb, userdata);
}

void MixerWindow::update_meters() {
	if (!connected_) return;

	struct hdsp_peak_levels lev;
	if (!dev_.get_levels(lev)) return;

	int nch = dev_.config().max_channels;

	/* Meters tab VU strips */
	for (int i = 0; i < nch && i < (int)vu_in_.size(); ++i) {
		vu_in_[i]->set_peak_raw(lev.input_peaks[i]);
		vu_in_[i]->tick();
	}
	for (int i = 0; i < nch && i < (int)vu_pb_.size(); ++i) {
		vu_pb_[i]->set_peak_raw(lev.playback_peaks[i]);
		vu_pb_[i]->tick();
	}
	for (int i = 0; i < nch + 2 && i < (int)vu_out_.size(); ++i) {
		vu_out_[i]->set_peak_raw(lev.output_peaks[i]);
		vu_out_[i]->tick();
	}

	/* Mixer tab VU strips */
	for (int i = 0; i < nch && i < (int)mix_pcm_.size(); ++i) {
		mix_pcm_[i]->vu->set_peak_raw(lev.playback_peaks[i]);
		mix_pcm_[i]->vu->tick();
	}
	for (int i = 0; i < nch && i < (int)mix_in_.size(); ++i) {
		mix_in_[i]->vu->set_peak_raw(lev.input_peaks[i]);
		mix_in_[i]->vu->tick();
	}
	for (int i = 0; i < nch + 2 && i < (int)mix_out_.size(); ++i) {
		mix_out_[i]->vu->set_peak_raw(lev.output_peaks[i]);
		mix_out_[i]->vu->tick();
	}

	/* Only repaint the active tab */
	Fl_Group *active = static_cast<Fl_Group *>(tabs_->value());
	if (active == tab_meters_)
		meter_scroll_->redraw();
	else if (active == tab_mixer_)
		mix_scroll_->redraw();
}

/* ------------------------------------------------------------------ */
/* Cell fader                                                           */
/* ------------------------------------------------------------------ */

void MixerWindow::on_cell_selected(uint32_t addr, uint16_t gain) {
	selected_addr_ = addr;

	/* Update label */
	bool is_cap = false;
	int  src = -1, out = -1;
	int  nch = dev_.config().max_channels;
	for (int o = 0; o < nch && src < 0; ++o) {
		for (int p = 0; p < nch; ++p) {
			if ((uint32_t)HDSP_PLAYBACK_ADDR(o, p) == addr) {
				src = p; out = o; is_cap = false;
				goto found;
			}
		}
		for (int c = 0; c < nch; ++c) {
			if ((uint32_t)HDSP_CAPTURE_ADDR(o, c) == addr) {
				src = c; out = o; is_cap = true;
				goto found;
			}
		}
	}
found:
	if (src >= 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%s %d → Out %d",
		    is_cap ? "In" : "PB", src + 1, out + 1);
		cell_label_->copy_label(buf);
	}

	/* Update fader position */
	double frac = (double)gain / HDSP_GAIN_UNITY;
	cell_fader_->value(frac);

	/* Update dB label */
	char db_buf[32];
	if (gain == 0)
		snprintf(db_buf, sizeof(db_buf), "-inf dB");
	else {
		float db = 20.0f * log10f((float)gain / HDSP_GAIN_UNITY);
		snprintf(db_buf, sizeof(db_buf), "%.1f dB", db);
	}
	cell_db_->copy_label(db_buf);

	tab_matrix_->redraw();
}

void MixerWindow::fader_cb(Fl_Widget *, void *data) {
	static_cast<MixerWindow *>(data)->on_fader_moved();
}

void MixerWindow::on_fader_moved() {
	if (!connected_) return;

	double frac = cell_fader_->value();
	auto gain = (uint16_t)(frac * HDSP_GAIN_UNITY);

	dev_.set_entry(selected_addr_, gain);
	matrix_->set_gain(selected_addr_, gain);

	/* Refresh dB label */
	char buf[32];
	if (gain == 0)
		snprintf(buf, sizeof(buf), "-inf dB");
	else {
		float db = 20.0f * log10f((float)gain / HDSP_GAIN_UNITY);
		snprintf(buf, sizeof(buf), "%.1f dB", db);
	}
	cell_db_->copy_label(buf);
}

/* ------------------------------------------------------------------ */
/* Mixer tab: channel strip builder                                     */
/* ------------------------------------------------------------------ */

void MixerWindow::build_mix_section(
    const char *title, int count, int addr_type,
    std::vector<MixerStrip *> &vec,
    int x0, int hdr_w, int &y_cur, Fl_Color lbl_col)
{
	/* Section header: full-width colored band with title */
	Fl_Box *hdr = new Fl_Box(FL_FLAT_BOX, x0, y_cur, hdr_w, kSHdrH, title);
	hdr->color(fl_color_average(lbl_col, FL_BLACK, 0.45f));
	hdr->labelcolor(FL_WHITE);
	hdr->labelfont(FL_HELVETICA_BOLD);
	hdr->labelsize(11);
	hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	y_cur += kSHdrH;

	const int fader_w = kStripW - kSVuW - 6;

	/* Create strips side-by-side at the current y_cur */
	for (int i = 0; i < count; ++i) {
		int sx = x0 + i * kStripW;
		int sy = y_cur;

		/* Resolve matrix address for this strip */
		int addr = -1;
		if (addr_type == 0)
			addr = (int)HDSP_PLAYBACK_ADDR(i, i);
		else if (addr_type == 1)
			addr = (int)HDSP_CAPTURE_ADDR(i, i);

		MixerStrip *s = new MixerStrip{nullptr, nullptr, nullptr, addr, this};

		/* Channel number label */
		char lbl[8];
		snprintf(lbl, sizeof(lbl), "%d", i + 1);
		Fl_Box *ch_lbl = new Fl_Box(sx, sy, kStripW, kSLabelH);
		ch_lbl->copy_label(lbl);
		ch_lbl->labelcolor(FL_WHITE);
		ch_lbl->labelsize(9);
		sy += kSLabelH;

		/* VU (left) and fader (right), same height */
		s->vu = new VuMeter(sx, sy, kSVuW, kSSlotH);

		int fx = sx + kSVuW + 4;
		s->fader = new Fl_Slider(fx, sy, fader_w, kSSlotH);
		s->fader->type(FL_VERT_NICE_SLIDER);
		s->fader->bounds(1.0, 0.0);

		if (addr >= 0) {
			uint16_t gain = matrix_->get_gain((uint32_t)addr);
			s->fader->value((double)gain / HDSP_GAIN_UNITY);
			s->fader->color(fl_rgb_color(60, 60, 60));
			s->fader->selection_color(fl_rgb_color(80, 160, 80));
			s->fader->callback(mix_fader_cb, s);
		} else {
			s->fader->value(0.0);
			s->fader->color(fl_rgb_color(38, 38, 38));
			s->fader->selection_color(fl_rgb_color(50, 50, 55));
			s->fader->deactivate();
		}

		/* dB readout */
		s->db_label = new Fl_Box(sx, sy + kSSlotH, kStripW, kSDbH);
		s->db_label->labelcolor(fl_rgb_color(150, 150, 150));
		s->db_label->labelsize(8);
		if (addr >= 0) {
			uint16_t gain = matrix_->get_gain((uint32_t)addr);
			char buf[16];
			if (gain == 0) snprintf(buf, sizeof(buf), "-inf");
			else           snprintf(buf, sizeof(buf), "%.0f", 20.0f * log10f((float)gain / HDSP_GAIN_UNITY));
			s->db_label->copy_label(buf);
		} else {
			s->db_label->copy_label("---");
			s->db_label->labelcolor(fl_rgb_color(80, 80, 80));
		}

		vec.push_back(s);
	}

	/* Advance y_cur by the total height used by strips + gap */
	y_cur += kSLabelH + kSSlotH + kSDbH + kSGapH;
}

/* ------------------------------------------------------------------ */
/* Mixer tab: strip fader callbacks                                     */
/* ------------------------------------------------------------------ */

void MixerWindow::mix_fader_cb(Fl_Widget *, void *data) {
	auto *s = static_cast<MixerStrip *>(data);
	s->win->on_mix_fader_moved(s);
}

void MixerWindow::on_mix_fader_moved(MixerStrip *s) {
	if (!connected_ || s->matrix_addr < 0) return;

	double frac = s->fader->value();
	auto gain = (uint16_t)(frac * HDSP_GAIN_UNITY);

	dev_.set_entry((uint32_t)s->matrix_addr, gain);
	matrix_->set_gain((uint32_t)s->matrix_addr, gain);

	char buf[16];
	if (gain == 0)
		snprintf(buf, sizeof(buf), "-inf");
	else {
		float db = 20.0f * log10f((float)gain / HDSP_GAIN_UNITY);
		snprintf(buf, sizeof(buf), "%.0f", db);
	}
	s->db_label->copy_label(buf);
}

/* ------------------------------------------------------------------ */
/* Config load / save                                                   */
/* ------------------------------------------------------------------ */

void MixerWindow::load_config() {
	const char *path = fl_file_chooser(
	    "Load HDSP mixer config", "*.bshdsp", nullptr);
	if (!path) return;

	FILE *fp = fopen(path, "r");
	if (!fp) { fl_alert("Cannot open %s", path); return; }

	struct hdsp_mixer_ioctl mx;
	memset(&mx, 0, sizeof(mx));

	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#' || line[0] == '\n') continue;
		unsigned addr, gain;
		if (sscanf(line, "%u %u", &addr, &gain) == 2
		    && addr < HDSP_MATRIX_MIXER_SIZE)
			mx.matrix[addr] = (uint16_t)gain;
	}
	fclose(fp);

	if (connected_) {
		if (!dev_.set_mixer(mx))
			fl_alert("Failed to write mixer state to device.");
	}
	matrix_->set_matrix(mx.matrix);
	set_status("Config loaded.");
}

void MixerWindow::save_config() {
	if (!connected_) {
		fl_alert("No device connected — nothing to save.");
		return;
	}

	const char *path = fl_file_chooser(
	    "Save HDSP mixer config", "*.bshdsp", "mixer.bshdsp");
	if (!path) return;

	struct hdsp_mixer_ioctl mx;
	if (!dev_.get_mixer(mx)) { fl_alert("Failed to read mixer state."); return; }

	FILE *fp = fopen(path, "w");
	if (!fp) { fl_alert("Cannot write %s", path); return; }

	const auto &cfg = dev_.config();
	fprintf(fp, "# bsm_hdsp mixer configuration\n");
	fprintf(fp, "# card: %s  fw: 0x%02x  ch: %d\n",
	    io_type_name(cfg.io_type), cfg.firmware_rev, cfg.max_channels);
	fprintf(fp, "# format: addr gain  (gain: 0=mute 32768=0dB)\n\n");

	for (int i = 0; i < HDSP_MATRIX_MIXER_SIZE; ++i)
		if (mx.matrix[i] != 0)
			fprintf(fp, "%d %d\n", i, mx.matrix[i]);

	fclose(fp);
	set_status("Config saved.");
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

void MixerWindow::set_status(const char *msg) {
	status_bar_->copy_label(msg);
	status_bar_->redraw();
}
