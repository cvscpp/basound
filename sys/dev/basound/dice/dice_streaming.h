/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dice_streaming.h - FreeBSD-native ISO DMA streaming for DICE devices
 *
 * Implements isochronous streaming using FreeBSD firewire stack's
 * native DMA API (fw_open_isodma, xferq, irx_enable/itx_enable).
 *
 * AM824 format: each 32-bit data channel is big-endian on the wire,
 * with the top 24 bits carrying the audio sample.  MIDI rides in one
 * additional quadlet per data block after the PCM quadlets.
 *
 * Dual-wire quirk (non-MAudio, >96 kHz): two PCM frames packed into
 * one AMDTP data block.  MAudio devices set disable_double_pcm_frames.
 *
 * DICE register model:
 *   TX_ISOCHRONOUS  — FireWire isochronous channel for TX (capture)
 *   RX_ISOCHRONOUS  — FireWire isochronous channel for RX (playback)
 *   TX_SPEED        — FireWire bus speed for TX
 *   GLOBAL_ENABLE   — enable/disable both streams at once
 */

#ifndef _BASOUND_DICE_STREAMING_H_
#define _BASOUND_DICE_STREAMING_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

/* Forward declarations — dice_streaming.h does NOT include dice_bsd.h
 * to avoid pulling firewire.h/firewirereg.h twice (those headers lack
 * include guards). */

/* Forward declarations */
struct dice_bsd_softc;

/* ------------------------------------------------------------------ */
/* ISO DMA channel state                                                */
/* ------------------------------------------------------------------ */

#define DICE_ISO_NCHUNKS	32
#define DICE_ISO_PACKET_SIZE	2048
#define CIP_HEADER_QUADLETS	2
#define CIP_FMT_AM		0x10
#define DICE_ISO_TAG_CIP	(1 << 6)

/* Number of DICE isochronous streams per direction (DICE II/Jr/Mini:
 * max 2 pairs).  Defined here (not via dice_bsd.h) because this header
 * deliberately avoids including firewire headers. */
#ifndef MAX_DICE_STREAMS
#define MAX_DICE_STREAMS	2
#endif

struct dice_iso_channel {
	int		dmach;		/* -1 = not allocated */
	void		*xferq;		/* struct fw_xferq * */
	void		*fc;		/* struct firewire_comm * */
	void		*bulkxfer;	/* struct fw_bulkxfer * array */
	void		**mbufs;	/* struct mbuf ** array */
	void		*ctx;		/* backpointer to dice_bsd_softc */
	int		direction;	/* 0=playback(RX), 1=capture(TX) */
	int		stream_index;	/* DICE stream index carried by this
					 * channel (0 or 1).  DICE devices
					 * with more than 16 PCM channels
					 * (e.g. the M-Audio ProFire 2626:
					 * RX/TX 26 = 16 + 10) spread their
					 * channels over two isochronous
					 * streams, each with its own
					 * data-block layout, DBC sequence
					 * and isochronous channel.  The
					 * DICE Jr./Mini ASICs cap a single
					 * stream at 16 data channels, so
					 * packing e.g. 27 quadlets (26 PCM +
					 * MIDI) into one block makes the
					 * firmware walk off its expected
					 * 16-quadlet block boundaries —
					 * garbled/no audio on every output.
					 */
};

/* ------------------------------------------------------------------ */
/* Per-stream state (one for playback=RX=host→device,                  */
/*                    one for capture=TX=device→host)                   */
/* ------------------------------------------------------------------ */

/*
 * Sampling frequency codes for the CIP FDF field (AMDTP_FDF_AM824 | sfc).
 */
enum { DICE_SFC_32000=0, DICE_SFC_44100=1, DICE_SFC_48000=2,
       DICE_SFC_88200=3, DICE_SFC_96000=4, DICE_SFC_176400=5,
       DICE_SFC_192000=6 };

/*
 * Layout of ONE DICE isochronous stream (stream index 0 or 1) for one
 * direction.  The device's channel complement is split across streams:
 * the ProFire 2626 uses 16 channels on stream 0 and 10 on stream 1 at
 * <=48 kHz (plus MIDI on one of them).  `first_ch` is the index of this
 * stream's first channel within the app's interleaved frame (0 for
 * stream 0, stream0.pcm_chs for stream 1, ...).
 */
struct dice_stream_layout {
	unsigned int	pcm_chs;	/* PCM channels of this DICE stream */
	unsigned int	midi_ports;	/* MIDI ports of this DICE stream */
	unsigned int	data_block_quadlets; /* quadlets per block (pcm + midi) */
	unsigned int	first_ch;	/* first channel index in the interleaved frame */
	unsigned int	dbc;		/* per-stream CIP data block counter */
	unsigned long	rx_pos_bytes;	/* cumulative bytes decoded into the
					 * capture ring (per-stream; all
					 * streams advance in lockstep) */
};

struct dice_pcm_stream {
	unsigned long   hwptr;		/* current ring buffer position (bytes) */
	unsigned long   period_accum;	/* bytes since last period_elapsed */
	unsigned int    period_bytes;	/* bytes per period */
	unsigned int    buffer_bytes;	/* total ring buffer bytes */

	unsigned int    pcm_channels;	/* app-negotiated channel count */
	unsigned int    device_channels; /* full device channel complement per data block */
	unsigned int    midi_ports;	/* MIDI port count from detection */
	unsigned int    rate;		/* sample rate (Hz) */
	unsigned int    sfc;		/* CIP sampling frequency code */

