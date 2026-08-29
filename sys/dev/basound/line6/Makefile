# basound_line6 - Line6 POD / POD Studio / TonePort USB audio driver
#
# Depends on the basound core module (ALSA shim) — loaded automatically
# via MODULE_DEPEND(basound_line6, basound).  Build from the project
# root with `make all-modules` or directly:
#
#   make -C sys/dev/basound/line6
#
# Boot-time disable (loader.conf):
#   hw.basound_line6.enable="0"

KMOD=	basound_line6
SRCS=	line6_bsd.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h

BASOUND_ROOT?=	${.CURDIR}/../../../..

CFLAGS+= -I${BASOUND_ROOT}/sys/alsa/include \
	-I${BASOUND_ROOT}/sys/alsa \
	-I${BASOUND_ROOT}/sys/dev/basound \
	-I${BASOUND_ROOT}/sys/dev/usb

.include <bsd.kmod.mk>
