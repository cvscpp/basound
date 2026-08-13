/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-midi.c - MIDI device for Digidesign Digi 002/003
 *
 * Uses FreeBSD midi(4) framework.  MIDI bytes ride in the DOT
 * isochronous packets: dot_write_midi() dequeues from midi_out(),
 * dot_read_midi() pushes received bytes into midi_in().
 *
 * The TX refill callout (1 ms) already calls dot_write_midi(),
 * so no separate timer is needed for output polling.
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kobj.h>
#include <sys/bus.h>

#include <dev/sound/midi/midi.h>
#include "mpu_if.h"

#include "digi00x.h"

MALLOC_DECLARE(M_MIDI);

/* ------------------------------------------------------------------ */
/* Minimal midi class for DOT MIDI ports — no hardware registers.      */
/* midi_init() requires a class that provides the mpu_* interface.     */
/* ------------------------------------------------------------------ */

static int
dg00x_midi_init(struct snd_midi *sm __unused, void *arg __unused)
{
	return (0);
}

static int
dg00x_midi_uninit(struct snd_midi *sm __unused, void *arg __unused)
{
	return (0);
}

static int
dg00x_midi_inqsize(struct snd_midi *sm __unused, void *arg __unused)
{
	return (128);
}

static int
dg00x_midi_outqsize(struct snd_midi *sm __unused, void *arg __unused)
{
	return (128);
}

static void
dg00x_midi_callback(struct snd_midi *sm __unused, void *arg __unused,
    int flags __unused)
{
}

static kobj_method_t dg00x_midi_methods[] = {
	KOBJMETHOD(mpu_init,     dg00x_midi_init),
	KOBJMETHOD(mpu_uninit,   dg00x_midi_uninit),
	KOBJMETHOD(mpu_inqsize,  dg00x_midi_inqsize),
	KOBJMETHOD(mpu_outqsize, dg00x_midi_outqsize),
	KOBJMETHOD(mpu_callback, dg00x_midi_callback),
	KOBJMETHOD_END
};
static DEFINE_CLASS(dg00x_midi, dg00x_midi_methods, 0);

/* ------------------------------------------------------------------ */
/* Create the MIDI input and output devices.                           */
/*                                                                     */
/* DOT_MIDI_OUT_PORTS = 2 (playback/host→device)                       */
/* DOT_MIDI_IN_PORTS  = 1 (capture/device→host)                        */
/* ------------------------------------------------------------------ */

int
dg00x_create_midi(struct snd_dg00x *dg00x)
{
	int i;

	/* Create MIDI output (host → device) ports. */
	for (i = 0; i < DOT_MIDI_OUT_PORTS; i++) {
		dg00x->tx_midi[i] = midi_init(&dg00x_midi_class, dg00x);
		if (dg00x->tx_midi[i] == NULL)
			return (ENOMEM);
	}

	/* Create MIDI input (device → host) ports. */
	for (i = 0; i < DOT_MIDI_IN_PORTS; i++) {
		dg00x->rx_midi[i] = midi_init(&dg00x_midi_class, dg00x);
		if (dg00x->rx_midi[i] == NULL)
			return (ENOMEM);
	}

	return (0);
}
