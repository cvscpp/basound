/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * amdtp-dot.c - a part of driver for Digidesign Digi 002/003 family
 *
 * Implementation of the double-oh-three encoding algorithm discovered
 * by Robin Gareus and Damien Zammit in 2012.
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 * Copyright (C) 2012 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2012 Damien Zammit <damien@zamaudio.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kobj.h>

#include <dev/sound/midi/midi.h>

#include "amdtp-dot.h"

#define MAGIC_DOT_BYTE 2	/* third byte of each 32-bit cell */

/*
 * double-oh-three look up table
 *
 * @param idx index byte (audio-sample data) 0x00..0xff
 * @param off channel offset shift
 * @return salt to XOR with given data
 */

/* Nominally 3125 bytes/second, but MIDI port clock might be 1% slow. */
#define MIDI_BYTES_PER_SECOND	3093

static uint8_t dot_scrt(uint8_t idx, unsigned int off)
{
	static const uint8_t len[16] = {0, 1, 3, 5, 7, 9, 11, 13, 14,
					12, 10, 8, 6, 4, 2, 0};
	static const uint8_t nib[15] = {0x8, 0x7, 0x9, 0x6, 0xa, 0x5, 0xb, 0x4,
					0xc, 0x3, 0xd, 0x2, 0xe, 0x1, 0xf};
	static const uint8_t hir[15] = {0x0, 0x6, 0xf, 0x8, 0x7, 0x5, 0x3, 0x4,
					0xc, 0xd, 0xe, 0x1, 0x2, 0xb, 0xa};
	static const uint8_t hio[16] = {0, 11, 12, 6, 7, 5, 1, 4,
					3, 0x00, 14, 13, 8, 9, 10, 2};

	uint8_t ln = idx & 0xf;
	uint8_t hn = (idx >> 4) & 0xf;
	uint8_t hr = (hn == 0x9) ? 0x9 : hir[(hio[hn] + off) % 15];

	if (len[ln] < off)
		return 0x00;

	return ((nib[14 + off - len[ln]]) | (hr << 4));
}

void
dot_encode_step(struct dot_state *state, uint32_t *buffer)
{
	uint8_t *data = (uint8_t *)buffer;

	if (data[MAGIC_DOT_BYTE] != 0x00) {
		state->off = 0;
		state->idx = data[MAGIC_DOT_BYTE] ^ state->carry;
	}
	data[MAGIC_DOT_BYTE] ^= state->carry;
	state->carry = dot_scrt(state->idx, ++(state->off));
}

void
dot_reset_state(struct dot_state *state)
{
	state->carry = 0x00;
	state->idx = 0x00;
	state->off = 0;
}

void
dot_write_pcm(struct dot_state *state, uint32_t *dest,
	      const int32_t *src, unsigned int channels,
	      unsigned int frames, unsigned int data_block_quadlets)
{
	unsigned int i, c;

	for (i = 0; i < frames; i++) {
		for (c = 0; c < channels; c++) {
			uint32_t sample = ((uint32_t)src[c] >> 8) | 0x40000000;
			dest[c] = htobe32(sample);
			dot_encode_step(state, &dest[c]);
		}
		dest += data_block_quadlets;
		src += channels;
	}
}

/*
 * Encode PCM into DOT data blocks, filling `channels` device channels per
 * frame.  The first `src_channels` channels come from the interleaved
 * host buffer; the remainder are filled with dot-encoded silence
 * (0x40000000) so the encoder state advances across the whole data block
 * and stays in sync with the device's decoder.
 *
 * The Digi 002/003 always carries its full channel complement in every
 * data block (18 channels at 44.1/48 kHz, 10 at 88.2/96 kHz), regardless
 * of how many channels the PCM application opened.  Transmitting a shorter
 * block (e.g. a stereo data block) makes the device's DOT decoder lose
 * sync and produces silence on all outputs, including the phones jack.
 */
