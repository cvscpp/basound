// SPDX-License-Identifier: GPL-3.0-or-later
#include "vu_meter.h"
#include "pf2626_device.h"
#include <FL/fl_draw.H>
#include <math.h>
#include <algorithm>
VuMeter::VuMeter(int x, int y, int w, int h, const char *label)
    : Fl_Widget(x, y, w, h, label),
      level_db_(kFloor),
      peak_hold_db_(kFloor),
      peak_hold_ttl_(0) {}
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
void VuMeter::set_peak_raw(uint32_t peak) {
    set_db(Pf2626Device::peak_to_db(peak));
}
void VuMeter::tick() {
    if (peak_hold_ttl_ > 0) {
        --peak_hold_ttl_;
    } else if (peak_hold_db_ > kFloor) {
        peak_hold_db_ -= 1.5f;
        if (peak_hold_db_ < kFloor)
            peak_hold_db_ = kFloor;
        damage(FL_DAMAGE_ALL);
    }
}
void VuMeter::draw() {
    int bx = x(), by = y(), bw = w(), bh = h();
    const int lh = label() ? 13 : 0;
    const int meter_h = bh - lh;
    if (meter_h < 4 || bw < 4) {
        fl_color(FL_BLACK);
        fl_rectf(bx, by, bw, bh);
        return;
    }
    fl_color(fl_rgb_color(12, 14, 18));
    fl_rectf(bx, by, bw, meter_h);
    const float frac = (level_db_ - kFloor) / -kFloor;
    const int fill = (int)(frac * meter_h);
    const float kGreenTop  = 0.67f;
    const float kYellowTop = 0.90f;
    for (int row = 0; row < fill; ++row) {
        float pos = (float)row / meter_h;
        Fl_Color col;
        if (pos < kGreenTop)
            col = fl_rgb_color(0, 200, 0);
        else if (pos < kYellowTop)
            col = fl_rgb_color(220, 200, 30);
        else
            col = fl_rgb_color(220, 30, 30);
        fl_color(col);
        int bar_y = by + meter_h - 1 - row;
        fl_line(bx, bar_y, bx + bw - 1, bar_y);
    }
    static const float kMajor[] = {0.f,-6.f,-12.f,-18.f,-24.f,-36.f,-60.f};
    for (float db = kFloor; db <= 0.0f; db += 3.0f) {
        float f   = (db - kFloor) / -kFloor;
        int   ty  = by + meter_h - 1 - (int)(f * (meter_h - 1));
        bool major = false;
        for (float m : kMajor) { if (fabsf(db - m) < 0.5f) { major = true; break; } }
        fl_color(major ? FL_WHITE : fl_rgb_color(80, 80, 80));
        fl_line(bx, ty, bx + bw - 1, ty);
    }
    if (peak_hold_db_ > kFloor) {
        float pf = (peak_hold_db_ - kFloor) / -kFloor;
        int   py = by + meter_h - 1 - (int)(pf * (meter_h - 1));
        fl_color(FL_WHITE);
        fl_line(bx, py, bx + bw - 1, py);
        fl_line(bx, py - 1, bx + bw - 1, py - 1);
    }
    fl_color(fl_rgb_color(100, 100, 110));
    fl_rect(bx, by, bw, meter_h);
    if (lh > 0) {
        fl_color(fl_rgb_color(50, 50, 50));
        fl_rectf(bx, by + meter_h, bw, lh);
        fl_color(FL_WHITE);
        fl_font(FL_HELVETICA, 9);
        fl_draw(label(), bx, by + meter_h, bw, lh, FL_ALIGN_CENTER | FL_ALIGN_CLIP);
    }
}
