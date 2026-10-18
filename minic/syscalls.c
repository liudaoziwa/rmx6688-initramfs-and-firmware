// SPDX-License-Identifier: GPL-2.0
/*
 * minic: the file/process syscalls the init uses.
 *
 * Every wrapper funnels into __syscall6() (crt0.S) and __syscall_ret() so
 * errno is set on error, mirroring the libc behaviour the init expects.
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "minic.h"

#define SIGCHLD 17

long __syscall_ret(long ret)
{
	if ((unsigned long)ret >= (unsigned long)-4095) {
		errno = (int)-ret;
		return -1;
	}
	return ret;
}

int close(int fd)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_close, fd, 0, 0, 0, 0, 0));
}

ssize_t read(int fd, void *buf, size_t n)
{
	return (ssize_t)__syscall_ret(__syscall6(MINIC_NR_read, fd,
						 (long)buf, (long)n, 0, 0, 0));
}

ssize_t write(int fd, const void *buf, size_t n)
{
	return (ssize_t)__syscall_ret(__syscall6(MINIC_NR_write, fd,
						 (long)buf, (long)n, 0, 0, 0));
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
	return (ssize_t)__syscall_ret(__syscall6(MINIC_NR_pread64, fd,
						 (long)buf, (long)n, off,
						 0, 0));
}

int open(const char *path, int flags, ...)
{
	unsigned int mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, unsigned int);
		va_end(ap);
	}
	return (int)__syscall_ret(__syscall6(MINIC_NR_openat, AT_FDCWD,
					     (long)path, flags, mode, 0, 0));
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	unsigned long arg;

	va_start(ap, request);
	arg = va_arg(ap, unsigned long);
	va_end(ap);
	return (int)__syscall_ret(__syscall6(MINIC_NR_ioctl, fd, (long)request,
					     (long)arg, 0, 0, 0));
}

int fsync(int fd)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_fsync, fd, 0, 0, 0, 0, 0));
}

int access(const char *path, int mode)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_faccessat, AT_FDCWD,
					     (long)path, mode, 0, 0, 0));
}

int chdir(const char *path)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_chdir, (long)path,
					     0, 0, 0, 0, 0));
}

int chroot(const char *path)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_chroot, (long)path,
					     0, 0, 0, 0, 0));
}

pid_t fork(void)
{
	/* clone() without CLONE_VM behaves like fork(); 0 means "copied". */
	return (pid_t)__syscall_ret(__syscall6(MINIC_NR_clone, SIGCHLD,
					       0, 0, 0, 0, 0));
}

pid_t setsid(void)
{
	return (pid_t)__syscall_ret(__syscall6(MINIC_NR_setsid, 0, 0, 0, 0, 0, 0));
}

void _exit(int status)
{
	__syscall6(MINIC_NR_exit_group, status, 0, 0, 0, 0, 0);
	for (;;)
		;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_nanosleep, (long)req,
					     (long)rem, 0, 0, 0, 0));
}

unsigned int sleep(unsigned int sec)
{
	struct timespec ts = { (time_t)sec, 0 };
	struct timespec rem;

	while (nanosleep(&ts, &rem) != 0) {
		if (errno != EINTR)
			return sec;
		ts = rem;
	}
	return 0;
}

int usleep(useconds_t usec)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(usec / 1000000);
	ts.tv_nsec = (long)(usec % 1000000) * 1000;
	return nanosleep(&ts, NULL);
}

int mount(const char *src, const char *target, const char *fstype,
	  unsigned long flags, const void *data)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_mount, (long)src,
					     (long)target, (long)fstype,
					     (long)flags, (long)data, 0));
}

int umount2(const char *target, int flags)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_umount2, (long)target,
					     flags, 0, 0, 0, 0));
}

int umount(const char *target)
{
	return umount2(target, 0);
}

int mkdir(const char *path, mode_t mode)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_mkdirat, AT_FDCWD,
					     (long)path, (long)mode, 0, 0, 0));
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	long ret = __syscall_ret(__syscall6(MINIC_NR_mmap, (long)addr,
					    (long)len, prot, flags, fd, off));

	return ret == -1 ? MAP_FAILED : (void *)ret;
}

int munmap(void *addr, size_t len)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_munmap, (long)addr,
					     (long)len, 0, 0, 0, 0));
}

int prctl(int option, ...)
{
	va_list ap;
	unsigned long a2, a3, a4, a5;

	va_start(ap, option);
	a2 = va_arg(ap, unsigned long);
	a3 = va_arg(ap, unsigned long);
	a4 = va_arg(ap, unsigned long);
	a5 = va_arg(ap, unsigned long);
	va_end(ap);
	return (int)__syscall_ret(__syscall6(MINIC_NR_prctl, option, (long)a2,
					     (long)a3, (long)a4, (long)a5, 0));
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_sched_setscheduler, pid,
					     policy, (long)param, 0, 0, 0));
}

int uname(struct utsname *buf)
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_uname, (long)buf,
					     0, 0, 0, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	return (int)__syscall_ret(__syscall6(MINIC_NR_execve, (long)path,
					     (long)argv, (long)envp,
					     0, 0, 0));
}

/*
 * The init only needs the two-argument form.  The environment carries the
 * conventional PATH/HOME so the distro init comes up the way it would have
 * under the kernel's own minimal environ.
 */
int execl(const char *path, const char *arg, ...)
{
	static char *const envp[] = {
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		"HOME=/",
		NULL,
	};
	char *argv[64];
	va_list ap;
	int i;

	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (i = 1; i < 63; i++) {
		argv[i] = va_arg(ap, char *);
		if (!argv[i])
			break;
	}
	va_end(ap);
	argv[63] = NULL;
	return execve(path, argv, envp);
}
