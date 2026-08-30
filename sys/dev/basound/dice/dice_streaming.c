/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dice_streaming.c - FreeBSD-native ISO DMA streaming for DICE devices
 *
 * DICE uses IEC 61883-6 AMDTP with AM824 data format (24-bit audio
 * in 32-bit big-endian containers).  MIDI rides in one extra quadlet
 * per data block after the PCM quadlets.
 *
 * Streaming state is allocated as sc->stream (pointer) to avoid
 * circular header dependencies between dice_bsd.h and this file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/libkern.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/mbuf.h>
#include <machine/bus.h>

#include <sys/lock.h>
#include <sys/mutex.h>

#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include "dice_bsd.h"
#include <dev/firewire/fwdma.h>
#include "dice_streaming.h"
#include "alsa_pcm_bsd.h"

MALLOC_DECLARE(M_ALSA);
MALLOC_DEFINE(M_DICE_ISO, "dice_iso", "DICE ISO DMA buffers");

/* Accessor macros for dice_iso_channel void* fields */
#define ISO_XFERQ(ch)	((struct fw_xferq *)(ch)->xferq)
#define ISO_FC(ch)	((struct firewire_comm *)(ch)->fc)
#define ISO_BX(ch)	((struct fw_bulkxfer *)(ch)->bulkxfer)
#define ISO_MB(ch)	((struct mbuf **)(ch)->mbufs)
#define DICE_SC(ch)	((struct dice_bsd_softc *)(ch)->ctx)
#define MAX_DICE_PCM_CH	   32

/* GLOBAL_CLOCK_SELECT / GLOBAL_STATUS field values (dice-interface.h). */
#define DICE_CLOCK_SOURCE_INTERNAL	0x0c
#define DICE_CLOCK_RATE_MASK		0x0000ff00
#define DICE_CLOCK_RATE_SHIFT		8

#define DSTREAM(sc)	((sc)->stream)

/* DEBUG (remove after diagnosis): count chunks in a queue. */
static unsigned int
dice_qcount(struct fw_xferq *x, int which)
{
	struct fw_bulkxfer *bx;
	struct fw_xferq *q = NULL;
	unsigned int n = 0;

	if (x == NULL)
		return (0);
	switch (which) {
	case 0: q = (struct fw_xferq *)&x->stfree; break;
	case 1: q = (struct fw_xferq *)&x->stdma; break;
	case 2: q = (struct fw_xferq *)&x->stvalid; break;
	}
	if (q == NULL)
		return (0);
	switch (which) {
	case 0: STAILQ_FOREACH(bx, &x->stfree, link) n++; break;
	case 1: STAILQ_FOREACH(bx, &x->stdma, link) n++; break;
	case 2: STAILQ_FOREACH(bx, &x->stvalid, link) n++; break;
	}
	return (n);
}

/*
 * TX stall watchdog limit (in 1 ms callout ticks).  While the stream is
 * active the OHCI completes ~8 packets per millisecond, so a healthy
 * context always has chunks to recycle.  If stfree stays empty for this
 * many consecutive ticks the context is dead (bus reset / DMA error)
 * and the whole stream must be restarted.
 */
#define DICE_TX_STALL_LIMIT	25

/* ------------------------------------------------------------------ */
/* Rate → SFC conversion                                               */
/* ------------------------------------------------------------------ */

unsigned int
dice_rate_to_sfc(unsigned int rate)
{
	switch (rate) {
	case  32000: return (DICE_SFC_32000);
	case  44100: return (DICE_SFC_44100);
	case  48000: return (DICE_SFC_48000);
	case  88200: return (DICE_SFC_88200);
	case  96000: return (DICE_SFC_96000);
	case 176400: return (DICE_SFC_176400);
	case 192000: return (DICE_SFC_192000);
	default:     return (DICE_SFC_48000);
	}
}

/* ------------------------------------------------------------------ */
/* IEC 61883-6 blocking mode + presentation timestamps                 */
/*                                                                     */
/* DICE devices recover their media clock from the SYT sequence of the */
/* host's playback stream; ALSA's dice driver is the only firewire     */
/* driver that uses CIP_BLOCKING *without* CIP_UNAWARE_SYT.  In        */
/* blocking mode each packet carries either syt_interval data blocks   */
/* plus a valid SYT, or zero data blocks with FDF=0xff / SYT=0xffff    */
/* (NODATA).  These helpers are direct ports of amdtp-stream.c         */
/* pool_ideal_syt_offsets()/compute_syt()/pool_blocking_data_blocks()  */
/* and the initial_state table in amdtp_stream_start().                */
/* ------------------------------------------------------------------ */

#define DICE_TICKS_PER_CYCLE	3072u	/* 125 us in 24.576 MHz ticks */
#define DICE_TICKS_PER_SECOND	24576000u
#define DICE_TRANSFER_DELAY_TICKS	0x2e00u /* 479.17 us */
#define DICE_SYT_NO_INFO	0xffffu
#define DICE_FDF_NO_DATA	0xffu

static unsigned int
dice_syt_interval(unsigned int rate)
{
	if (rate <= 48000)
		return (8);
	if (rate <= 96000)
		return (16);
	return (32);
}

/*
 * Initialise the blocking-mode sequence state for a stream.  rate is the
 * AMDTP stream rate, i.e. the sample rate halved when the dual-wire quirk
 * packs two PCM frames per data block at >96 kHz (ALSA keep_resources()).
 */
static void
dice_blocking_init(struct dice_pcm_stream *ps, unsigned int rate)
{
	ps->base_44100 = (rate == 44100 || rate == 88200 ||
			  rate == 176400);
	ps->syt_interval = dice_syt_interval(rate);
	ps->transfer_delay = DICE_TRANSFER_DELAY_TICKS - DICE_TICKS_PER_CYCLE +
			     (DICE_TICKS_PER_SECOND * ps->syt_interval) / rate;
	ps->last_syt_offset = DICE_TICKS_PER_CYCLE;
	if (ps->base_44100)
		ps->syt_offset_state = 67;
	else if (rate == 32000)
		ps->syt_offset_state = 3072;
	else
		ps->syt_offset_state = 1024;
}

/*
 * Generate the SYT offset (ticks from this packet's cycle to the next
 * presentation point) for one packet.  Returns DICE_SYT_NO_INFO for
 * empty cycles.  Port of amdtp-stream.c calculate_syt_offset().
 */
static unsigned int
dice_calculate_syt_offset(struct dice_pcm_stream *ps)
{
	unsigned int syt_offset;

	if (ps->last_syt_offset < DICE_TICKS_PER_CYCLE) {
		if (!ps->base_44100) {
			syt_offset = ps->last_syt_offset +
				     ps->syt_offset_state;
		} else {
			/*
			 * The time, in ticks, of the n'th SYT_INTERVAL
			 * sample is n * SYT_INTERVAL * 24576000 / rate.
			 * Modulo TICKS_PER_CYCLE the difference between
			 * successive elements is about 1386.23; rounding
			 * to SYT precision yields 1386 1386 1387 ...
			 * The 147-phase sequence reproduces it exactly.
			 */
			unsigned int phase = ps->syt_offset_state;
			unsigned int index = phase % 13;

			syt_offset = ps->last_syt_offset;
			syt_offset += 1386 + ((index && !(index & 3)) ||
					      phase == 146);
			if (++phase >= 147)
				phase = 0;
			ps->syt_offset_state = phase;
		}
	} else {
		syt_offset = ps->last_syt_offset - DICE_TICKS_PER_CYCLE;
	}
	ps->last_syt_offset = syt_offset;

	if (syt_offset >= DICE_TICKS_PER_CYCLE)
		syt_offset = DICE_SYT_NO_INFO;

	return (syt_offset);
}

/*
 * Turn an SYT offset and the packet's transmission cycle into the 16-bit
 * CIP SYT field (4-bit cycle + 12-bit offset).  Port of
 * amdtp-stream.c compute_syt().
 */
static unsigned int
dice_compute_syt(unsigned int syt_offset, unsigned int cycle,
		 unsigned int transfer_delay)
{
	unsigned int syt;

	syt_offset += transfer_delay;
	syt = ((cycle + syt_offset / DICE_TICKS_PER_CYCLE) << 12) |
	      (syt_offset % DICE_TICKS_PER_CYCLE);
	return (syt & DICE_SYT_NO_INFO);
}

/*
 * Determine the framing of the next TX packet (blocking mode): either
 * syt_interval data blocks with a valid SYT, or an empty NODATA packet.
 * Also advances the transmission-cycle counter (the OHCI IT context
 * transmits exactly one packet per 125 us cycle).
 */
static void
dice_blocking_framing(struct dice_pcm_stream *ps, unsigned int *data_blocks,
		      unsigned int *syt, bool *no_data)
{
	unsigned int syt_offset;

	syt_offset = dice_calculate_syt_offset(ps);
	if (syt_offset != DICE_SYT_NO_INFO) {
		*data_blocks = ps->syt_interval;
		*syt = dice_compute_syt(syt_offset, ps->tx_cycle,
					ps->transfer_delay);
		*no_data = false;
	} else {
		*data_blocks = 0;
		*syt = DICE_SYT_NO_INFO;
		*no_data = true;
	}
	ps->tx_cycle = (ps->tx_cycle + 1) & 0x1fff;
}

/*
 * Cycle number at which the first queued TX packet will be transmitted.
 * Mirrors fwohci_next_cycle() (CYCLE_DELAY 8, rounded up to 16) which
 * fwohci_itxbuf_enable() uses for the IT context's cycle match.
 */
