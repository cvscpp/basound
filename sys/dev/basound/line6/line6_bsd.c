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

/* Line6 USB driver - bridges FreeBSD usb_device to ALSA Line6 driver */

MALLOC_DECLARE(M_ALSA);

/*
 * Test tone sysctl: set to non-zero to inject a 1kHz square wave into USB
 * output, bypassing the DMA/feeder path.  Useful for verifying the hardware
 * output path works independently of the FreeBSD PCM stack.
 *
 * Usage:
 *   # Keep the device open with continuous writes:
 *   dd if=/dev/zero of=/dev/dsp2 &
 *   # Enable 1kHz test tone:
 *   sysctl hw.basound.line6.test_tone=1
 *   # Return to normal playback:
 *   sysctl hw.basound.line6.test_tone=0
 *
 * If the tone is audible: USB output path works; silence is because the app
 * writes zero samples.  Test with: cat /dev/urandom > /dev/dsp2
 * If still silent: hardware routing or USB framing issue.
 */
static int line6_test_tone = 0;
static uint32_t line6_test_tone_phase = 0; /* sample counter for test tone */

SYSCTL_DECL(_hw);
SYSCTL_NODE(_hw, OID_AUTO, basound, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "basound USB audio driver");
SYSCTL_NODE(_hw_basound, OID_AUTO, line6, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Line6 USB audio");
SYSCTL_INT(_hw_basound_line6, OID_AUTO, test_tone, CTLFLAG_RW,
    &line6_test_tone, 0,
    "Inject 1kHz test tone into USB output (1=enable, 0=normal)");

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
	uint32_t	 audio_active;	/* bitmask: 1=play, 2=rec; 0=none active */
};

MALLOC_DEFINE(M_LINE6_BSD, "line6_bsd", "Line6 BSD softc");

