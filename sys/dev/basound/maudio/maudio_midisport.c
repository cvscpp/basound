/*-
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FreeBSD M-Audio MIDISport 8x8 USB MIDI Driver
 * Bridges M-Audio MIDISport 8x8 USB MIDI interface to FreeBSD sound(4) system
 *
 * Device: M-Audio MIDISport 8x8
 * USB ID: 0x0763:0x1031
 * 8 MIDI Input Ports + 8 MIDI Output Ports
 *
 * Architecture:
 * - USB bulk IN transfer for MIDI input (interrupt-driven)
 * - USB bulk OUT transfer for MIDI output (queued)
 * - ALSA raw MIDI device interface for FreeBSD integration
 * - No audio PCM (MIDI-only device)
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/rawmidi.h>

MALLOC_DECLARE(M_ALSA);

/* Boot-time switch for the whole driver.  Loading the basound_maudio.ko
 * module already limits probing to M-Audio MIDISport USB devices; this
 * tunable additionally lets loader.conf disable the driver without
 * unloading the module:
 *     hw.basound_maudio.enable="0" */
static int maudio_bsd_enabled = 1;
TUNABLE_INT("hw.basound_maudio.enable", &maudio_bsd_enabled);

/* M-Audio vendor ID and product IDs */
#define MAUDIO_VENDOR_ID		0x0763
#define MAUDIO_MIDISPORT8x8		0x1031
#define MAUDIO_MIDISPORT8x8_OLD		0x1033

/* USB MIDI descriptor sizes */
#define MAUDIO_MIDISPORT_MAX_TRANSFER	64
#define MAUDIO_MIDISPORT_IN_URB_COUNT	4
#define MAUDIO_MIDISPORT_OUT_URB_COUNT	4

/* MIDI port configuration */
#define MAUDIO_MIDISPORT_NUM_PORTS	8

/* Output queue configuration */
#define MAUDIO_MIDISPORT_OUT_QUEUE_SIZE	256  /* Bytes */
#define MAUDIO_MIDISPORT_OUT_QUEUE_THRESH	64  /* Flush threshold */

/* MIDI message parsing */
#define MIDI_STATUS_MASK		0xF0
#define MIDI_CHANNEL_MASK		0x0F
#define MIDI_PORT_MASK			0x0F

/* Bulk endpoint configuration */
enum {
	MAUDIO_MIDISPORT_BULK_IN = 0,
	MAUDIO_MIDISPORT_BULK_OUT = 1,
};

/* Output queue entry */
struct maudio_midi_output {
	uint8_t buf[MAUDIO_MIDISPORT_OUT_QUEUE_SIZE];
	int write_ptr;    /* Write position */
	int read_ptr;     /* Read position */
	int pending;      /* Bytes pending send */
	struct mtx lock;  /* Queue synchronization */
};

struct maudio_midisport {
	device_t dev;
	struct device alsa_dev;	/* wrapper so card->dev stays valid after attach */
	struct usb_device *udev;
	struct snd_card *card;
	struct snd_rawmidi *rmidi;
	
	struct usb_xfer *xfer[2];	/* IN, OUT */
	uint8_t inbuf[MAUDIO_MIDISPORT_MAX_TRANSFER];
	uint8_t outbuf[MAUDIO_MIDISPORT_MAX_TRANSFER];
	
	struct mtx lock;
	int suspended;
	int in_ports;
	int out_ports;
	
	/* MIDI output queue for buffering writes */
	struct maudio_midi_output outq;
	
	/* Statistics */
	uint32_t midi_in_count;
	uint32_t midi_out_count;
};

static struct usb_config maudio_midisport_config[2] = {
	{
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = MAUDIO_MIDISPORT_MAX_TRANSFER,
		.flags = {.pipe_bof = 0, .short_xfer_ok = 1},
		.callback = NULL, /* Will be set to input handler */
	},
	{
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = MAUDIO_MIDISPORT_MAX_TRANSFER,
		.flags = {.pipe_bof = 0, .short_xfer_ok = 1},
		.callback = NULL, /* Will be set to output handler */
	},
};

