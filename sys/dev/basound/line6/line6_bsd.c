/*-
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FreeBSD Line6 USB Audio Driver
 * Bridges Line6 USB audio devices to FreeBSD sound(4) system
 *
 * Supported Devices:
 * - POD (USB ID 0E41:4750)
 * - POD XT (USB ID 0E41:4753)
 * - POD XT Live (USB ID 0E41:4642)
 * - Bass POD XT (USB ID 0E41:4750)
 * - POD HD (USB ID 0E41:5057)
 * - TonePort (USB ID 0E41:4154, 0E41:4159)
 * - Variax (USB ID 0E41:4756)
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/sysctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbhid.h>
#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>
#include <alsa_pcm_bsd.h>
#include "basound_debug.h"

/* Line6 USB driver - bridges FreeBSD usb_device to ALSA Line6 driver */

MALLOC_DECLARE(M_ALSA);

SYSCTL_DECL(_hw_basound);
SYSCTL_NODE(_hw_basound, OID_AUTO, line6, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Line6 USB audio");

struct line6_bsd_softc;
static int line6_toneport_enable(struct line6_bsd_softc *sc);
static int line6_toneport_set_capture_source(struct line6_bsd_softc *sc);
static struct line6_bsd_softc *line6_active_sc;

/*
 * TonePort/POD Studio capture source selector (matches Linux toneport.c):
 *   0 = Microphone   (0x0a01)
 *   1 = Line         (0x0801)
 *   2 = Instrument   (0x0b01)
 *   3 = Inst & Mic   (0x0901)
 */
static int line6_capture_source = 0;

static int
sysctl_line6_capture_source(SYSCTL_HANDLER_ARGS)
{
	int err, v;

	v = line6_capture_source;
	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (v < 0 || v > 3)
		return (EINVAL);
	line6_capture_source = v;

	/* Apply immediately when a Line6 device is attached. */
	if (line6_active_sc != NULL)
		(void)line6_toneport_enable(line6_active_sc);

	return (0);
}

SYSCTL_PROC(_hw_basound_line6, OID_AUTO, capture_source,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, 0, 0,
    sysctl_line6_capture_source, "I",
    "TonePort/POD Studio capture source: 0=mic 1=line 2=instrument 3=inst+mic");

/*
 * USB isochronous transport parameters.
 * Full-speed USB (12 Mbps) has 1 ms frames → 1000 fps.
 * Use 8 frames per transfer → 8ms batches.
 *
 * POD Studio / TonePort layout: single USB interface (0) with multiple
 * alt settings.  Alt 0 = zero-bandwidth (interrupt IN only).
 * Alt 2 = audio streaming (ISO OUT 0x01 + ISO IN 0x82 on interface 0).
 * This matches what Linux sound/usb/line6/toneport.c selects.
 */
#define LINE6_NFRAMES		8	/* ISO frames per USB transfer */
#define LINE6_NCHANBUFS		4	/* double-buffered outstanding transfers */
#define LINE6_ALT_AUDIO		2	/* bAlternateSetting index for ISO audio */
#define LINE6_ALT_MIN		1
#define LINE6_ALT_MAX		4

/*
 * Per-direction audio stream state.  One instance lives in the softc for
 * playback and one for capture.  Protected by sc->sc_lock (the USB mutex).
 */
struct line6_audio_stream {
	struct usb_xfer	*xfer[LINE6_NCHANBUFS + 1]; /* +1 for sync endpoint */
	struct pcm_channel *pcm_ch;	/* FreeBSD PCM channel (for chn_intr) */
	uint8_t		*start;		/* DMA ring buffer start */
	uint8_t		*end;		/* DMA ring buffer end */
	uint8_t		*cur;		/* Current read/write position */
	uint32_t	 hwptr_bytes;	/* hardware pointer offset in DMA ring */
	uint32_t	 sample_size;	/* bytes per PCM frame (all channels) */
	uint32_t	 bytes_per_frame[2]; /* [0]=base bytes, [1]=base+sample_sz */
	uint32_t	 intr_frames;	/* USB frames per transfer */
	uint32_t	 frames_per_second; /* 1000 for FS, 8000 for HS */
	uint32_t	 sample_rem;	/* sample_rate % frames_per_second */
	uint32_t	 sample_curr;	/* jitter correction accumulator */
	int		 running;	/* 1 while transfers are active */
};

/* Line6 vendor ID */
#define LINE6_VENDOR_ID		0x0e41

/* Line6 device product IDs */
#define LINE6_PRODUCT_POD	0x4750
#define LINE6_PRODUCT_PODXT	0x4753
#define LINE6_PRODUCT_POD_XT_LIVE 0x4642
#define LINE6_PRODUCT_BASS_POD_XT 0x4050
#define LINE6_PRODUCT_PODHD300	0x5057
#define LINE6_PRODUCT_PODHD400	0x5058
#define LINE6_PRODUCT_PODHD500	0x5073
#define LINE6_PRODUCT_PODSTUDIO_UX1 0x4150
#define LINE6_PRODUCT_PODSTUDIO_UX2 0x4151
#define LINE6_PRODUCT_TONEPORT_UX1 0x4141
#define LINE6_PRODUCT_TONEPORT_UX2 0x4142
#define LINE6_PRODUCT_TONEPORT_GX 0x4147
#define LINE6_PRODUCT_VARIAX	0x4756

/* Device capability flags */
#define LINE6_CAP_CONTROL	0x0001  /* Device has MIDI control */
#define LINE6_CAP_AUDIO_IN	0x0002  /* Device has audio input */
#define LINE6_CAP_AUDIO_OUT	0x0004  /* Device has audio output */
#define LINE6_CAP_MIDI		0x0008  /* Device has MIDI I/O */
#define LINE6_CAP_FIRMWARE	0x0010  /* Device supports firmware updates */
#define LINE6_CAP_INIT_TONEPORT	0x0020	/* Device needs 0x0301 init command */

struct line6_device_info {
	uint16_t product_id;
	const char *name;
	const char *card_id;
	unsigned int capabilities;
	uint8_t ep_audio_r;
	uint8_t ep_audio_w;
};

/* Line6 device database */
static const struct line6_device_info line6_devices[] = {
	{
		.product_id = LINE6_PRODUCT_POD,
		.name = "Line6 POD",
		.card_id = "Line6POD",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_PODXT,
		.name = "Line6 POD XT",
		.card_id = "Line6PODXT",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_POD_XT_LIVE,
		.name = "Line6 POD XT Live",
		.card_id = "Line6PODXTLive",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_BASS_POD_XT,
		.name = "Line6 Bass POD XT",
		.card_id = "Line6BassPODXT",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_PODHD300,
		.name = "Line6 POD HD300",
		.card_id = "Line6PODHD300",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02
	},
	{
		.product_id = LINE6_PRODUCT_PODHD400,
		.name = "Line6 POD HD400",
		.card_id = "Line6PODHD400",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02
	},
	{
		.product_id = LINE6_PRODUCT_PODHD500,
		.name = "Line6 POD HD500",
		.card_id = "Line6PODHD500",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02
	},
	{
		.product_id = LINE6_PRODUCT_PODSTUDIO_UX1,
		.name = "Line6 POD Studio UX1",
		.card_id = "Line6PODStudioUX1",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI |
				LINE6_CAP_INIT_TONEPORT,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_PODSTUDIO_UX2,
		.name = "Line6 POD Studio UX2",
		.card_id = "Line6PODStudioUX2",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI |
				LINE6_CAP_INIT_TONEPORT,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_TONEPORT_UX1,
		.name = "Line6 TonePort UX1",
		.card_id = "Line6TonePortUX1",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI |
				LINE6_CAP_INIT_TONEPORT,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_TONEPORT_UX2,
		.name = "Line6 TonePort UX2",
		.card_id = "Line6TonePortUX2",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI |
				LINE6_CAP_INIT_TONEPORT,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_TONEPORT_GX,
		.name = "Line6 TonePort GX",
		.card_id = "Line6TonePortGX",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_AUDIO_IN | 
				LINE6_CAP_AUDIO_OUT | LINE6_CAP_MIDI |
				LINE6_CAP_INIT_TONEPORT,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01
	},
	{
		.product_id = LINE6_PRODUCT_VARIAX,
		.name = "Line6 Variax",
		.card_id = "Line6Variax",
		.capabilities = LINE6_CAP_CONTROL | LINE6_CAP_MIDI | 
				LINE6_CAP_FIRMWARE,
		.ep_audio_r = 0,
		.ep_audio_w = 0
	},
	{ 0, NULL, NULL, 0, 0, 0 }
};

struct line6_bsd_softc {
	device_t	 dev;
	struct device	 alsa_dev;	/* wrapper so card->dev stays valid */
	struct usb_device *usbdev;
	usb_interface_descriptor_t *idesc;
	void		*alsa_line6;
	unsigned int	 capabilities;
	const char	*device_name;
	uint8_t		 ep_audio_r;
	uint8_t		 ep_audio_w;

	/* Audio transport */
	struct mtx	 sc_lock;	/* serialises USB state; USB callback mutex */
	struct line6_audio_stream play;
	struct line6_audio_stream rec;
	uint8_t		 ctrl_iface_index; /* USB interface for AudioControl */
	uint8_t		 audio_iface_index; /* USB interface hosting ISO endpoints */
	uint8_t		 audio_altsetting; /* preferred audio streaming alt */
	uint32_t	 audio_active;	/* bitmask: 1=play, 2=rec; 0=none active */
	uint8_t		 toneport_enabled; /* 0x0301 accepted at least once */

	/* Helper playback stream for TonePort devices.
	 * The TonePort firmware requires the ISO OUT endpoint (playback) to be
	 * streaming data — even silence — for the capture path to route audio
	 * correctly through the internal DSP.  Without this, capture produces
	 * metallic distortion.
	 *
	 * When capture starts without playback, we start a "helper" playback
	 * stream that sends zeros.  play_helper is set while active.
	 */
#define PLAY_HELPER_BUF_SIZE	65536	/* silence ring, ~1.5s at 44.1k */
	uint8_t		*play_helper_buf;
	uint8_t		 play_helper;	/* non-zero while helper is active */

	/* Software monitoring.
	 * TonePort devices have no hardware monitoring, so we mix captured
	 * audio into the playback stream in software.  monitor_volume follows
	 * the Linux convention: 0=off, 256=unity gain.
	 */
#define MONITOR_BUF_SIZE		2048	/* max one USB frame of S16_LE */
	uint8_t		 monitor_fbuf[MONITOR_BUF_SIZE];
	uint32_t	 monitor_fsize;
	int		 monitor_volume;
};

MALLOC_DEFINE(M_LINE6_BSD, "line6_bsd", "Line6 BSD softc");

/* Forward declarations for USB isochronous callbacks */
static usb_callback_t line6_play_callback;
static usb_callback_t line6_rec_callback;

/* Forward declarations for TonePort helper playback */
static int line6_start_play_helper(struct line6_bsd_softc *sc, uint32_t sample_size);
static void line6_stop_play_helper(struct line6_bsd_softc *sc);

/* USB isochronous config for playback (HOST → DEVICE, OUT) */
static const struct usb_config line6_play_cfg[LINE6_NCHANBUFS] = {
	[0 ... LINE6_NCHANBUFS - 1] = {
		.type = UE_ISOCHRONOUS,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = 0,		/* wMaxPacketSize × frames */
		.frames = LINE6_NFRAMES,
		.flags = {.short_xfer_ok = 1},
		.callback = &line6_play_callback,
	},
};

/* USB isochronous config for capture (DEVICE → HOST, IN) */
static const struct usb_config line6_rec_cfg[LINE6_NCHANBUFS] = {
	[0 ... LINE6_NCHANBUFS - 1] = {
		.type = UE_ISOCHRONOUS,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = 0,
		.frames = LINE6_NFRAMES,
		.flags = {.short_xfer_ok = 1},
		.callback = &line6_rec_callback,
	},
};

/*
 * Check whether interface 0 has ISO audio endpoints on some alt setting.
 * Line6 POD Studio / TonePort devices have a single USB interface (0).
 * Alt setting 0 = zero-bandwidth (AudioControl only, interrupt IN).
 * Alt settings 1..4 = audio streaming (ISO OUT 0x01 + ISO IN 0x82).
 * Returns non-zero if ISO endpoints are found, 0 if none.
 */
static int
line6_has_iso_endpoints(struct usb_device *udev)
{
	struct usb_config_descriptor *cd;
	struct usb_descriptor *desc;
	struct usb_endpoint_descriptor *ed;

	cd = usbd_get_config_descriptor(udev);
	if (cd == NULL)
		return 0;

	desc = NULL;
	while ((desc = usb_desc_foreach(cd, desc)) != NULL) {
		if (desc->bDescriptorType == UDESC_ENDPOINT) {
			ed = (struct usb_endpoint_descriptor *)desc;
			if (UE_GET_XFERTYPE(ed->bmAttributes) == UE_ISOCHRONOUS)
				return 1;
		}
	}
	return 0;
}

/*
 * Select an audio alt setting robustly. Some devices/firmware return
 * transient errors on one alt even though other valid audio alts work.
 */
static usb_error_t
line6_set_audio_alt(struct line6_bsd_softc *sc)
{
	usb_error_t err;

	err = usbd_set_alt_interface_index(sc->usbdev, sc->audio_iface_index,
	    sc->audio_altsetting);
	if (err == 0)
		return 0;

	for (uint8_t alt = LINE6_ALT_MIN; alt <= LINE6_ALT_MAX; alt++) {
		if (alt == sc->audio_altsetting)
			continue;
		err = usbd_set_alt_interface_index(sc->usbdev,
		    sc->audio_iface_index, alt);
		if (err == 0) {
			device_printf(sc->dev,
			    "audio alt %u selected (fallback from %u)\n",
			    alt, sc->audio_altsetting);
			sc->audio_altsetting = alt;
			return 0;
		}
	}

	return err;
}

static uint32_t
line6_set_playback_frame_lengths(struct line6_audio_stream *st,
    struct usb_xfer *xfer)
{
	uint32_t total = 0;
	uint32_t n, frame_len;

	usbd_xfer_set_frames(xfer, st->intr_frames);
	for (n = 0; n < st->intr_frames; n++) {
		st->sample_curr += st->sample_rem;
		if (st->sample_curr >= st->frames_per_second) {
			st->sample_curr -= st->frames_per_second;
			frame_len = st->bytes_per_frame[1];
		} else {
			frame_len = st->bytes_per_frame[0];
		}
		if (frame_len == 0)
			frame_len = st->sample_size;
		usbd_xfer_set_frame_len(xfer, n, frame_len);
		total += frame_len;
	}

	return (total);
}

/*
 * Compute per-frame lengths for the CAPTURE (IN) direction with rate jitter
 * correction.  The USB DMA buffer lays out frames contiguously at these
 * configured lengths.  Returns total bytes expected across all frames.
 *
 * Must be called with the transfer lock held.
 */
static uint32_t
line6_set_capture_frame_lengths(struct line6_audio_stream *st,
    struct usb_xfer *xfer)
{
	uint32_t total = 0;
	uint32_t n, frame_len;

	usbd_xfer_set_frames(xfer, st->intr_frames);
	for (n = 0; n < st->intr_frames; n++) {
		st->sample_curr += st->sample_rem;
		if (st->sample_curr >= st->frames_per_second) {
			st->sample_curr -= st->frames_per_second;
			frame_len = st->bytes_per_frame[1];
		} else {
			frame_len = st->bytes_per_frame[0];
		}
		if (frame_len == 0)
			frame_len = st->sample_size;
		usbd_xfer_set_frame_len(xfer, n, frame_len);
		total += frame_len;
	}

	return (total);
}

/*
 * USB isochronous playback callback.
 * Called with sc->sc_lock held (it is the USB transfer mutex).
 *
 * On USB_ST_SETUP (first invocation) and USB_ST_TRANSFERRED (previous packet
 * delivered): fill the next LINE6_NFRAMES worth of packets from the DMA ring
 * buffer and submit.  After data is consumed, notify the PCM layer so it can
 * refill the ring buffer — but we must drop sc_lock first to avoid a lock
 * order reversal with the PCM channel lock.
 */
static void
line6_play_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct line6_audio_stream *st = usbd_xfer_softc(xfer);
	struct line6_bsd_softc *sc =
	    __containerof(st, struct line6_bsd_softc, play);
	struct usb_page_cache *pc;
	uint32_t total, n, offset;
	int actlen, sumlen;
	static uint32_t dbg_cnt = 0;

	usbd_xfer_status(xfer, &actlen, &sumlen, NULL, NULL);
	dbg_cnt++;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		/* FALLTHROUGH */
	case USB_ST_TRANSFERRED: {
		if (!st->running || st->start == NULL || st->start == st->end)
			break;

		/* Compute per-frame lengths with rate jitter correction */
		total = line6_set_playback_frame_lengths(st, xfer);

		/* Copy from DMA ring buffer into USB isochronous packet */
		offset = 0;
		pc = usbd_xfer_get_frame(xfer, 0);
		if (basound_debug_tone_enabled()) {
			/*
			 * Shared basound debug tone path. We still advance st->cur
			 * so feeder/PCM progress remains consistent while testing.
			 */
			uint32_t remaining = total;
			uint8_t tbuf[512];
			while (remaining > 0) {
				uint32_t chunk = MIN(remaining,
				    (uint32_t)sizeof(tbuf));
				if (!basound_debug_tone_fill_s16le(tbuf, chunk, 2,
				    44100))
					memset(tbuf, 0, chunk);
				usbd_copy_in(pc, offset, tbuf, chunk);
				offset += chunk;
				remaining -= chunk;
			}
			/* Advance ring pointer to keep feeder draining */
			n = total;
			while (n > 0) {
				uint32_t chunk = (uint32_t)(st->end - st->cur);
				if (chunk > n) chunk = n;
				st->cur += chunk;
				n -= chunk;
				if (st->cur >= st->end)
					st->cur = st->start;
			}
		} else {
			while (total > 0) {
				n = (uint32_t)(st->end - st->cur);
				if (n > total)
					n = total;
				usbd_copy_in(pc, offset, st->cur, n);
				total -= n;
				st->cur += n;
				offset += n;
				if (st->cur >= st->end)
					st->cur = st->start;
			}
		}

		/* Pointer seen by FreeBSD PCM layer is byte offset in ring. */
		if (st->end > st->start)
			st->hwptr_bytes = (uint32_t)(st->cur - st->start);

		/*
		 * Notify the PCM layer only after we advanced the hardware
		 * pointer, so chn_getptr sees forward progress.
		 */
		if (st->pcm_ch != NULL) {
			mtx_unlock(&sc->sc_lock);
			chn_intr(st->pcm_ch);
			mtx_lock(&sc->sc_lock);
			if (!st->running)
				break;
		}

		if (dbg_cnt % 200 == 0)
			printf("line6 play cb#%u state=%s hwptr=%u total=%u\n",
			    dbg_cnt,
			    USB_GET_STATE(xfer) == USB_ST_SETUP ? "SETUP" : "XFRD",
			    st->hwptr_bytes, offset);

		/*
		 * Software monitoring: mix captured audio into the playback
		 * stream sent to the device, so the user hears their input.
		 *
		 * The monitor_fbuf holds the last USB frame of captured S16_LE
		 * stereo audio saved by the rec callback.  Mix at the volume
		 * set by the "Monitor Playback Volume" ALSA control.
		 *
		 * The data we just wrote to the USB buffer (pc at offset 0)
		 * has already been sent to the DMA engine, so we read the
		 * buffer back, add the monitor signal, and write it back.
		 */
		if (sc->monitor_volume > 0 && sc->monitor_fsize > 0 &&
		    sc->monitor_fsize <= total && sc->monitor_fsize <= MONITOR_BUF_SIZE) {
			uint32_t nsamples = sc->monitor_fsize / 2;
			int16_t buf[512];
			uint32_t j;

			/* Read back what we just wrote */
			usbd_copy_out(pc, 0, (uint8_t *)buf,
			    MIN(sc->monitor_fsize, sizeof(buf)));
			/* Mix monitor signal */
			for (j = 0; j < MIN(nsamples, sizeof(buf)/2); j++) {
				int16_t *mon = (int16_t *)sc->monitor_fbuf;
				int32_t mixed;

				mixed = (int32_t)buf[j] +
				    ((int32_t)mon[j] * sc->monitor_volume) / 256;
				if (mixed > 32767)
					mixed = 32767;
				if (mixed < -32768)
					mixed = -32768;
				buf[j] = (int16_t)mixed;
			}
			usbd_copy_in(pc, 0, (uint8_t *)buf,
			    MIN(sc->monitor_fsize, sizeof(buf)));
		}

		usbd_transfer_submit(xfer);
		break;
	}

	default:	/* Error */
		if (error != USB_ERR_CANCELLED && st->running) {
			/* Transient error: re-arm with proper frame sizing */
			(void)line6_set_playback_frame_lengths(st, xfer);
			usbd_transfer_submit(xfer);
		}
		break;
	}
}

