// SPDX-License-Identifier: GPL-3.0-or-later
#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/sound/pcm/sound.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pci.h>

#include "hdsp.h"

/* RME HDSP PCI IDs */
#define PCI_VENDOR_ID_RME		0x10ee
#define PCI_DEVICE_ID_RME_DIGIFACE	0x3fc5
#define PCI_DEVICE_ID_RME_MULTIFACE	0x3fc6
#define PCI_DEVICE_ID_RME_H9652		0x3fc4

/* Boot-time switch for the whole driver.  Loading the basound_hdsp.ko
 * module already limits probing to RME HDSP PCI devices; this tunable
 * additionally lets loader.conf disable the driver without unloading
 * the module:
 *     hw.basound_hdsp.enable="0" */
static int hdsp_bsd_enabled = 1;
TUNABLE_INT("hw.basound_hdsp.enable", &hdsp_bsd_enabled);

struct hdsp_bsd_softc {
	struct hdsp chip;
	struct resource *irq_res;
	void *irq_cookie;
};

static int
hdsp_bsd_probe(device_t dev)
{
	uint16_t vendor = pci_get_vendor(dev);
	uint16_t device = pci_get_device(dev);

	if (!hdsp_bsd_enabled)
		return (ENXIO);

	if (vendor != PCI_VENDOR_ID_RME)
		return (ENXIO);

	switch (device) {
	case PCI_DEVICE_ID_RME_DIGIFACE:
		device_set_desc(dev, "RME Hammerfall DSP Digiface");
		return (BUS_PROBE_DEFAULT);
	case PCI_DEVICE_ID_RME_MULTIFACE:
		device_set_desc(dev, "RME Hammerfall DSP Multiface");
		return (BUS_PROBE_DEFAULT);
	case PCI_DEVICE_ID_RME_H9652:
		device_set_desc(dev, "RME Hammerfall DSP 9652");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
hdsp_bsd_attach(device_t dev)
{
	struct hdsp_bsd_softc *sc = device_get_softc(dev);
	struct snd_card *card;
	struct device *alsa_dev;
	int err, rid;

	sc->chip.pci = malloc(sizeof(struct pci_dev), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->chip.pci->bsddev = dev;
	sc->chip.pci->vendor = pci_get_vendor(dev);
	sc->chip.pci->device = pci_get_device(dev);

	/* Enable PCI bus mastering so the HDSP DMA engine can
	 * read/write system memory.  Without this the hardware
	 * generates a PCI master abort → NMI → machine reboot. */
	pci_enable_busmaster(dev);

	/* Read PCI revision byte — selects which FPGA bitstream to upload */
	sc->chip.firmware_rev = pci_get_revid(dev);

	/* Map BAR 0 - The HDSP has one BAR for all registers */
	rid = PCIR_BAR(0);
	sc->chip.pci->res_rid[0] = rid;
	sc->chip.pci->res[0] = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->chip.pci->res_rid[0], RF_ACTIVE);
	if (sc->chip.pci->res[0] == NULL) {
		device_printf(dev, "Failed to allocate BAR 0\n");
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}

	/* Allocate interrupt */
	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE | RF_SHAREABLE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "Failed to allocate IRQ\n");
		bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}
	
	err = bus_setup_intr(dev, sc->irq_res, INTR_TYPE_AV | INTR_MPSAFE, NULL, 
			     snd_hdsp_interrupt, &sc->chip, &sc->irq_cookie);
	if (err) {
		device_printf(dev, "Failed to setup interrupt\n");
		bus_release_resource(dev, SYS_RES_IRQ, rid, sc->irq_res);
		bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}

	/* In our shim, struct device is just a wrapper for device_t, 
	 * and we've mapped pci_dev to it via casting or member access.
	 * Here we'll treat pci_dev as the 'struct device' for ALSA calls.
	 */
	alsa_dev = (struct device *)sc->chip.pci;

	/* 1. Create ALSA card */
	err = snd_card_new(alsa_dev, 0, "HDSP", NULL, 0, &card);
	if (err) {
		device_printf(dev, "Failed to create ALSA card\n");
		bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}

	/* 2. Create HDSP chip instance */
	err = snd_hdsp_create(card, &sc->chip);
	if (err) {
		device_printf(dev, "Failed to create HDSP chip\n");
		snd_card_free(card);
		bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}

	/* 3. Upload firmware */
	err = snd_hdsp_upload_firmware(&sc->chip);
	if (err) {
		device_printf(dev, "Failed to upload firmware\n");
		snd_card_free(card);
		if (sc->irq_cookie)
			bus_teardown_intr(dev, sc->irq_res, sc->irq_cookie);
		if (sc->irq_res)
			bus_release_resource(dev, SYS_RES_IRQ, rid, sc->irq_res);
		bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
		return (ENXIO);
	}

	/* 4. Register the card */
	snd_card_register(card);

	/* 5. Expose /dev/hdspN for the native mixer tool */
	if (hdsp_cdev_create(&sc->chip, device_get_unit(dev)) != 0)
		device_printf(dev, "Warning: failed to create /dev/hdsp%d\n",
		    device_get_unit(dev));

	device_printf(dev, "HDSP attached (%s)\n", sc->chip.card_name);

	return (0);
}

static int
hdsp_bsd_detach(device_t dev)
{
	struct hdsp_bsd_softc *sc = device_get_softc(dev);

	/* Make sure the 1 ms feeder callout is fully quiesced before
	 * tearing down the card (it must not touch a freed softc). */
	callout_drain(&sc->chip.pcm_callout);

	hdsp_cdev_destroy(&sc->chip);

	/* Free planar DMA buffers before tearing down the card and PCI resources. */
	hdsp_free_dma_buffers(&sc->chip);

	/* Tear down MIDI ports (mpu401_uninit → midi_uninit). */
	snd_hdsp_free_midi(&sc->chip);

	if (sc->chip.card) {
		snd_card_free(sc->chip.card);
	}
	
	if (sc->irq_cookie)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_cookie);
	if (sc->irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
		
	if (sc->chip.pci) {
		if (sc->chip.pci->res[0])
			bus_release_resource(dev, SYS_RES_MEMORY, sc->chip.pci->res_rid[0], sc->chip.pci->res[0]);
		free(sc->chip.pci, M_DEVBUF);
	}
	return (0);
}

static device_method_t hdsp_bsd_methods[] = {
	DEVMETHOD(device_probe,		hdsp_bsd_probe),
	DEVMETHOD(device_attach,	hdsp_bsd_attach),
	DEVMETHOD(device_detach,	hdsp_bsd_detach),
	/* Bus methods needed to host the "pcm" child device */
	DEVMETHOD(bus_add_child,	bus_generic_add_child),
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD_END
};

static driver_t hdsp_bsd_driver = {
	"basound_hdsp",
	hdsp_bsd_methods,
	sizeof(struct hdsp_bsd_softc),
};

DRIVER_MODULE(basound_hdsp, pci, hdsp_bsd_driver, 0, 0);
MODULE_DEPEND(basound_hdsp, basound, 1, 1, 1);
MODULE_DEPEND(basound_hdsp, pci, 1, 1, 1);
MODULE_DEPEND(basound_hdsp, sound, SOUND_MINVER, SOUND_PREFVER, SOUND_MAXVER);
