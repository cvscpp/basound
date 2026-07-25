/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-hwdep.c - HWDEP device for Digidesign Digi 002/003
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <sound/core.h>
#include <sound/hwdep.h>

#include "digi00x.h"

int
dg00x_create_hwdep(struct snd_dg00x *dg00x)
{
	struct snd_hwdep *hwdep;
	int err;

	err = snd_hwdep_new(dg00x->card, "Digi00x", 0, &hwdep);
	if (err < 0)
		return (err);

	strcpy(hwdep->name, "Digi00x");
	hwdep->private_data = dg00x;

	return (0);
}
