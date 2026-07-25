/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-midi.c - MIDI device for Digidesign Digi 002/003
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <sound/core.h>
#include <sound/rawmidi.h>

#include "digi00x.h"

int
dg00x_create_midi(struct snd_dg00x *dg00x)
{
	struct snd_rawmidi *rmidi;
	int err;

	err = snd_rawmidi_new(dg00x->card, dg00x->card->driver, 0,
			      DOT_MIDI_OUT_PORTS, DOT_MIDI_IN_PORTS, &rmidi);
	if (err != 0)
		return (err);

	rmidi->private_data = dg00x;
	snprintf(rmidi->name, sizeof(rmidi->name), "%s MIDI",
		 dg00x->card->shortname);
	return (0);
}
