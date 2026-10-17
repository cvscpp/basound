PORTNAME=	basound
DISTVERSION=	0.1
CATEGORIES=	audio

MAINTAINER=	miroslav.shaltev@shaltev.de
COMMENT=	ALSA-based FreeBSD kernel drivers for professional audio devices
WWW=		https://codeberg.org/govedo/basound

LICENSE=	GPLv3+
LICENSE_FILE=	${WRKSRC}/LICENSE

USES=		kmod

USE_GITHUB=	yes
GH_ACCOUNT=	cvscpp
GH_PROJECT=	basound
# Snapshot of the upstream master branch.  Update both this hash and
# DISTVERSION when a new release is tagged.
GH_TAGNAME=	f5d5389d77c15aca54e95b344c0c4031c4a12afe

SUB_FILES=	pkg-message
SUB_LIST=	KMODDIR=${KMODDIR} \
		OPSYS=${OPSYS} \
		OSREL=${OSREL} \
		SRC_BASE=${SRC_BASE}

# The pinned snapshot predates adding pci_if.h to the top-level SRCS;
# generate it explicitly.  This is harmless once the Makefile fix is in
# the distfile (the target is then already up to date).
do-build:
	@(cd ${WRKSRC} && ${SETENV} ${MAKE_ENV} ${MAKE_CMD} pci_if.h)
	@(cd ${WRKSRC} && ${SETENV} ${MAKE_ENV} ${MAKE_CMD} all-modules)

do-install:
	${MKDIR} ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/basound.ko ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/sys/dev/basound/dice/basound_dice.ko ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/sys/dev/basound/digi00x/basound_digi00x.ko ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/sys/dev/basound/hdsp/basound_hdsp.ko ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/sys/dev/basound/line6/basound_line6.ko ${STAGEDIR}${KMODDIR}
	${INSTALL_KLD} ${WRKSRC}/sys/dev/basound/maudio/basound_maudio.ko ${STAGEDIR}${KMODDIR}

.include <bsd.port.mk>
