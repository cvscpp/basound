// SPDX-License-Identifier: GPL-3.0-or-later
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "basound_debug.h"

static struct mtx basound_debug_lock;
static int basound_debug_test_tone = 0;
static int basound_debug_test_tone_freq_hz = 1000;
static int basound_debug_test_tone_length_ms = 0;
static uint32_t basound_debug_tone_phase = 0;
static uint32_t basound_debug_tone_frames_emitted = 0;

SYSCTL_DECL(_hw);
SYSCTL_NODE(_hw, OID_AUTO, basound, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "basound USB audio driver");
SYSCTL_NODE(_hw_basound, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "basound debug controls");

static int
sysctl_basound_debug_test_tone(SYSCTL_HANDLER_ARGS)
{
	int err, val;

	val = basound_debug_test_tone;
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	val = (val != 0) ? 1 : 0;

	mtx_lock(&basound_debug_lock);
	basound_debug_test_tone = val;
	basound_debug_tone_phase = 0;
	basound_debug_tone_frames_emitted = 0;
	mtx_unlock(&basound_debug_lock);

	return (0);
}

SYSCTL_PROC(_hw_basound_debug, OID_AUTO, test_tone,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, 0, 0,
    sysctl_basound_debug_test_tone, "I",
    "Inject debug test tone into playback path (1=enable, 0=normal)");

SYSCTL_INT(_hw_basound_debug, OID_AUTO, test_tone_freq_hz, CTLFLAG_RW,
    &basound_debug_test_tone_freq_hz, 0,
    "Debug test tone frequency in Hz");

SYSCTL_INT(_hw_basound_debug, OID_AUTO, test_tone_length_ms, CTLFLAG_RW,
    &basound_debug_test_tone_length_ms, 0,
    "Debug test tone length in milliseconds (0=continuous)");

static void
basound_debug_sysinit(void *arg __unused)
{
	mtx_init(&basound_debug_lock, "basound_debug", NULL, MTX_DEF);
}
SYSINIT(basound_debug_sysinit, SI_SUB_DRIVERS, SI_ORDER_MIDDLE,
    basound_debug_sysinit, NULL);

static void
basound_debug_sysuninit(void *arg __unused)
{
	mtx_destroy(&basound_debug_lock);
}
SYSUNINIT(basound_debug_sysuninit, SI_SUB_DRIVERS, SI_ORDER_MIDDLE,
    basound_debug_sysuninit, NULL);

int
basound_debug_tone_enabled(void)
{
	return (basound_debug_test_tone != 0);
}

/*
 * Fill a buffer of interleaved int32_t samples with a square-wave test
 * tone, using the "S32_LE with 24 significant bits in the upper 24
 * bits" convention used elsewhere in basound (e.g. digi00x's AM824/DOT
 * encoding expects raw_sample >> 8 to yield the 24-bit sample).  Shares
 * the same phase/enable state as basound_debug_tone_fill_s16le so a
 * single set of sysctls drives the tone regardless of which driver
 * (USB 16-bit or FireWire 32-bit) is under test.
 */
int
basound_debug_tone_fill_s32le(int32_t *buf, unsigned int frames,
    unsigned int channels, unsigned int sample_rate)
{
	uint32_t freq_hz, max_frames = 0;
	uint32_t half_period_frames;
	int32_t amp = 0x300000;	/* ~37% of 24-bit full scale */
	int length_ms;
	int active;
	unsigned int i, c;

	if (buf == NULL || frames == 0 || channels == 0 || sample_rate == 0)
		return (0);

	if (!basound_debug_test_tone)
		return (0);

	mtx_lock(&basound_debug_lock);
	active = (basound_debug_test_tone != 0);
	if (!active) {
		mtx_unlock(&basound_debug_lock);
		return (0);
	}

	freq_hz = (uint32_t)basound_debug_test_tone_freq_hz;
	if (freq_hz < 20)
		freq_hz = 20;
	if (freq_hz >= sample_rate / 2)
		freq_hz = (sample_rate > 2) ? (sample_rate / 2 - 1) : 1;
	if (freq_hz == 0)
		freq_hz = 1;

	half_period_frames = sample_rate / (freq_hz * 2);
	if (half_period_frames == 0)
		half_period_frames = 1;

	length_ms = basound_debug_test_tone_length_ms;
	if (length_ms > 0) {
		uint64_t tmp = (uint64_t)sample_rate * (uint64_t)length_ms;
		tmp /= 1000;
		if (tmp > UINT32_MAX)
			tmp = UINT32_MAX;
		max_frames = (uint32_t)tmp;
	}

	for (i = 0; i < frames; i++) {
		int32_t sample = 0;
		int emit = 1;

		if (max_frames != 0 &&
		    basound_debug_tone_frames_emitted >= max_frames) {
			emit = 0;
			basound_debug_test_tone = 0;
		}

		if (emit) {
			sample = ((basound_debug_tone_phase %
			    (half_period_frames * 2)) < half_period_frames) ?
			    (amp << 8) : (-amp << 8);
			basound_debug_tone_phase++;
			basound_debug_tone_frames_emitted++;
		}

		for (c = 0; c < channels; c++)
			*buf++ = sample;
	}
	mtx_unlock(&basound_debug_lock);

	return (1);
}

int
basound_debug_tone_fill_s16le(void *buf, size_t len, unsigned int channels,
    unsigned int sample_rate)
{
	uint16_t *out;
	size_t frames, i, c;
	uint32_t freq_hz, max_frames = 0;
	uint32_t half_period_frames;
	uint32_t amp = 30000;
	int length_ms;
	int active;

	if (buf == NULL || len == 0 || channels == 0 || sample_rate == 0)
		return (0);

	if (!basound_debug_test_tone)
		return (0);

	frames = len / (channels * sizeof(uint16_t));
	if (frames == 0)
		return (0);

	mtx_lock(&basound_debug_lock);
	active = (basound_debug_test_tone != 0);
	if (!active) {
		mtx_unlock(&basound_debug_lock);
		return (0);
	}

	freq_hz = (uint32_t)basound_debug_test_tone_freq_hz;
	if (freq_hz < 20)
		freq_hz = 20;
	if (freq_hz >= sample_rate / 2)
		freq_hz = (sample_rate > 2) ? (sample_rate / 2 - 1) : 1;
	if (freq_hz == 0)
		freq_hz = 1;

	half_period_frames = sample_rate / (freq_hz * 2);
	if (half_period_frames == 0)
		half_period_frames = 1;

	length_ms = basound_debug_test_tone_length_ms;
	if (length_ms > 0) {
		uint64_t tmp = (uint64_t)sample_rate * (uint64_t)length_ms;
		tmp /= 1000;
		if (tmp > UINT32_MAX)
			tmp = UINT32_MAX;
		max_frames = (uint32_t)tmp;
	}

	out = (uint16_t *)buf;
	for (i = 0; i < frames; i++) {
		int16_t sample = 0;
		int emit = 1;

		if (max_frames != 0 &&
		    basound_debug_tone_frames_emitted >= max_frames) {
			emit = 0;
			basound_debug_test_tone = 0;
		}

		if (emit) {
			sample = ((basound_debug_tone_phase %
			    (half_period_frames * 2)) < half_period_frames) ?
			    (int16_t)amp : (int16_t)-amp;
			basound_debug_tone_phase++;
			basound_debug_tone_frames_emitted++;
		}

		for (c = 0; c < channels; c++)
			*out++ = (uint16_t)sample;
	}
	mtx_unlock(&basound_debug_lock);

	return (1);
}
