# $FreeBSD$

DO32=1

.include <bsd.init.mk>

LIB=sa32
LIBSA_CPUARCH=${MACHINE_CPUARCH:C/amd64/i386/:C/powerpc64/powerpc/}


.PATH:	${SASRC}
.include "${SASRC}/Makefile"
