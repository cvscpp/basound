# basound_digi00x - Digidesign Digi 002/003 FireWire driver
#
# Depends on the basound core module (ALSA shim) — loaded automatically
# via MODULE_DEPEND(basound_digi00x, basound).  Build from the project
# root with `make all-modules` or directly:
#
#   make -C sys/dev/basound/digi00x
#
# Boot-time disable (loader.conf):
#   hw.basound_digi00x.enable="0"

KMOD=	basound_digi00x
SRCS=	digi00x_bsd.c digi00x-stream.c digi00x-streaming.c digi00x-pcm.c \
	digi00x-midi.c digi00x-transaction.c digi00x-hwdep.c digi00x-proc.c \
	amdtp-dot.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h \
	mpu_if.h mpufoi_if.h

BASOUND_ROOT?=	${.CURDIR}/../../../..

CFLAGS+= -I${BASOUND_ROOT}/sys/alsa/include \
	-I${BASOUND_ROOT}/sys/alsa \
	-I${BASOUND_ROOT}/sys/dev/basound \
	-I${BASOUND_ROOT}/sys/dev/basound/digi00x

# FreeBSD MIDI interface headers (mpu_if.m / mpufoi_if.m)
.PATH: ${SYSDIR}/dev/sound/midi

.include <bsd.kmod.mk>
