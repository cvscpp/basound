# basound - ALSA glue layer for FreeBSD
#
# This top-level Makefile builds the CORE module (basound.ko): the ALSA
# shim + FreeBSD sound integration only.  It contains no device drivers.
#
# Each supported device class is a separate loadable module, built from
# its own directory:
#
#   sys/dev/basound/hdsp/Makefile    -> basound_hdsp.ko    (RME PCI HDSP)
#   sys/dev/basound/dice/Makefile    -> basound_dice.ko    (DICE FireWire)
#   sys/dev/basound/digi00x/Makefile -> basound_digi00x.ko (Digi 002/003 FW)
#   sys/dev/basound/line6/Makefile   -> basound_line6.ko   (Line6 USB)
#   sys/dev/basound/maudio/Makefile  -> basound_maudio.ko  (M-Audio MIDI USB)
#
# Load only the driver module(s) matching your hardware; the core is
# pulled in automatically via MODULE_DEPEND.  Each driver also honours
# a hw.basound_<class>.enable tunable (loader.conf) for boot-time gating.
#
#   make                 build the core module only
#   make all-modules     build the core + all driver modules
#   make clean           clean the core module
#   make clean-modules   clean all driver modules

KMOD=	basound
SRCS=	sys/dev/basound/basound.c \
	sys/dev/basound/basound_debug.c \
	sys/alsa/alsa_card.c \
	sys/alsa/alsa_control.c \
	sys/alsa/alsa_firmware.c \
	sys/alsa/alsa_hwdep.c \
	sys/alsa/alsa_info.c \
	sys/alsa/alsa_mem.c \
	sys/alsa/alsa_midi.c \
	sys/alsa/alsa_mixer_bsd.c \
	sys/alsa/alsa_pci.c \
	sys/alsa/alsa_pcm.c \
	sys/alsa/alsa_pcm_bsd.c \
	sys/alsa/alsa_work.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h vnode_if.h

CFLAGS+= -I${.CURDIR}/sys/alsa/include \
	-I${.CURDIR}/sys/alsa \
	-I${.CURDIR}/sys/dev/basound \
	-I${.CURDIR}/sys/dev/basound/hdsp

# Device-class modules (built in their own directories).
BASOUND_DRIVERS=	hdsp dice digi00x line6 maudio

all-modules: all
	@for d in ${BASOUND_DRIVERS}; do \
		echo "==> Building basound_$$d"; \
		${MAKE} -C ${.CURDIR}/sys/dev/basound/$$d; \
	done

clean-modules:
	@for d in ${BASOUND_DRIVERS}; do \
		${MAKE} -C ${.CURDIR}/sys/dev/basound/$$d clean; \
	done

.include <bsd.kmod.mk>