static unsigned int
dice_first_tx_cycle(struct dice_bsd_softc *sc)
{
	struct firewire_comm *fc = sc->fwdev->fc;
	unsigned int cycle_now, cycle;

	cycle_now = (fc->cyctimer(fc) >> 12) & 0x7fff;
	cycle = cycle_now & 0x1fff;
	cycle += 8;
	if (cycle >= 8000)
		cycle -= 8000;
	cycle = roundup2(cycle, 16);
	if (cycle >= 8000)
		cycle = (cycle == 8000) ? 0 : 16;
	return (cycle & 0x1fff);
}

/* ------------------------------------------------------------------ */
/* CIP header builder                                                  */
/* ------------------------------------------------------------------ */

void
dice_build_cip_header(uint32_t *hdr, unsigned int node_id,
		      unsigned int dbs, unsigned int dbc,
		      unsigned int fmt, unsigned int fdf, unsigned int syt)
{
	hdr[0] = htobe32(
	    ((node_id & 0x3f) << 24) | ((dbs & 0xff) << 16) | (dbc & 0xff));
	hdr[1] = htobe32(
	    (1u << 31) | (((unsigned int)fmt & 0x3f) << 24) |
	    ((fdf & 0xff) << 16) | (syt & 0xffff));
}

/* ------------------------------------------------------------------ */
/* AMDTP fractional framing                                            */
/* ------------------------------------------------------------------ */

unsigned int
dice_frames_this_packet(struct dice_pcm_stream *ps)
{
	unsigned int phase;

	switch (ps->rate) {
	case 44100:
		phase = ps->frame_cycle;
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 80) ps->frame_cycle = 0;
		return (5 + ((phase & 1) ^
		    ((phase == 0 || phase >= 40) ? 1 : 0)));
	case 88200:
		phase = ps->frame_cycle;
		ps->frame_cycle = phase + 1;
		if (ps->frame_cycle >= 40) ps->frame_cycle = 0;
		return (11 + ((phase == 0) ? 1 : 0));
	default:
		return (ps->frames_per_packet);
	}
}

/* ------------------------------------------------------------------ */
/* ISO DMA channel open/close                                          */
/* ------------------------------------------------------------------ */

static int
dice_iso_open(struct firewire_comm *fc, struct dice_iso_channel *ch,
	      int is_tx, int stream_index)
{
	struct fw_xferq *xferq;
	int i;

	ch->dmach = fw_open_isodma(fc, is_tx);
	if (ch->dmach < 0) return (-ENOMEM);

	ch->fc = (void *)fc;
	ch->stream_index = stream_index;
	ch->direction = is_tx;
	xferq = is_tx ? fc->it[ch->dmach] : fc->ir[ch->dmach];
	ch->xferq = (void *)xferq;

	/*
	 * Reset the full software context state.  FWXFERQ_RUNNING in
	 * particular must be cleared: if a previous session left the
	 * OHCI context running (e.g. a module reload without a clean
	 * stop), fwohci_tx_enable()/fwohci_rx_enable() see RUNNING and
	 * return early without (re)programming the OHCI — the context
	 * stays stuck, TX chunks sit in stdma forever (hwptr frozen)
	 * and irx_enable() reports "IR DMA no free chunk".
	 */
	xferq->flag &= ~(FWXFERQ_MODEMASK | FWXFERQ_OPEN |
			 FWXFERQ_STREAM | FWXFERQ_CHTAGMASK |
			 FWXFERQ_RUNNING);
	xferq->flag |= FWXFERQ_OPEN | FWXFERQ_STREAM |
		       FWXFERQ_HANDLER | FWXFERQ_EXTBUF;
	xferq->psize = DICE_ISO_PACKET_SIZE;
	xferq->bnchunk = DICE_ISO_NCHUNKS;
	xferq->bnpacket = 1;
	xferq->queued = 0;
	xferq->sc = (caddr_t)ch;

	ch->bulkxfer = malloc(sizeof(struct fw_bulkxfer) * DICE_ISO_NCHUNKS,
	    M_DICE_ISO, M_NOWAIT | M_ZERO);
	if (ch->bulkxfer == NULL) return (-ENOMEM);
	ch->mbufs = malloc(sizeof(struct mbuf *) * DICE_ISO_NCHUNKS,
	    M_DICE_ISO, M_NOWAIT | M_ZERO);
	if (ch->mbufs == NULL) {
		free(ch->bulkxfer, M_DICE_ISO);
		ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	STAILQ_INIT(&xferq->stfree);
	STAILQ_INIT(&xferq->stdma);
	STAILQ_INIT(&xferq->stvalid);

	xferq->buf = fwdma_malloc_multiseg(fc,
	    DICE_ISO_PACKET_SIZE, DICE_ISO_PACKET_SIZE,
	    DICE_ISO_NCHUNKS, M_NOWAIT);
	if (xferq->buf == NULL) {
		free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
		free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL;
		return (-ENOMEM);
	}

	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		struct mbuf *m = NULL;

		/*
		 * Only the TX context has historically carried mbufs, and
		 * fwohci's TX path never looks at them.  For RX, leaving
		 * chunk->mbuf non-NULL selects fwohci's mbuf receive path
		 * (fwohci_irx_enable calls bus_dmamap_load_mbuf() on it),
		 * which DMAs received packets into the mbuf instead of the
		 * EXTBUF fwdma segments that dice_rx_handler() reads.
		 * Force the EXTBUF receive path for RX by keeping mbuf NULL.
		 */
		if (is_tx) {
			m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
			if (m == NULL) {
				int j;
				for (j = 0; j < i; j++) m_freem(ISO_MB(ch)[j]);
				free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
				free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL;
				fwdma_free_multiseg(xferq->buf); xferq->buf = NULL;
				return (-ENOMEM);
			}
			m->m_len = DICE_ISO_PACKET_SIZE;
			ISO_MB(ch)[i] = m;
			ISO_BX(ch)[i].mbuf = m;
			ISO_BX(ch)[i].start = mtod(m, caddr_t);
			ISO_BX(ch)[i].end = mtod(m, caddr_t) + DICE_ISO_PACKET_SIZE;
		} else {
			ISO_MB(ch)[i] = NULL;
			ISO_BX(ch)[i].mbuf = NULL;
			ISO_BX(ch)[i].start = NULL;
			ISO_BX(ch)[i].end = NULL;
		}
		ISO_BX(ch)[i].poffset = i;
		STAILQ_INSERT_TAIL(&xferq->stfree, &ISO_BX(ch)[i], link);
	}
	xferq->bulkxfer = ISO_BX(ch);
	return (0);
}

static void
dice_iso_close(struct dice_iso_channel *ch)
{
	struct fw_xferq *xferq = ISO_XFERQ(ch);
	int i;

	ch->dmach = -1;
	if (ch->mbufs) {
		for (i = 0; i < DICE_ISO_NCHUNKS; i++)
			if (ISO_MB(ch)[i]) m_freem(ISO_MB(ch)[i]);
		free(ch->mbufs, M_DICE_ISO); ch->mbufs = NULL;
	}
	if (ch->bulkxfer) { free(ch->bulkxfer, M_DICE_ISO); ch->bulkxfer = NULL; }
	if (xferq) {
		if (xferq->buf) { fwdma_free_multiseg(xferq->buf); xferq->buf = NULL; }
		xferq->flag &= ~(FWXFERQ_OPEN | FWXFERQ_HANDLER |
				 FWXFERQ_STREAM | FWXFERQ_EXTBUF);
		xferq->sc = NULL; xferq->hand = NULL;
	}
	ch->xferq = NULL; ch->fc = NULL;
}

/* ------------------------------------------------------------------ */
/* AM824 encode/decode                                                  */
/* ------------------------------------------------------------------ */

/*
 * AM824 multi-bit linear audio label.  The device discards (or treats as
 * non-audio) data channels without this label, which manifests as a
 * running ISO stream with no sound.  Linux amdtp-am824.c ORs the same bit
 * into every PCM data channel, including silence/padding.
 */
#define AM824_LABEL_PCM	0x40000000

static void
dice_encode_am824(uint32_t *dest, const int32_t *src, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++)
		dest[c] = htobe32(((uint32_t)src[c] >> 8) | AM824_LABEL_PCM);
}

/*
 * Encode ONE data block of ONE DICE stream from a full interleaved
 * frame row (`frame_row` holds nch samples of the frame).  The stream's
 * channels are frame_row[first_ch .. first_ch + pcm_chs); any of them
 * beyond the app's negotiated count are zero (AM824 label only, silence).
 *
 * The MIDI conformant-data quadlet (if the stream carries MIDI) follows
 * the PCM quadlets — the same layout ALSA's amdtp-am824.c produces
 * (pcm_positions[0..pcm_chs), midi_position = pcm_chs).
 */
static void
dice_encode_stream_block(uint32_t *blk, const int32_t *frame_row,
			 unsigned int nch, const struct dice_stream_layout *sl)
{
	unsigned int c;

	for (c = 0; c < sl->pcm_chs; c++) {
		uint32_t s = AM824_LABEL_PCM;
		unsigned int src = sl->first_ch + c;

		if (src < nch)
			s |= ((uint32_t)frame_row[src] >> 8);
		blk[c] = htobe32(s);
	}
	if (sl->midi_ports > 0)
		/* Empty MIDI conformant-data word (0x80 00 00 00 on the
		 * wire, big-endian). */
		blk[sl->pcm_chs] = htobe32(0x80000000);
}

static void
dice_decode_am824(int32_t *dest, const uint32_t *src, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++)
		dest[c] = (int32_t)(be32toh(src[c]) << 8);
}

/* ------------------------------------------------------------------ */
/* Float <-> S32 conversion (integer-only, no kernel FPU)              */
/* ------------------------------------------------------------------ */

