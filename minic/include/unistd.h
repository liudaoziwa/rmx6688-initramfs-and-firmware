/* SPDX-License-Identifier: GPL-2.0 */
/* minic: the POSIX-ish process/file calls the init uses */
#ifndef _MINIC_UNISTD_H
#define _MINIC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO	0
#define STDOUT_FILENO	1
#define STDERR_FILENO	2

#define F_OK	0
#define X_OK	1
#define W_OK	2
#define R_OK	4

typedef unsigned int useconds_t;

int close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
int fsync(int fd);

int access(const char *path, int mode);
int chdir(const char *path);
int chroot(const char *path);
pid_t fork(void);
pid_t setsid(void);
void _exit(int status) __attribute__((noreturn));

unsigned int sleep(unsigned int sec);
int usleep(useconds_t usec);

int execve(const char *path, char *const argv[], char *const envp[]);
int execl(const char *path, const char *arg, ...);

#endif /* _MINIC_UNISTD_H */
