#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The epoll and pidfd portions of the Cix repository's
 * include/linux_compat.h, carried across because both hazards apply
 * verbatim here. Kept to just what this server uses.
 *
 * TCC ignores __attribute__((packed)) entirely -- confirmed even on a
 * struct written locally with the attribute, not only on system
 * headers. The real kernel ABI for struct epoll_event is 12 bytes
 * (uint32_t events at offset 0, an 8-byte data union at offset 4, no
 * padding), which is exactly why <sys/epoll.h> marks it packed. Under
 * TCC's default alignment the system struct compiles to 16 bytes with
 * data at offset 8, so every epoll_ctl()/epoll_wait() call silently
 * corrupts the data field -- intermittently, depending on what garbage
 * lands at the misread offset. There is no compile error; the server
 * simply dispatches events against a wild pointer.
 *
 * TCC does honor #pragma pack, so this is a byte-exact replacement
 * used everywhere in place of the system struct. The glibc wrappers
 * only forward the pointer to the kernel -- they never interpret the
 * fields -- so casting a correctly-laid-out pointer at the call site
 * is safe.
 */
#pragma pack(push, 1)
union cix_epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
};
struct cix_epoll_event {
	uint32_t events;
	union cix_epoll_data data;
};
#pragma pack(pop)

static inline int cix_epoll_ctl(int epfd, int op, int fd, struct cix_epoll_event *ev)
{
	return epoll_ctl(epfd, op, fd, (struct epoll_event *)ev);
}

static inline int cix_epoll_wait(int epfd, struct cix_epoll_event *events, int maxevents,
                                 int timeout)
{
	return epoll_wait(epfd, (struct epoll_event *)events, maxevents, timeout);
}

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

/*
 * Called through syscall() rather than the glibc wrapper: the wrapper
 * only appeared in glibc 2.36, and this server has no reason to
 * require a libc newer than the hosts it serves.
 */
static inline int cix_pidfd_open(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

#endif /* LINUX_COMPAT_H */