/*
 * The AFMT_FLOAT path scales IEEE-754 single-precision samples to the
 * same 24-bit-in-32 scale the AM824 encoder consumes: full-scale ±1.0
 * maps to ±0x7fffffff, whose top 24 bits (>> 8) cover the full wire
 * range.
 *
 * Both conversion sites run in contexts where the FreeBSD kernel FPU
 * is NOT registered: the RX handler runs inside fwohci_task_dma while
 * FW_GLOCK is held, and the TX fill runs from the 1 ms PCM callout.
 * Executing any FPU instruction there panics with "Unregistered use of
 * FPU in kernel", so these helpers use pure integer bit manipulation.
 */
static inline int32_t
dice_f32_to_s32(uint32_t fbits)
{
	uint32_t sign = fbits >> 31;
	uint32_t exp = (fbits >> 23) & 0xff;
	uint32_t mant = fbits & 0x7fffff;
	uint32_t mag;
	int32_t val;

	if (exp == 0xff) {
		if (mant == 0)
			/* ±Inf: clamp like f >= 1.0f / f <= -1.0f. */
			val = (sign != 0) ? (-2147483647 - 1) : 2147483647;
		else
			/* NaN: cvttss2si semantics. */
			val = -2147483647 - 1;
	} else if (exp == 0) {
		/* ±0 and subnormals (< 2^-126) are below the S32 LSB. */
		val = 0;
	} else {
		uint32_t m = 0x800000 | mant;	/* 24-bit significand */
		int shift = (int)exp - 119;

		if (shift >= 8) {
			val = (sign != 0) ? (-2147483647 - 1) : 2147483647;
		} else if (shift >= 0) {
			mag = m << shift;
			val = (sign != 0) ? -(int32_t)mag : (int32_t)mag;
		} else {
			if (-shift >= 24)
				mag = 0;
			else
				mag = m >> -shift;
			val = (sign != 0) ? -(int32_t)mag : (int32_t)mag;
		}
	}
	return (val);
}

static inline uint32_t
dice_s32_to_f32(int32_t v)
{
	uint32_t sign, mag, exp, keep;
	int p;

	if (v == 0)
		return (0);

	sign = ((uint32_t)v >> 31) & 1;
	mag = (uint32_t)v;
	if (sign != 0)
		mag = 0u - mag;		/* |INT32_MIN| = 2^31 fits uint32 */

	p = fls(mag) - 1;		/* mag in [2^p, 2^(p+1)) */
	if (p > 23) {
		uint32_t shift = p - 23;
		uint32_t rem = mag & ((1u << shift) - 1);
		uint32_t half = 1u << (shift - 1);

		keep = mag >> shift;
		if (rem > half || (rem == half && (keep & 1)))
			keep++;
		if (keep == (1u << 24)) {
			keep >>= 1;
			p++;
		}
	} else {
		keep = mag << (23 - p);
	}

	exp = p + 96;
	return ((sign << 31) | (exp << 23) | (keep & 0x7fffff));
}

/* ------------------------------------------------------------------ */
/* DICE register helpers — use the proper dice_write_quad from dice_bsd.c */
/* ------------------------------------------------------------------ */

static int
dice_reg_write(struct dice_bsd_softc *sc, uint64_t addr, uint32_t val)
{
	return (dice_write_quad(sc->fwdev, addr, htobe32(val)));
}

static int
dice_program_iso(struct dice_bsd_softc *sc, int is_tx, unsigned int idx, int ch)
{
	uint64_t addr = DICE_PRIVATE_SPACE;
	uint32_t val = htobe32(ch >= 0 ? (uint32_t)ch : 0xffffffff);

	if (is_tx)
		addr += sc->tx_offset + idx * 0x20 + 0x008; /* TX_ISOCHRONOUS */
	else
		addr += sc->rx_offset + idx * 0x20 + 0x008; /* RX_ISOCHRONOUS */

	return (dice_write_quad(sc->fwdev, addr, val));
}

static int
dice_enable(struct dice_bsd_softc *sc, bool en)
{
	uint64_t addr = DICE_PRIVATE_SPACE + sc->global_offset + 0x050;
	uint32_t val = htobe32(en ? 1 : 0);
	return (dice_write_quad(sc->fwdev, addr, val));
}

/*
 * Set the device clock rate (ALSA select_clock()).  Without this the
 * device keeps whatever rate it was last set to while we stream CIP with
 * the FDF of the rate the app negotiated — the device never locks, no
 * audio, meter LEDs dark.
 *
 * The iO26 in the field was observed stuck at 48 kHz with GLOBAL_STATUS
 * bit 0 (SOURCE_LOCKED) clear and nominal rate 0x02 (48 kHz) even after
 * writing CLOCK_RATE_44100 — i.e. the selected clock source was external
 * (SPDIF/ADAT/AES) and unlocked, and the firmware refuses a rate change
 * it cannot lock.  ALSA keeps the user-selected source (it has a mixer
 * for it); we have none, so force the source to INTERNAL (always
 * available, always lockable) together with the rate.
 *
 * After the write, poll GLOBAL_STATUS until its nominal rate matches
 * (bounded, non-fatal).  Like ALSA, also accept a CLOCK_SELECT read-back
 * that already shows the new rate (write accepted, PLL still settling).
 */
static int
dice_set_rate(struct dice_bsd_softc *sc, unsigned int rate)
{
	uint32_t val, cur, status, rb;
	unsigned int sfc;
	int err, tries;

	err = dice_read_global(sc, 0x04c /* GLOBAL_CLOCK_SELECT */, &val, 4);
	if (err != 0)
		return (err);
	cur = be32toh(val);
	sfc = dice_rate_to_sfc(rate);
	cur = (cur & ~0x000000ff) | DICE_CLOCK_SOURCE_INTERNAL;	/* 0x0c */
	cur = (cur & ~DICE_CLOCK_RATE_MASK) | (sfc << DICE_CLOCK_RATE_SHIFT);
	err = dice_write_quad(sc->fwdev,
	    DICE_PRIVATE_SPACE + sc->global_offset + 0x04c, htobe32(cur));
	if (err != 0)
		return (err);

	status = 0;
	for (tries = 0; tries < 50; tries++) {
		err = dice_read_global(sc, 0x054 /* GLOBAL_STATUS */,
				       &status, 4);
		if (err != 0)
			break;
		if (((be32toh(status) & DICE_CLOCK_RATE_MASK) >>
		     DICE_CLOCK_RATE_SHIFT) == sfc)
			return (0);
		DELAY(10000);	/* 10 ms */
	}

	/* Write may have been accepted with the PLL still settling. */
	rb = 0;
	if (dice_read_global(sc, 0x04c, &rb, 4) == 0 &&
	    ((be32toh(rb) & DICE_CLOCK_RATE_MASK) >> DICE_CLOCK_RATE_SHIFT) == sfc)
		return (0);

	device_printf(sc->dev,
	    "dice: rate %u not confirmed on device (CLOCK_SELECT=0x%08x "
	    "GLOBAL_STATUS=0x%08x, err=%d)\n", rate,
	    err != 0 ? 0 : be32toh(rb), err != 0 ? 0 : be32toh(status), err);
	return (err);
}

/* ------------------------------------------------------------------ */
/* TX fill — encode PCM into the ISO chunks (playback direction)        */
/* ------------------------------------------------------------------ */

/* Shared per-slot framing for one TX packet (see dice_tx_framing_next). */
struct dice_tx_framing {
	unsigned int	frames;		/* data blocks in this packet (0 = NODATA) */
	unsigned int	syt;		/* CIP SYT field */
	bool		no_data;	/* FDF=0xff empty packet */
};

/*
 * Advance the blocking-mode SYT sequence once per refill slot.  Both
 * isochronous streams of a direction are transmitted on the same bus
 * cycles with the same media clock, so their packets must carry the
 * same frame count and presentation time — compute the framing once
 * here and hand the same values to every stream's packet builder.
 */
static void
dice_tx_framing_next(struct dice_pcm_stream *ps, struct dice_tx_framing *tf)
{
	unsigned int syt;

	dice_blocking_framing(ps, &tf->frames, &syt, &tf->no_data);
	tf->syt = syt;
}

/*
 * Fetch `frames` frames of `nch`-channel interleaved audio from the OSS
 * sndbuf at ps->hwptr, converting it to the S32-in-32 scale the AM824
 * encoder consumes.  S16_LE is shifted left 16 so the sample sits in the
 * top half of the 32-bit word (bits 31..16) — the AM824 encoder emits
 * the top 24 bits (>> 8), so a plain sign-extended sample (bits 15..0)
 * would land in the bits the hardware ignores and reduce 16-bit audio
 * to ~1/256 amplitude (JACK's default "-w 16" produced silence on the
 * HDSP until the identical shift-left-16 fix there).  AFMT_FLOAT is
 * converted with integer math (no kernel FPU in callout context).
 *
 * Advances hwptr/period_accum and bumps the underrun/shortfall
 * counters.  Returns the sample source: either the DMA ring directly
 * (fast path) or `scratch` (wrap / shortfall / underrun / conversion).
 * A NODATA packet (frames == 0) fetches nothing and leaves hwptr alone.
 */
