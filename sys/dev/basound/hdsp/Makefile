# basound_hdsp - RME Hammerfall DSP (Digiface / Multiface / 9652) PCI driver
#
# Depends on the basound core module (ALSA shim) — loaded automatically
# via MODULE_DEPEND(basound_hdsp, basound).  Build from the project root
# with `make all-modules` or directly:
#
#   make -C sys/dev/basound/hdsp
#
# Boot-time disable (loader.conf):
#   hw.basound_hdsp.enable="0"

KMOD=	basound_hdsp
SRCS=	hdsp_bsd.c hdsp_main.c hdsp_mixer.c hdsp_midi.c hdsp_cdev.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h pci_if.h \
	mpu_if.h mpufoi_if.h

BASOUND_ROOT?=	${.CURDIR}/../../../..

CFLAGS+= -I${BASOUND_ROOT}/sys/alsa/include \
	-I${BASOUND_ROOT}/sys/alsa \
	-I${BASOUND_ROOT}/sys/dev/basound

# FreeBSD MIDI interface headers (mpu_if.m / mpufoi_if.m)
.PATH: ${SYSDIR}/dev/sound/midi

.include <bsd.kmod.mk>