/*
 * USB isochronous capture callback.
 * Called with sc->sc_lock held (it is the USB transfer mutex).
 *
 * On USB_ST_TRANSFERRED: copy received ISO packets into the DMA ring buffer,
 * then notify the PCM layer (drop sc_lock first for the same reason as play).
 * On USB_ST_SETUP: arm the receive transfer.
 */
static void
line6_rec_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct line6_audio_stream *st = usbd_xfer_softc(xfer);
	struct line6_bsd_softc *sc =
	    __containerof(st, struct line6_bsd_softc, rec);
	struct usb_page_cache *pc;
	uint32_t n, len;
	int actlen, nframes, i;
	static uint32_t dbg_cnt = 0;

	usbd_xfer_status(xfer, &actlen, NULL, NULL, &nframes);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED: {
		dbg_cnt++;
		if (st->start == NULL || st->start == st->end)
			goto tr_setup;

		/*
		 * Copy received data from the USB isochronous DMA buffer into
		 * the PCM ring buffer.
		 *
		 * IMPORTANT: The USB DMA buffer lays out isochronous frames
		 * contiguously at the CONFIGURED (submission-time) frame
		 * lengths, not the actual received lengths.  We must read
		 * each frame from its configured offset in the DMA buffer.
		 * Tracking offsets by actual received lengths would cause
		 * progressive misalignment and garbage data (fuzz).
		 *
		 * Use usbd_xfer_old_frame_length() to get the configured
		 * length at submission time, which gives us the correct
		 * inter-frame DMA buffer stride.
		 */
		pc = usbd_xfer_get_frame(xfer, 0);
		for (i = 0; i < nframes; i++) {
			uint32_t frame_offset;
			uint32_t j;

			len = usbd_xfer_frame_len(xfer, i);
			if (len == 0)
				continue;

			/* Compute DMA buffer offset from configured lengths */
			frame_offset = 0;
			for (j = 0; j < (uint32_t)i; j++)
				frame_offset +=
				    usbd_xfer_old_frame_length(xfer, j);

			while (len > 0) {
				n = (uint32_t)(st->end - st->cur);
				if (n > len)
					n = len;
				usbd_copy_out(pc, frame_offset, st->cur, n);
				len -= n;
				st->cur += n;
				frame_offset += n;
				if (st->cur >= st->end)
					st->cur = st->start;
			}
		}
		if (st->end > st->start)
			st->hwptr_bytes = (uint32_t)(st->cur - st->start);

		/*
		 * Save last frame for software monitoring if volume is set.
		 * The USB DMA buffer will be reused on the next transfer, so
		 * we must copy the data into our persistent monitor_fbuf now.
		 *
		 * Recompute the last frame's offset from configured lengths.
		 */
		if (sc->monitor_volume > 0 && nframes > 0) {
			int last = nframes - 1;
			uint32_t mlen = usbd_xfer_frame_len(xfer, last);
			uint32_t moff = 0;
			uint32_t mi;

			for (mi = 0; mi < (uint32_t)last; mi++)
				moff += usbd_xfer_old_frame_length(xfer, mi);
			if (mlen > 0 && mlen <= MONITOR_BUF_SIZE &&
			    moff + mlen <= usbd_xfer_max_len(xfer)) {
				usbd_copy_out(pc, moff,
				    sc->monitor_fbuf, mlen);
				sc->monitor_fsize = mlen;
			}
		} else if (sc->monitor_volume == 0) {
			sc->monitor_fsize = 0;
		}

		if (dbg_cnt % 200 == 0)
			printf("line6 rec cb#%u cur+%u actlen=%d pcm_ch=%p name=%s intr=%u\n",
			    dbg_cnt, (uint32_t)(st->cur - st->start), actlen,
			    st->pcm_ch,
			    (st->pcm_ch != NULL) ? st->pcm_ch->name : "<null>",
			    (st->pcm_ch != NULL) ? st->pcm_ch->interrupts : 0);

		if (st->pcm_ch != NULL) {
			mtx_unlock(&sc->sc_lock);
			chn_intr(st->pcm_ch);
			mtx_lock(&sc->sc_lock);
		}
		/* FALLTHROUGH to re-arm */
	}
	case USB_ST_SETUP:
tr_setup:
		if (!st->running)
			break;
		/* Use jitter-corrected frame lengths so the DMA buffer layout
		 * matches what the device actually sends. */
		(void)line6_set_capture_frame_lengths(st, xfer);
		usbd_transfer_submit(xfer);
		break;

	default:	/* Error */
		if (error != USB_ERR_CANCELLED && st->running) {
			/* Re-arm with jitter correction on error recovery.
			 * Use a single frame to minimise latency. */
			st->sample_curr += st->sample_rem;
			if (st->sample_curr >= st->frames_per_second) {
				st->sample_curr -= st->frames_per_second;
				usbd_xfer_set_frames(xfer, 1);
				usbd_xfer_set_frame_len(xfer, 0,
				    st->bytes_per_frame[1]);
			} else {
				usbd_xfer_set_frames(xfer, 1);
				usbd_xfer_set_frame_len(xfer, 0,
				    st->bytes_per_frame[0]);
			}
			usbd_transfer_submit(xfer);
		}
		break;
	}
}
static const struct line6_device_info *
line6_bsd_find_device(uint16_t product_id)
{
	int i;

	for (i = 0; line6_devices[i].product_id != 0; i++) {
		if (line6_devices[i].product_id == product_id)
			return &line6_devices[i];
	}

	return NULL;
}