static const int32_t *
dice_tx_fetch(struct dice_bsd_softc *sc, unsigned int frames,
	      unsigned int nch, unsigned int sample_bytes, bool is_float,
	      int32_t *scratch)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct basound_chan *txch = ps->substream ?
	    ps->substream->private_data : NULL;
	struct snd_dbuf *sb;
	unsigned int bytes, source_off;
	unsigned int ready_bytes = 0, pending_bytes = 0, read_bytes = 0;
	bool underrun = false;
	const int32_t *sp;
	uint8_t *dma;

	if (frames == 0)
		return (scratch);

	/*
	 * Re-sync the playback source with the OSS channel's live buffer
	 * and format on every slot.  chn_resizebuf() can remap/remalloc
	 * the sndbuf mid-stream; the runtime DMA view must track it.
	 */
	if (txch != NULL && txch->channel != NULL && txch->buffer != NULL) {
		if (txch->channel->format != 0)
			txch->format = txch->channel->format;
		if (ps->substream->runtime != NULL) {
			ps->substream->runtime->dma_area = txch->buffer->buf;
			ps->substream->runtime->dma_addr = txch->buffer->buf_addr;
			ps->substream->runtime->dma_bytes = txch->buffer->bufsize;
		}
		ps->buffer_bytes = txch->buffer->bufsize;
		ps->period_bytes = txch->blocksize;
		if (ps->buffer_bytes > 0)
			ps->hwptr %= ps->buffer_bytes;
	}

	sb = txch ? txch->buffer : NULL;
	bytes = frames * nch * sample_bytes;
	source_off = ps->hwptr;

	if (sb != NULL) {
		unsigned int rp = sndbuf_getreadyptr(sb);

		ready_bytes = sndbuf_getready(sb);
		if (source_off >= rp)
			pending_bytes = source_off - rp;
		else
			pending_bytes = ps->buffer_bytes - rp + source_off;

		if (pending_bytes > ready_bytes) {
			underrun = true; read_bytes = 0; ps->tx_underruns++;
		} else {
			read_bytes = ready_bytes - pending_bytes;
			if (read_bytes > bytes) read_bytes = bytes;
			if (read_bytes < bytes) ps->tx_shortfalls++;
		}
	} else {
		underrun = true; ps->tx_underruns++;
	}
	if (sample_bytes > 1)
		read_bytes -= read_bytes % sample_bytes;

	dma = (uint8_t *)ps->substream->runtime->dma_area;

	if (underrun) {
		memset(scratch, 0, frames * nch * sizeof(int32_t));
		sp = scratch;
	} else if (read_bytes >= bytes && sample_bytes == 4 && !is_float &&
	    source_off + bytes <= ps->buffer_bytes) {
		/* Fast path: full packet of S32_LE, no wrap. */
		sp = (const int32_t *)(dma + source_off);
	} else {
		unsigned int fi;
		unsigned int samples = frames * nch;

		for (fi = 0; fi < samples; fi++) scratch[fi] = 0;
		if (read_bytes > 0 && sample_bytes == 4) {
			unsigned int ns = read_bytes / 4;

			if (source_off + read_bytes <= ps->buffer_bytes)
				memcpy(scratch, dma + source_off, read_bytes);
			else {
				unsigned int first = ps->buffer_bytes - source_off;
				memcpy(scratch, dma + source_off, first);
				memcpy((uint8_t *)scratch + first, dma,
				    read_bytes - first);
			}
			if (is_float) {
				uint32_t *u = (uint32_t *)scratch;

				for (fi = 0; fi < ns; fi++)
					scratch[fi] = dice_f32_to_s32(u[fi]);
			}
		} else if (read_bytes > 0 && sample_bytes == 2) {
			unsigned int rd = read_bytes / 2;
			const int16_t *s16;
			/* Wrap-around scratch, sized for the worst case
			 * (32 frames × 32 ch × 2 bytes at 192 kHz). */
			uint8_t sbuf[MAX_DICE_PCM_CH * 12 * 2];

			if (source_off + read_bytes <= ps->buffer_bytes)
				s16 = (const int16_t *)(dma + source_off);
			else {
				unsigned int first = ps->buffer_bytes - source_off;
				memcpy(sbuf, dma + source_off, first);
				memcpy(sbuf + first, dma, read_bytes - first);
				s16 = (const int16_t *)sbuf;
			}
			for (fi = 0; fi < rd; fi++)
				scratch[fi] = ((int32_t)s16[fi]) << 16;
		}
		sp = scratch;
	}

	ps->hwptr += bytes;
	if (ps->hwptr >= ps->buffer_bytes) ps->hwptr = 0;
	ps->period_accum += bytes;
	return (sp);
}

/*
 * Build one ISO TX packet for DICE stream `idx` into chunk `bx`.
 *
 * dbs comes from the stream's own layout (16 quadlets for ProFire
 * stream 0, 10+MIDI for stream 1 — never the summed total: the DICE
 * firmware parses each stream with its own declared block size).  The
 * frame count and SYT are shared across the direction's streams
 * (dice_tx_framing_next); the DBC counter is per-stream.
 */
static void
dice_fill_tx_chunk(struct dice_bsd_softc *sc, unsigned int idx,
		   struct fw_xferq *xferq, struct fw_bulkxfer *bx,
		   const struct dice_tx_framing *tf, const int32_t *sp,
		   unsigned int nch)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct dice_stream_layout *sl = &ps->streams[idx];
	unsigned int dbs = sl->data_block_quadlets;
	unsigned int dbc, pkt_len, i;
	struct fw_pkt *fp;
	uint32_t *payload;

	dbc = sl->dbc;
	sl->dbc = (dbc + tf->frames) & 0xff;

	/* Empty packets still carry the 8-byte CIP header. */
	pkt_len = 8 + tf->frames * dbs * 4;

	fp = (struct fw_pkt *)fwdma_v_addr(xferq->buf, bx->poffset);
	if (fp == NULL)
		return;

	fp->mode.stream.len = pkt_len;
	payload = (uint32_t *)fp->mode.stream.payload;

	dice_build_cip_header(&payload[0], sc->fwdev->fc->nodeid,
	    dbs, dbc, CIP_FMT_AM,
	    tf->no_data ? DICE_FDF_NO_DATA : ps->fdf, tf->syt);

	/*
	 * Fill data blocks.  Each block takes this stream's channel
	 * slice (first_ch .. first_ch + pcm_chs) out of the full
	 * interleaved frame row; channels beyond the app's negotiated
	 * count are silence (AM824 label only).
	 *
	 * IMPORTANT: the block loop must use its own loop variable —
	 * the old code reused the frame index inside the sample
	 * conversion loops, filling only block 0 and reading past the
	 * end of the stack tmpbuf (audible garbage).
	 */
	for (i = 0; i < tf->frames; i++)
		dice_encode_stream_block(&payload[CIP_HEADER_QUADLETS + i * dbs],
		    &sp[i * nch], nch, sl);
}

/* ------------------------------------------------------------------ */
/* RX handler — decode ISO packets into PCM DMA buffer                  */
/* ------------------------------------------------------------------ */

/*
 * Write one DICE stream's decoded channel slice into the capture ring.
 *
 * `pos` is the byte offset (within buffer_bytes) of frame 0 of the
 * packet; each frame's slice is contiguous in the ring (channel
 * first_ch..first_ch+pcm_chs of the interleaved frame) unless it wraps
 * the end.  S32 passes through raw; S16 keeps the top half (the wire
 * carries 24 bits MSB-aligned, so the 16-bit audio sits in bits 31..16);
 * FLOAT is converted with integer math (no kernel FPU here).
 */
static void
dice_rx_write_slice(struct dice_pcm_stream *ps, uint8_t *dma,
		    const int32_t *tmp, unsigned int frames,
		    const struct dice_stream_layout *sl, unsigned int nch,
		    unsigned int sample_bytes, bool is_float,
		    unsigned long pos)
{
	unsigned int frame_bytes = nch * sample_bytes;
	unsigned int slice_bytes = sl->pcm_chs * sample_bytes;
	unsigned int fi;

	if (sample_bytes == 4 && !is_float) {
		for (fi = 0; fi < frames; fi++) {
			unsigned long base = pos + (unsigned long)fi *
			    frame_bytes + sl->first_ch * sample_bytes;
			const uint8_t *src = (const uint8_t *)
			    &tmp[fi * sl->pcm_chs];

			base %= ps->buffer_bytes;
			if (base + slice_bytes <= ps->buffer_bytes)
				memcpy(dma + base, src, slice_bytes);
			else {
				unsigned int first = ps->buffer_bytes - base;

				memcpy(dma + base, src, first);
				memcpy(dma, src + first, slice_bytes - first);
			}
		}
	} else {
		unsigned int c;

		for (fi = 0; fi < frames; fi++) {
			for (c = 0; c < sl->pcm_chs; c++) {
				unsigned long off = pos +
				    (unsigned long)fi * frame_bytes +
				    (sl->first_ch + c) * sample_bytes;
				uint8_t *dst = dma + (off % ps->buffer_bytes);
				int32_t v = tmp[fi * sl->pcm_chs + c];

				if (is_float) {
					uint32_t f = dice_s32_to_f32(v);
					memcpy(dst, &f, sizeof(f));
				} else if (sample_bytes == 2) {
					int16_t s16 = (int16_t)(v >> 16);
					memcpy(dst, &s16, sizeof(s16));
				} else {
					memcpy(dst, &v, sizeof(v));
				}
			}
		}
	}
}

/*
 * Per-channel RX completion handler.  Each DICE stream is received on
 * its own IR context; the handler decodes that stream's channel slice
 * out of every completed data block.
 *
 * Ring position: every stream of a direction carries the same frame
 * count per packet (blocking-mode SYT sequence), so each stream tracks
 * its own cumulative byte count (`sl->rx_pos_bytes`) and both counters
 * progress identically — every stream therefore writes its slice for
 * packet N at the same ring offset, producing a correctly interleaved
 * capture even though the two handlers run independently (serialised by
 * FW_GLOCK inside fwohci_task_dma).  The OSS-visible hwptr and period
 * accounting follow stream 0's counter.
 */
