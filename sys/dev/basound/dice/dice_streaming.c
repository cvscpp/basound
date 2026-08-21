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
dice_iso_open(struct firewire_comm *fc, struct dice_iso_channel *ch, int is_tx)
{
	struct fw_xferq *xferq;
	int i;

	ch->dmach = fw_open_isodma(fc, is_tx);
	if (ch->dmach < 0) return (-ENOMEM);

	ch->fc = (void *)fc;
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

static void
dice_encode_am824_padded(uint32_t *dest, const int32_t *src,
			 unsigned int src_channels, unsigned int channels)
{
	unsigned int c;
	for (c = 0; c < channels; c++) {
		uint32_t s = AM824_LABEL_PCM;

		if (c < src_channels)
			s |= ((uint32_t)src[c] >> 8);
		dest[c] = htobe32(s);
	}
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
/* TX fill — encode PCM into one ISO chunk (playback direction)         */
/* ------------------------------------------------------------------ */

static void
dice_fill_tx_chunk(struct dice_bsd_softc *sc, struct fw_xferq *xferq,
		   struct fw_bulkxfer *bx)
{
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	unsigned int dbs = ps->data_block_quadlets;
	unsigned int nch, frames, dbc, bytes, pkt_len;
	struct fw_pkt *fp;
	uint32_t *payload;
	unsigned int i;
	struct basound_chan *txch = ps->substream ?
	    ps->substream->private_data : NULL;
	unsigned int sample_bytes;
	bool is_float;

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
		if (ps->buffer_bytes > 0) ps->hwptr %= ps->buffer_bytes;
	}

	nch = ps->pcm_channels;
	if (txch != NULL) {
		unsigned int fmt_ch = AFMT_CHANNEL(txch->format);
		if (fmt_ch != 0 && fmt_ch <= ps->device_channels) {
			nch = fmt_ch; ps->pcm_channels = nch;
		}
	}
	if (nch > ps->device_channels) nch = ps->device_channels;

	/*
	 * Blocking-mode framing: either syt_interval data blocks with a
	 * computed SYT presentation time, or an empty NODATA packet
	 * (FDF=0xff, SYT=0xffff).  DICE devices recover their media
	 * clock from this sequence; ALSA's dice driver is the only
	 * firewire driver that is both blocking and SYT-aware.
	 */
	{
		unsigned int syt;
		bool no_data;

		dice_blocking_framing(ps, &frames, &syt, &no_data);
		dbc = ps->tx_dbc;
		ps->tx_dbc = (dbc + frames) & 0xff;

		sample_bytes = (txch != NULL) ? AFMT_BPS(txch->format) : 4;
		if (sample_bytes != 2 && sample_bytes != 4) sample_bytes = 4;
		is_float = (txch != NULL) &&
		    (AFMT_ENCODING(txch->format) == AFMT_FLOAT);

		bytes = frames * nch * sample_bytes;
		/* Empty packets still carry the 8-byte CIP header. */
		pkt_len = 8 + frames * dbs * 4;

		fp = (struct fw_pkt *)fwdma_v_addr(xferq->buf, bx->poffset);
		if (fp == NULL) return;

		fp->mode.stream.len = pkt_len;
		payload = (uint32_t *)fp->mode.stream.payload;

		dice_build_cip_header(&payload[0], sc->fwdev->fc->nodeid,
		    dbs, dbc, CIP_FMT_AM,
		    no_data ? DICE_FDF_NO_DATA : ps->fdf, syt);
	}

	/* Fill data blocks.
	 *
	 * IMPORTANT: the conversion and the block-fill loops MUST use
	 * separate loop variables.  The old code reused the frame index
	 * inside the sample conversion loops, so after the first block
	 * `i` had advanced to frames*nch: only one data block was ever
	 * filled per packet, blocks 1..frames-1 carried stale/uninitialized
	 * DMA memory (audible garbage), and the AM824 encode read past the
	 * end of the stack tmpbuf (out-of-bounds stack read). */
	{
		struct snd_dbuf *sb = txch ? txch->buffer : NULL;
		int32_t tmpbuf[MAX_DICE_PCM_CH * 12];
		const int32_t *sp;
		unsigned int source_off = ps->hwptr;
		unsigned int ready_bytes = 0, pending_bytes = 0, read_bytes = 0;
		bool underrun = false;

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
		if (sample_bytes > 1) read_bytes -= read_bytes % sample_bytes;

		if (underrun) {
			memset(tmpbuf, 0, frames * nch * sizeof(int32_t));
			sp = tmpbuf;
		} else if (read_bytes >= bytes && sample_bytes == 4 &&
		    !is_float && source_off + bytes <= ps->buffer_bytes) {
			/* Fast path: full packet of S32_LE, no wrap. */
			sp = (const int32_t *)((const uint8_t *)
			    ps->substream->runtime->dma_area + source_off);
		} else {
			unsigned int fi;
			unsigned int samples = frames * nch;

			for (fi = 0; fi < samples; fi++) tmpbuf[fi] = 0;
			if (read_bytes > 0 && sample_bytes == 4) {
				unsigned int ns = read_bytes / 4;

				if (source_off + read_bytes <= ps->buffer_bytes)
					memcpy(tmpbuf,
					    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
					    read_bytes);
				else {
					unsigned int first = ps->buffer_bytes - source_off;
					memcpy(tmpbuf,
					    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
					    first);
					memcpy((uint8_t *)tmpbuf + first,
					    ps->substream->runtime->dma_area,
					    read_bytes - first);
				}
				if (is_float) {
					uint32_t *u = (uint32_t *)tmpbuf;

					for (fi = 0; fi < ns; fi++)
						tmpbuf[fi] = dice_f32_to_s32(u[fi]);
				}
			} else if (read_bytes > 0 && sample_bytes == 2) {
				unsigned int rd = read_bytes / 2;
				const int16_t *s16;
				uint8_t sbuf[18*12*2];
				if (source_off + read_bytes <= ps->buffer_bytes)
					s16 = (const int16_t *)((const uint8_t *)
					    ps->substream->runtime->dma_area + source_off);
				else {
					unsigned int first = ps->buffer_bytes - source_off;
					memcpy(sbuf,
					    (const uint8_t *)ps->substream->runtime->dma_area + source_off,
					    first);
					memcpy(sbuf + first,
					    ps->substream->runtime->dma_area,
					    read_bytes - first);
					s16 = (const int16_t *)sbuf;
				}
				for (fi = 0; fi < rd; fi++)
					tmpbuf[fi] = ((int32_t)s16[fi]) << 16;
			}
			sp = tmpbuf;
		}

		for (i = 0; i < frames; i++) {
			uint32_t *blk = &payload[CIP_HEADER_QUADLETS + i * dbs];

			/*
			 * DICE data-block layout: the PCM quadlets come first,
			 * followed by the MIDI conformant-data quadlet (see
			 * dice-interface.h RX_NUMBER_MIDI and Linux
			 * amdtp-am824.c, which sets pcm_positions[i] = i and
			 * midi_position = pcm_channels).  Placing MIDI first
			 * shifted every PCM channel by one quadlet, so the
			 * device saw stream channel 1 as "left" and channel 2
			 * as "right" — the reported "rear 2+3" routing offset.
			 */
			dice_encode_am824_padded(blk,
			    &sp[i * nch], nch, ps->device_channels);
			if (ps->midi_ports > 0)
				blk[ps->device_channels] = 0x00000080; /* no MIDI */
		}
	}

	ps->hwptr += bytes;
	if (ps->hwptr >= ps->buffer_bytes) ps->hwptr = 0;
	ps->period_accum += bytes;
}

/* ------------------------------------------------------------------ */
/* RX handler — decode ISO packets into PCM DMA buffer                  */
/* ------------------------------------------------------------------ */

static void
dice_rx_handler(struct fw_xferq *xferq)
{
	struct dice_iso_channel *ch;
	struct dice_bsd_softc *sc;
	struct dice_pcm_stream *ps;
	struct fw_bulkxfer *bx;
	unsigned int frames, dbs, bytes;
	int recycled = 0;

	if (xferq == NULL || xferq->sc == NULL) return;
	ch = (struct dice_iso_channel *)xferq->sc;
	if (ch->ctx == NULL) return;
	sc = DICE_SC(ch);
	ps = &DSTREAM(sc)->capture;

	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);

		if (ps->active && ps->substream != NULL &&
		    ps->substream->runtime != NULL &&
		    ps->substream->runtime->dma_area != NULL &&
		    xferq->buf != NULL) {
			struct basound_chan *rxch = ps->substream->private_data;
			unsigned int sample_bytes;
			bool is_float;
			uint32_t *payload;
			unsigned int nch;

			/*
			 * Re-sync the capture DMA target with the OSS
			 * channel's live buffer on every completed packet.
			 * chn_resizebuf() can remap/remalloc the hardware
			 * sndbuf after the ALSA runtime was set up, and
			 * dice_pcm_hw_params()/snd_pcm_lib_malloc_pages()
			 * may have pointed runtime->dma_area at a different
			 * allocation.  The OSS read path consumes ch->buffer,
			 * so write decoded samples there.
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
				if (ps->buffer_bytes > 0)
					ps->hwptr %= ps->buffer_bytes;
			}

			sample_bytes = (rxch != NULL) ?
			    AFMT_BPS(rxch->format) : 4;
			if (sample_bytes != 2 && sample_bytes != 4)
				sample_bytes = 4;
			is_float = (rxch != NULL) &&
			    (AFMT_ENCODING(rxch->format) == AFMT_FLOAT);

			payload = (uint32_t *)fwdma_v_addr(xferq->buf, bx->poffset);
			/*
			 * Blocking-mode capture: the device transmits either
			 * syt_interval data blocks (valid SYT) or empty
			 * NODATA packets (FDF=0xff) on the cycles between.
			 * The FDF field of the received CIP header tells
			 * which; data-block count is otherwise fixed.
			 */
			if (payload != NULL &&
			    ((be32toh(payload[1]) >> 16) & 0xff) == DICE_FDF_NO_DATA)
				frames = 0;
			else
				frames = ps->syt_interval;
			dbs = ps->data_block_quadlets;
			nch = ps->pcm_channels;
			if (nch > ps->device_channels) nch = ps->device_channels;
			bytes = frames * nch * sample_bytes;

			{
				int32_t tmpbuf[MAX_DICE_PCM_CH * 12];
				unsigned int fi;

				for (fi = 0; fi < frames; fi++) {
					const uint32_t *blk = &payload[CIP_HEADER_QUADLETS + fi * dbs];

					/* PCM quadlets are first; MIDI (if any)
					 * follows after the audio quadlets. */
					dice_decode_am824(&tmpbuf[fi * nch], blk, nch);
				}

				if (is_float) {
					unsigned int s;

					for (s = 0; s < frames * nch; s++)
						tmpbuf[s] = (int32_t)
						    dice_s32_to_f32(tmpbuf[s]);
				}

				if (sample_bytes == 4 &&
				    ps->hwptr + bytes <= ps->buffer_bytes) {
					memcpy((uint8_t *)ps->substream->runtime->dma_area + ps->hwptr,
					    tmpbuf, bytes);
				} else if (sample_bytes == 4) {
					unsigned int first = ps->buffer_bytes - ps->hwptr;
					uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
					if (first >= bytes)
						memcpy(dst, tmpbuf, bytes);
					else {
						memcpy(dst, tmpbuf, first);
						memcpy(ps->substream->runtime->dma_area,
						    (uint8_t *)tmpbuf + first, bytes - first);
					}
				} else {
					int16_t tmp16[MAX_DICE_PCM_CH * 12];
					unsigned int s;
					for (s = 0; s < frames * nch; s++)
						tmp16[s] = (int16_t)(tmpbuf[s] >> 16);
					{
						unsigned int first = ps->buffer_bytes - ps->hwptr;
						uint8_t *dst = (uint8_t *)ps->substream->runtime->dma_area + ps->hwptr;
						if (first >= bytes)
							memcpy(dst, tmp16, bytes);
						else {
							memcpy(dst, tmp16, first);
							memcpy(ps->substream->runtime->dma_area,
							    (uint8_t *)tmp16 + first, bytes - first);
						}
					}
				}
			}
			ps->hwptr += bytes;
			if (ps->hwptr >= ps->buffer_bytes) ps->hwptr = 0;
			ps->period_accum += bytes;
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
		struct fw_xferq *tx = ISO_XFERQ(&DSTREAM(sc)->iso_tx);
		struct fw_xferq *rx = ISO_XFERQ(&DSTREAM(sc)->iso_rx);

		if (++cb_ticks <= 2000 && (cb_ticks % 100 == 1)) {
			device_printf(sc->dev,
			    "dice: cb tick=%u pb=%d/%p cap=%d/%p "
			    "refilled-now hwptr=%lu stall=%u "
			    "tx(f=%u,d=%u,v=%u,r=%d) rx(f=%u,d=%u,v=%u,r=%d)\n",
			    cb_ticks, pb->active, (void *)pb->substream,
			    cap->active, (void *)cap->substream,
			    pb->hwptr, DSTREAM(sc)->tx_stall_ticks,
			    dice_qcount(tx, 0), dice_qcount(tx, 1),
			    dice_qcount(tx, 2),
			    (tx->flag & FWXFERQ_RUNNING) != 0,
			    dice_qcount(rx, 0), dice_qcount(rx, 1),
			    dice_qcount(rx, 2),
			    (rx->flag & FWXFERQ_RUNNING) != 0);
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
	int err;

	if (sc->fwdev == NULL || sc->fwdev->fc == NULL) return (-ENODEV);
	if (sc->stream != NULL) return (0);

	sc->stream = malloc(sizeof(struct dice_streaming), M_DICE_ISO,
	    M_WAITOK | M_ZERO);
	if (sc->stream == NULL) return (-ENOMEM);

	fc = sc->fwdev->fc;

	err = dice_iso_open(fc, &DSTREAM(sc)->iso_tx, 1);
	if (err < 0) { free(sc->stream, M_DICE_ISO); sc->stream = NULL; return (err); }
	DSTREAM(sc)->iso_tx.ctx = sc;

	err = dice_iso_open(fc, &DSTREAM(sc)->iso_rx, 0);
	if (err < 0) {
		dice_iso_close(&DSTREAM(sc)->iso_tx);
		free(sc->stream, M_DICE_ISO); sc->stream = NULL;
		return (err);
	}
	DSTREAM(sc)->iso_rx.ctx = sc;
	ISO_XFERQ(&DSTREAM(sc)->iso_rx)->hand = dice_rx_handler;

	/* DEBUG (remove after diagnosis). */
	device_printf(sc->dev,
	    "dice: iso init tx dmach=%d free=%u dma=%u val=%u run=%d | "
	    "rx dmach=%d free=%u dma=%u val=%u run=%d\n",
	    DSTREAM(sc)->iso_tx.dmach,
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_tx), 0),
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_tx), 1),
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_tx), 2),
	    (ISO_XFERQ(&DSTREAM(sc)->iso_tx)->flag & FWXFERQ_RUNNING) != 0,
	    DSTREAM(sc)->iso_rx.dmach,
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_rx), 0),
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_rx), 1),
	    dice_qcount(ISO_XFERQ(&DSTREAM(sc)->iso_rx), 2),
	    (ISO_XFERQ(&DSTREAM(sc)->iso_rx)->flag & FWXFERQ_RUNNING) != 0);

	callout_init(&DSTREAM(sc)->callout, 1);
	mtx_init(&DSTREAM(sc)->playback_lock, "dice_playback", NULL, MTX_DEF);
	mtx_init(&DSTREAM(sc)->capture_lock, "dice_capture", NULL, MTX_DEF);
	/*
	 * The FreeBSD firewire stack exposes no isochronous channel
	 * allocator, so (like digi00x) use fixed channel numbers.  DICE
	 * uses one channel per direction: RX is host->device (playback),
	 * TX is device->host (capture).
	 */
	DSTREAM(sc)->rx_channel = 2;
	DSTREAM(sc)->tx_channel = 3;
	return (0);
}

