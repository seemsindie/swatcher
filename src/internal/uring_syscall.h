#ifndef SWATCHER_URING_SYSCALL_H
#define SWATCHER_URING_SYSCALL_H

#if defined(__linux__)

#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

static inline int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                                  unsigned flags, void *sig)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, 0);
}

static inline int io_uring_register(int fd, unsigned opcode, void *arg, unsigned nr_args)
{
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

#endif /* __linux__ */
#endif /* SWATCHER_URING_SYSCALL_H */
