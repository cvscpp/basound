// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _ALSA_PCM_BSD_H_
#define _ALSA_PCM_BSD_H_

#include <dev/sound/pcm/sound.h>
#include <sound/pcm.h>

struct basound_chan {
	struct snd_pcm_substream *substream;
	struct pcm_channel *channel;
	struct snd_dbuf *buffer;
	struct snd_pcm_runtime *runtime;
	uint32_t format;
	uint32_t speed;
	uint32_t blocksize;
	struct pcmchan_caps caps;
};

/* Must match BASOUND_DMA_BUFSIZE in alsa_card.c — the maximum single
 * DMA allocation the card-level DMA tag supports.  256 KB is sufficient
 * for the interleaved sndbuf staging buffer. */
#define BASOUND_DMA_BUFSIZE	(256 * 1024)

#endif /* _ALSA_PCM_BSD_H_ */
