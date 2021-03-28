/*-
 * Copyright (c) 2000-2001 Benno Rice
 * Copyright (c) 2007 Semihalf, Rafal Jaworowski <raj@semihalf.com>
 * All rights reserved.
 * Copyright (c) 2021 Brandon Bergren <bdragon@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <stand.h>
#include <net.h>
#include <netif.h>

#include "dev_net.h"
#include "host_syscall.h"

#define PKTALIGN        32

static int	kbootnet_probe(struct netif *, void *);
static int	kbootnet_match(struct netif *, void *);
static void	kbootnet_init(struct iodesc *, void *);
static ssize_t	kbootnet_get(struct iodesc *, void **, time_t);
static ssize_t	kbootnet_put(struct iodesc *, void *, size_t);
static void	kbootnet_end(struct netif *);

extern struct netif_stats kbootnet_stats[];

struct netif_dif kbootnet_ifs[] = {
	/*	dif_unit	dif_nsel	dif_stats	dif_private */
	{	0,		1,		&kbootnet_stats[0],	0,	},
};

struct netif_stats kbootnet_stats[nitems(kbootnet_ifs)];

struct netif_driver kbootnet = {
	.netif_bname = "net",
	.netif_match = kbootnet_match,
	.netif_probe = kbootnet_probe,
	.netif_init = kbootnet_init,
	.netif_get = kbootnet_get,
	.netif_put = kbootnet_put,
	.netif_end = kbootnet_end,
	.netif_ifs = kbootnet_ifs,
	.netif_nifs = nitems(kbootnet_ifs)
};

struct kbootnet_softc {
	uint32_t	sc_pad;
	uint8_t		sc_rxbuf[ETHER_MAX_LEN];
	uint8_t		sc_txbuf[ETHER_MAX_LEN + PKTALIGN];
	uint8_t		*sc_txbufp;
	int		sc_socket;
};

static struct kbootnet_softc kbootnet_softc;

struct kbootnet_lldata {
	unsigned short protocol;
	int ifindex;
	unsigned short hatype;
	unsigned char pkttype;
	unsigned char halen;
	unsigned char addr[8];
} __packed;

struct kbootnet_sockaddr {
	unsigned short family;
	union {
		char data[14];
		struct kbootnet_lldata lldata;
		int index; 
	} addr;
} __packed;

struct kbootnet_ifr {
	char devname[16];
	struct kbootnet_sockaddr sa;
} __packed;
	
static void
get_env_net_params()
{
	/* XXX Come up with a way of participating with petitboot here. */ 
}

static int
kbootnet_match(struct netif *nif, void *machdep_hint)
{

	/* We always assume eth0. */
	return (1);

}

static int
kbootnet_probe(struct netif *nif, void *machdep_hint)
{
	int fd;

	/*
	 * XXX SOCK_NONBLOCK is new in Linux 2.6.27.
	 * Are all petitboots new enough to have this?
	 */
	fd = host_socket(17, /* AF_PACKET */
	    3 | 04000, /* SOCK_RAW | SOCK_NONBLOCK  */ 
	    3 /* ETH_P_ALL */ /*255*/ /* IPPROTO_RAW */);

	if (fd == -1) {
		printf("kbootnet_probe: Unable to establish socket!\n");
		return (-1);
	}


#if defined(NETIF_DEBUG)
	printf("kbootnet_probe: network device found: %d\n", fd);
#endif
	kbootnet_softc.sc_socket = fd;

	return (0);
}

static ssize_t
kbootnet_put(struct iodesc *desc, void *pkt, size_t len)
{
	struct netif *nif = desc->io_netif;
	struct kbootnet_softc *sc = nif->nif_devdata;
	size_t sendlen;
	ssize_t rv;

#if defined(NETIF_DEBUG)
	struct ether_header *eh;

	printf("net_put: desc %p, pkt %p, len %d\n", desc, pkt, len);
	eh = pkt;
	printf("dst: %s ", ether_sprintf(eh->ether_dhost));
	printf("src: %s ", ether_sprintf(eh->ether_shost));
	printf("type: 0x%x\n", eh->ether_type & 0xffff);
#endif

	if (len < ETHER_MIN_LEN - ETHER_CRC_LEN) {
		sendlen = ETHER_MIN_LEN - ETHER_CRC_LEN;
		bzero(sc->sc_txbufp, sendlen);
	} else
		sendlen = len;

	memcpy(sc->sc_txbufp, pkt, len);

	rv = host_write(sc->sc_socket, sc->sc_txbufp, sendlen);

#if defined(NETIF_DEBUG)
	printf("kbootnet_put: host_write returned %d\n", rv);
#endif
	return (rv);
}

