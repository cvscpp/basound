KMOD=	basound
SRCS=	sys/dev/basound/basound.c \
	sys/dev/basound/basound_debug.c \
	sys/alsa/alsa_card.c \
	sys/alsa/alsa_pcm.c \
	sys/alsa/alsa_pcm_bsd.c \
	sys/alsa/alsa_mem.c \
	sys/alsa/alsa_midi.c \
	sys/alsa/alsa_control.c \
	sys/alsa/alsa_mixer_bsd.c \
	sys/alsa/alsa_info.c \
	sys/alsa/alsa_pci.c \
	sys/alsa/alsa_firmware.c \
	sys/alsa/alsa_work.c \
	sys/alsa/alsa_hwdep.c \
	sys/dev/basound/hdsp/hdsp_bsd.c \
	sys/dev/basound/hdsp/hdsp_main.c \
	sys/dev/basound/hdsp/hdsp_mixer.c \
	sys/dev/basound/hdsp/hdsp_midi.c \
	sys/dev/basound/hdsp/hdsp_cdev.c \
	sys/dev/basound/dice/dice_bsd.c \
	sys/dev/basound/line6/line6_bsd.c \
	sys/dev/basound/maudio/maudio_midisport.c \
	device_if.h bus_if.h feeder_if.h mixer_if.h channel_if.h pci_if.h vnode_if.h

CFLAGS+= -I${.CURDIR}/sys/alsa/include \
	-I${.CURDIR}/sys/alsa \
	-I${.CURDIR}/sys/dev/basound \
	-I${.CURDIR}/sys/dev/basound/hdsp \
	-I${.CURDIR}/sys/dev/usb

.include <bsd.kmod.mk>
