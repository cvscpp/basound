/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * digi00x.h - a part of driver for Digidesign Digi 002/003 family
 *
 * Register definitions for the Digi 002/003 Rack/Console.
 * Uses FreeBSD native firewire API (no Linux shim dependencies).
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 */

#ifndef SOUND_DIGI00X_H_INCLUDED
#define SOUND_DIGI00X_H_INCLUDED

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

/* firewirereg.h includes firewire.h - do not include both.
 * Forward-declare what we need from firewire. */
struct fw_device;

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/firewire.h>
#include <sound/hwdep.h>
#include <sound/rawmidi.h>
#include <linux/wait.h>

#include "amdtp-dot.h"

struct snd_dg00x;

/* In-memory state for DG isochronous resources */
struct dg00x_resources {
	int channel;		/* isochronous channel (-1 = unallocated) */
	int bandwidth;		/* bandwidth in units */
	int generation;		/* bus generation at allocation time */
};

/* Simple stream state */
struct dg00x_stream {
	bool running;
	unsigned int data_block_quadlets;
	unsigned int syt_interval;
	unsigned int pcm_channels;
	unsigned int midi_ports;
	unsigned int rate;
	struct dot_state state;
	struct snd_rawmidi_substream *midi[3];
	int midi_fifo_used[3];
	int midi_fifo_limit;
};

/* PCM streaming state — managed by the callout-based streaming engine */
struct dg00x_pcm_stream {
	struct callout  callout;       /* periodic timer for PCM timing */
	unsigned long   hwptr;         /* current buffer position (bytes) */
	unsigned int    period_bytes;  /* bytes per period */
	unsigned int    buffer_bytes;  /* total buffer bytes */
	unsigned int    pcm_channels;  /* negotiated channel count */
	unsigned int    rate;          /* sample rate */
	bool            active;        /* callout is running */
	struct dot_state dot;          /* DOT encoder/decoder state */
	int             direction;     /* SNDRV_PCM_STREAM_PLAYBACK or CAPTURE */
	struct snd_pcm_substream *substream; /* PCM substream for period_elapsed() */
};

struct snd_dg00x {
	struct snd_card *card;
	struct fw_device *fwdev;
	device_t dev;

	struct mtx lock;
	struct mtx mutex;

	struct dg00x_stream tx_stream;
	struct dg00x_resources tx_resources;

	struct dg00x_stream rx_stream;
	struct dg00x_resources rx_resources;

	/* Callout-based PCM streaming engines */
	struct dg00x_pcm_stream pcm_playback;
	struct dg00x_pcm_stream pcm_capture;

	unsigned int substreams_counter;

	/* for uapi */
	int dev_lock_count;
	bool dev_lock_changed;
	wait_queue_head_t hwdep_wait;

	/* Console models have additional MIDI ports. */
	bool is_console;

	/* Async message addr registered with device */
	uint64_t async_handler_offset;
	uint32_t msg;
};

/* Register address base for all Digi 00x devices */
#define DG00X_ADDR_BASE		0xffffe0000000ull

#define DG00X_OFFSET_STREAMING_STATE	0x0000
#define DG00X_OFFSET_STREAMING_SET	0x0004
#define DG00X_OFFSET_MESSAGE_ADDR	0x0014
#define DG00X_OFFSET_ISOC_CHANNELS	0x0100
#define DG00X_OFFSET_LOCAL_RATE		0x0110
#define DG00X_OFFSET_EXTERNAL_RATE	0x0114
#define DG00X_OFFSET_CLOCK_SOURCE	0x0118
#define DG00X_OFFSET_OPT_IFACE_MODE	0x011c
#define DG00X_OFFSET_DETECT_EXTERNAL	0x012c

enum snd_dg00x_rate {
	SND_DG00X_RATE_44100 = 0,
	SND_DG00X_RATE_48000,
	SND_DG00X_RATE_88200,
	SND_DG00X_RATE_96000,
	SND_DG00X_RATE_COUNT,
};

enum snd_dg00x_clock {
	SND_DG00X_CLOCK_INTERNAL = 0,
	SND_DG00X_CLOCK_SPDIF,
	SND_DG00X_CLOCK_ADAT,
	SND_DG00X_CLOCK_WORD,
	SND_DG00X_CLOCK_COUNT,
};

enum snd_dg00x_optical_mode {
	SND_DG00X_OPT_IFACE_MODE_ADAT = 0,
	SND_DG00X_OPT_IFACE_MODE_SPDIF,
	SND_DG00X_OPT_IFACE_MODE_COUNT,
};

/* Transaction helpers */
int dg00x_read_quad(struct fw_device *fwdev, uint64_t addr, uint32_t *val);
int dg00x_write_quad(struct fw_device *fwdev, uint64_t addr, uint32_t val);

/* Stream management */
extern const unsigned int snd_dg00x_stream_rates[SND_DG00X_RATE_COUNT];
extern const unsigned int snd_dg00x_stream_pcm_channels[SND_DG00X_RATE_COUNT];

int dg00x_get_local_rate(struct snd_dg00x *dg00x, unsigned int *rate);
int dg00x_set_local_rate(struct snd_dg00x *dg00x, unsigned int rate);
int dg00x_get_clock(struct snd_dg00x *dg00x, enum snd_dg00x_clock *clock);
int dg00x_check_external(struct snd_dg00x *dg00x, bool *detect);
int dg00x_get_external_rate(struct snd_dg00x *dg00x, unsigned int *rate);

/* Session stream control */
int dg00x_begin_session(struct snd_dg00x *dg00x, int tx_ch, int rx_ch);
void dg00x_finish_session(struct snd_dg00x *dg00x);

/* Async message handler */
int dg00x_register_message_handler(struct snd_dg00x *dg00x);
void dg00x_unregister_message_handler(struct snd_dg00x *dg00x);
int dg00x_reregister_message_handler(struct snd_dg00x *dg00x);

/* ISOC resource management */
int dg00x_alloc_isoc_resources(struct snd_dg00x *dg00x);
void dg00x_free_isoc_resources(struct snd_dg00x *dg00x);

/* PCM creation */
int dg00x_create_pcm(struct snd_dg00x *dg00x);

/* MIDI creation */
int dg00x_create_midi(struct snd_dg00x *dg00x);

/* HWDEP creation */
int dg00x_create_hwdep(struct snd_dg00x *dg00x);

/* Proc init */
void dg00x_proc_init(struct snd_dg00x *dg00x);

#endif