/*
 * Send a vendor-specific control command to the Line6 device.
 * Used for initialization (e.g., 0x0301 to enable TonePort audio).
 */
static usb_error_t
line6_send_cmd(struct usb_device *udev, uint16_t cmd1, uint16_t cmd2)
{
	struct usb_device_request req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = 0x67;
	USETW(req.wValue, cmd1);
	USETW(req.wIndex, cmd2);
	USETW(req.wLength, 0);		/* No data */

	return (usbd_do_request(udev, NULL, &req, NULL));
}

static usb_error_t
line6_send_cmd_retry(struct usb_device *udev, uint16_t cmd1, uint16_t cmd2,
    int retries)
{
	usb_error_t err = USB_ERR_NORMAL_COMPLETION;

	for (int i = 0; i < retries; i++) {
		err = line6_send_cmd(udev, cmd1, cmd2);
		if (err == 0)
			return 0;
		pause("line6cmd", hz / 20);
	}
	return err;
}

static int
line6_toneport_set_capture_source(struct line6_bsd_softc *sc)
{
	static const uint16_t capture_source_code[] = {
		0x0a01,	/* microphone */
		0x0801,	/* line */
		0x0b01,	/* instrument */
		0x0901,	/* inst + mic */
	};
	uint16_t cmd;
	int src;

	if (sc == NULL || !(sc->capabilities & LINE6_CAP_INIT_TONEPORT))
		return -EOPNOTSUPP;

	src = line6_capture_source;
	if (src < 0 || src >= (int)(sizeof(capture_source_code) /
	    sizeof(capture_source_code[0])))
		src = 0;
	cmd = capture_source_code[src];
	if (line6_send_cmd_retry(sc->usbdev, cmd, 0x0000, 3) != 0)
		return -EIO;
	return 0;
}

static int
line6_toneport_enable(struct line6_bsd_softc *sc)
{
	if (line6_send_cmd_retry(sc->usbdev, 0x0301, 0x0000, 5) != 0)
		return -EIO;
	(void)line6_toneport_set_capture_source(sc);
	return 0;
}

/*
 * Write data to a specific address on the Line6 device using vendor request 0x67.
 */
static usb_error_t
line6_write_data(struct usb_device *udev, uint16_t address, void *data, uint16_t length)
{
	struct usb_device_request req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = 0x67;
	USETW(req.wValue, address);
	USETW(req.wIndex, 0);
	USETW(req.wLength, length);

	return (usbd_do_request(udev, NULL, &req, data));
}

/*
 * Read data from a specific address on the Line6 device using vendor request 0x67.
 */