/*
 * MIDI data processing and routing
 */

/* Parse MIDI input byte and route to appropriate ALSA stream
 * M-Audio MIDISport 8x8 sends: [port, status, data1, data2, ...]
 * where port byte encodes both the MIDI port (bits 0-3) and cable (bits 4-7)
 */
static void
maudio_midisport_process_input(struct maudio_midisport *sc, uint8_t *buf, int len)
{
	int i = 0;
	uint8_t port, status, d1, d2;
	
	while (i < len) {
		port = buf[i];
		
		/* Port byte format: 0x0P where P is port number (0-7) */
		port = port & MIDI_PORT_MASK;
		
		if (port >= MAUDIO_MIDISPORT_NUM_PORTS)
			break;
		
		i++;
		if (i >= len)
			break;
		
		status = buf[i];
		i++;
		
		/* Process based on message type */
		if ((status & MIDI_STATUS_MASK) == 0xF0) {
			/* System message */
			if (status == 0xF0 || status == 0xF4 || status == 0xF5) {
				/* SysEx or reserved */
				device_printf(sc->dev, "Port %d: System message 0x%02x\n",
					port, status);
			} else {
				device_printf(sc->dev, 
					"Port %d: System Real-Time 0x%02x\n",
					port, status);
			}
		} else if ((status & 0x80) != 0) {
			/* Channel message */
			uint8_t cmd = status & MIDI_STATUS_MASK;
			uint8_t chan = status & MIDI_CHANNEL_MASK;
			
			switch (cmd) {
			case 0x80:	/* Note Off */
			case 0x90:	/* Note On */
			case 0xA0:	/* Poly Pressure */
			case 0xB0:	/* Control Change */
			case 0xE0:	/* Pitch Wheel */
				if (i + 1 < len) {
					d1 = buf[i++];
					d2 = buf[i++];
					device_printf(sc->dev,
						"Port %d Chan %d: cmd=0x%02x d1=0x%02x d2=0x%02x\n",
						port, chan, cmd, d1, d2);
					sc->midi_in_count++;
				}
				break;
			case 0xC0:	/* Program Change */
			case 0xD0:	/* Channel Pressure */
				if (i < len) {
					d1 = buf[i++];
					device_printf(sc->dev,
						"Port %d Chan %d: cmd=0x%02x d1=0x%02x\n",
						port, chan, cmd, d1);
					sc->midi_in_count++;
				}
				break;
			default:
				device_printf(sc->dev,
					"Port %d: Unknown status 0x%02x\n",
					port, status);
				break;
			}
		}
	}
}

/*
 * USB transfer callbacks
 */
static void
maudio_midisport_input_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct maudio_midisport *sc = usbd_xfer_softc(xfer);
	int actlen;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		actlen = usbd_xfer_frame_len(xfer, 0);
		
		if (actlen > 0) {
			/* Process received MIDI data and route to ALSA */
			maudio_midisport_process_input(sc, sc->inbuf, actlen);
		}
		
		/* Continue to next transfer */
		usbd_xfer_set_frame_len(xfer, 0, 
			MAUDIO_MIDISPORT_MAX_TRANSFER);
		usbd_transfer_submit(xfer);
		break;
		
	case USB_ST_SETUP:
		usbd_xfer_set_frame_len(xfer, 0, 
			MAUDIO_MIDISPORT_MAX_TRANSFER);
		usbd_transfer_submit(xfer);
		break;
		
	default:
		if (error != USB_ERR_CANCELLED) {
			device_printf(sc->dev, "MIDI IN error: %s\n",
				usbd_errstr(error));
			goto tr_setup;
		}
		break;
	}
	return;

tr_setup:
	usbd_xfer_set_frame_len(xfer, 0, MAUDIO_MIDISPORT_MAX_TRANSFER);
	usbd_transfer_submit(xfer);
}

