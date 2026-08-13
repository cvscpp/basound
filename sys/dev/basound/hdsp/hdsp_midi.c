// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hdsp_midi.c - MIDI support for RME Hammerfall DSP cards
 *
 * Bridges the hardware MIDI UART (two ports at HDSP_midiDataIn/Out*)
 * to FreeBSD's midi(4) framework via mpu401_init() / mpufoi interface.
 *
 * The HDSP interrupt handler (snd_hdsp_interrupt in hdsp_main.c)
 * already detects midi0/midi1 pending and calls schedule_work().
 * The work function here calls (*mpu_intr)() which is mpu401_intr:
 * it reads HDSP_midiStatus, drains available input bytes into
 * midi_in(), gets output bytes from midi_out(), writes them to the
 * hardware, and resets the mpu401 callout if TX remains enabled.
 *
 * When no hardware interrupt has fired yet but userland opens the
 * MIDI device and starts writing, mpu401_mcallback() fires the
 * callout → mpu401_timeout → hdsp_midi_softintr (si) → which
 * delegates to the work function.  The 1-tick callout loop keeps
 * the output queue drained even without hardware interrupts.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/stddef.h>
#include <sys/kobj.h>
#include <sys/bus.h>
#include <dev/sound/midi/midi.h>
#include <dev/sound/midi/mpu401.h>
#include "mpufoi_if.h"
#include <linux/workqueue.h>

#include "hdsp.h"

MALLOC_DECLARE(M_MIDI);

/* ------------------------------------------------------------------ */
/* mpufoi class: map HDSP hardware registers to MPU-401 semantics       */
/*                                                                      */
/* MPU register layout used by mpu401_intr:                             */
/*   reg=0  MPU_DATAPORT   →  read/write MIDI data byte                */
/*   reg=1  MPU_STATPORT   →  read  (MPU_INPUTBUSY | MPU_OUTPUTBUSY)   */
/*   reg=1  MPU_CMDPORT    →  write (MPU_RESET=0xff, MPU_UART=0x3f)   */
/* ------------------------------------------------------------------ */

static unsigned char
hdsp_midi_read(struct mpu401 *arg __unused, void *cookie, int reg)
{
	struct hdsp_midi *hmidi = cookie;
	struct hdsp *hdsp = hmidi->hdsp;

	if (reg == 0) {
		/* MPU_DATAPORT — read MIDI byte from hardware */
		return (unsigned char)hdsp_read(hdsp,
		    (hmidi->id == 0) ? HDSP_midiDataIn0 : HDSP_midiDataIn1);
	} else {
		/* MPU_STATPORT — translate HDSP status to MPU status bits */
		uint32_t status = hdsp_read(hdsp, HDSP_midiStatus);
		unsigned char rc = 0;

		/*
		 * HDSP_midiStatusInputAvailable (bit 0) = data waiting.
		 * MPU_INPUTBUSY (0x80) = NOT ready to read.
		 * So input available → clear MPU_INPUTBUSY.
		 */
		if (!(status & HDSP_midiStatusInputAvailable))
			rc |= 0x80;	/* MPU_INPUTBUSY */

		/*
		 * HDSP_midiStatusOutputReady (bit 1) = can write.
		 * MPU_OUTPUTBUSY (0x40) = NOT ready to write.
		 * So output ready → clear MPU_OUTPUTBUSY.
		 */
		if (!(status & HDSP_midiStatusOutputReady))
			rc |= 0x40;	/* MPU_OUTPUTBUSY */

		return (rc);
	}
}

static void
hdsp_midi_write(struct mpu401 *arg __unused, void *cookie, int reg,
    unsigned char b)
{
	struct hdsp_midi *hmidi = cookie;
	struct hdsp *hdsp = hmidi->hdsp;

	if (reg == 0) {
		/* MPU_DATAPORT — write MIDI byte to hardware */
		hdsp_write(hdsp,
		    (hmidi->id == 0) ? HDSP_midiDataOut0 : HDSP_midiDataOut1,
		    (uint32_t)b);
	}
	/* reg==1 (MPU_CMDPORT) is a no-op: the HDSP UART is
	 * always in UART mode once enabled; reset/uninit is
	 * handled via the control register below. */
}