void
dice_streaming_fini(struct dice_bsd_softc *sc)
{
	if (sc->stream == NULL) return;

	callout_drain(&DSTREAM(sc)->callout);

	if (DSTREAM(sc)->iso_tx.dmach >= 0) {
		ISO_FC(&DSTREAM(sc)->iso_tx)->itx_disable(
		    ISO_FC(&DSTREAM(sc)->iso_tx), DSTREAM(sc)->iso_tx.dmach);
		dice_iso_close(&DSTREAM(sc)->iso_tx);
	}
	if (DSTREAM(sc)->iso_rx.dmach >= 0) {
		ISO_FC(&DSTREAM(sc)->iso_rx)->irx_disable(
		    ISO_FC(&DSTREAM(sc)->iso_rx), DSTREAM(sc)->iso_rx.dmach);
		dice_iso_close(&DSTREAM(sc)->iso_rx);
	}

	mtx_destroy(&DSTREAM(sc)->playback_lock);
	mtx_destroy(&DSTREAM(sc)->capture_lock);
	free(sc->stream, M_DICE_ISO);
	sc->stream = NULL;
}

static void
dice_stream_configure(struct dice_pcm_stream *ps, int dir, unsigned int rate)
{
	unsigned int eff_rate;

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
	ps->tx_dbc = 0;
	ps->frame_cycle = 0;
	ps->frames_per_packet = rate / 8000;
	ps->frame_remainder = rate % 8000;

	/* Blocking-mode SYT sequence (DICE media clock recovery). */
	eff_rate = rate;
	if (ps->double_pcm_frames && rate > 96000)
		eff_rate /= 2;	/* dual-wire: 2 PCM frames per data block */
	dice_blocking_init(ps, eff_rate);
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
 * the RX channel complement and its TX (device->host) blocks the TX one —
 * on the iO26 those are 8 and 26 PCM channels.  The other stream's layout
 * is therefore re-derived from the direction's own channel map at the
 * shared rate instead of copying the starting stream's geometry.
 */
static void
dice_clone_geometry(struct dice_bsd_softc *sc, struct dice_pcm_stream *from)
{
	struct dice_pcm_stream *to = (from == &DSTREAM(sc)->playback) ?
	    &DSTREAM(sc)->capture : &DSTREAM(sc)->playback;
	unsigned int mode_idx, device_ch = 0, midi = 0, i;
	int to_capture = (to == &DSTREAM(sc)->capture);

	to->rate = from->rate;
	to->sfc = from->sfc;
	to->fdf = from->fdf;

	if (from->rate <= 48000)
		mode_idx = SND_DICE_RATE_MODE_LOW;
	else if (from->rate <= 96000)
		mode_idx = SND_DICE_RATE_MODE_MIDDLE;
	else
		mode_idx = SND_DICE_RATE_MODE_HIGH;

	for (i = 0; i < MAX_DICE_STREAMS; i++) {
		if (to_capture) {
			device_ch += sc->cfg.tx_pcm_chs[i][mode_idx];
			midi += sc->cfg.tx_midi_ports[i];
		} else {
			device_ch += sc->cfg.rx_pcm_chs[i][mode_idx];
			midi += sc->cfg.rx_midi_ports[i];
		}
	}

	to->pcm_channels = from->pcm_channels;
	if (device_ch == 0)
		device_ch = to->pcm_channels;
	if (to->pcm_channels > device_ch)
		to->pcm_channels = device_ch;
	to->device_channels = device_ch;
	to->midi_ports = midi;
	to->double_pcm_frames = !sc->cfg.disable_double_pcm_frames;
	to->data_block_quadlets = device_ch + (midi > 0 ? 1 : 0);
	if (to->double_pcm_frames && from->rate > 96000)
		to->data_block_quadlets *= 2;

	to->frames_per_packet = from->frames_per_packet;
	to->frame_remainder = from->frame_remainder;
	to->frame_cycle = 0;
	to->tx_dbc = 0;

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
 * channels, TX speed, then GLOBAL_ENABLE=1.  The DICE firmware latches
 * the stream configuration when streaming is (re)started, so this must
 * run before the host DMA contexts are enabled.  Only needed when the
 * session transitions from idle.
 */
static int
dice_program_device(struct dice_bsd_softc *sc)
{
	int err;

	err = dice_program_iso(sc, 0, 0, DSTREAM(sc)->rx_channel);
	if (err != 0) {
		device_printf(sc->dev, "dice: RX_ISOCHRONOUS write failed (%d)\n",
		    err);
		return (err);
	}
	err = dice_program_iso(sc, 1, 0, DSTREAM(sc)->tx_channel);
	if (err != 0) {
		device_printf(sc->dev, "dice: TX_ISOCHRONOUS write failed (%d)\n",
		    err);
		return (err);
	}
	err = dice_write_quad(sc->fwdev,
	    DICE_PRIVATE_SPACE + sc->tx_offset + 0x014, /* TX_SPEED */
	    htobe32(sc->fwdev->fc->speed));
	if (err != 0) {
		device_printf(sc->dev, "dice: TX_SPEED write failed (%d)\n",
		    err);
		return (err);
	}
	err = dice_enable(sc, true);
	if (err != 0)
		device_printf(sc->dev, "dice: GLOBAL_ENABLE write failed (%d)\n",
		    err);
	return (err);
}

/*
 * Ensure the host isochronous transmit context (playback direction) is
 * running.  Reference counted (rx_use_count): the first caller drains
 * and refills all chunks (silent when no playback app is open) and
 * enables the IT context; later callers just bump the count.
 */
static int
dice_ensure_host_tx(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	uint32_t fv;
	int err, i;

	if (sc->stream == NULL || ch->dmach < 0)
		return (0);
	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);
	if (fc == NULL || xferq == NULL)
		return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	if (DSTREAM(sc)->rx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->playback_lock);
		return (0);
	}
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	/*
	 * The IT context is (re)armed at a fresh cycle match, so restart
	 * the blocking-mode SYT sequence and re-sync the transmission
	 * cycle counter with the bus.
	 */
	{
		struct dice_pcm_stream *pb = &DSTREAM(sc)->playback;

		dice_stream_configure(pb, SNDRV_PCM_STREAM_PLAYBACK,
				      pb->rate);
		pb->tx_cycle = dice_first_tx_cycle(sc);
	}

	/* DEBUG (remove after diagnosis). */
	device_printf(sc->dev,
	    "dice: ensure_tx dmach=%d pre free=%u dma=%u val=%u run=%d "
	    "first_cycle=%u\n",
	    ch->dmach,
	    dice_qcount(xferq, 0), dice_qcount(xferq, 1),
	    dice_qcount(xferq, 2),
	    (xferq->flag & FWXFERQ_RUNNING) != 0,
	    DSTREAM(sc)->playback.tx_cycle);

	FW_GLOCK(fc);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stdma);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stvalid);
	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL)
			break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);
		dice_fill_tx_chunk(sc, xferq, bx);
		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
	}
	FW_GUNLOCK(fc);

	fv = DICE_ISO_TAG_CIP |
	    (DSTREAM(sc)->rx_channel >= 0 ?
	     (DSTREAM(sc)->rx_channel & 0x3f) : 0);
	xferq->flag = (xferq->flag & ~FWXFERQ_CHTAGMASK) | fv;
	err = fc->itx_enable(fc, ch->dmach);
	/* DEBUG (remove after diagnosis): dump first packet as built. */
	{
		struct fw_pkt *fp0 = (struct fw_pkt *)
		    fwdma_v_addr(xferq->buf, 0);
		uint32_t *pl0 = fp0 ? (uint32_t *)fp0->mode.stream.payload : NULL;

		device_printf(sc->dev,
		    "dice: itx_enable ret=%d run=%d free=%u dma=%u val=%u "
		    "len=%u q0=0x%08x q1=0x%08x midi=0x%08x pcm0=0x%08x\n",
		    err, (xferq->flag & FWXFERQ_RUNNING) != 0,
		    dice_qcount(xferq, 0), dice_qcount(xferq, 1),
		    dice_qcount(xferq, 2),
		    fp0 ? fp0->mode.stream.len : 0,
		    pl0 ? be32toh(pl0[0]) : 0,
		    pl0 ? be32toh(pl0[1]) : 0,
		    pl0 ? be32toh(pl0[2]) : 0,
		    pl0 ? be32toh(pl0[3]) : 0);
	}
	if (err != 0) {
		device_printf(sc->dev, "dice: itx_enable failed (%d)\n", err);
		mtx_lock(&DSTREAM(sc)->playback_lock);
		DSTREAM(sc)->rx_use_count = 0;
		mtx_unlock(&DSTREAM(sc)->playback_lock);
		return (-err);
	}
	DSTREAM(sc)->tx_stall_ticks = 0;
	DSTREAM(sc)->tx_restart = false;
	return (0);
}