void
dot_write_pcm_padded(struct dot_state *state, uint32_t *dest,
		     const int32_t *src, unsigned int src_channels,
		     unsigned int channels, unsigned int frames,
		     unsigned int data_block_quadlets)
{
	unsigned int i, c;

	for (i = 0; i < frames; i++) {
		for (c = 0; c < channels; c++) {
			uint32_t sample = 0x40000000;

			if (c < src_channels)
				sample |= ((uint32_t)src[c] >> 8);
			dest[c] = htobe32(sample);
			dot_encode_step(state, &dest[c]);
		}
		dest += data_block_quadlets;
		src += src_channels;
	}
}

void
dot_read_pcm(struct dot_state *state, int32_t *dest,
	     const uint32_t *src, unsigned int channels,
	     unsigned int frames, unsigned int data_block_quadlets)
{
	unsigned int i, c;

	for (i = 0; i < frames; i++) {
		for (c = 0; c < channels; c++) {
			uint32_t sample = be32toh(src[c]);
			dest[c] = (int32_t)(sample << 8);
		}
		src += data_block_quadlets;
		dest += channels;
	}
}

void
dot_write_silence(uint32_t *dest, unsigned int channels,
		  unsigned int data_blocks,
		  unsigned int data_block_quadlets)
{
	unsigned int i, c;

	for (i = 0; i < data_blocks; i++) {
		for (c = 0; c < channels; c++)
			dest[c] = htobe32(0x40000000);
		dest += data_block_quadlets;
	}
}

/*
 * Write MIDI bytes from the FreeBSD midi(4) output queue into DOT data
 * blocks.  One byte per data block per port: b[0] = 0x80 | (port+1),
 * b[1] = MIDI byte, b[2] = 0x02 (DOT MIDI marker), b[3] = 0.
 * Called from the TX refill callout (every 1 ms).
 */
/*
 * Build a single DOT MIDI quadlet (host byte order) for one data block.
 * Returns the final quadlet; callers write it into the TX packet.
 */
uint32_t
dot_write_midi_one(struct snd_midi *mo[DOT_MAX_MIDI_PORTS])
{
	uint8_t b[4] = {0x80, 0, 0, 0};
	uint8_t midi_byte;
	unsigned int p;

	/* Try each output port in order. */
	for (p = 0; p < DOT_MIDI_OUT_PORTS; p++) {
		if (mo[p] != NULL &&
		    midi_out(mo[p], &midi_byte, 1) == 1) {
			b[0] = 0x80 | (p + 1);
			b[1] = midi_byte;
			b[2] = 0x02;	/* DOT MIDI marker */
			break;
		}
	}

	return ((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

/*
 * Decode a single DOT MIDI quadlet from the RX packet and push the byte
 * into the FreeBSD midi(4) input queue.
 */
void
dot_read_midi_one(uint32_t quadlet,
		  struct snd_midi *mi[DOT_MAX_MIDI_PORTS])
{
	unsigned int port = quadlet & 0x0f;
	uint8_t midi_byte = (quadlet >> 8) & 0xff;

	if (port > 0 && port <= DOT_MIDI_IN_PORTS &&
	    mi[port - 1] != NULL)
		midi_in(mi[port - 1], &midi_byte, 1);
}

/*
 * Read MIDI bytes from DOT data blocks into the FreeBSD midi(4) input
 * queue.  b[0] low nibble = port+1 (0 means no MIDI), b[1] = MIDI byte.
 * Called from the ISO receive handler.
 */
void
dot_read_midi(const uint32_t *buffer, unsigned int data_blocks,
	      struct snd_midi *mi[DOT_MAX_MIDI_PORTS],
	      unsigned int data_block_quadlets)
{
	unsigned int f;

	for (f = 0; f < data_blocks; f++) {
		const uint8_t *b = (const uint8_t *)&buffer[0];
		unsigned int port = b[0] & 0x0f;

		/* Port 0 means no MIDI data in this block. */
		if (port > 0 && port <= DOT_MIDI_IN_PORTS &&
		    mi[port - 1] != NULL) {
			uint8_t midi_byte = b[1];

			midi_in(mi[port - 1], &midi_byte, 1);
		}

		buffer += data_block_quadlets;
	}
}