static void
dice_rx_handler(struct fw_xferq *xferq)
{
	struct dice_iso_channel *ch;
	struct dice_bsd_softc *sc;
	struct dice_pcm_stream *ps;
	struct dice_stream_layout *sl;
	struct fw_bulkxfer *bx;
	unsigned int idx, frames;
	int recycled = 0;

	if (xferq == NULL || xferq->sc == NULL) return;
	ch = (struct dice_iso_channel *)xferq->sc;
	if (ch->ctx == NULL) return;
	sc = DICE_SC(ch);
	ps = &DSTREAM(sc)->capture;
	idx = ch->stream_index;
	sl = &ps->streams[idx];

	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);

		if (ps->active && ps->substream != NULL &&
		    ps->substream->runtime != NULL &&
		    ps->substream->runtime->dma_area != NULL &&
		    xferq->buf != NULL && sl->pcm_chs > 0) {
			struct basound_chan *rxch = ps->substream->private_data;
			unsigned int sample_bytes, nch, dbs, fi;
			bool is_float;
			uint32_t *payload;
			int32_t tmpbuf[MAX_DICE_PCM_CH * 12];

			/*
			 * Re-sync the capture DMA target with the OSS
			 * channel's live buffer on every completed packet.
			 * chn_resizebuf() can remap/remalloc the hardware
			 * sndbuf after the ALSA runtime was set up; the
			 * OSS read path consumes ch->buffer, so write
			 * decoded samples there.
			 */
			if (rxch != NULL && rxch->buffer != NULL &&
			    rxch->channel != NULL) {
				if (rxch->channel->format != 0)
					rxch->format = rxch->channel->format;
				if (ps->substream->runtime != NULL) {
					ps->substream->runtime->dma_area =
					    rxch->buffer->buf;
					ps->substream->runtime->dma_addr =
					    rxch->buffer->buf_addr;
					ps->substream->runtime->dma_bytes =
					    rxch->buffer->bufsize;
				}
				ps->buffer_bytes = rxch->buffer->bufsize;
				ps->period_bytes = rxch->blocksize;
				if (ps->buffer_bytes > 0) {
					ps->hwptr %= ps->buffer_bytes;
					sl->rx_pos_bytes %= ps->buffer_bytes;
				}
			}

			sample_bytes = (rxch != NULL) ?
			    AFMT_BPS(rxch->format) : 4;
			if (sample_bytes != 2 && sample_bytes != 4)
				sample_bytes = 4;
			is_float = (rxch != NULL) &&
			    (AFMT_ENCODING(rxch->format) == AFMT_FLOAT);

			payload = (uint32_t *)fwdma_v_addr(xferq->buf,
			    bx->poffset);
			/*
			 * Blocking-mode capture: the device transmits either
			 * syt_interval data blocks (valid SYT) or empty
			 * NODATA packets (FDF=0xff) on the cycles between.
			 * The FDF field of the received CIP header tells
			 * which; data-block count is otherwise fixed.
			 */
			if (payload != NULL &&
			    ((be32toh(payload[1]) >> 16) & 0xff) ==
			    DICE_FDF_NO_DATA)
				frames = 0;
			else
				frames = ps->syt_interval;
			dbs = sl->data_block_quadlets;
			nch = ps->pcm_channels;
			if (nch > ps->device_channels)
				nch = ps->device_channels;

			if (frames > 0) {
				unsigned long pos = sl->rx_pos_bytes %
				    ps->buffer_bytes;
				unsigned int bytes = frames * nch *
				    sample_bytes;

				for (fi = 0; fi < frames; fi++) {
					const uint32_t *blk = &payload[
					    CIP_HEADER_QUADLETS + fi * dbs];

					/* PCM quadlets are first; MIDI (if
					 * any) follows after the audio. */
					dice_decode_am824(&tmpbuf[fi * sl->pcm_chs],
					    blk, sl->pcm_chs);
				}
				dice_rx_write_slice(ps,
				    (uint8_t *)ps->substream->runtime->dma_area,
				    tmpbuf, frames, sl, nch, sample_bytes,
				    is_float, pos);

				sl->rx_pos_bytes += bytes;
				sl->rx_pos_bytes %= ps->buffer_bytes;
				if (idx == 0) {
					/* hwptr is the OSS-visible capture
					 * position; both streams' counters
					 * advance in lockstep, so stream
					 * 0's alone drives it. */
					ps->hwptr = sl->rx_pos_bytes;
					ps->period_accum += bytes;
				}
			}
		}
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
		recycled++;
	}
	if (recycled > 0)
		ISO_FC(ch)->irx_enable(ISO_FC(ch), ch->dmach);
}

/* ------------------------------------------------------------------ */
/* TX refill + period signalling callout                                */
/* ------------------------------------------------------------------ */

static void
dice_pcm_stream_cb(void *arg)
{
	struct dice_bsd_softc *sc = arg;
	struct dice_pcm_stream *pb = &DSTREAM(sc)->playback;
	struct dice_pcm_stream *cap = &DSTREAM(sc)->capture;

	dice_streaming_refill_tx(sc);

	/* DEBUG (remove after diagnosis): periodic stream state. */
	{
		static unsigned int cb_ticks;
		struct fw_xferq *tx0 = ISO_XFERQ(&DSTREAM(sc)->iso_tx[0]);
		struct fw_xferq *tx1 = pb->stream_count > 1 ?
		    ISO_XFERQ(&DSTREAM(sc)->iso_tx[1]) : NULL;
		struct fw_xferq *rx0 = ISO_XFERQ(&DSTREAM(sc)->iso_rx[0]);

		if (++cb_ticks <= 2000 && (cb_ticks % 100 == 1)) {
			device_printf(sc->dev,
			    "dice: cb tick=%u pb=%d/%p cap=%d/%p "
			    "hwptr=%lu stall=%u "
			    "tx0(f=%u,d=%u,v=%u,r=%d) "
			    "tx1(f=%u,d=%u,v=%u,r=%d) "
			    "rx0(f=%u,d=%u,v=%u,r=%d)\n",
			    cb_ticks, pb->active, (void *)pb->substream,
			    cap->active, (void *)cap->substream,
			    pb->hwptr, DSTREAM(sc)->tx_stall_ticks,
			    dice_qcount(tx0, 0), dice_qcount(tx0, 1),
			    dice_qcount(tx0, 2),
			    (tx0->flag & FWXFERQ_RUNNING) != 0,
			    tx1 ? dice_qcount(tx1, 0) : 0,
			    tx1 ? dice_qcount(tx1, 1) : 0,
			    tx1 ? dice_qcount(tx1, 2) : 0,
			    tx1 ? (tx1->flag & FWXFERQ_RUNNING) != 0 : 0,
			    dice_qcount(rx0, 0), dice_qcount(rx0, 1),
			    dice_qcount(rx0, 2),
			    (rx0->flag & FWXFERQ_RUNNING) != 0);
		}
	}

	if (pb->active && pb->substream != NULL)
		snd_pcm_period_elapsed(pb->substream);
	if (cap->active && cap->substream != NULL)
		snd_pcm_period_elapsed(cap->substream);

	if (!pb->active && !cap->active) return;
	callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int
dice_streaming_init(struct dice_bsd_softc *sc)
{
	struct firewire_comm *fc;
	int err, i;

	if (sc->fwdev == NULL || sc->fwdev->fc == NULL) return (-ENODEV);
	if (sc->stream != NULL) return (0);

	sc->stream = malloc(sizeof(struct dice_streaming), M_DICE_ISO,
	    M_WAITOK | M_ZERO);
	if (sc->stream == NULL) return (-ENOMEM);

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		DSTREAM(sc)->iso_tx[i].dmach = -1;
		DSTREAM(sc)->iso_rx[i].dmach = -1;
	}

	fc = sc->fwdev->fc;

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		err = dice_iso_open(fc, &DSTREAM(sc)->iso_tx[i], 1, i);
		if (err < 0)
			goto fail;
		DSTREAM(sc)->iso_tx[i].ctx = sc;

		err = dice_iso_open(fc, &DSTREAM(sc)->iso_rx[i], 0, i);
		if (err < 0)
			goto fail;
		DSTREAM(sc)->iso_rx[i].ctx = sc;
		ISO_XFERQ(&DSTREAM(sc)->iso_rx[i])->hand = dice_rx_handler;
	}

	callout_init(&DSTREAM(sc)->callout, 1);
	mtx_init(&DSTREAM(sc)->playback_lock, "dice_playback", NULL, MTX_DEF);
	mtx_init(&DSTREAM(sc)->capture_lock, "dice_capture", NULL, MTX_DEF);
	/*
	 * The FreeBSD firewire stack exposes no isochronous channel
	 * allocator, so (like digi00x) use fixed channel numbers — one
	 * pair per DICE stream index.  RX is host->device (playback),
	 * TX is device->host (capture).
	 */
	DSTREAM(sc)->rx_channel[0] = 2;	/* playback stream 0 */
	DSTREAM(sc)->rx_channel[1] = 4;	/* playback stream 1 */
	DSTREAM(sc)->tx_channel[0] = 3;	/* capture stream 0 */
	DSTREAM(sc)->tx_channel[1] = 5;	/* capture stream 1 */
	return (0);

fail:
	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		if (DSTREAM(sc)->iso_tx[i].dmach >= 0)
			dice_iso_close(&DSTREAM(sc)->iso_tx[i]);
		if (DSTREAM(sc)->iso_rx[i].dmach >= 0)
			dice_iso_close(&DSTREAM(sc)->iso_rx[i]);
	}
	free(sc->stream, M_DICE_ISO);
	sc->stream = NULL;
	return (err);
}

