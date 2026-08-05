// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _BASOUND_DEBUG_H_
#define _BASOUND_DEBUG_H_

#include <sys/types.h>

/*
 * Driver-independent debug tone generator controls:
 *   hw.basound.debug.test_tone           (0/1)
 *   hw.basound.debug.test_tone_freq_hz   (Hz)
 *   hw.basound.debug.test_tone_length_ms (0=continuous, >0 one-shot length)
 */
int basound_debug_tone_enabled(void);
int basound_debug_tone_fill_s16le(void *buf, size_t len, unsigned int channels,
    unsigned int sample_rate);
int basound_debug_tone_fill_s32le(int32_t *buf, unsigned int frames,
    unsigned int channels, unsigned int sample_rate);

#endif /* _BASOUND_DEBUG_H_ */
