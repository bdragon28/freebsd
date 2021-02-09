# $FreeBSD$

NO_OBJ=t

.include <bsd.init.mk>

SUBDIR.yes=		boot1.chrp ofw

.if "${MACHINE_ARCH}" == "powerpc64"
SUBDIR.${MK_FDT}+=	uboot kboot
.endif

.include <bsd.subdir.mk>
