// SPDX-License-Identifier: GPL-3.0-or-later
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sys/systm.h>
#include <machine/bus.h>

MALLOC_DEFINE(M_ALSA, "alsa", "ALSA shim memory");

/*
 * 256 KB per channel — plenty for the interleaved sndbuf staging buffer
 * that the FreeBSD PCM layer writes to.  The HDSP planar DMA buffers
 * are allocated separately in hdsp_alloc_dma_buffers (~1.2 MB each).
 * Using 4 MB here causes bus_dmamem_alloc to fail on systems with
 * fragmented DMA address space, especially after module reloads.
 */
#define BASOUND_DMA_BUFSIZE	(256 * 1024)

int
snd_card_new(struct device *parent, int idx, const char *xid,
	     struct module *module, int extra_size,
	     struct snd_card **card_ret)
{
	struct snd_card *card;

	card = malloc(sizeof(*card) + extra_size, M_ALSA, M_WAITOK | M_ZERO);
	if (card == NULL)
		return -ENOMEM;

	card->dev = parent;
	STAILQ_INIT(&card->pcm_list);
	STAILQ_INIT(&card->ctl_list);

	if (extra_size > 0)
		card->private_data = (void *)(card + 1);

	if (xid)
		strlcpy(card->id, xid, sizeof(card->id));

	/* Create a card-level DMA tag sized for channel buffers.
	 * bus_dmamem_alloc allocates dmatag->maxsize bytes, so we must
	 * set maxsize to the per-channel buffer size, not BUS_SPACE_MAXSIZE. */
	if (parent != NULL && parent->bsddev != NULL) {
		if (bus_dma_tag_create(
		    bus_get_dma_tag(parent->bsddev),	/* parent */
		    0x10000, 0,				/* align=64KB, boundary=none */
		    BUS_SPACE_MAXADDR_32BIT,		/* lowaddr */
		    BUS_SPACE_MAXADDR,			/* highaddr */
		    NULL, NULL,				/* filter */
		    BASOUND_DMA_BUFSIZE,			/* maxsize */
		    1,					/* nsegments */
		    BASOUND_DMA_BUFSIZE,			/* maxsegsz */
		    0, NULL, NULL,			/* flags, lock */
		    &card->dmatag) != 0)
			card->dmatag = NULL;
	}

	*card_ret = card;
	return 0;
}

int
snd_card_free(struct snd_card *card)
{
	struct snd_kcontrol *kctl, *kctl_tmp;

	if (card == NULL)
		return 0;

	/* device_delete_child triggers pcm_unregister → chn_kill → sndbuf_free,
	 * which uses card->dmatag.  Destroy the tag only after that completes. */
	if (card->pcm_dev != NULL) {
		device_t parent = device_get_parent(card->pcm_dev);
		device_delete_child(parent, card->pcm_dev);
		card->pcm_dev = NULL;
	}

	/* Clean up controls */
	STAILQ_FOREACH_SAFE(kctl, &card->ctl_list, next_ctl, kctl_tmp) {
		STAILQ_REMOVE(&card->ctl_list, kctl, snd_kcontrol, next_ctl);
		free(kctl, M_ALSA);
	}

	if (card->dmatag != NULL) {
		bus_dma_tag_destroy(card->dmatag);
		card->dmatag = NULL;
	}

	free(card, M_ALSA);
	return 0;
}

int
snd_card_register(struct snd_card *card)
{
	struct snd_pcm *pcm;

	STAILQ_FOREACH(pcm, &card->pcm_list, next_pcm) {
		basound_pcm_register(pcm);
	}

	basound_mixer_register(card);

	dev_info(card->dev, "Registered card %s (%s)\n", card->id, card->shortname);
	return 0;
}
