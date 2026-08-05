// SPDX-License-Identifier: GPL-3.0-or-later
#include <sound/pcm.h>
#include <sound/memalloc.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <dev/sound/pcm/sound.h>
#include "alsa_pcm_bsd.h"

MALLOC_DECLARE(M_ALSA);

void
snd_pcm_period_elapsed(struct snd_pcm_substream *substream)
{
	struct basound_chan *ch = substream->private_data;

	if (ch && ch->channel)
		chn_intr(ch->channel);
}

static int
snd_pcm_new_stream(struct snd_pcm *pcm, int stream, int count)
{
	struct snd_pcm_str *pstr = &pcm->streams[stream];
	struct snd_pcm_substream *substream;
	int i;

	if (count <= 0)
		return 0;

	pstr->pcm = pcm;
	pstr->stream = stream;
	pstr->substream_count = count;
	pstr->substream = malloc(sizeof(*substream) * count, M_ALSA, M_WAITOK | M_ZERO);

	for (i = 0; i < count; i++) {
		substream = &pstr->substream[i];
		substream->pcm = pcm;
		substream->pstr = pstr;
		substream->number = i;
		substream->stream = stream;
		snprintf(substream->name, sizeof(substream->name), "sub%d", i);
	}

	return 0;
}

int
snd_pcm_new(struct snd_card *card, const char *id, int device,
	    int playback_count, int capture_count,
	    struct snd_pcm **rpcm)
{
	struct snd_pcm *pcm;

	pcm = malloc(sizeof(*pcm), M_ALSA, M_WAITOK | M_ZERO);
	if (pcm == NULL)
		return -ENOMEM;

	pcm->card = card;
	if (id)
		strlcpy(pcm->id, id, sizeof(pcm->id));

	snd_pcm_new_stream(pcm, SNDRV_PCM_STREAM_PLAYBACK, playback_count);
	snd_pcm_new_stream(pcm, SNDRV_PCM_STREAM_CAPTURE, capture_count);

	STAILQ_INSERT_TAIL(&card->pcm_list, pcm, next_pcm);

	if (rpcm)
		*rpcm = pcm;

	dev_info(card->dev, "Created PCM %s (%d playback, %d capture)\n",
		 pcm->id, playback_count, capture_count);
	return 0;
}

void
snd_pcm_set_ops(struct snd_pcm *pcm, int direction,
		const struct snd_pcm_ops *ops)
{
	struct snd_pcm_str *pstr = &pcm->streams[direction];
	pstr->ops = ops;
}

int
snd_pcm_lib_malloc_pages(struct snd_pcm_substream *substream, size_t size)
{
	struct snd_dma_buffer *dmab;
	int err;

	dmab = malloc(sizeof(*dmab), M_ALSA, M_WAITOK | M_ZERO);
	err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, substream->pcm->card->dev, size, dmab);
	if (err) {
		free(dmab, M_ALSA);
		return err;
	}

	substream->runtime->dma_area = dmab->area;
	substream->runtime->dma_addr = dmab->addr;
	substream->runtime->dma_bytes = size;
	substream->runtime->private_data = dmab;

	return 0;
}

int
snd_pcm_lib_free_pages(struct snd_pcm_substream *substream)
{
	struct snd_dma_buffer *dmab;
	
	if (substream->runtime == NULL)
		return 0;
		
	dmab = substream->runtime->private_data;

	if (dmab) {
		snd_dma_free_pages(dmab);
		free(dmab, M_ALSA);
		substream->runtime->private_data = NULL;
	}

	return 0;
}