static usb_error_t
line6_read_data(struct usb_device *udev, uint16_t address, void *data, uint16_t length)
{
	struct usb_device_request req;

	req.bmRequestType = UT_READ_VENDOR_DEVICE;
	req.bRequest = 0x67;
	USETW(req.wValue, address);
	USETW(req.wIndex, 0);
	USETW(req.wLength, length);

	return (usbd_do_request(udev, NULL, &req, data));
}

/* PCM callback stubs - implement basic audio stream handling */
static int
line6_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	
	if (runtime == NULL)
		return -ENOMEM;
	
	/* Set hardware constraints for playback/capture */
	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S16_LE;
	runtime->hw.rates = SNDRV_PCM_RATE_44100;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 44100;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.buffer_bytes_max = 60000;
	runtime->hw.period_bytes_min = 64;
	runtime->hw.period_bytes_max = 8192;
	runtime->hw.periods_min = 1;
	runtime->hw.periods_max = 1024;
	
	return 0;
}

static int
line6_pcm_close(struct snd_pcm_substream *substream)
{
	/* Clean up any stream-specific resources */
	return 0;
}

static int
line6_pcm_hw_params(struct snd_pcm_substream *substream, void *hw_params)
{
	/*
	 * DMA memory is already managed by basound_chan_init via sndbuf_alloc;
	 * runtime->dma_area/dma_addr/dma_bytes are set there.  Do not call
	 * snd_pcm_lib_malloc_pages here — that would overwrite private_data
	 * with a snd_dma_buffer pointer and corrupt the runtime state.
	 */
	return 0;
}

static int
line6_pcm_hw_free(struct snd_pcm_substream *substream)
{
	/* DMA buffers are owned by basound_chan_init/basound_chan_free */
	return 0;
}

static int
line6_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	
	if (runtime == NULL || runtime->dma_area == NULL)
		return -EINVAL;
	
	/* Reset playback/capture position */
	runtime->dma_position = 0;
	
	return 0;
}

static int
line6_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct basound_chan *ch;
	struct snd_pcm_runtime *runtime;
	struct line6_bsd_softc *sc;
	struct line6_audio_stream *st;

	if (substream == NULL || substream->runtime == NULL ||
	    substream->pcm == NULL || substream->pcm->card == NULL ||
	    substream->pcm->card->dev == NULL)
		return -EINVAL;

	ch = (struct basound_chan *)substream->private_data;
	if (ch == NULL)
		return -EINVAL;

	runtime = substream->runtime;

	/* Recover softc via the alsa_dev back-pointer embedded in softc */
	sc = __containerof(substream->pcm->card->dev,
	    struct line6_bsd_softc, alsa_dev);

	st = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
	    &sc->play : &sc->rec;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START: {
		const struct usb_config *cfg;
		usb_error_t uerr;
		uint32_t channels, bps, sample_size;
		uint32_t stream_bit;

		if (runtime->dma_area == NULL || runtime->dma_bytes == 0)
			return -EINVAL;

		/* Derive USB frame size from the negotiated format + rate */
		channels = AFMT_CHANNEL(ch->format);
		if (channels == 0)
			channels = 2;
		if (ch->format & AFMT_S32_LE)
			bps = 4;
#ifdef AFMT_S24_PACKED
		else if (ch->format & AFMT_S24_PACKED)
			bps = 3;
#endif
		else if (ch->format & AFMT_S24_LE)
			bps = 4;
		else
			bps = 2;	/* S16_LE default */
		sample_size = channels * bps;
		if (sample_size == 0)
			sample_size = 4;	/* stereo S16_LE safety */

		cfg = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		    line6_play_cfg : line6_rec_cfg;
		stream_bit = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		    1 : 2;

		/*
		 * Switch interface 0 to the audio alt setting so the ISO
		 * endpoints become visible.  Safe to call for both play and rec
		 * — each call is idempotent once the interface is already at the
		 * requested alt setting.
		 *
		 * Some Line6 devices return USB_ERR_IOERROR from SET_INTERFACE
		 * but still switch to the requested alt setting (error in the
		 * USB status phase only).  Treat this as a non-fatal warning and
		 * let usbd_transfer_setup below be the real gate — it will fail
		 * with a clear error if the ISO endpoints are not actually present.
		 *
		 * Must NOT be called with sc_lock held (sleepable lock internally).
		 */
		uerr = line6_set_audio_alt(sc);
		if (uerr != 0) {
			device_printf(sc->dev,
			    "set audio alt warning on iface %u: %s — continuing\n",
			    sc->audio_iface_index,
			    usbd_errstr(uerr));
		}

		/*
		 * TonePort/POD Studio command 0x0301 is required on some
		 * devices, but failures are observed on certain firmware
		 * revisions. Keep it best-effort so stream startup is not
		 * blocked by control request quirks.
		 */
		if ((sc->capabilities & LINE6_CAP_INIT_TONEPORT) &&
		    !sc->toneport_enabled) {
			if (line6_toneport_enable(sc) < 0) {
				device_printf(sc->dev,
				    "toneport enable (0x0301) failed at stream "
				    "start; continuing\n");
			} else {
				sc->toneport_enabled = 1;
			}
		}

		/*
		 * Re-apply capture source on capture stream start so sysctl
		 * changes take effect without module reload.
		 */
		if ((sc->capabilities & LINE6_CAP_INIT_TONEPORT) &&
		    substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			if (line6_toneport_set_capture_source(sc) < 0) {
				device_printf(sc->dev,
				    "capture source command failed at stream "
				    "start; continuing\n");
			}
		}

		/*
		 * Set up ISO transfers now that alt endpoints are visible.
		 * usbd_transfer_setup must NOT be called with sc_lock held.
		 */
		struct usb_config cfg_copy[LINE6_NCHANBUFS];
		memcpy(cfg_copy, cfg, sizeof(cfg_copy));
		for (int i = 0; i < LINE6_NCHANBUFS; i++) {
			cfg_copy[i].endpoint = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
			    sc->ep_audio_w : sc->ep_audio_r;
		}

		uerr = usbd_transfer_setup(sc->usbdev, &sc->audio_iface_index,
		    st->xfer, cfg_copy, LINE6_NCHANBUFS, st, &sc->sc_lock);
		if (uerr != 0) {
			device_printf(sc->dev,
			    "transfer setup failed: %s\n", usbd_errstr(uerr));
			if (sc->audio_active == 0)
				(void)usbd_set_alt_interface_index(sc->usbdev,
				    sc->audio_iface_index, 0);
			return -EIO;
		}

		mtx_lock(&sc->sc_lock);
		sc->audio_active |= stream_bit;
		st->frames_per_second = usbd_get_isoc_fps(sc->usbdev);
		if (st->frames_per_second == 0)
			st->frames_per_second = 1000;	/* full-speed fallback */
		st->intr_frames = LINE6_NFRAMES;
		st->sample_size = sample_size;
		st->bytes_per_frame[0] =
		    (ch->speed / st->frames_per_second) * sample_size;
		st->sample_rem = ch->speed % st->frames_per_second;
		st->bytes_per_frame[1] = st->bytes_per_frame[0] + sample_size;
		if (st->bytes_per_frame[0] == 0 && st->sample_rem == 0) {
			st->bytes_per_frame[0] = sample_size;
			st->bytes_per_frame[1] = sample_size;
		}
		st->sample_curr = 0;

		st->start = (uint8_t *)runtime->dma_area;
		st->end   = (uint8_t *)runtime->dma_area + runtime->dma_bytes;
		st->cur   = (uint8_t *)runtime->dma_area;
		st->hwptr_bytes = 0;
		st->pcm_ch = ch->channel;
		st->running = 1;
		printf("line6 trigger: dir=%d pcm_ch=%p name=%s format=0x%08x speed=%u\n",
		    substream->stream, st->pcm_ch,
		    (st->pcm_ch != NULL) ? st->pcm_ch->name : "<null>",
		    (st->pcm_ch != NULL) ? st->pcm_ch->format : 0, ch->speed);

		{
			const uint8_t *d = (const uint8_t *)runtime->dma_area;
			uint32_t nz = 0;
			for (uint32_t i = 0; i < 1024 && i < runtime->dma_bytes; i++)
				if (d[i]) nz++;
			printf("line6 pcm trigger START dir=%d speed=%u\n",
			    substream->stream, ch->speed);
			printf("line6 trigger: dma_area=%p dma_bytes=%zu"
			    " nonzero_in_first_1024=%u bytes=[%02x %02x %02x %02x"
			    " %02x %02x %02x %02x]\n",
			    runtime->dma_area, runtime->dma_bytes, nz,
			    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
		}

		for (int i = 0; i < LINE6_NCHANBUFS; i++)
			usbd_transfer_start(st->xfer[i]);
		mtx_unlock(&sc->sc_lock);

		runtime->state = SNDRV_PCM_STATE_RUNNING;
		runtime->dma_position = 0;

		/*
		 * TonePort/POD Studio devices need the ISO OUT endpoint to be
		 * streaming for clean capture audio.  If only capture was
		 * started (no playback), start a helper playback stream that
		 * sends silence.
		 */
		if ((sc->capabilities & LINE6_CAP_INIT_TONEPORT) &&
		    substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		    !(sc->audio_active & 1)) {
			int perr;

			perr = line6_start_play_helper(sc, sample_size);
			if (perr != 0)
				device_printf(sc->dev,
				    "play helper start failed: %d\n", perr);
		}

		/*
		 * If playback started while helper was active, stop the
		 * helper — real playback takes over.
		 */
		if ((sc->capabilities & LINE6_CAP_INIT_TONEPORT) &&
		    substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
		    sc->play_helper)
			line6_stop_play_helper(sc);

		return 0;
	}

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH: {
		uint32_t stream_bit = (substream->stream ==
		    SNDRV_PCM_STREAM_PLAYBACK) ? 1 : 2;

		printf("line6 pcm trigger STOP dir=%d\n", substream->stream);
		mtx_lock(&sc->sc_lock);
		st->running = 0;
		sc->audio_active &= ~stream_bit;
		for (int i = 0; i < LINE6_NCHANBUFS; i++)
			usbd_transfer_stop(st->xfer[i]);
		mtx_unlock(&sc->sc_lock);

		/* Must be called without sc_lock held */
		usbd_transfer_unsetup(st->xfer, LINE6_NCHANBUFS);

		/* Release ISO bandwidth only when both streams are stopped */
		if (sc->audio_active == 0)
			(void)usbd_set_alt_interface_index(sc->usbdev,
			    sc->audio_iface_index, 0);

		/*
		 * If capture stopped and helper was active, stop the helper.
		 * If playback stopped but capture is still active, start helper.
		 */
		if (sc->capabilities & LINE6_CAP_INIT_TONEPORT) {
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
			    sc->play_helper)
				line6_stop_play_helper(sc);
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
			    sc->audio_active & 2) {
				/* capture still active, start helper */
				(void)line6_start_play_helper(sc, 4);
			}
		}

		runtime->state = SNDRV_PCM_STATE_STOPPED;
		return 0;
	}

	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE: {
		const struct usb_config *cfg = (substream->stream ==
		    SNDRV_PCM_STREAM_PLAYBACK) ?
		    line6_play_cfg : line6_rec_cfg;
		uint32_t stream_bit = (substream->stream ==
		    SNDRV_PCM_STREAM_PLAYBACK) ? 1 : 2;
		usb_error_t uerr;

		uerr = line6_set_audio_alt(sc);
		if (uerr != 0) {
			device_printf(sc->dev,
			    "set audio alt warning on iface %u: %s — continuing\n",
			    sc->audio_iface_index,
			    usbd_errstr(uerr));
		}

		struct usb_config cfg_copy[LINE6_NCHANBUFS];
		memcpy(cfg_copy, cfg, sizeof(cfg_copy));
		for (int i = 0; i < LINE6_NCHANBUFS; i++) {
			cfg_copy[i].endpoint = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
			    sc->ep_audio_w : sc->ep_audio_r;
		}

		uerr = usbd_transfer_setup(sc->usbdev, &sc->audio_iface_index,
		    st->xfer, cfg_copy, LINE6_NCHANBUFS, st, &sc->sc_lock);
		if (uerr != 0) {
			if (sc->audio_active == 0)
				(void)usbd_set_alt_interface_index(sc->usbdev,
				    sc->audio_iface_index, 0);
			return -EIO;
		}
		mtx_lock(&sc->sc_lock);
		sc->audio_active |= stream_bit;
		st->running = 1;
		for (int i = 0; i < LINE6_NCHANBUFS; i++)
			usbd_transfer_start(st->xfer[i]);
		mtx_unlock(&sc->sc_lock);
		runtime->state = SNDRV_PCM_STATE_RUNNING;
		return 0;
	}

	default:
		return -EINVAL;
	}
}

