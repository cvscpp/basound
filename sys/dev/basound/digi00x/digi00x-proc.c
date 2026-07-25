/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-proc.c - Proc interface for Digidesign Digi 002/003
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <sound/core.h>
#include <sound/info.h>

#include "digi00x.h"

static void
proc_read_clock(struct snd_info_entry *entry, void *buf)
{
	struct snd_dg00x *dg00x = entry->private_data;
	static const char *const source_name[] = {
		[SND_DG00X_CLOCK_INTERNAL] = "internal",
		[SND_DG00X_CLOCK_SPDIF] = "s/pdif",
		[SND_DG00X_CLOCK_ADAT] = "adat",
		[SND_DG00X_CLOCK_WORD] = "word clock",
	};
	unsigned int rate;
	enum snd_dg00x_clock clock;
	bool detect;

	if (dg00x_get_local_rate(dg00x, &rate) != 0)
		return;
	if (dg00x_get_clock(dg00x, &clock) != 0)
		return;

	printf("Sampling Rate: %u\n", rate);
	printf("Clock Source: %s\n", source_name[clock]);

	if (clock == SND_DG00X_CLOCK_INTERNAL)
		return;

	if (dg00x_check_external(dg00x, &detect) != 0)
		return;
	printf("External source: %s\n", detect ? "detected" : "not detected");
	if (!detect)
		return;

	if (dg00x_get_external_rate(dg00x, &rate) == 0)
		printf("External rate: %u\n", rate);
}

void
dg00x_proc_init(struct snd_dg00x *dg00x)
{
	struct snd_info_entry *entry;

	if (snd_card_proc_new(dg00x->card, "clock", &entry) == 0)
		snd_info_set_text_ops(entry, dg00x, proc_read_clock);
}
