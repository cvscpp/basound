// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <FL/Fl_Widget.H>

/*
 * VuMeter — vertical bar-graph level meter widget (self-contained).
 *
 * Color zones (bottom to top):
 *   green   0% .. 67%   (≈ -18 dBFS and below)
 *   yellow 67% .. 90%   (-18 .. -6 dBFS)
 *   red    90% .. 100%  (-6 .. 0 dBFS)
 *
 * Input range: -60 dBFS (empty) to 0 dBFS (full).
 * Peak hold indicator shown as a 2-pixel line, decays after 2 s.
 */
class VuMeter : public Fl_Widget {
public:
	VuMeter(int x, int y, int w, int h, const char *label = nullptr);

	/* Feed a new level.  Clamps to [-60, 0] dBFS. */
	void set_db(float db);

	/* Feed a raw 16-bit sample peak value. */
	void set_peak_i16(uint16_t peak);

	/* Tick the peak-hold decay counter (call once per meter-update cycle). */
	void tick();

	void draw() override;

	/* Convert raw 16-bit peak to dBFS. */
	static float peak_i16_to_db(uint16_t peak);
	static float peak_to_db(uint32_t peak, uint32_t max_val);

private:
	float level_db_;       /* current level in dBFS */
	float peak_hold_db_;   /* peak-hold level in dBFS */
	int   peak_hold_ttl_;  /* frames until peak starts decaying */

	static constexpr float kFloor = -60.0f;
	static constexpr int   kHoldFrames = 50; /* 2 s @ 25 Hz */

	float db_to_frac(float db) const;
};