static unsigned long
line6_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct line6_bsd_softc *sc;
	struct line6_audio_stream *st;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long bytes;

	if (substream == NULL || substream->pcm == NULL ||
	    substream->pcm->card == NULL ||
	    substream->pcm->card->dev == NULL ||
	    runtime == NULL)
		return 0;

	sc = __containerof(substream->pcm->card->dev,
	    struct line6_bsd_softc, alsa_dev);

	st = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
	    &sc->play : &sc->rec;

	if (st->start == NULL || st->cur == NULL || st->pcm_ch == NULL)
		return 0;

	bytes = st->hwptr_bytes;
	return bytes;
}

/*
 * Start the helper playback stream that sends silence to the TonePort.
 * This keeps the ISO OUT endpoint active so the device's internal DSP
 * routes capture audio correctly.  Without this, capture produces metallic
 * distortion on TonePort/POD Studio devices.
 *
 * Must NOT be called with sc_lock held.
 */
static int
line6_start_play_helper(struct line6_bsd_softc *sc, uint32_t sample_size)
{
	struct usb_config cfg_copy[LINE6_NCHANBUFS];
	struct line6_audio_stream *ps = &sc->play;
	usb_error_t uerr;

	if (sc->play_helper)
		return 0;		/* already running */
	if (sc->play_helper_buf == NULL) {
		sc->play_helper_buf = malloc(PLAY_HELPER_BUF_SIZE,
		    M_ALSA, M_WAITOK | M_ZERO);
		if (sc->play_helper_buf == NULL)
			return -ENOMEM;
	}

	/* Set up isochronous transfers for the OUT endpoint */
	memcpy(cfg_copy, line6_play_cfg, sizeof(cfg_copy));
	for (int i = 0; i < LINE6_NCHANBUFS; i++)
		cfg_copy[i].endpoint = sc->ep_audio_w;

	uerr = usbd_transfer_setup(sc->usbdev, &sc->audio_iface_index,
	    ps->xfer, cfg_copy, LINE6_NCHANBUFS, ps, &sc->sc_lock);
	if (uerr != 0) {
		device_printf(sc->dev,
		    "play helper setup failed: %s\n", usbd_errstr(uerr));
		return -EIO;
	}

	mtx_lock(&sc->sc_lock);
	ps->frames_per_second = usbd_get_isoc_fps(sc->usbdev);
	if (ps->frames_per_second == 0)
		ps->frames_per_second = 1000;
	ps->intr_frames = LINE6_NFRAMES;
	ps->sample_size = sample_size;
	ps->bytes_per_frame[0] =
	    (44100 / ps->frames_per_second) * sample_size;
	ps->sample_rem = 44100 % ps->frames_per_second;
	ps->bytes_per_frame[1] = ps->bytes_per_frame[0] + sample_size;
	if (ps->bytes_per_frame[0] == 0 && ps->sample_rem == 0) {
		ps->bytes_per_frame[0] = sample_size;
		ps->bytes_per_frame[1] = sample_size;
	}
	ps->sample_curr = 0;

	/* Point the stream at the zeroed silence ring buffer */
	memset(sc->play_helper_buf, 0, PLAY_HELPER_BUF_SIZE);
	ps->start = sc->play_helper_buf;
	ps->end   = sc->play_helper_buf + PLAY_HELPER_BUF_SIZE;
	ps->cur   = sc->play_helper_buf;
	ps->hwptr_bytes = 0;
	ps->pcm_ch = NULL;		/* no PCM channel to notify */
	ps->running = 1;
	sc->play_helper = 1;

	for (int i = 0; i < LINE6_NCHANBUFS; i++)
		usbd_transfer_start(ps->xfer[i]);
	mtx_unlock(&sc->sc_lock);

	return 0;
}

