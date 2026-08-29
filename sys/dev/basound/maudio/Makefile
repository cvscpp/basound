# basound_maudio - M-Audio MIDISport 8x8 USB MIDI driver
#
# Depends on the basound core module (ALSA shim) — loaded automatically
# via MODULE_DEPEND(basound_maudio, basound).  Build from the project
# root with `make all-modules` or directly:
#
#   make -C sys/dev/basound/maudio
#
# Boot-time disable (loader.conf):
#   hw.basound_maudio.enable="0"

KMOD=	basound_maudio
SRCS=	maudio_midisport.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h

BASOUND_ROOT?=	${.CURDIR}/../../../..

CFLAGS+= -I${BASOUND_ROOT}/sys/alsa/include \
	-I${BASOUND_ROOT}/sys/alsa \
	-I${BASOUND_ROOT}/sys/dev/usb

.include <bsd.kmod.mk>
