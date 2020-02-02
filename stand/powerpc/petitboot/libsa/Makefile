# $FreeBSD$

.include <bsd.init.mk>

.PATH:	${SASRC}
.include "${SASRC}/Makefile"

# Stupid targeting trick.
# Target linux, but build in FreeBSD standalone mode.
# This prevents compiler-rt from pulling in float.h.
CFLAGS+=	-target powerpc64le-unknown-linux -D_STANDALONE -D__FreeBSD__