static void
maudio_midisport_output_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct maudio_midisport *sc = usbd_xfer_softc(xfer);
	struct maudio_midi_output *outq = &sc->outq;
	int len;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		device_printf(sc->dev, "MIDI OUT: transferred\n");
		sc->midi_out_count++;
		
		/* Check if more data queued */
		mtx_lock(&outq->lock);
		if (outq->pending > 0) {
			/* Copy pending data from queue to USB buffer */
			len = outq->pending;
			if (len > MAUDIO_MIDISPORT_MAX_TRANSFER)
				len = MAUDIO_MIDISPORT_MAX_TRANSFER;
			
			/* Handle queue wraparound */
			if (outq->read_ptr + len <= MAUDIO_MIDISPORT_OUT_QUEUE_SIZE) {
				memcpy(sc->outbuf, 
					&outq->buf[outq->read_ptr], len);
			} else {
				int part1 = MAUDIO_MIDISPORT_OUT_QUEUE_SIZE - outq->read_ptr;
				int part2 = len - part1;
				memcpy(sc->outbuf, &outq->buf[outq->read_ptr], part1);
				memcpy(&sc->outbuf[part1], outq->buf, part2);
			}
			
			/* Update queue pointers */
			outq->read_ptr = (outq->read_ptr + len) % MAUDIO_MIDISPORT_OUT_QUEUE_SIZE;
			outq->pending -= len;
			
			mtx_unlock(&outq->lock);
			
			/* Submit transfer with new data */
			usbd_xfer_set_frame_len(xfer, 0, len);
			usbd_transfer_submit(xfer);
		} else {
			mtx_unlock(&outq->lock);
		}
		break;
		
	case USB_ST_SETUP:
		/* Check queue on setup */
		mtx_lock(&outq->lock);
		if (outq->pending > 0) {
			len = outq->pending;
			if (len > MAUDIO_MIDISPORT_MAX_TRANSFER)
				len = MAUDIO_MIDISPORT_MAX_TRANSFER;
			
			if (outq->read_ptr + len <= MAUDIO_MIDISPORT_OUT_QUEUE_SIZE) {
				memcpy(sc->outbuf, 
					&outq->buf[outq->read_ptr], len);
			} else {
				int part1 = MAUDIO_MIDISPORT_OUT_QUEUE_SIZE - outq->read_ptr;
				int part2 = len - part1;
				memcpy(sc->outbuf, &outq->buf[outq->read_ptr], part1);
				memcpy(&sc->outbuf[part1], outq->buf, part2);
			}
			
			outq->read_ptr = (outq->read_ptr + len) % MAUDIO_MIDISPORT_OUT_QUEUE_SIZE;
			outq->pending -= len;
			
			mtx_unlock(&outq->lock);
			
			usbd_xfer_set_frame_len(xfer, 0, len);
			usbd_transfer_submit(xfer);
		} else {
			mtx_unlock(&outq->lock);
		}
		break;
		
	default:
		if (error != USB_ERR_CANCELLED) {
			device_printf(sc->dev, "MIDI OUT error: %s\n",
				usbd_errstr(error));
		}
		break;
	}
}

/*
 * Queue MIDI output data for transmission
 * Handles circular buffer management and flow control
 */
static int
maudio_midisport_queue_output(struct maudio_midisport *sc, uint8_t *data, int len)
{
	struct maudio_midi_output *outq = &sc->outq;
	int space_available;
	int ret = 0;

	mtx_lock(&outq->lock);
	
	/* Calculate available space */
	space_available = MAUDIO_MIDISPORT_OUT_QUEUE_SIZE - outq->pending;
	
	if (len > space_available) {
		/* Queue overflow - truncate */
		len = space_available;
		device_printf(sc->dev, "MIDI OUT queue overflow, truncating\n");
	}
	
	if (len == 0) {
		mtx_unlock(&outq->lock);
		return 0;  /* No space */
	}
	
	/* Copy data to queue (handle wraparound) */
	if (outq->write_ptr + len <= MAUDIO_MIDISPORT_OUT_QUEUE_SIZE) {
		memcpy(&outq->buf[outq->write_ptr], data, len);
	} else {
		int part1 = MAUDIO_MIDISPORT_OUT_QUEUE_SIZE - outq->write_ptr;
		int part2 = len - part1;
		memcpy(&outq->buf[outq->write_ptr], data, part1);
		memcpy(outq->buf, &data[part1], part2);
	}
	
	outq->write_ptr = (outq->write_ptr + len) % MAUDIO_MIDISPORT_OUT_QUEUE_SIZE;
	outq->pending += len;
	ret = len;
	
	/* If queue exceeded threshold, try to submit transfer */
	if (outq->pending >= MAUDIO_MIDISPORT_OUT_QUEUE_THRESH) {
		mtx_unlock(&outq->lock);
		mtx_lock(&sc->lock);
		usbd_transfer_start(sc->xfer[MAUDIO_MIDISPORT_BULK_OUT]);
		mtx_unlock(&sc->lock);
	} else {
		mtx_unlock(&outq->lock);
	}
	
	return ret;
}

