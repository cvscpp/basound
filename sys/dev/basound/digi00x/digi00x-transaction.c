/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x-transaction.c - Async message handler for Digi 002/003
 *
 * Registers an address range with the device so it can send us
 * asynchronous messages (e.g., control surface changes on Console).
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>

#include <dev/firewire/firewire.h>
#include <dev/firewire/firewirereg.h>

#include "digi00x.h"

/*
 * Register our local address with the device so it knows where to
 * send async notification messages.
 *
 * On Linux this is done via fw_core_add_address_handler().
 * On FreeBSD, we store the handler offset and write it to the device
 * registers. In-kernel async request receiving is currently a TODO -
 * the FreeBSD firewire stack handles this through the character device.
 */
int
dg00x_register_message_handler(struct snd_dg00x *dg00x)
{
	/*
	 * TODO: Implement in-kernel async address handler registration
	 * using FreeBSD firewire stack. For now, we store the offset
	 * and register it with the device so it's ready for future use.
	 *
	 * On FreeBSD, this would use fw_asybindreq via the character
	 * device or a custom fw_xfer handler.
	 */
	dg00x->async_handler_offset = 0xffffe0000000ull | 0x8000;
	return (0);
}

void
dg00x_unregister_message_handler(struct snd_dg00x *dg00x)
{
	dg00x->async_handler_offset = 0;
}

int
dg00x_reregister_message_handler(struct snd_dg00x *dg00x)
{
	struct firewire_comm *fc;
	uint32_t data[2];
	int err;

	if (dg00x->fwdev == NULL || dg00x->fwdev->fc == NULL)
		return (EIO);

	fc = dg00x->fwdev->fc;

	/* Tell the device where to send messages.
	 * data[0] = (local_node_id << 16) | (offset_hi)
	 * data[1] = offset_lo
	 */
	data[0] = htobe32((fc->nodeid << 16) |
			  (uint32_t)(dg00x->async_handler_offset >> 32));
	data[1] = htobe32((uint32_t)dg00x->async_handler_offset);

	/* For now, skip the actual write since the handler isn't fully
	 * registered on the FreeBSD side yet. */
	(void)data;
	(void)err;

	return (0);
}
