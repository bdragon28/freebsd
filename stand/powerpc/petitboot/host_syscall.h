/*-
 * Copyright (C) 2014 Nathan Whitehorn
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _HOST_SYSCALL_H
#define _HOST_SYSCALL_H

#include <stand.h>

/* Always assume the host uses 64k pages. */
#define HOST_PAGE_SIZE 0x10000

ssize_t host_read(int fd, void *buf, size_t nbyte);
ssize_t host_write(int fd, const void *buf, size_t nbyte);
ssize_t host_seek(int fd, int64_t offset, int whence);
int host_open(const char *path, int flags, int mode);
ssize_t host_llseek(int fd, int32_t offset_high, int32_t offset_lo, uint64_t *result, int whence);
int host_close(int fd);
int host_ioctl(int fd, unsigned int command, void *arg);
int host_exit(int errno);
void *host_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t);
#define host_getmem(size) host_mmap(0, size, 3 /* RW */, 0x22 /* ANON */, -1, 0);
struct old_utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
};
int host_uname(struct old_utsname *);
struct host_timeval {
	long tv_sec;
	long tv_usec;
};
#define HOST_NCCS 19
struct host_termios {
	unsigned int c_iflag;
	unsigned int c_oflag;
	unsigned int c_cflag;
	unsigned int c_lflag;
#define HOST_ICANON (1 << 8)
#define HOST_ECHO (1 << 3)
	unsigned char c_cc[HOST_NCCS];
	char c_line;
	int c_ispeed;
	int c_ospeed;
};
#define HOST_TCGETS (0x40000000 | ((sizeof(struct host_termios)) << 16) | ('t' << 8) | (19))
#define HOST_TCSETS (0x80000000 | ((sizeof(struct host_termios)) << 16) | ('t' << 8) | (20))
int host_gettimeofday(struct host_timeval *a, void *b);
int host_select(int nfds, long *readfds, long *writefds, long *exceptfds,
    struct host_timeval *timeout);
int kexec_load(uint64_t start, uint64_t nsegs, uint64_t segs, uint64_t flags);
#define HOST_KEXEC_ARCH_PPC64        (21 << 16)
int host_execve(const char *filename, char *const argv[], char *const envp[]);
int host_reboot(int, int, int, uint32_t);
int host_getdents(int fd, void *dirp, int count);

#endif
