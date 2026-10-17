PORTNAME=	basound-tools
DISTVERSION=	0.1
CATEGORIES=	audio

MAINTAINER=	miroslav.shaltev@shaltev.de
COMMENT=	FLTK mixer and control utilities for BASound kernel drivers
WWW=		https://codeberg.org/govedo/basound

LICENSE=	GPLv3+
LICENSE_FILE=	${WRKSRC}/LICENSE

LIB_DEPENDS=	libfltk.so:x11-toolkits/fltk \
		libfontconfig.so:x11-fonts/fontconfig

USES=		compiler:c++14-lang gmake xorg
USE_XORG=	x11 xcursor xext xfixes xft xinerama xrender

SUB_FILES=	pkg-message

USE_GITHUB=	yes
GH_ACCOUNT=	cvscpp
GH_PROJECT=	basound
# Must match the snapshot used by audio/basound.
GH_TAGNAME=	f5d5389d77c15aca54e95b344c0c4031c4a12afe

BASOUND_TOOLS=	bsm_hdsp bsm_digi00x bsm_line6 bsm_pf2626

PLIST_FILES=	${BASOUND_TOOLS:S,^,bin/,}

do-build:
.for tool in ${BASOUND_TOOLS}
	@(cd ${WRKSRC}/tools/${tool} && ${SETENV} ${MAKE_ENV} \
		${GMAKE} CXX="${CXX}" all)
.endfor

do-install:
.for tool in ${BASOUND_TOOLS}
	${INSTALL_PROGRAM} ${WRKSRC}/tools/${tool}/${tool} ${STAGEDIR}${PREFIX}/bin
.endfor

.include <bsd.port.mk>