/*
 * Ensure the host isochronous receive context (capture direction) is
 * running.  Reference counted (tx_use_count).  The device only starts
 * transmitting its capture stream once it receives the host's playback
 * stream, so this context is enabled for playback sessions too; the RX
 * handler is a no-op while no capture app is active.
 */
static int
dice_ensure_host_rx(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	uint32_t fv;
	int err;

	if (sc->stream == NULL || ch->dmach < 0)
		return (0);
	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);
	if (fc == NULL || xferq == NULL)
		return (-ENODEV);

	mtx_lock(&DSTREAM(sc)->capture_lock);
	if (DSTREAM(sc)->tx_use_count++ > 0) {
		mtx_unlock(&DSTREAM(sc)->capture_lock);
		return (0);
	}
	mtx_unlock(&DSTREAM(sc)->capture_lock);

	/* DEBUG (remove after diagnosis). */
	device_printf(sc->dev,
	    "dice: ensure_rx dmach=%d pre free=%u dma=%u val=%u run=%d\n",
	    ch->dmach,
	    dice_qcount(xferq, 0), dice_qcount(xferq, 1),
	    dice_qcount(xferq, 2),
	    (xferq->flag & FWXFERQ_RUNNING) != 0);

	fv = DICE_ISO_TAG_CIP |
	    (DSTREAM(sc)->tx_channel >= 0 ?
	     (DSTREAM(sc)->tx_channel & 0x3f) : 0);
	xferq->flag = (xferq->flag & ~FWXFERQ_CHTAGMASK) | fv;

	/*
	 * Drain leftover chunks back to stfree before arming the IR
	 * context.  fwohci never drains the queues itself: on stop the
	 * chunks stay in stdma/stvalid, so a subsequent irx_enable()
	 * finds stfree empty and prints "IR DMA no free chunk" and
	 * never arms the context (capture dead — jackd's
	 * "Discard error bytes read = -1").  Mirror digi00x_start_rx().
	 */
	FW_GLOCK(fc);
	while ((bx = STAILQ_FIRST(&xferq->stdma)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stdma, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	while ((bx = STAILQ_FIRST(&xferq->stvalid)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stvalid, link);
		STAILQ_INSERT_TAIL(&xferq->stfree, bx, link);
	}
	err = fc->irx_enable(fc, ch->dmach);
	FW_GUNLOCK(fc);
	/* DEBUG (remove after diagnosis). */
	device_printf(sc->dev,
	    "dice: ensure_rx post free=%u dma=%u val=%u run=%d ret=%d\n",
	    dice_qcount(xferq, 0), dice_qcount(xferq, 1),
	    dice_qcount(xferq, 2),
	    (xferq->flag & FWXFERQ_RUNNING) != 0, err);
	if (err != 0) {
		device_printf(sc->dev, "dice: irx_enable failed (%d)\n", err);
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
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx;
	struct firewire_comm *fc;

	if (sc->stream == NULL || ch->dmach < 0)
		return;
	fc = ISO_FC(ch);

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

	fc->itx_disable(fc, ch->dmach);
}

static void
dice_release_host_rx(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_rx;
	struct firewire_comm *fc;

	if (sc->stream == NULL || ch->dmach < 0)
		return;
	fc = ISO_FC(ch);

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

	fc->irx_disable(fc, ch->dmach);
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
	    "dev_ch=%u dbs=%u fdf=0x%02x\n", ps->rate, ps->pcm_channels,
	    ps->device_channels, ps->data_block_quadlets, ps->fdf);
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
	    "dev_ch=%u dbs=%u fdf=0x%02x\n", ps->rate, ps->pcm_channels,
	    ps->device_channels, ps->data_block_quadlets, ps->fdf);
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
 * Called from the refill watchdog when the OHCI IT context has stopped
 * completing packets (e.g. a FireWire bus reset — fwohci halts all ISO
 * contexts on reset and never restarts them, and the DICE firmware
 * clears GLOBAL_ENABLE).  Nothing would ever be recycled into stfree
 * again, so the sndbuf fills up and the app's write() blocks forever.
 *
 * Sequence, mirroring start_playback:
 *   1. re-program the device (rate + RX/TX channels + enable) — fwmem
 *      transactions may tsleep up to 5s each, so no locks are held;
 *   2. itx_disable() the dead context — its internal pause() sleeps
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
	struct dice_iso_channel *ch = &DSTREAM(sc)->iso_tx;
	struct dice_pcm_stream *ps = &DSTREAM(sc)->playback;
	struct firewire_comm *fc;
	struct fw_xferq *xferq;
	struct fw_bulkxfer *bx;
	int refilled = 0, i;

	if (sc->stream == NULL || ch->dmach < 0 || !ps->active) {
		if (sc->stream != NULL) {
			DSTREAM(sc)->tx_restart = false;
			DSTREAM(sc)->tx_stall_ticks = 0;
		}
		return;
	}
	fc = ISO_FC(ch);
	xferq = ISO_XFERQ(ch);
	if (fc == NULL || xferq == NULL) return;

	device_printf(sc->dev, "dice: TX DMA stalled, restarting stream\n");

	/* Device side. */
	(void)dice_set_rate(sc, ps->rate);
	(void)dice_program_device(sc);

	/* Host side: tear down the dead context (pause 1s inside). */
	fc->itx_disable(fc, ch->dmach);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	FW_GLOCK(fc);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stdma);
	STAILQ_CONCAT(&xferq->stfree, &xferq->stvalid);

	/* Fresh blocking-mode sequence; the rearmed IT context starts at
	 * a new cycle match, so re-sync the transmission cycle too. */
	dice_stream_configure(ps, SNDRV_PCM_STREAM_PLAYBACK, ps->rate);
	ps->tx_cycle = dice_first_tx_cycle(sc);

	for (i = 0; i < DICE_ISO_NCHUNKS; i++) {
		bx = STAILQ_FIRST(&xferq->stfree);
		if (bx == NULL) break;
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);
		dice_fill_tx_chunk(sc, xferq, bx);
		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
		refilled++;
	}
	FW_GUNLOCK(fc);
	if (refilled > 0)
		fc->itx_enable(fc, ch->dmach);
	DSTREAM(sc)->tx_stall_ticks = 0;
	DSTREAM(sc)->tx_restart = false;
	mtx_unlock(&DSTREAM(sc)->playback_lock);
}

