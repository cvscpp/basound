// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <FL/Fl_Widget.H>
/*
 * VuMeter — vertical bar-graph level meter widget for pf2626.
 * Input range: -60 dBFS (empty) to 0 dBFS (full).
 */
class VuMeter : public Fl_Widget {
public:
    VuMeter(int x, int y, int w, int h, const char *label = nullptr);
    void set_db(float db);
    void set_peak_raw(uint32_t peak);
    void tick();
    void draw() override;
private:
    float level_db_;
    float peak_hold_db_;
    int   peak_hold_ttl_;
    static constexpr float kFloor = -60.0f;
    static constexpr int   kHoldFrames = 50;
};