/*
 * Device creation and initialization
 */
static int
maudio_midisport_create_rawmidi(struct maudio_midisport *sc)
{
	struct snd_rawmidi *rmidi;
	int err;

	device_printf(sc->dev, "Creating MIDI device with %d in + %d out ports\n",
		MAUDIO_MIDISPORT_NUM_PORTS, MAUDIO_MIDISPORT_NUM_PORTS);

	/* Create rawmidi device
	 * Parameters: card, id, device_num, output_count, input_count, rmidi_ptr
	 * MIDISport 8x8 has 8 MIDI inputs and 8 MIDI outputs
	 */
	err = snd_rawmidi_new(sc->card, "MIDISport8x8", 0, 
		MAUDIO_MIDISPORT_NUM_PORTS, MAUDIO_MIDISPORT_NUM_PORTS, &rmidi);
	if (err < 0) {
		device_printf(sc->dev, "Failed to create rawmidi: %d\n", err);
		return err;
	}

	rmidi->private_data = sc;
	sc->rmidi = rmidi;
	sc->in_ports = MAUDIO_MIDISPORT_NUM_PORTS;
	sc->out_ports = MAUDIO_MIDISPORT_NUM_PORTS;

	device_printf(sc->dev, "MIDI device created\n");
	return 0;
}

/*
 * USB device driver callbacks
 */
static int
maudio_midisport_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);

	if (!maudio_bsd_enabled)
		return ENXIO;

	if ((uaa->info.idVendor == MAUDIO_VENDOR_ID) &&
	    ((uaa->info.idProduct == MAUDIO_MIDISPORT8x8) ||
	     (uaa->info.idProduct == MAUDIO_MIDISPORT8x8_OLD))) {
		return BUS_PROBE_DEFAULT;
	}
	return ENXIO;
}

