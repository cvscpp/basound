// SPDX-License-Identifier: GPL-3.0-or-later
#include "mixer_window.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include "vu_meter.h"
#include <cstdio>
#include <cstring>
#include <cmath>

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

static uint8_t make_blk(uint8_t id, uint8_t ch) {
	return (uint8_t)(((id & 0x0f) << 4) | (ch & 0x0f));
}

static uint32_t make_entry(uint8_t src_blk, uint8_t src_ch,
			   uint8_t dst_blk, uint8_t dst_ch) {
	uint32_t src = make_blk(src_blk, src_ch);
	uint32_t dst = make_blk(dst_blk, dst_ch);
	return (src << 8) | dst;
}

MixerWindow::MixerWindow(int w, int h, const char *title)
    : Fl_Double_Window(w, h, title),
      tab_meters_(nullptr), meter_scroll_(nullptr),
      connected_(false),
      menu_(nullptr), tabs_(nullptr),
      tab_router_(nullptr), router_scroll_(nullptr),
      router_mode_(nullptr), router_apply_(nullptr),
      router_rate_mode_(0),
      tab_mixer_(nullptr), mixer_scroll_(nullptr),
      mixer_apply_(nullptr), mixer_status_(nullptr),
      tab_clock_(nullptr), clock_rate_(nullptr), clock_source_(nullptr),
      clock_apply_(nullptr), clock_status_(nullptr),
      status_bar_(nullptr)
{
	menu_ = new Fl_Menu_Bar(0, 0, w, 24);
	tabs_ = new Fl_Tabs(0, 28, w, h - 28 - 24);

	build_device_menu();

	tabs_->begin();
	build_router_tab();
	build_mixer_tab();
	build_meter_tab();
	build_clock_tab();
	tabs_->end();

	/* Open on the Meters tab: live level meters are the primary view of
	 * this tool (the digi00x sibling shows them on the main screen).
	 * A user clicking the Router/Mixer/Clock tabs gets them back via the
	 * tab buttons. */
	tabs_->value(tab_meters_);

	status_bar_ = new Fl_Box(FL_NO_BOX, 0, h - 22, w, 22,
	    "No device connected");
	status_bar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	end();
	resizable(tabs_);
}

MixerWindow::~MixerWindow() {
}

void MixerWindow::set_status(const char *msg) {
	status_bar_->copy_label(msg);
}

void MixerWindow::build_device_menu() {
	menu_->clear();
	menu_->add("Device/Refresh", 0, menu_cb, this);
	menu_->add("Device/Disconnect", 0, menu_cb, this);

	for (const auto &p : Pf2626Device::scan())
		menu_->add(("Device/" + p).c_str(), 0, menu_cb, this);
}

void MixerWindow::menu_cb(Fl_Widget *w, void *data) {
	MixerWindow *win = (MixerWindow *)data;
	const char *label = w->label();
	if (!label)
		return;
	if (strcmp(label, "Device/Refresh") == 0) {
		win->build_device_menu();
	} else if (strcmp(label, "Device/Disconnect") == 0) {
		win->disconnect();
	} else if (strncmp(label, "Device//dev/", 12) == 0) {
		win->connect(label + 7); /* strip "Device/" */
	}
}

void MixerWindow::connect(const char *path) {
	disconnect();
	if (!dev_.open(path)) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Failed to open %s", path);
		set_status(buf);
		return;
	}
	connected_ = true;

	router_rate_mode_ = 0;
	load_router();
	load_mixer();
	load_clock();
	Fl::add_timeout(0.04, meter_timer_cb, this);

	char buf[128];
	snprintf(buf, sizeof(buf), "Connected: %s", path);
	set_status(buf);
}

void MixerWindow::disconnect() {
	if (connected_) {
		dev_.close();
		connected_ = false;
		Fl::remove_timeout(meter_timer_cb, this);
		clear_meters();
	}
	set_status("No device connected");
}

/* ------------------------------------------------------------------ */
/* Router tab                                                          */
/* ------------------------------------------------------------------ */

static void add_src(std::vector<std::string> &labels,
		    std::vector<std::pair<uint8_t,uint8_t>> &ids,
		    uint8_t blk, uint8_t ch, const char *name)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%s %d", name, ch + 1);
	labels.push_back(buf);
	ids.push_back(std::make_pair(blk, ch));
}