static int
hdsp_midi_uninit(struct mpu401 *arg __unused, void *cookie)
{
	struct hdsp_midi *hmidi = cookie;
	struct hdsp *hdsp = hmidi->hdsp;
	uint32_t ctrl;

	/* Disable MIDI I/O on this port.  Both ports share the
	 * control register; only clear the bits for our port. */
	ctrl = hdsp_read(hdsp, HDSP_midiControl);
	ctrl &= ~(HDSP_midiControlInputEnable | HDSP_midiControlOutputEnable);
	hdsp_write(hdsp, HDSP_midiControl, ctrl);

	return (0);
}

static kobj_method_t hdsp_mpu_methods[] = {
	KOBJMETHOD(mpufoi_read,   hdsp_midi_read),
	KOBJMETHOD(mpufoi_write,  hdsp_midi_write),
	KOBJMETHOD(mpufoi_uninit, hdsp_midi_uninit),
	KOBJMETHOD_END
};
static DEFINE_CLASS(hdsp_mpu, hdsp_mpu_methods, 0);

/* ------------------------------------------------------------------ */
/* Soft-interrupt handler called by the mpu401 callout (1-tick timer)  */
/* when the output queue has data to send but no hardware interrupt    */
/* has fired.  Delegates to the work function.                         */
/* ------------------------------------------------------------------ */

static void
hdsp_midi_softintr(void *arg)
{
	struct hdsp_midi *hmidi = arg;
	struct hdsp *hdsp = hmidi->hdsp;

	schedule_work(&hdsp->midi_work);
}

/* ------------------------------------------------------------------ */
/* MIDI work function — called from the HDSP interrupt handler or from */
/* the mpu401 callout via hdsp_midi_softintr.                          */
/* ------------------------------------------------------------------ */

void
snd_hdsp_midi_work(struct work_struct *work)
{
	struct hdsp *hdsp = (struct hdsp *)((char *)work -
	    offsetof(struct hdsp, midi_work));
	int i;

	for (i = 0; i < 2; i++) {
		struct hdsp_midi *hmidi = &hdsp->midi[i];

		if (hmidi->mpu_intr != NULL)
			hmidi->mpu_intr(hmidi->mpu);
	}
}

/* ------------------------------------------------------------------ */
/* Initialise both MIDI ports via mpu401_init().                       */
/* ------------------------------------------------------------------ */

int
snd_hdsp_create_midi(struct hdsp *hdsp)
{
	uint32_t ctrl;
	int i;

	for (i = 0; i < 2; i++) {
		struct hdsp_midi *hmidi = &hdsp->midi[i];

		/* mpu401_init wraps midi_init() with the standard
		 * MPU-401 class, creating /dev/midi%d.%d nodes.
		 * hdsp_midi_softintr is called by the mpu401
		 * callout when userland writes to the device. */
		hmidi->mpu = mpu401_init(&hdsp_mpu_class, hmidi,
		    hdsp_midi_softintr, &hmidi->mpu_intr);
		if (hmidi->mpu == NULL)
			return (ENOMEM);
	}

	/* Enable MIDI I/O on both ports. */
	ctrl = hdsp_read(hdsp, HDSP_midiControl);
	ctrl |= (HDSP_midiControlInputEnable | HDSP_midiControlOutputEnable);
	hdsp_write(hdsp, HDSP_midiControl, ctrl);

	return (0);
}

/* ------------------------------------------------------------------ */
/* Tear down both MIDI ports.                                          */
/* ------------------------------------------------------------------ */

void
snd_hdsp_free_midi(struct hdsp *hdsp)
{
	int i;

	for (i = 0; i < 2; i++) {
		struct hdsp_midi *hmidi = &hdsp->midi[i];

		if (hmidi->mpu != NULL) {
			mpu401_uninit(hmidi->mpu);
			hmidi->mpu = NULL;
			hmidi->mpu_intr = NULL;
		}
	}
}
