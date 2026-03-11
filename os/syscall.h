#ifndef SYSCALL_H
#define SYSCALL_H

extern void *do_write(void *arg);
extern void *do_read(void *arg);
extern void *do_ioctl(void *arg);
extern void *do_fstat(void *arg);
extern void *do_getpagesize(void *arg);
extern void *do_sbrk(void *arg);
extern void *do_execve(void *arg);
extern void *do_getpid(void *arg);
extern void *do_fork(void *arg);
extern void *do_exit(void *arg);
extern void *do_getdtablesize(void *arg);
extern void *do_close(void *arg);
extern void *do_wait(void *arg);
extern void *do_getppid(void *arg);
extern void *do_pipe(void *arg);
extern void *do_dup(void *arg);
extern void *do_dup2(void *arg);

#endif

