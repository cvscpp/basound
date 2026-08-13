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

/* Forward declaration from dev/sound/midi/midi.h */
struct snd_midi;

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

/*
 * MIDI encode/decode within DOT data block.
 *
 * DOT MIDI format (one byte per data block per port):
 *   b[0] = 0x80 (no MIDI) or 0x81 | port (MIDI present)
 *   b[1] = MIDI data byte
 *   b[2] = 0x02 (DOT MIDI marker), set by dot_encode_step()
 *   b[3] = 0
 *
 * midi_out is called to pull bytes from the FreeBSD midi(4) output
 * queue; midi_in pushes received bytes into the input queue.
 */
void dot_write_midi(uint32_t *buffer, unsigned int data_blocks,
		    struct snd_midi *midi_out[DOT_MAX_MIDI_PORTS],
		    unsigned int data_block_quadlets);

void dot_read_midi(const uint32_t *buffer, unsigned int data_blocks,
		   struct snd_midi *midi_in[DOT_MAX_MIDI_PORTS],
		   unsigned int data_block_quadlets);

/*
 * Write MIDI bytes into a single DOT data-block MIDI quadlet.
 * Called once per data block from the TX fill path.
 * Returns the MIDI quadlet value in host byte order.
 */
uint32_t dot_write_midi_one(struct snd_midi *mo[DOT_MAX_MIDI_PORTS]);
void dot_read_midi_one(uint32_t quadlet,
		       struct snd_midi *mi[DOT_MAX_MIDI_PORTS]);

#endif