void
dice_streaming_fini(struct dice_bsd_softc *sc)
{
	int i;

	if (sc->stream == NULL) return;

	callout_drain(&DSTREAM(sc)->callout);

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		if (DSTREAM(sc)->iso_tx[i].dmach >= 0) {
			ISO_FC(&DSTREAM(sc)->iso_tx[i])->itx_disable(
			    ISO_FC(&DSTREAM(sc)->iso_tx[i]),
			    DSTREAM(sc)->iso_tx[i].dmach);
			dice_iso_close(&DSTREAM(sc)->iso_tx[i]);
		}
		if (DSTREAM(sc)->iso_rx[i].dmach >= 0) {
			ISO_FC(&DSTREAM(sc)->iso_rx[i])->irx_disable(
			    ISO_FC(&DSTREAM(sc)->iso_rx[i]),
			    DSTREAM(sc)->iso_rx[i].dmach);
			dice_iso_close(&DSTREAM(sc)->iso_rx[i]);
		}
	}

	mtx_destroy(&DSTREAM(sc)->playback_lock);
	mtx_destroy(&DSTREAM(sc)->capture_lock);
	free(sc->stream, M_DICE_ISO);
	sc->stream = NULL;
}

static void
dice_stream_configure(struct dice_pcm_stream *ps, int dir, unsigned int rate)
{
	unsigned int eff_rate, i;

	ps->direction = dir;
	ps->sfc = dice_rate_to_sfc(rate);
	/*
	 * CIP FDF field: AMDTP_FDF_AM824 (0x00) | sfc — i.e. the plain
	 * sampling-frequency code (1..6 for 44.1..192 kHz).  The AM824
	 * format tag 0x10 belongs in the FMT field (bits 29-24 of CIP
	 * header quadlet 1), which dice_build_cip_header() already
	 * supplies separately.  OR-ing CIP_FMT_AM into FDF here produced
	 * 0x10|sfc (e.g. 0x12 at 48 kHz); the DICE firmware checks FDF
	 * (unlike the Digi 002/003's DOT firmware) and never recognised
	 * the stream format — the device did not lock, the meter LEDs
	 * stayed dark, and its own capture transmitter never started.
	 */
	ps->fdf = ps->sfc;
	ps->frame_cycle = 0;
	ps->frames_per_packet = rate / 8000;
	ps->frame_remainder = rate % 8000;
	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		ps->streams[i].dbc = 0;
		ps->streams[i].rx_pos_bytes = 0;
	}

	/* Blocking-mode SYT sequence (DICE media clock recovery). */
	eff_rate = rate;
	if (ps->double_pcm_frames && rate > 96000)
		eff_rate /= 2;	/* dual-wire: 2 PCM frames per data block */
	dice_blocking_init(ps, eff_rate);
}

/*
 * Build the per-stream AM824 data-block layouts for one direction from
 * the detected device config at the given sample rate.
 *
 * The device's channel complement is split over up to two isochronous
 * streams (the DICE II/Jr/Mini ASICs cap a single stream at 16 data
 * channels):
 *   ProFire 2626 @ <=48 kHz: stream 0 = 16 ch, stream 1 = 10 ch (+MIDI)
 *   iO26 RX @ <=48 kHz:      stream 0 = 8 ch,  stream 1 = 0 (unused)
 * Each stream's CIP DBS field and data blocks use ITS OWN channel count.
 * Packing the summed total (e.g. 27 quadlets for the ProFire) into one
 * stream makes the firmware walk off its expected 16-quadlet block
 * boundaries — the ProFire 2626 playback was garbled until this split.
 */
void
dice_stream_build_layout(struct dice_bsd_softc *sc,
			 struct dice_pcm_stream *ps, bool is_capture,
			 unsigned int rate)
{
	unsigned int mode_idx, first = 0, i;

	if (rate <= 48000)
		mode_idx = SND_DICE_RATE_MODE_LOW;
	else if (rate <= 96000)
		mode_idx = SND_DICE_RATE_MODE_MIDDLE;
	else
		mode_idx = SND_DICE_RATE_MODE_HIGH;

	ps->double_pcm_frames = !sc->cfg.disable_double_pcm_frames;
	ps->device_channels = 0;
	ps->midi_ports = 0;
	ps->stream_count = 0;

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		struct dice_stream_layout *sl = &ps->streams[i];
		unsigned int chs, midi;

		if (is_capture) {
			chs = sc->cfg.tx_pcm_chs[i][mode_idx];
			midi = sc->cfg.tx_midi_ports[i];
		} else {
			chs = sc->cfg.rx_pcm_chs[i][mode_idx];
			midi = sc->cfg.rx_midi_ports[i];
		}
		if (chs == 0 && midi == 0)
			continue;

		sl->pcm_chs = chs;
		sl->midi_ports = midi;
		sl->first_ch = first;
		sl->data_block_quadlets = chs + (midi > 0 ? 1 : 0);
		if (ps->double_pcm_frames && rate > 96000)
			sl->data_block_quadlets *= 2;	/* dual-wire */
		sl->dbc = 0;
		sl->rx_pos_bytes = 0;

		first += chs;
		ps->device_channels += chs;
		ps->midi_ports += midi;
		ps->stream_count++;
	}

	if (ps->stream_count == 0) {
		/* Detection returned nothing; fall back to a single
		 * stereo stream so the wire format stays valid. */
		ps->streams[0].pcm_chs = 2;
		ps->streams[0].midi_ports = 0;
		ps->streams[0].first_ch = 0;
		ps->streams[0].data_block_quadlets = 2;
		ps->streams[0].dbc = 0;
		ps->streams[0].rx_pos_bytes = 0;
		ps->device_channels = 2;
		ps->stream_count = 1;
	}
}

/*
 * When only one direction starts (e.g. capture-only for JACK), configure
 * the other stream so its complementary ISO context can run with valid
 * framing (silent packets).  DICE devices will not transmit their capture
 * stream unless the host is also transmitting to them ("No packets are
 * transmitted without receiving packets" — the same duplex rule as the
 * Digi 002/003), so the host TX context must keep running, silent if no
 * playback app is open.
 *
 * The two directions share the device clock (same rate/FDF), but each has
 * its own data-block layout: the device's RX (host->device) blocks carry
 * the RX channel complement and its TX (device->host) blocks the TX one.
 * The other stream's layout is therefore re-derived from the direction's
 * own channel map at the shared rate instead of copying the starting
 * stream's geometry.
 */
static void
dice_clone_geometry(struct dice_bsd_softc *sc, struct dice_pcm_stream *from)
{
	struct dice_pcm_stream *to = (from == &DSTREAM(sc)->playback) ?
	    &DSTREAM(sc)->capture : &DSTREAM(sc)->playback;
	int to_capture = (to == &DSTREAM(sc)->capture);

	to->rate = from->rate;
	to->sfc = from->sfc;
	to->fdf = from->fdf;
	to->pcm_channels = from->pcm_channels;

	dice_stream_build_layout(sc, to, to_capture, from->rate);
	if (to->pcm_channels > to->device_channels)
		to->pcm_channels = to->device_channels;

	to->frames_per_packet = from->frames_per_packet;
	to->frame_remainder = from->frame_remainder;
	to->frame_cycle = 0;

	/* Blocking-mode SYT sequence for the complementary stream. */
	{
		unsigned int eff_rate = to->rate;

		if (to->double_pcm_frames && to->rate > 96000)
			eff_rate /= 2;
		dice_blocking_init(to, eff_rate);
	}
}

/*
 * Program the device side of the streaming session: RX/TX isochronous
 * channels and TX speed for every stream index, then GLOBAL_ENABLE=1.
 * The DICE firmware latches the stream configuration when streaming is
 * (re)started, so this must run before the host DMA contexts are
 * enabled.  Only needed when the session transitions from idle.
 */
static int
dice_program_device(struct dice_bsd_softc *sc)
{
	unsigned int i;
	int err;

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		err = dice_program_iso(sc, 0, i, DSTREAM(sc)->rx_channel[i]);
		if (err != 0) {
			device_printf(sc->dev,
			    "dice: RX_ISOCHRONOUS[%u] write failed (%d)\n",
			    i, err);
			return (err);
		}
		err = dice_program_iso(sc, 1, i, DSTREAM(sc)->tx_channel[i]);
		if (err != 0) {
			device_printf(sc->dev,
			    "dice: TX_ISOCHRONOUS[%u] write failed (%d)\n",
			    i, err);
			return (err);
		}
		/* TX_SPEED is per stream index (ALSA start_streams). */
		err = dice_write_quad(sc->fwdev,
		    DICE_PRIVATE_SPACE + sc->tx_offset + i * 0x20 + 0x014,
		    htobe32(sc->fwdev->fc->speed));
		if (err != 0) {
			device_printf(sc->dev,
			    "dice: TX_SPEED[%u] write failed (%d)\n", i, err);
			return (err);
		}
	}
	err = dice_enable(sc, true);
	if (err != 0)
		device_printf(sc->dev, "dice: GLOBAL_ENABLE write failed (%d)\n",
		    err);
	return (err);
}

/*
 * Fill one TX slot: one packet per active playback stream, all carrying
 * the same blocking-mode framing (dice_tx_framing_next).  The caller
 * must hold FW_GLOCK.  Returns 1 when a slot was filled, 0 when any
 * active stream's stfree queue is empty.
 */