/*
 * Stop the helper playback stream.
 */
static void
line6_stop_play_helper(struct line6_bsd_softc *sc)
{
	struct line6_audio_stream *ps = &sc->play;

	if (!sc->play_helper)
		return;

	mtx_lock(&sc->sc_lock);
	ps->running = 0;
	for (int i = 0; i < LINE6_NCHANBUFS; i++)
		usbd_transfer_stop(ps->xfer[i]);
	mtx_unlock(&sc->sc_lock);

	usbd_transfer_unsetup(ps->xfer, LINE6_NCHANBUFS);
	sc->play_helper = 0;
	ps->start = NULL;
	ps->end = NULL;
	ps->cur = NULL;
}

static const struct snd_pcm_ops line6_pcm_ops = {
	.open = line6_pcm_open,
	.close = line6_pcm_close,
	.ioctl = NULL,
	.hw_params = line6_pcm_hw_params,
	.hw_free = line6_pcm_hw_free,
	.prepare = line6_pcm_prepare,
	.trigger = line6_pcm_trigger,
	.pointer = line6_pcm_pointer,
};

/*
 * ALSA-style volume controls for Line6 devices.
 * The Linux driver applies volume purely in software (change_volume() in
 * playback.c scales PCM samples before sending to USB).  The UX1 has no
 * documented vendor-register for PCM or monitor volume, so we follow Linux
 * and handle volume at the PCM software layer only.
 */

static int
line6_control_pcm_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	/* Volume is applied by the PCM software-mixing layer; nothing to do here. */
	(void)kctl;
	(void)ucontrol;
	return 1;
}

static int
line6_control_monitor_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	/* Monitor volume is managed via sysctl hw.basound.line6.monitor_volume */
	(void)kctl;
	(void)ucontrol;
	return 1;
}

static struct snd_kcontrol_new line6_controls[] = {
	{
		.name = "PCM Playback Volume",
		.count = 2,
		.put = line6_control_pcm_put,
	},
	{
		.name = "Monitor Playback Volume",
		.count = 2,
		.put = line6_control_monitor_put,
	},
};

/* Sysctl for monitor volume control */
static int sysctl_monitor_volume = 0;

static int
sysctl_line6_monitor_volume(SYSCTL_HANDLER_ARGS)
{
	int err, v;

	v = sysctl_monitor_volume;
	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (v < 0)
		v = 0;
	if (v > 256)
		v = 256;
	sysctl_monitor_volume = v;

	/* Apply to active device */
	if (line6_active_sc != NULL) {
		line6_active_sc->monitor_volume = v;
		if (v == 0)
			line6_active_sc->monitor_fsize = 0;
	}

	return (0);
}

SYSCTL_PROC(_hw_basound_line6, OID_AUTO, monitor_volume,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, 0, 0,
    sysctl_line6_monitor_volume, "I",
    "Monitor volume: 0=off 256=unity (software monitor mix)");

/* USB device probe routine */
static int
line6_bsd_probe(device_t dev)
{
	struct usb_attach_arg *uaa;
	const struct line6_device_info *info;

	uaa = device_get_ivars(dev);
	if (uaa == NULL)
		return ENXIO;

	/* Only attach to the AudioControl interface (bInterfaceNumber == 0).
	 * The driver then reaches out to the AudioStreaming interfaces itself. */
	if (uaa->info.bIfaceNum != 0)
		return ENXIO;

	/* Check for Line6 vendor ID */
	if (USB_GET_DRIVER_INFO(uaa) == 0) {
		if (uaa->info.idVendor != LINE6_VENDOR_ID)
			return ENXIO;
	}

	/* Find device in database */
	info = line6_bsd_find_device(uaa->info.idProduct);
	if (info == NULL)
		return ENXIO;

	return BUS_PROBE_DEFAULT;
}

