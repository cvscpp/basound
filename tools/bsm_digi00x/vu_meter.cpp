// SPDX-License-Identifier: GPL-3.0-or-later
#include "vu_meter.h"

#include <FL/fl_draw.H>
#include <math.h>
#include <algorithm>

VuMeter::VuMeter(int x, int y, int w, int h, const char *label)
    : Fl_Widget(x, y, w, h),
      level_db_(kFloor),
      peak_hold_db_(kFloor),
      peak_hold_ttl_(0),
      label_(label ? label : "")
{
}

void VuMeter::set_db(float db) {
	if (db < kFloor) db = kFloor;
	if (db > 0.0f)   db = 0.0f;
	level_db_ = db;

	if (db >= peak_hold_db_) {
		peak_hold_db_ = db;
		peak_hold_ttl_ = kHoldFrames;
	}
	damage(FL_DAMAGE_ALL);
}

void VuMeter::set_peak_i16(uint16_t peak) {
	set_db(peak_i16_to_db(peak));
}

void VuMeter::tick() {
	if (peak_hold_ttl_ > 0) {
		--peak_hold_ttl_;
	} else if (peak_hold_db_ > kFloor) {
		peak_hold_db_ -= 1.5f; /* decay 1.5 dB per frame */
		if (peak_hold_db_ < kFloor)
			peak_hold_db_ = kFloor;
		damage(FL_DAMAGE_ALL);
	}
}

float VuMeter::peak_i16_to_db(uint16_t peak) {
	if (peak == 0)
		return -144.0f;
	return 20.0f * log10f((float)peak / 32767.0f);
}

float VuMeter::peak_to_db(uint32_t peak, uint32_t max_val) {
	if (peak == 0 || max_val == 0)
		return -144.0f;
	return 20.0f * log10f((float)peak / (float)max_val);
}

float VuMeter::db_to_frac(float db) const {
	/* Linear mapping: kFloor → 0.0, 0 dBFS → 1.0 */
	return (db - kFloor) / (-kFloor);
}

void VuMeter::draw() {
	const int bx = x(), by = y(), bw = w(), bh = h();

	/* Reserve label area at bottom */
	const int lh = label_.empty() ? 0 : 16;
	const int meter_h = bh - lh;

	if (meter_h < 4 || bw < 4) {
		fl_color(FL_BLACK);
		fl_rectf(bx, by, bw, bh);
		return;
	}

	/* Bar interior: slightly narrowed from border */
	const int bar_x  = bx + 2;
	const int bar_w  = bw - 4;

	/* ---- background ---- */
	fl_color(fl_rgb_color(12, 14, 18));
	fl_rectf(bx, by, bw, meter_h);

	/* ---- colored fill bar (bottom up) ---- */
	const float frac = db_to_frac(level_db_);
	const int   fill = (int)(frac * meter_h);

	/* Green / yellow / red thresholds */
	const float kGreenTop  = 0.67f; /* ≈ -18 dBFS */
	const float kYellowTop = 0.90f; /* ≈  -6 dBFS */

	for (int row = 0; row < fill; ++row) {
		float pos = (float)row / meter_h;
		Fl_Color col;
		if (pos < kGreenTop)
			col = fl_rgb_color(0, 200, 0);
		else if (pos < kYellowTop)
			col = fl_rgb_color(220, 190, 0);
		else
			col = fl_rgb_color(220, 30, 30);
		fl_color(col);
		int bar_y = by + meter_h - 1 - row;
		fl_line(bar_x, bar_y, bar_x + bar_w - 1, bar_y);
	}

	/* ---- scale tick marks — always drawn so the meter is visible ----
	 * Major marks (0, -6, -12, -18, -24, -36, -60 dBFS): bright
	 * Minor marks (every 3 dB): dim                                   */
	static const float kMajor[] = {0.f,-6.f,-12.f,-18.f,-24.f,-36.f,-60.f};
	for (float db = kFloor; db <= 0.0f; db += 3.0f) {
		float f   = db_to_frac(db);
		int   ty  = by + meter_h - 1 - (int)(f * (meter_h - 1));

		bool major = false;
		for (float m : kMajor) { if (fabsf(db - m) < 0.5f) { major = true; break; } }

		if (major)
			fl_color(fl_rgb_color(90, 90, 90));
		else
			fl_color(fl_rgb_color(30, 30, 30));
		fl_line(bar_x, ty, bar_x + bar_w - 1, ty);
	}

	/* 0 dBFS top marker: slightly red tint */
	{
		int top_y = by + 1;
		fl_color(fl_rgb_color(120, 40, 40));
		fl_line(bar_x, top_y, bar_x + bar_w - 1, top_y);
	}

	/* ---- peak hold line ---- */
	if (peak_hold_db_ > kFloor) {
		float pf = db_to_frac(peak_hold_db_);
		int   py = by + meter_h - 1 - (int)(pf * (meter_h - 1));
		fl_color(FL_WHITE);
		fl_line(bar_x, py, bar_x + bar_w - 1, py);
		fl_line(bar_x, py - 1, bar_x + bar_w - 1, py - 1);
	}

	/* ---- border ---- */
	fl_color(fl_rgb_color(100, 100, 110));
	fl_rect(bx, by, bw, meter_h);

	/* ---- channel label ---- */
	if (lh > 0) {
		/* Separator line between bar and label */
		int sep_y = by + meter_h;
		fl_color(fl_rgb_color(80, 80, 90));
		fl_line(bx, sep_y, bx + bw - 1, sep_y);

		/* Label background */
		fl_color(fl_rgb_color(60, 60, 68));
		fl_rectf(bx, sep_y + 1, bw, lh - 1);

		/* Label text — bold, larger, bottom-aligned to fit in tight space */
		fl_color(fl_rgb_color(220, 220, 230));
		fl_font(FL_HELVETICA_BOLD, 10);
		fl_draw(label_.c_str(), bx, by + meter_h + 1, bw, lh - 1,
		        FL_ALIGN_CENTER | FL_ALIGN_BOTTOM | FL_ALIGN_CLIP);
	}
}