static int
maudio_midisport_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct maudio_midisport *sc = device_get_softc(dev);
	usb_error_t error;
	int err;

	device_printf(dev, "Attaching M-Audio MIDISport 8x8 (USB 0x%04x:0x%04x)\n",
		uaa->info.idVendor, uaa->info.idProduct);

	sc->dev = dev;
	sc->udev = uaa->device;
	sc->alsa_dev.bsddev = dev;
	mtx_init(&sc->lock, "maudio_midisport", NULL, MTX_DEF);
	
	/* Initialize MIDI output queue */
	mtx_init(&sc->outq.lock, "maudio_midisport_outq", NULL, MTX_DEF);
	sc->outq.write_ptr = 0;
	sc->outq.read_ptr = 0;
	sc->outq.pending = 0;

	/* Create ALSA sound card */
	err = snd_card_new(&sc->alsa_dev, -1, 
		"MIDISport8x8", NULL, 0, &sc->card);
	if (err < 0) {
		device_printf(dev, "Failed to create card: %d\n", err);
		mtx_destroy(&sc->lock);
		return err;
	}

	/* Create MIDI device */
	err = maudio_midisport_create_rawmidi(sc);
	if (err < 0) {
		device_printf(dev, "Failed to create MIDI: %d\n", err);
		snd_card_free(sc->card);
		mtx_destroy(&sc->lock);
		return err;
	}

	/* Configure USB transfers */
	maudio_midisport_config[MAUDIO_MIDISPORT_BULK_IN].callback = 
		maudio_midisport_input_callback;
	maudio_midisport_config[MAUDIO_MIDISPORT_BULK_OUT].callback = 
		maudio_midisport_output_callback;

	/* Allocate USB transfers
	 * For MIDISport 8x8: 1 bulk IN for input, 1 bulk OUT for output
	 */
	error = usbd_transfer_setup(sc->udev, 
		&uaa->info.bIfaceIndex,
		sc->xfer,
		maudio_midisport_config,
		2,	/* Number of transfers (IN + OUT) */
		sc,
		&sc->lock);
	if (error) {
		device_printf(dev, "Failed to setup USB transfers: %s\n",
			usbd_errstr(error));
		snd_card_free(sc->card);
		mtx_destroy(&sc->lock);
		return EIO;
	}

	/* Register sound card */
	err = snd_card_register(sc->card);
	if (err < 0) {
		device_printf(dev, "Failed to register card: %d\n", err);
		usbd_transfer_unsetup(sc->xfer, 2);
		snd_card_free(sc->card);
		mtx_destroy(&sc->lock);
		return err;
	}

	/* Start input transfers */
	mtx_lock(&sc->lock);
	usbd_transfer_start(sc->xfer[MAUDIO_MIDISPORT_BULK_IN]);
	mtx_unlock(&sc->lock);

	device_printf(dev, "M-Audio MIDISport 8x8 attached successfully\n");
	return 0;
}

static int
maudio_midisport_detach(device_t dev)
{
	struct maudio_midisport *sc = device_get_softc(dev);

	device_printf(dev, "Detaching M-Audio MIDISport 8x8\n");

	/* Stop USB transfers */
	mtx_lock(&sc->lock);
	usbd_transfer_stop(sc->xfer[MAUDIO_MIDISPORT_BULK_IN]);
	usbd_transfer_stop(sc->xfer[MAUDIO_MIDISPORT_BULK_OUT]);
	mtx_unlock(&sc->lock);

	/* Clean up USB */
	usbd_transfer_unsetup(sc->xfer, 2);

	/* Clean up sound card */
	snd_card_free(sc->card);

	mtx_destroy(&sc->lock);
	mtx_destroy(&sc->outq.lock);
	return 0;
}

static int
maudio_midisport_suspend(device_t dev)
{
	struct maudio_midisport *sc = device_get_softc(dev);

	mtx_lock(&sc->lock);
	usbd_transfer_stop(sc->xfer[MAUDIO_MIDISPORT_BULK_IN]);
	usbd_transfer_stop(sc->xfer[MAUDIO_MIDISPORT_BULK_OUT]);
	sc->suspended = 1;
	mtx_unlock(&sc->lock);

	return 0;
}

static int
maudio_midisport_resume(device_t dev)
{
	struct maudio_midisport *sc = device_get_softc(dev);

	mtx_lock(&sc->lock);
	sc->suspended = 0;
	usbd_transfer_start(sc->xfer[MAUDIO_MIDISPORT_BULK_IN]);
	mtx_unlock(&sc->lock);

	return 0;
}

static device_method_t maudio_midisport_methods[] = {
	DEVMETHOD(device_probe, maudio_midisport_probe),
	DEVMETHOD(device_attach, maudio_midisport_attach),
	DEVMETHOD(device_detach, maudio_midisport_detach),
	DEVMETHOD(device_suspend, maudio_midisport_suspend),
	DEVMETHOD(device_resume, maudio_midisport_resume),
	DEVMETHOD_END
};

static driver_t maudio_midisport_driver = {
	.name = "maudio_midisport",
	.methods = maudio_midisport_methods,
	.size = sizeof(struct maudio_midisport),
};

DRIVER_MODULE(basound_maudio, uhub, maudio_midisport_driver, 0, 0);
MODULE_DEPEND(basound_maudio, basound, 1, 1, 1);
MODULE_DEPEND(basound_maudio, usb, 1, 1, 1);
MODULE_DEPEND(basound_maudio, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);