/* USB device attach routine */
static int
line6_bsd_attach(device_t dev)
{
	struct usb_attach_arg *uaa;
	struct line6_bsd_softc *sc;
	const struct line6_device_info *info;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;

	uaa = device_get_ivars(dev);
	if (uaa == NULL)
		return ENXIO;

	/* Find device info */
	info = line6_bsd_find_device(uaa->info.idProduct);
	if (info == NULL)
		return ENXIO;

	sc = device_get_softc(dev);
	if (sc == NULL)
		return ENXIO;

	sc->dev = dev;
	sc->usbdev = uaa->device;
	sc->idesc = usbd_get_interface_descriptor(uaa->iface);
	if (sc->idesc == NULL) {
		device_printf(dev, "Failed to get interface descriptor\n");
		return ENXIO;
	}
	sc->ctrl_iface_index = uaa->info.bIfaceIndex;

	sc->capabilities = info->capabilities;
	sc->device_name = info->name;
	sc->ep_audio_r = info->ep_audio_r;
	sc->ep_audio_w = info->ep_audio_w;
	sc->alsa_dev.bsddev = dev;

	mtx_init(&sc->sc_lock, "line6_lock", NULL, MTX_DEF);

	device_printf(dev, "Attaching %s (USB %04x:%04x), Caps: 0x%x\n",
	    info->name, uaa->info.idVendor, uaa->info.idProduct, sc->capabilities);

	/*
	 * POD Studio / TonePort: single USB interface (0) with multiple alt
	 * settings.  Alt 0 = zero-bandwidth.  Alt LINE6_ALT_AUDIO activates
	 * ISO OUT (0x01) and ISO IN (0x82) endpoints on the same interface.
	 * The ISO endpoints appear in the config descriptor under alt settings
	 * of interface 0, which is why ifaces_max=1 is normal and correct.
	 */
	sc->audio_iface_index = sc->ctrl_iface_index; /* always iface 0 */
	sc->audio_altsetting = LINE6_ALT_AUDIO;
	sc->audio_active = 0;
	sc->toneport_enabled = 0;

	/*
	 * Do NOT call usbd_set_alt_interface_index(alt=0) here.
	 * The USB stack sends SET_CONFIGURATION during enumeration which
	 * already resets all interfaces to alt=0.  An explicit SET_INTERFACE
	 * (even to alt=0) can stall the device's control endpoint, making
	 * subsequent vendor requests (0x80c2 firmware read, 0x0301 enable)
	 * fail with USB_ERR_IOERROR.  The reset belongs in line6_bsd_detach()
	 * so that a clean kldunload → kldload cycle never sees a stale alt.
	 */

	if (sc->capabilities & LINE6_CAP_INIT_TONEPORT) {
		uint32_t ticks;
		struct timespec ts;
		uint8_t fw_version;

		device_printf(dev, "Initializing TonePort/POD Studio\n");

		/* Read firmware version (matches Linux toneport_init) */
		err = line6_read_data(sc->usbdev, 0x80c2, &fw_version, 1);
		if (err == 0)
			device_printf(dev, "Firmware version: %d\n", fw_version);

		/* Sync time on device with host (Linux toneport_setup does this) */
		getnanotime(&ts);
		ticks = (uint32_t)ts.tv_sec;
		line6_write_data(sc->usbdev, 0x80c6, &ticks, 4);

		err = line6_toneport_enable(sc);
		if (err != 0) {
			device_printf(dev,
			    "Initialization (0x0301) failed; will retry on "
			    "stream start\n");
		} else {
			sc->toneport_enabled = 1;
			device_printf(dev, "Device enabled (0x0301 success)\n");
		}
	}

	if (info->capabilities & (LINE6_CAP_AUDIO_IN | LINE6_CAP_AUDIO_OUT)) {
		if (!line6_has_iso_endpoints(sc->usbdev))
			device_printf(dev, "warning: no ISO endpoints found in "
			    "descriptor (alt setting change will be needed)\n");
	}

	/* Create ALSA sound card */
	err = snd_card_new(&sc->alsa_dev, 0, info->card_id, NULL, 0, &card);
	if (err < 0 || card == NULL) {
		device_printf(dev, "Failed to create sound card: %d\n", err);
		goto fail;
	}

	/* Register hardware volume controls to ALSA card */
	for (int i = 0; i < nitems(line6_controls); i++) {
		snd_ctl_add(card, snd_ctl_new1(&line6_controls[i], sc));
	}

	/* Set card properties */
	snprintf(card->driver, sizeof(card->driver), "line6_bsd");
	snprintf(card->shortname, sizeof(card->shortname), "%s", info->name);
	snprintf(card->longname, sizeof(card->longname),
	    "%s at USB", info->name);

	/* Create PCM device for audio I/O */
	if (info->capabilities & (LINE6_CAP_AUDIO_IN | LINE6_CAP_AUDIO_OUT)) {
		int playback_count =
		    (info->capabilities & LINE6_CAP_AUDIO_OUT) ? 1 : 0;
		int capture_count =
		    (info->capabilities & LINE6_CAP_AUDIO_IN)  ? 1 : 0;

		err = snd_pcm_new(card, info->card_id, 0,
		    playback_count, capture_count, &pcm);
		if (err < 0 || pcm == NULL) {
			device_printf(dev, "Failed to create PCM device: %d\n",
			    err);
			snd_card_free(card);
			goto fail;
		}

		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &line6_pcm_ops);
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &line6_pcm_ops);

		snprintf(pcm->name, sizeof(pcm->name), "%s", info->name);
	}

	/* Create MIDI device for instrument control */
	if (info->capabilities & LINE6_CAP_MIDI) {
		struct snd_rawmidi *rmidi;

		err = snd_rawmidi_new(card, "Line6 MIDI", 0, 1, 1, &rmidi);
		if (err < 0)
			device_printf(dev,
			    "Failed to create MIDI device: %d\n", err);
	}

	/* Register sound card with FreeBSD */
	err = snd_card_register(card);
	if (err < 0) {
		device_printf(dev, "Failed to register sound card: %d\n", err);
		snd_card_free(card);
		goto fail;
	}

	sc->alsa_line6 = card;
	line6_active_sc = sc;

	device_printf(dev, "Line6 device attached - %s registered\n",
	    info->name);
	return 0;

fail:
	if (mtx_initialized(&sc->sc_lock))
		mtx_destroy(&sc->sc_lock);
	return ENXIO;
}

static int
line6_bsd_detach(device_t dev)
{
	struct line6_bsd_softc *sc;
	struct snd_card *card;

	sc = device_get_softc(dev);
	if (sc == NULL)
		return 0;
	if (line6_active_sc == sc)
		line6_active_sc = NULL;

	/* Stop helper playback and tear down USB isochronous transfers */
	line6_stop_play_helper(sc);
	if (sc->play_helper_buf != NULL) {
		free(sc->play_helper_buf, M_ALSA);
		sc->play_helper_buf = NULL;
	}
	mtx_lock(&sc->sc_lock);
	sc->play.running = 0;
	sc->rec.running = 0;
	for (int i = 0; i < LINE6_NCHANBUFS + 1; i++) {
		if (sc->play.xfer[i] != NULL) usbd_transfer_stop(sc->play.xfer[i]);
		if (sc->rec.xfer[i] != NULL) usbd_transfer_stop(sc->rec.xfer[i]);
	}
	mtx_unlock(&sc->sc_lock);

	usbd_transfer_unsetup(sc->play.xfer, LINE6_NCHANBUFS + 1);
	usbd_transfer_unsetup(sc->rec.xfer, LINE6_NCHANBUFS + 1);

	/*
	 * Reset to zero-bandwidth alt setting so the device is in a clean
	 * state after unload.  Without this the device stays in alt=2 (ISO
	 * endpoints active) and the next kldload will see USB_ERR_IOERROR on
	 * vendor control requests (firmware confusion without active ISO DMA).
	 */
	(void)usbd_set_alt_interface_index(sc->usbdev,
	    sc->audio_iface_index, 0);

	if (sc->alsa_line6 != NULL) {
		card = (struct snd_card *)sc->alsa_line6;
		snd_card_free(card);
		sc->alsa_line6 = NULL;
	}

	if (mtx_initialized(&sc->sc_lock))
		mtx_destroy(&sc->sc_lock);

	return 0;
}

static device_method_t line6_bsd_methods[] = {
	DEVMETHOD(device_probe,		line6_bsd_probe),
	DEVMETHOD(device_attach,	line6_bsd_attach),
	DEVMETHOD(device_detach,	line6_bsd_detach),
	/* Bus methods needed to host the "pcm" child device */
	DEVMETHOD(bus_add_child,	bus_generic_add_child),
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD_END
};

static driver_t line6_bsd_driver = {
	"basound_line6",
	line6_bsd_methods,
	sizeof(struct line6_bsd_softc),
};

DRIVER_MODULE(basound_line6, uhub, line6_bsd_driver, 0, 0);
MODULE_DEPEND(basound_line6, basound, 1, 1, 1);
MODULE_DEPEND(basound_line6, usb, 1, 1, 1);
MODULE_DEPEND(basound_line6, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
