# basound_dice - DICE FireWire audio interface family driver
#
# Depends on the basound core module (ALSA shim) — loaded automatically
# via MODULE_DEPEND(basound_dice, basound).  Build from the project root
# with `make all-modules` or directly:
#
#   make -C sys/dev/basound/dice
#
# Boot-time disable (loader.conf):
#   hw.basound_dice.enable="0"

KMOD=	basound_dice
SRCS=	dice_bsd.c dice_alesis_bsd.c dice_maudio_bsd.c dice_streaming.c \
	dice_cdev.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h

BASOUND_ROOT?=	${.CURDIR}/../../../..

CFLAGS+= -I${BASOUND_ROOT}/sys/alsa/include \
	-I${BASOUND_ROOT}/sys/alsa \
	-I${BASOUND_ROOT}/sys/dev/basound

.include <bsd.kmod.mk>