void
dice_streaming_refill_tx(struct dice_bsd_softc *sc)
{
	struct dice_iso_channel *ch;
	struct fw_xferq *xferq;
	struct firewire_comm *fc;
	struct fw_bulkxfer *bx;
	int refilled = 0;

	if (sc->stream == NULL)
		return;
	ch = &DSTREAM(sc)->iso_tx;

	/*
	 * Keep refilling whenever the IT context is running, even if no
	 * playback app is open (capture-only session): dice_fill_tx_chunk
	 * produces silent, correctly framed packets for that case.  The
	 * old gate on ps->substream/runtime/dma_area stopped the refill
	 * dead whenever playback wasn't open, so a capture-only session
	 * never fed the device and the device never transmitted.
	 */
	if (ch->dmach < 0 || DSTREAM(sc)->rx_use_count == 0)
		return;

	xferq = ISO_XFERQ(ch);
	fc = ISO_FC(ch);

	mtx_lock(&DSTREAM(sc)->playback_lock);
	FW_GLOCK(fc);
	while ((bx = STAILQ_FIRST(&xferq->stfree)) != NULL) {
		STAILQ_REMOVE_HEAD(&xferq->stfree, link);
		dice_fill_tx_chunk(sc, xferq, bx);
		STAILQ_INSERT_TAIL(&xferq->stvalid, bx, link);
		refilled++;
	}
	FW_GUNLOCK(fc);
	if (refilled > 0) {
		DSTREAM(sc)->tx_stall_ticks = 0;
		fc->itx_enable(fc, ch->dmach);
	} else if (++DSTREAM(sc)->tx_stall_ticks >= DICE_TX_STALL_LIMIT) {
		/*
		 * No chunk has been recycled for the whole limit window.
		 * A healthy context completes ~8 packets/ms, so this means
		 * the OHCI IT context is dead (bus reset / DMA error).
		 * Defer the restart until after playback_lock is dropped:
		 * it sleeps (itx_disable pause, fwmem timeouts) and must
		 * not run with the mutex held.
		 */
		DSTREAM(sc)->tx_stall_ticks = 0;
		DSTREAM(sc)->tx_restart = true;
	}
	mtx_unlock(&DSTREAM(sc)->playback_lock);

	if (DSTREAM(sc)->tx_restart)
		dice_streaming_restart_tx(sc);
}