void MixerWindow::build_router_tab() {
	tab_router_ = new Fl_Group(0, 30, w(), h() - 60, "Router");
	tab_router_->begin();

	router_mode_ = new Fl_Choice(10, 45, 120, 25, "Rate mode");
	router_mode_->add("Low (<=48k)");
	router_mode_->add("Mid (<=96k)");
	router_mode_->add("High (<=192k)");
	router_mode_->value(0);

	router_apply_ = new Fl_Button(140, 45, 90, 25, "Apply");
	router_apply_->callback(router_apply_cb, this);

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

	router_scroll_ = new Fl_Scroll(10, 80, w() - 20, h() - 150);
	router_scroll_->begin();
	int y = 90;
	for (auto &d : dests_) {
		new Fl_Box(FL_NO_BOX, 20, y, 180, 25, d.label.c_str());
		d.choice = new Fl_Choice(210, y, 260, 25);
		for (const auto &s : src_labels_)
			d.choice->add(s.c_str());
		d.choice->value(0);
		y += 30;
	}
	router_scroll_->end();

	tab_router_->end();
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

void MixerWindow::router_apply_cb(Fl_Widget *w, void *data) {
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
/* Mixer tab                                                           */
/* ------------------------------------------------------------------ */

void MixerWindow::build_mixer_tab() {
	tab_mixer_ = new Fl_Group(0, 30, w(), h() - 60, "Mixer");
	tab_mixer_->begin();

	mixer_apply_ = new Fl_Button(10, 45, 90, 25, "Apply");
	mixer_apply_->callback(mixer_apply_cb, this);
	mixer_status_ = new Fl_Box(FL_NO_BOX, 110, 45, 400, 25,
	    "Unity = 16384 (0 dB)");
	mixer_status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	mixer_scroll_ = new Fl_Scroll(10, 80, w() - 20, h() - 150);
	mixer_scroll_->begin();
	/* Widgets are added dynamically in load_mixer() when the device
	 * geometry is known. */
	mixer_scroll_->end();

	tab_mixer_->end();
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

	/* Rebuild sliders.  clear() removes and deletes the old children. */
	mixer_scroll_->clear();
	cells_.clear();

	mixer_scroll_->begin();
	int y = 0;
	for (unsigned int o = 0; o < outputs; o++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Mix out %d", o + 1);
		new Fl_Box(FL_NO_BOX, 0, y, 90, 20, buf);
		for (unsigned int i = 0; i < inputs; i++) {
			MixerCell c;
			c.out = (int)o;
			c.in = (int)i;
			c.slider = new Fl_Hor_Slider(100 + (int)i * 46, y, 44, 18);
			c.slider->type(FL_HORIZONTAL);
			c.slider->range(0, 32768);
			c.slider->step(1);
			c.slider->value(mixer_coeff_[o * inputs + i] & 0xffff);
			c.slider->callback(mixer_cell_cb, this);
			cells_.push_back(c);
		}
		y += 24;
	}
	mixer_scroll_->end();
	mixer_scroll_->redraw();
}

void MixerWindow::mixer_apply_cb(Fl_Widget *w, void *data) {
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

/* ------------------------------------------------------------------ */
/* Meter tab --------------------------------------------------------- */
void MixerWindow::build_meter_tab() {
	tab_meters_ = new Fl_Group(0, 30, w(), h() - 60, "Meters");
	tab_meters_->begin();

	/* The ProFire 2626 has 26 capture and 26 playback channels
	 * (8 analog + 8+8 ADAT + 2 S/PDIF on each side), so the meter
	 * widgets are built here once; update_meters() only refreshes
	 * levels. */
	meter_scroll_ = new Fl_Scroll(0, 30, w(), h() - 90);
	meter_scroll_->box(FL_FLAT_BOX);
	meter_scroll_->color(fl_rgb_color(30, 30, 30));
	meter_scroll_->begin();

	Fl_Box *in_cap = new Fl_Box(FL_NO_BOX, 10, 2, 900, 12,
	    "Inputs (capture): 1-8 analog, 9-16 ADAT A, 17-24 ADAT B, 25-26 S/PDIF");
	in_cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	in_cap->labelcolor(fl_rgb_color(170, 180, 190));
	in_cap->labelsize(10);

	for (int i = 0; i < PF2626_MAX_INPUTS; ++i) {
		char buf[16];
		snprintf(buf, sizeof(buf), "In %d", i + 1);
		vu_in_.push_back(new VuMeter(10 + i * 40, 16, 34, 170, buf));
	}

	Fl_Box *out_cap = new Fl_Box(FL_NO_BOX, 10, 198, 900, 12,
	    "Outputs (playback): 1-8 analog, 9-16 ADAT A, 17-24 ADAT B, 25-26 S/PDIF");
	out_cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	out_cap->labelcolor(fl_rgb_color(170, 180, 190));
	out_cap->labelsize(10);

	for (int i = 0; i < PF2626_MAX_OUTPUTS; ++i) {
		char buf[16];
		snprintf(buf, sizeof(buf), "Out %d", i + 1);
		vu_out_.push_back(new VuMeter(10 + i * 40, 212, 34, 170, buf));
	}

	meter_scroll_->end();

	tab_meters_->resizable(meter_scroll_);
	tab_meters_->end();
}

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
}

void MixerWindow::meter_timer_cb(void *userdata) {
    MixerWindow *win = static_cast<MixerWindow *>(userdata);
    win->update_meters();
    Fl::repeat_timeout(0.04, meter_timer_cb, userdata);
}

/* Clock tab                                                           */
/* ------------------------------------------------------------------ */

void MixerWindow::build_clock_tab() {
	tab_clock_ = new Fl_Group(0, 30, w(), h() - 60, "Clock");
	tab_clock_->begin();

	clock_rate_ = new Fl_Choice(120, 50, 140, 25, "Sample rate");
	clock_rate_->add("44100 Hz");
	clock_rate_->add("48000 Hz");
	clock_rate_->add("88200 Hz");
	clock_rate_->add("96000 Hz");
	clock_rate_->add("176400 Hz");
	clock_rate_->add("192000 Hz");
	clock_rate_->value(1);

	clock_source_ = new Fl_Choice(120, 90, 140, 25, "Clock source");
	clock_source_->add("Internal");
	clock_source_->add("ADAT");
	clock_source_->add("SPDIF");
	clock_source_->add("Word Clock");
	clock_source_->add("TDIF");
	clock_source_->value(0);

	clock_apply_ = new Fl_Button(280, 90, 90, 25, "Apply");
	clock_apply_->callback(clock_apply_cb, this);

	clock_status_ = new Fl_Box(FL_NO_BOX, 20, 130, 500, 25, "");
	clock_status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	tab_clock_->end();
}

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

void MixerWindow::clock_apply_cb(Fl_Widget *w, void *data) {
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
