/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * amdtp-dot.h - a part of driver for Digidesign Digi 002/003 family
 *
 * The double-oh-three algorithm was discovered by Robin Gareus and Damien
 * Zammit in 2012, with reverse-engineering for Digi 003 Rack.
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 * Copyright (C) 2012 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2012 Damien Zammit <damien@zamaudio.com>
 */

#ifndef SOUND_FIREWIRE_AMDTP_DOT_H_INCLUDED
#define SOUND_FIREWIRE_AMDTP_DOT_H_INCLUDED

#include <sys/types.h>

/* Forward declaration from sound/rawmidi.h */
struct snd_rawmidi_substream;

/*
 * The double-oh-three algorithm. Each audio sample (32-bit cell) has its
 * third byte XOR-scrambled using a stateful table-driven transformation.
 */

/* 3 = MAX(DOT_MIDI_IN_PORTS, DOT_MIDI_OUT_PORTS) + 1. */
#define DOT_MAX_MIDI_PORTS		3

#define DOT_MIDI_IN_PORTS		1
#define DOT_MIDI_OUT_PORTS		2

struct dot_state {
	uint8_t carry;
	uint8_t idx;
	unsigned int off;
};

/* DOT encoding/decoding functions */
void dot_encode_step(struct dot_state *state, uint32_t *buffer);
void dot_reset_state(struct dot_state *state);

/* PCM write helpers - encode from host s32 to DOT-encoded big-endian */
void dot_write_pcm(struct dot_state *state, uint32_t *dest,
		   const int32_t *src, unsigned int channels,
		   unsigned int frames, unsigned int data_block_quadlets);

/* Like dot_write_pcm but fills `channels` device channels per frame,
 * taking the first `src_channels` from the interleaved host buffer and
 * padding the rest with dot-encoded silence. */
void dot_write_pcm_padded(struct dot_state *state, uint32_t *dest,
			  const int32_t *src, unsigned int src_channels,
			  unsigned int channels, unsigned int frames,
			  unsigned int data_block_quadlets);

/* PCM read helpers - decode from DOT-encoded big-endian to host s32 */
void dot_read_pcm(struct dot_state *state, int32_t *dest,
		  const uint32_t *src, unsigned int channels,
		  unsigned int frames, unsigned int data_block_quadlets);

/* Generate silence DOT blocks */
void dot_write_silence(uint32_t *dest, unsigned int channels,
		       unsigned int data_blocks,
		       unsigned int data_block_quadlets);

/* MIDI encode/decode within DOT data block (byte 0 = MIDI byte, byte 3 = control) */
void dot_write_midi(uint32_t *buffer, unsigned int data_blocks,
		    unsigned int data_block_counter,
		    struct snd_rawmidi_substream *midi[3],
		    int midi_fifo_used[3], int midi_fifo_limit,
		    unsigned int syt_interval, unsigned int sfc_rate,
		    unsigned int data_block_quadlets);

void dot_read_midi(const uint32_t *buffer, unsigned int data_blocks,
		   struct snd_rawmidi_substream *midi[3],
		   unsigned int data_block_quadlets);

#endif