static int
dice_refill_tx_slot(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct dice_tx_framing tf;
	struct fw_xferq *xq[MAX_DICE_STREAMS];
	struct fw_bulkxfer *bx[MAX_DICE_STREAMS];
	int32_t scratch[MAX_DICE_PCM_CH * 12];
	const int32_t *sp;
	struct basound_chan *txch;
	unsigned int nch, sample_bytes, scount, i;
	bool is_float;

	scount = ps->stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return (0);

	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx[i];

		if (ch->dmach < 0)
			return (0);
		xq[i] = ISO_XFERQ(ch);
		bx[i] = STAILQ_FIRST(&xq[i]->stfree);
		if (bx[i] == NULL)
			return (0);
	}

	txch = ps->substream ? ps->substream->private_data : NULL;
	nch = ps->pcm_channels;
	if (txch != NULL) {
		unsigned int fmt_ch = AFMT_CHANNEL(txch->format);

		if (fmt_ch != 0 && fmt_ch <= ps->device_channels)
			nch = fmt_ch;
		ps->pcm_channels = nch;
	}
	if (nch > ps->device_channels)
		nch = ps->device_channels;
	sample_bytes = (txch != NULL) ? AFMT_BPS(txch->format) : 4;
	if (sample_bytes != 2 && sample_bytes != 4)
		sample_bytes = 4;
	is_float = (txch != NULL) &&
	    (AFMT_ENCODING(txch->format) == AFMT_FLOAT);

	dice_tx_framing_next(ps, &tf);
	sp = dice_tx_fetch(sc, tf.frames, nch, sample_bytes, is_float,
	    scratch);

	for (i = 0; i < scount; i++) {
		STAILQ_REMOVE_HEAD(&xq[i]->stfree, link);
		dice_fill_tx_chunk(sc, i, xq[i], bx[i], &tf, sp, nch);
		STAILQ_INSERT_TAIL(&xq[i]->stvalid, bx[i], link);
	}
	return (1);
}

/*
 * Ensure the host isochronous transmit contexts (playback direction) are
 * running.  Reference counted (rx_use_count): the first caller drains
 * and refills all chunks (silent when no playback app is open) and
 * enables the IT contexts; later callers just bump the count.
 */
static int
dice_ensure_host_tx(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *pb = &DSTREAM(sc)->playback;
	struct fw_xferq *xferq[MAX_DICE_STREAMS];
	struct firewire_comm *fc;
	uint32_t fv;
	unsigned int scount, i;
	int err;

	if (sc->stream == NULL)
		return (0);
	scount = pb->stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return (0);
	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx[i];

		if (ch->dmach < 0)
			return (0);
		xferq[i] = ISO_XFERQ(ch);
	}
	fc = ISO_FC(&DSTREAM(sc)->iso_tx[0]);
	if (fc == NULL || xferq[0] == NULL)
		return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	if (DSTREAM(sc)->rx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->playback_lock);
		return (0);
	}
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	/*
	 * The IT contexts are (re)armed at a fresh cycle match, so restart
	 * the blocking-mode SYT sequence and re-sync the transmission
	 * cycle counter with the bus.
	 */
	dice_stream_configure(pb, SNDRV_PCM_STREAM_PLAYBACK, pb->rate);
	pb->tx_cycle = dice_first_tx_cycle(sc);

	/* DEBUG (remove after diagnosis). */
	device_printf(sc->dev, "dice: ensure_tx streams=%u first_cycle=%u\n",
	    scount, pb->tx_cycle);

	FW_GLOCK(fc);
	for (i = 0; i < scount; i++) {
		STAILQ_CONCAT(&xferq[i]->stfree, &xferq[i]->stdma);
		STAILQ_CONCAT(&xferq[i]->stfree, &xferq[i]->stvalid);
	}
	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		if (dice_refill_tx_slot(sc) == 0)
			break;
	}
	FW_GUNLOCK(fc);

	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx[i];

		fv = DICE_ISO_TAG_CIP |
		    (DSTREAM(sc)->rx_channel[i] >= 0 ?
		     (DSTREAM(sc)->rx_channel[i] & 0x3f) : 0);
		xferq[i]->flag = (xferq[i]->flag & ~FWXFERQ_CHTAGMASK) | fv;
		err = fc->itx_enable(fc, ch->dmach);
		if (err != 0) {
			device_printf(sc->dev,
			    "dice: itx_enable[%u] failed (%d)\n", i, err);
			mtx_lock(&DSTREAM(sc)->playback_lock);
			DSTREAM(sc)->rx_use_count = 0;
			mtx_unlock(&DSTREAM(sc)->playback_lock);
			return (-err);
		}
	}
	DSTREAM(sc)->tx_stall_ticks = 0;
	DSTREAM(sc)->tx_restart = false;
	return (0);
}

/*
 * Ensure the host isochronous receive contexts (capture direction) are
 * running.  Reference counted (tx_use_count).  The device only starts
 * transmitting its capture stream once it receives the host's playback
 * stream, so these contexts are enabled for playback sessions too; the
 * RX handlers are no-ops while no capture app is active.
 */
static int
dice_ensure_host_rx(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *cap = &DSTREAM(sc)->capture;
	struct firewire_comm *fc;
	struct fw_xferq *xferq[MAX_DICE_STREAMS];
	struct fw_bulkxfer *bx;
	uint32_t fv;
	unsigned int scount, i;
	int err = 0;

	if (sc->stream == NULL)
		return (0);
	scount = cap->stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return (0);
	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx[i];

		if (ch->dmach < 0)
			return (0);
		xferq[i] = ISO_XFERQ(ch);
	}
	fc = ISO_FC(&DSTREAM(sc)->iso_rx[0]);
	if (fc == NULL || xferq[0] == NULL)
		return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->capture_lock);
	if (DSTREAM(sc)->tx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return (0);
	}
	mtx_unlock(&DSTREAM(sc)->capture_lock);

	/*
	 * Drain leftover chunks back to stfree before arming the IR
	 * contexts.  fwohci never drains the queues itself: on stop the
	 * chunks stay in stdma/stvalid, so a subsequent irx_enable()
	 * finds stfree empty and prints "IR DMA no free chunk" and
	 * never arms the context (capture dead — jackd's
	 * "Discard error bytes read = -1").  Mirror digi00x_start_rx().
	 */
	FW_GLOCK(fc);
	for (i = 0; i < scount; i++) {
		while ((bx = STAILQ_FIRST(&xferq[i]->stdma)) != NULL) {
			STAILQ_REMOVE_HEAD(&xferq[i]->stdma, link);
			STAILQ_INSERT_TAIL(&xferq[i]->stfree, bx, link);
		}
		while ((bx = STAILQ_FIRST(&xferq[i]->stvalid)) != NULL) {
			STAILQ_REMOVE_HEAD(&xferq[i]->stvalid, link);
			STAILQ_INSERT_TAIL(&xferq[i]->stfree, bx, link);
		}
	}
	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx[i];

		fv = DICE_ISO_TAG_CIP |
		    (DSTREAM(sc)->tx_channel[i] >= 0 ?
		     (DSTREAM(sc)->tx_channel[i] & 0x3f) : 0);
		xferq[i]->flag = (xferq[i]->flag & ~FWXFERQ_CHTAGMASK) | fv;
		err = fc->irx_enable(fc, ch->dmach);
		if (err != 0)
			break;
	}
	FW_GUNLOCK(fc);
	if (err != 0) {
		device_printf(sc->dev, "dice: irx_enable[%u] failed (%d)\n",
		    i, err);
		mtx_lock(&DSTREAM(sc)->capture_lock);
		DSTREAM(sc)->tx_use_count = 0;
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return (-err);
	}
	return (0);
}

static void
dice_release_host_tx(struct dice_bsd_softc *sc)
{
	struct firewire_comm *fc;
	unsigned int i, scount;

	if (sc->stream == NULL)
		return;
	scount = DSTREAM(sc)->playback.stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return;
	fc = ISO_FC(&DSTREAM(sc)->iso_tx[0]);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	if (DSTREAM(sc)->rx_use_count == 0) {
		mtx_unlock(&DSTREAM(sc)->playback_lock);
		return;
	}
	if (--DSTREAM(sc)->rx_use_count > 0) {
		mtx_unlock(&DSTREAM(sc)->playback_lock);
		return;
	}
	DSTREAM(sc)->tx_stall_ticks = 0;
	DSTREAM(sc)->tx_restart = false;
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	for (i = 0; i < scount; i++)
		fc->itx_disable(fc, DSTREAM(sc)->iso_tx[i].dmach);
}

static void
dice_release_host_rx(struct dice_bsd_softc *sc)
{
	struct firewire_comm *fc;
	unsigned int i, scount;

	if (sc->stream == NULL)
		return;
	scount = DSTREAM(sc)->capture.stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return;
	fc = ISO_FC(&DSTREAM(sc)->iso_rx[0]);

	mtx_lock(&DSTREAM(sc)->capture_lock);
	if (DSTREAM(sc)->tx_use_count == 0) {
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return;
	}
	if (--DSTREAM(sc)->tx_use_count > 0) {
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return;
	}
	mtx_unlock(&DSTREAM(sc)->capture_lock);

	for (i = 0; i < scount; i++)
		fc->irx_disable(fc, DSTREAM(sc)->iso_rx[i].dmach);
}

int
dice_streaming_start_playback(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	int err;

	if (sc->stream == NULL)
		return (0);

	dice_stream_configure(ps, SNDRV_PCM_STREAM_PLAYBACK, ps->rate);
	dice_clone_geometry(sc, ps);

	/* Always (re)select the clock; the register may be changed while
	 * streams run and the device keeps its last value otherwise. */
	err = dice_set_rate(sc, ps->rate);
	if (err != 0)
		device_printf(sc->dev, "dice: failed to set rate %u (%d)\n",
		    ps->rate, err);

	/* EAP devices (ProFire 2626) need their internal router set up so
	 * FireWire playback actually reaches the physical outputs. */
	if (sc->cfg.setup_router != NULL) {
		err = sc->cfg.setup_router(sc, ps->rate);
		if (err != 0)
			return (-err);
	}

	/* Program the device only when the session leaves idle. */
	if (DSTREAM(sc)->active_streams == 0) {
		err = dice_program_device(sc);
		if (err != 0)
			return (-err);
	}

	ps->active = true;
	err = dice_ensure_host_tx(sc);
	if (err != 0) {
		ps->active = false;
		return (err);
	}
	err = dice_ensure_host_rx(sc);
	if (err != 0) {
		ps->active = false;
		dice_release_host_tx(sc);	/* undo the TX claim */
		return (err);
	}

	if (DSTREAM(sc)->active_streams == 0)
		callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
	DSTREAM(sc)->active_streams++;
	device_printf(sc->dev, "dice: playback start rate=%u pcm_ch=%u "
	    "dev_ch=%u streams=%u fdf=0x%02x\n", ps->rate, ps->pcm_channels,
	    ps->device_channels, ps->stream_count, ps->fdf);
	return (0);
}

