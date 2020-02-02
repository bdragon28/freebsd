# $FreeBSD$

.include <bsd.init.mk>

.PATH:	${FICLSRC}
.include "${FICLSRC}/Makefile"

CFLAGS+=	-target powerpc64le-unknown-linux
CFLAGS+=	-DFICL_INT=int64_t -DFICL_UNS=uint64_t -DBITS_PER_CELL=64 -DFICL_ALIGN=3