/* Forward declarations for USB isochronous callbacks */
static usb_callback_t line6_play_callback;
static usb_callback_t line6_rec_callback;

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
	uint32_t total, n, offset, frame_len;
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

		/*
		 * Notify the PCM layer that the ring buffer was consumed and
		 * allow the feeder to refill it.  Called for both SETUP and
		 * TRANSFERRED so the feeder stays ahead of the USB pipeline
		 * during the initial burst of SETUP submissions.
		 *
		 * Drop sc_lock first: chn_intr acquires the PCM channel lock,
		 * and the channel lock → sc_lock order (from trigger) means we
		 * must not hold sc_lock when taking the channel lock.
		 */
		if (st->pcm_ch != NULL) {
			uint32_t ready_before = sndbuf_getready(st->pcm_ch->bufhard);
			uint32_t fp_before = sndbuf_getfreeptr(st->pcm_ch->bufhard);
			mtx_unlock(&sc->sc_lock);
			chn_intr(st->pcm_ch);
			mtx_lock(&sc->sc_lock);
			if (!st->running)
				break;
			if (dbg_cnt % 200 == 0) {
				uint32_t ready_after = sndbuf_getready(st->pcm_ch->bufhard);
				uint32_t fp_after = sndbuf_getfreeptr(st->pcm_ch->bufhard);
				uint32_t ring_size = (uint32_t)(st->end - st->start);

				/* Bytes just written by feeder_mixer start at fp_before */
				const int16_t *sf = (const int16_t *)(st->start +
				    fp_before % ring_size);
				int f0 = (fp_before + 8 <= ring_size) ? sf[0] : 0;
				int f1 = (fp_before + 8 <= ring_size) ? sf[1] : 0;
				int f2 = (fp_before + 8 <= ring_size) ? sf[2] : 0;
				int f3 = (fp_before + 8 <= ring_size) ? sf[3] : 0;

				/* Bytes about to be USB-sent start at st->cur */
				const int16_t *su = (const int16_t *)st->cur;
				int ns0 = (st->cur + 8 <= st->end) ? su[0] : 0;
				int ns1 = (st->cur + 8 <= st->end) ? su[1] : 0;
				int ns2 = (st->cur + 8 <= st->end) ? su[2] : 0;
				int ns3 = (st->cur + 8 <= st->end) ? su[3] : 0;

				printf("line6 play cb#%u state=%s"
				    " cur=%p(+%u) ring=[%u..%u]"
				    " ready=%u->%u fp=%u->%u"
				    " new=[%d,%d,%d,%d] usb=[%d,%d,%d,%d]\n",
				    dbg_cnt,
				    USB_GET_STATE(xfer) == USB_ST_SETUP ?
				        "SETUP" : "XFRD",
				    st->cur, (uint32_t)(st->cur - st->start),
				    0, ring_size,
				    ready_before, ready_after,
				    fp_before, fp_after,
				    f0, f1, f2, f3,
				    ns0, ns1, ns2, ns3);
			}
		}

		/* Compute per-frame lengths with rate jitter correction */
		usbd_xfer_set_frames(xfer, st->intr_frames);
		total = 0;
		for (n = 0; n < st->intr_frames; n++) {
			st->sample_curr += st->sample_rem;
			if (st->sample_curr >= st->frames_per_second) {
				st->sample_curr -= st->frames_per_second;
				frame_len = st->bytes_per_frame[1];
			} else {
				frame_len = st->bytes_per_frame[0];
			}
			usbd_xfer_set_frame_len(xfer, n, frame_len);
			total += frame_len;
		}

		/* Copy from DMA ring buffer into USB isochronous packet */
		offset = 0;
		pc = usbd_xfer_get_frame(xfer, 0);
		if (line6_test_tone) {
			/*
			 * Test tone mode: fill USB frames with a 1kHz square
			 * wave (S16_LE stereo, amplitude 16384) to verify that
			 * the hardware output path works independently of the
			 * PCM stack.  Toggle polarity every 22 samples ≈ 1kHz.
			 *
			 * st->cur still advances so chn_intr sees correct
			 * delta → feeder keeps draining the vchan bufsoft →
			 * the app (e.g. dd if=/dev/zero) does not block.
			 */
			uint32_t remaining = total;
			uint8_t tbuf[4]; /* one stereo S16_LE sample */
			while (remaining >= 4) {
				int16_t v = (line6_test_tone_phase % 44 < 22) ?
				    16384 : -16384;
				tbuf[0] = (uint8_t)(v & 0xff);
				tbuf[1] = (uint8_t)((v >> 8) & 0xff);
				tbuf[2] = tbuf[0]; /* same for right channel */
				tbuf[3] = tbuf[1];
				usbd_copy_in(pc, offset, tbuf, 4);
				offset += 4;
				remaining -= 4;
				line6_test_tone_phase++;
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
		usbd_transfer_submit(xfer);
		break;
	}

	default:	/* Error */
		if (error != USB_ERR_CANCELLED && st->running) {
			/* Transient error: send one silent frame and recover */
			usbd_xfer_set_frames(xfer, 1);
			usbd_xfer_set_frame_len(xfer, 0, st->bytes_per_frame[0]);
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
	uint32_t offset, n, len;
	int actlen, nframes, i;
	static uint32_t dbg_cnt = 0;

	usbd_xfer_status(xfer, &actlen, NULL, NULL, &nframes);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED: {
		dbg_cnt++;
		if (st->start == NULL || st->start == st->end)
			goto tr_setup;

		pc = usbd_xfer_get_frame(xfer, 0);
		offset = 0;
		for (i = 0; i < nframes; i++) {
			len = usbd_xfer_frame_len(xfer, i);
			while (len > 0) {
				n = (uint32_t)(st->end - st->cur);
				if (n > len)
					n = len;
				usbd_copy_out(pc, offset, st->cur, n);
				len -= n;
				st->cur += n;
				offset += n;
				if (st->cur >= st->end)
					st->cur = st->start;
			}
		}

		if (dbg_cnt % 200 == 0)
			printf("line6 rec cb#%u cur+%u actlen=%d\n",
			    dbg_cnt, (uint32_t)(st->cur - st->start), actlen);

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
		usbd_xfer_set_frames(xfer, st->intr_frames);
		for (i = 0; i < (int)st->intr_frames; i++)
			usbd_xfer_set_frame_len(xfer, i, st->bytes_per_frame[1]);
		usbd_transfer_submit(xfer);
		break;

	default:	/* Error */
		if (error != USB_ERR_CANCELLED && st->running) {
			usbd_xfer_set_frames(xfer, 1);
			usbd_xfer_set_frame_len(xfer, 0, st->bytes_per_frame[1]);
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
	runtime->hw.buffer_bytes_max = 1 << 20;
	runtime->hw.period_bytes_min = 64;
	runtime->hw.period_bytes_max = 1 << 16;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 128;
	
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
		if (ch->format & AFMT_S32_LE)
			bps = 4;
		else if (ch->format & AFMT_S24_LE)
			bps = 3;
		else
			bps = 2;	/* S16_LE default */
		sample_size = channels * bps;

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
		uerr = usbd_set_alt_interface_index(sc->usbdev,
		    sc->audio_iface_index, LINE6_ALT_AUDIO);
		if (uerr != 0) {
			device_printf(sc->dev,
			    "set alt %u warning on iface %u: %s — continuing\n",
			    LINE6_ALT_AUDIO, sc->audio_iface_index,
			    usbd_errstr(uerr));
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
		st->bytes_per_frame[0] =
		    (ch->speed / st->frames_per_second) * sample_size;
		st->sample_rem = ch->speed % st->frames_per_second;
		st->bytes_per_frame[1] = st->bytes_per_frame[0] + sample_size;
		st->sample_curr = 0;

		st->start = (uint8_t *)runtime->dma_area;
		st->end   = (uint8_t *)runtime->dma_area + runtime->dma_bytes;
		st->cur   = (uint8_t *)runtime->dma_area;
		st->pcm_ch = ch->channel;
		st->running = 1;

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

		uerr = usbd_set_alt_interface_index(sc->usbdev,
		    sc->audio_iface_index, LINE6_ALT_AUDIO);
		if (uerr != 0) {
			device_printf(sc->dev,
			    "set alt %u warning on iface %u: %s — continuing\n",
			    LINE6_ALT_AUDIO, sc->audio_iface_index,
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

	bytes = (unsigned long)(st->cur - st->start);

	/* Convert bytes to frames for ALSA shim */
	uint32_t frame_bytes = AFMT_BPS(st->pcm_ch->format) * AFMT_CHANNEL(st->pcm_ch->format);
	if (frame_bytes == 0)
		return 0;
	return (unsigned long)(bytes / frame_bytes);
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
	/* Monitor volume is software-only; no USB vendor command needed. */
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
	sc->audio_active = 0;

	/*
	 * Explicitly set alt=0 (zero-bandwidth) at attach time.  If a previous
	 * module load crashed without resetting the interface, the device may
	 * still be in alt=2 (audio streaming) which causes vendor control
	 * requests (e.g. 0x0301) to fail with USB_ERR_IOERROR.  Setting alt=0
	 * first brings the device back to a known state.  Ignore errors here
	 * since the device may already be at alt=0.
	 */
	(void)usbd_set_alt_interface_index(sc->usbdev,
	    sc->audio_iface_index, 0);

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

		/* 1. Enable device */
		err = line6_send_cmd(sc->usbdev, 0x0301, 0x0000);
		if (err != 0) {
			device_printf(dev, "Initialization (0x0301) failed: %s\n",
			    usbd_errstr(err));
		} else {
			device_printf(dev, "Device enabled (0x0301 success)\n");

			/* 2. Select input source: Microphone (0x0a01) */
			line6_send_cmd(sc->usbdev, 0x0a01, 0x0000);
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

	/* Stop and tear down USB isochronous transfers if still running */
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