int
dice_streaming_start_capture(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->capture;
	int err;

	if (sc->stream == NULL)
		return (0);

	dice_stream_configure(ps, SNDRV_PCM_STREAM_CAPTURE, ps->rate);
	dice_clone_geometry(sc, ps);

	err = dice_set_rate(sc, ps->rate);
	if (err != 0)
		device_printf(sc->dev, "dice: failed to set rate %u (%d)\n",
		    ps->rate, err);

	if (sc->cfg.setup_router != NULL) {
		err = sc->cfg.setup_router(sc, ps->rate);
		if (err != 0)
			return (-err);
	}

	if (DSTREAM(sc)->active_streams == 0) {
		err = dice_program_device(sc);
		if (err != 0)
			return (-err);
	}

	ps->active = true;
	/* The device will not transmit its capture stream until it is
	 * receiving the host's playback stream, so the host TX context
	 * must be running (silent) even though no playback app is open. */
	err = dice_ensure_host_tx(sc);
	if (err != 0) {
		ps->active = false;
		return (err);
	}
	err = dice_ensure_host_rx(sc);
	if (err != 0) {
		ps->active = false;
		dice_release_host_tx(sc);	/* undo the TX claim */
		return (err);
	}

	if (DSTREAM(sc)->active_streams == 0)
		callout_reset(&DSTREAM(sc)->callout, 1, dice_pcm_stream_cb, sc);
	DSTREAM(sc)->active_streams++;
	device_printf(sc->dev, "dice: capture start rate=%u pcm_ch=%u "
	    "dev_ch=%u streams=%u fdf=0x%02x\n", ps->rate, ps->pcm_channels,
	    ps->device_channels, ps->stream_count, ps->fdf);
	return (0);
}

void
dice_streaming_stop_playback(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps;

	if (sc->stream == NULL)
		return;
	ps = &DSTREAM(sc)->playback;
	if (!ps->active)
		return;

	ps->active = false;
	/* Both ISO contexts are shared between the directions (duplex
	 * session); release this direction's claim on each. */
	dice_release_host_tx(sc);
	dice_release_host_rx(sc);

	if (DSTREAM(sc)->active_streams > 0)
		DSTREAM(sc)->active_streams--;
	if (DSTREAM(sc)->active_streams == 0)
		dice_enable(sc, false);
}

void
dice_streaming_stop_capture(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps;

	if (sc->stream == NULL)
		return;
	ps = &DSTREAM(sc)->capture;
	if (!ps->active)
		return;

	ps->active = false;
	dice_release_host_tx(sc);
	dice_release_host_rx(sc);

	if (DSTREAM(sc)->active_streams > 0)
		DSTREAM(sc)->active_streams--;
	if (DSTREAM(sc)->active_streams == 0)
		dice_enable(sc, false);
}

/*
 * Restart a stalled TX stream.
 *
 * Called from the refill watchdog when the OHCI IT contexts have stopped
 * completing packets (e.g. a FireWire bus reset — fwohci halts all ISO
 * contexts on reset and never restarts them, and the DICE firmware
 * clears GLOBAL_ENABLE).  Nothing would ever be recycled into stfree
 * again, so the sndbuf fills up and the app's write() blocks forever.
 *
 * Sequence, mirroring start_playback:
 *   1. re-program the device (rate + RX/TX channels + enable) — fwmem
 *      transactions may tsleep up to 5s each, so no locks are held;
 *   2. itx_disable() the dead contexts — their internal pause() sleeps
 *      1s, again with no locks held;
 *   3. under playback_lock + FW_GLOCK, drain every chunk back to stfree,
 *      refill, and itx_enable() to rebuild the DMA descriptors.
 *
 * hwptr is deliberately NOT reset: the sound layer disposes the sndbuf
 * based on the hwptr delta reported by pcm_pointer(), so moving it
 * backwards would double-play buffered audio.
 */
static void
dice_streaming_restart_tx(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct firewire_comm *fc;
	struct fw_xferq *xferq[MAX_DICE_STREAMS];
	unsigned int scount, i;
	int refilled = 0;

	if (sc->stream == NULL || !ps->active) {
		if (sc->stream != NULL) {
			DSTREAM(sc)->tx_restart = false;
			DSTREAM(sc)->tx_stall_ticks = 0;
		}
		return;
	}
	scount = ps->stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS)
		return;
	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx[i];

		if (ch->dmach < 0)
			return;
		xferq[i] = ISO_XFERQ(ch);
	}
	fc = ISO_FC(&DSTREAM(sc)->iso_tx[0]);
	if (fc == NULL)
		return;

	device_printf(sc->dev, "dice: TX DMA stalled, restarting stream\n");

	/* Device side. */
	(void)dice_set_rate(sc, ps->rate);
	(void)dice_program_device(sc);

	/* Host side: tear down the dead contexts (pause 1s inside). */
	for (i = 0; i < scount; i++)
		fc->itx_disable(fc, DSTREAM(sc)->iso_tx[i].dmach);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	FW_GLOCK(fc);
	for (i = 0; i < scount; i++) {
		STAILQ_CONCAT(&xferq[i]->stfree, &xferq[i]->stdma);
		STAILQ_CONCAT(&xferq[i]->stfree, &xferq[i]->stvalid);
	}

	/* Fresh blocking-mode sequence; the rearmed IT contexts start at
	 * a new cycle match, so re-sync the transmission cycle too. */
	dice_stream_configure(ps, SNDRV_PCM_STREAM_PLAYBACK, ps->rate);
	ps->tx_cycle = dice_first_tx_cycle(sc);

	while (dice_refill_tx_slot(sc) > 0 && refilled++ < DICE_ISO_NCHUNKS)
		;
	FW_GUNLOCK(fc);
	for (i = 0; i < scount; i++)
		fc->itx_enable(fc, DSTREAM(sc)->iso_tx[i].dmach);
	DSTREAM(sc)->tx_stall_ticks = 0;
	DSTREAM(sc)->tx_restart = false;
	mtx_unlock(&DSTREAM(sc)->playback_lock);
}

void
dice_streaming_refill_tx(struct dice_bsd_softc *sc)
{
	struct dice_pcm_stream *ps;
	struct fw_xferq *xferq[MAX_DICE_STREAMS];
	struct firewire_comm *fc;
	unsigned int scount, i;
	bool all_empty;
	int refilled = 0;

	if (sc->stream == NULL)
		return;
	ps = &DSTREAM(sc)->playback;
	scount = ps->stream_count;
	if (scount == 0 || scount > MAX_DICE_STREAMS ||
	    DSTREAM(sc)->rx_use_count == 0)
		return;
	for (i = 0; i < scount; i++) {
		struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx[i];

		if (ch->dmach < 0)
			return;
		xferq[i] = ISO_XFERQ(ch);
	}
	fc = ISO_FC(&DSTREAM(sc)->iso_tx[0]);
	if (fc == NULL)
		return;

	/*
	 * Keep refilling whenever the IT contexts are running, even if no
	 * playback app is open (capture-only session): dice_tx_fetch
	 * produces silent, correctly framed packets for that case.  The
	 * old gate on ps->substream/runtime/dma_area stopped the refill
	 * dead whenever playback wasn't open, so a capture-only session
	 * never fed the device and the device never transmitted.
	 */
	mtx_lock(&DSTREAM(sc)->playback_lock);
	FW_GLOCK(fc);
	while (dice_refill_tx_slot(sc) > 0 &&
	    refilled++ < DICE_ISO_NCHUNKS)
		;
	FW_GUNLOCK(fc);
	if (refilled > 0) {
		DSTREAM(sc)->tx_stall_ticks = 0;
		for (i = 0; i < scount; i++)
			fc->itx_enable(fc, DSTREAM(sc)->iso_tx[i].dmach);
	} else {
		/*
		 * No slot was filled this tick.  Distinguish a genuine
		 * stall (ALL active streams' queues empty — the OHCI IT
		 * contexts stopped completing packets, e.g. after a bus
		 * reset) from a transient imbalance where one stream's
		 * queue has chunks but another's is momentarily empty
		 * (the paired fill stops at the first empty queue).
		 */
		all_empty = true;
		for (i = 0; i < scount; i++) {
			if (STAILQ_FIRST(&xferq[i]->stfree) != NULL) {
				all_empty = false;
				break;
			}
		}
		if (all_empty &&
		    ++DSTREAM(sc)->tx_stall_ticks >= DICE_TX_STALL_LIMIT) {
			/*
			 * A healthy context completes ~8 packets/ms, so a
			 * whole limit window with no recycled chunk means
			 * the contexts are dead.  Defer the restart until
			 * after playback_lock is dropped: it sleeps
			 * (itx_disable pause, fwmem timeouts) and must not
			 * run with the mutex held.
			 */
			DSTREAM(sc)->tx_stall_ticks = 0;
			DSTREAM(sc)->tx_restart = true;
		}
	}
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	if (DSTREAM(sc)->tx_restart)
		dice_streaming_restart_tx(sc);
}