static ssize_t
kbootnet_get(struct iodesc *desc, void **pkt, time_t timeout)
{
	struct netif *nif = desc->io_netif;
	struct kbootnet_softc *sc = nif->nif_devdata;
	time_t t;
	int rlen;
	size_t len;
	char *buf;

#if defined(NETIF_DEBUG)
	printf("net_get: pkt %p, timeout %d\n", pkt, timeout);
#endif
	t = getsecs();
	len = sizeof(sc->sc_rxbuf);
	do {
		rlen = host_read(sc->sc_socket, sc->sc_rxbuf, len);
	} while ((rlen == -1 || rlen == 0) && (getsecs() - t < timeout));

#if defined(NETIF_DEBUG)
	printf("kbootnet_get: received len %d (%x)\n", rlen, rlen);
#endif

	if (rlen > 0) {
		buf = malloc(rlen + ETHER_ALIGN);
		if (buf == NULL)
			return (-1);
		memcpy(buf + ETHER_ALIGN, sc->sc_rxbuf, rlen);
		*pkt = buf;
		return ((ssize_t)rlen);
	}

	return (-1);
}

static void
kbootnet_init(struct iodesc *desc, void *machdep_hint)
{
	struct netif *nif = desc->io_netif;
	struct kbootnet_softc *sc;
	struct kbootnet_ifr req;
	struct kbootnet_sockaddr addr;

	int fd;

	sc = nif->nif_devdata = &kbootnet_softc;
	fd = sc->sc_socket;

	/* Bind to eth0. */
	bzero(&req, sizeof(req));
	strcpy(req.devname, "eth0");

	if (host_ioctl(fd, 0x8933 /* SIOCGIFINDEX */, &req) < 0) {
		panic("kbootnet: Unable to find eth0!\n");
	}

	bzero(&addr, sizeof(addr));
	addr.family = 17; /* PF_PACKET */
	addr.addr.lldata.ifindex = req.sa.addr.index;

	if (host_bind(fd, &addr, sizeof(addr)) < 0) {
		panic("kbootnet: Unable to bind!\n");
	}

	if (host_ioctl(fd, 0x8927 /* SIOCGHWADDR */, &req) < 0) {
		panic("kbootnet: Unable to determine MAC!\n");
	}

	printf("family %d mac %s\n", req.sa.family, req.sa.addr.data);

	memcpy(desc->myea, (char*)req.sa.addr.data, 6);
	if (memcmp (desc->myea, "\0\0\0\0\0\0", 6) == 0) {
		panic("%s%d: empty ethernet address!",
		    nif->nif_driver->netif_bname, nif->nif_unit);
	}

	/* Attempt to get netboot params */
	get_env_net_params();
	if (myip.s_addr != 0)
		desc->myip = myip;

#if defined(NETIF_DEBUG)
	printf("network: %s%d attached to %s\n", nif->nif_driver->netif_bname,
	    nif->nif_unit, ether_sprintf(desc->myea));
#endif

	/* Set correct alignment for TX packets */
	sc->sc_txbufp = sc->sc_txbuf;
	if ((unsigned long)sc->sc_txbufp % PKTALIGN)
		sc->sc_txbufp += PKTALIGN -
		    (unsigned long)sc->sc_txbufp % PKTALIGN;
}

static void
kbootnet_end(struct netif *nif)
{
	struct kbootnet_softc *sc = nif->nif_devdata;
	int err;

	if ((err = host_close(sc->sc_socket)) != 0)
		panic("%s%d: net_end failed with error %d",
		    nif->nif_driver->netif_bname, nif->nif_unit, err);
}
