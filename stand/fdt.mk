# $FreeBSD$

.if ${MK_FDT} == "yes"
CFLAGS+=	-I${FDTSRC}
CFLAGS+=	-I${BOOTOBJ}/fdt
CFLAGS+=	-I${SYSDIR}/contrib/libfdt
CFLAGS+=	-DLOADER_FDT_SUPPORT
LIBFDT=		${BOOTOBJ}/fdt/libfdt.a
.if ${MACHINE} == "i386"
LIBFDT32=	${LIBFDT}
.else
LIBFDT32=	${BOOTOBJ}/fdt32/libfdt.a
.endif
.endif
