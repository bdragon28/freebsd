# $FreeBSD$

DO32=0

SUBDIR.yes=		fdt
SUBDIR.yes=		ficl
SUBDIR.yes=		libsa

LOADER_CD9660_SUPPORT?=	yes
LOADER_MSDOS_SUPPORT?=	yes
LOADER_EXT2FS_SUPPORT?=	yes
LOADER_UFS_SUPPORT?=	yes
LOADER_NET_SUPPORT?=	yes
LOADER_NFS_SUPPORT?=	yes
LOADER_TFTP_SUPPORT?=	no
LOADER_GZIP_SUPPORT?=	yes
LOADER_BZIP2_SUPPORT?=	no

.include <bsd.init.mk>

PROG=		loader.petitboot
NEWVERSWHAT=	"petitboot loader" ${MACHINE_ARCH}
INSTALLFLAGS=	-b

# Architecture-specific loader code
SRCS=		conf.c vers.c main.c ppc64_elf_freebsd.c
SRCS+=		host_init.S host_syscall.S hostcons.c hostdisk.c kerneltramp.S kbootfdt.c
SRCS+=		ucmpdi2.c

.include	"${BOOTSRC}/fdt.mk"

# Always add MI sources
.include	"${BOOTSRC}/loader.mk"
.PATH:		${SYSDIR}/libkern

LIBFICL=	${BOOTOBJ}/powerpc/petitboot/ficl/libficl.a
LIBFDT=		${BOOTOBJ}/powerpc/petitboot/fdt/libfdt.a
LIBSA=		${BOOTOBJ}/powerpc/petitboot/libsa/libsa.a

CFLAGS+=	-Wall -DAIM -target powerpc64le-unknown-linux
CFLAGS+=        -DFICL_INT=int64_t -DFICL_UNS=uint64_t -DBITS_PER_CELL=64 -DFICL_ALIGN=3

# load address. set in linker script
RELOC?=		0x0
CFLAGS+=	-DRELOC=${RELOC}

LDFLAGS=	-nostdlib -static -T ${.CURDIR}/ldscript.powerpc
#LDFLAGS=	-nostdlib -static

DPADD=		${LIBFICL} ${LIBOFW} ${LIBFDT} ${LIBSA}
LDADD=		${LIBFICL} ${LIBOFW} ${LIBFDT} ${LIBSA}

.include <bsd.prog.mk>

.include <bsd.subdir.mk>