	bool		active;		/* stream is active */
	int		direction;	/* SNDRV_PCM_STREAM_PLAYBACK or CAPTURE */
	bool		double_pcm_frames; /* dual-wire at >96 kHz */

	/* Per-DICE-stream AM824 data-block layouts (0..stream_count-1) */
	struct dice_stream_layout streams[MAX_DICE_STREAMS];
	unsigned int	stream_count;	/* number of active DICE streams for this
					 * direction at the current rate mode */

	/* CIP header state */
	unsigned int	fdf;		/* CIP FDF field */

	/* AMDTP fractional framing (non-blocking fallback) */
	unsigned int	frames_per_packet;
	unsigned int	frame_cycle;
	unsigned int	frame_remainder;

	/*
	 * IEC 61883-6 blocking mode with presentation timestamps.
	 *
	 * DICE firmware recovers the media clock from the SYT sequence of
	 * the host's playback stream (ALSA's dice driver is the only
	 * firewire driver that uses CIP_BLOCKING *without* CIP_UNAWARE_SYT).
	 * In blocking mode a packet carries either syt_interval data
	 * blocks with a valid SYT, or zero blocks and FDF=0xff (NODATA).
	 * These fields mirror amdtp-stream.c pool_ideal_syt_offsets() /
	 * compute_syt() / pool_blocking_data_blocks().
	 */
	unsigned int	syt_interval;	/* data blocks per SYT packet (8/16/32) */
	unsigned int	transfer_delay;	/* presentation-time lead, in ticks */
	unsigned int	last_syt_offset;
	unsigned int	syt_offset_state;
	unsigned int	tx_cycle;	/* 13-bit cycle of the next TX packet */
	bool		base_44100;	/* 44.1/88.2/176.4 kHz family */

	struct snd_pcm_substream *substream;

	/* Peak meter (TX only) */
	uint32_t	tx_peak[32];

	/* Debug */
	unsigned long	tx_underruns;
	unsigned long	tx_shortfalls;
	unsigned int	tx_dbg_budget;
};

/* ------------------------------------------------------------------ */
/* Streaming state embedded in the softc                                */
/* ------------------------------------------------------------------ */

struct dice_streaming {
	/* One ISO DMA context per DICE stream index (0..MAX_DICE_STREAMS-1).
	 * Devices whose direction uses a single stream (e.g. the Alesis
	 * iO26 RX: 8 channels) only ever enable iso_rx[0]/iso_tx[0]; the
	 * second context stays idle.  Dual-stream devices (M-Audio ProFire
	 * 2626: 16+10 channels) use both. */
	struct dice_iso_channel   iso_tx[MAX_DICE_STREAMS]; /* capture */
	struct dice_iso_channel   iso_rx[MAX_DICE_STREAMS]; /* playback */
	struct dice_pcm_stream    playback;
	struct dice_pcm_stream    capture;

	/* ISO resource allocation (fixed channels, one per stream index) */
	int	tx_channel[MAX_DICE_STREAMS];
	int	rx_channel[MAX_DICE_STREAMS];

	struct callout	callout;	/* shared TX refill + period signalling */
	unsigned int	active_streams;

	unsigned int	tx_use_count;
	unsigned int	rx_use_count;

	/* TX stall watchdog: consecutive 1 ms callout ticks during which
	 * no ISO chunk was recycled (the OHCI IT context has stopped
	 * completing packets, e.g. after a FireWire bus reset — fwohci
	 * halts all ISO contexts on reset and never restarts them, and
	 * the DICE firmware clears GLOBAL_ENABLE).  When the counter
	 * reaches the limit the whole stream is restarted. */
	unsigned int	tx_stall_ticks;
	bool		tx_restart;

	struct mtx	playback_lock;	/* serialises TX (playback) start/stop/refill */
	struct mtx	capture_lock;	/* serialises RX (capture) start/stop */

	bool		streaming_setup; /* ISO channels opened? */
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int  dice_streaming_init(struct dice_bsd_softc *sc);
void dice_streaming_fini(struct dice_bsd_softc *sc);
int  dice_streaming_start_playback(struct dice_bsd_softc *sc);
int  dice_streaming_start_capture(struct dice_bsd_softc *sc);
void dice_streaming_stop_playback(struct dice_bsd_softc *sc);
void dice_streaming_stop_capture(struct dice_bsd_softc *sc);
void dice_streaming_refill_tx(struct dice_bsd_softc *sc);

/* Utility */
unsigned int dice_rate_to_sfc(unsigned int rate);
void dice_build_cip_header(uint32_t *hdr, unsigned int node_id,
			   unsigned int dbs, unsigned int dbc,
			   unsigned int fmt, unsigned int fdf,
			   unsigned int syt);
unsigned int dice_frames_this_packet(struct dice_pcm_stream *ps);

/*
 * Build the per-stream AM824 data-block layouts for one direction from
 * the detected device config at the given sample rate.  Fills
 * ps->streams[], ps->stream_count, ps->device_channels and
 * ps->midi_ports.  Called by dice_stream_sync_from_channel() (dice_bsd.c)
 * and dice_clone_geometry() before stream start.
 */
void dice_stream_build_layout(struct dice_bsd_softc *sc,
			      struct dice_pcm_stream *ps, bool is_capture,
			      unsigned int rate);

#endif /* _BASOUND_DICE_STREAMING_H_ */
