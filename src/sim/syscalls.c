/* sh-elf-run low-level layer: simulator syscalls ("trapa #34", newlib/libgloss convention),
   POSIX-ish file descriptor functions required by fxlibc stdio, process start-up and exit.

   Reference: gdb-15.2/sim/sh/interp.c, trap(). Anything the simulator does not implement
   (unlink, mkdir, isatty, fstat, getcwd...) is stubbed to fail with ENOSYS/ENOTTY. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fxlibc/hal.h>
#include "sim.h"

/*--- raw syscall ------------------------------------------------------------------------------*/

/* newlib errno values (as returned in r1 by the simulator) -> fxlibc errno values. */
static int errno_from_newlib(int e)
{
	switch(e) {
	case 0:  return 0;
	case 1:  return EPERM;
	case 2:  return ENOENT;
	case 5:  return EIO;
	case 9:  return EBADF;
	case 12: return ENOMEM;
	case 13: return EACCES;
	case 17: return EEXIST;
	case 20: return ENOTDIR;
	case 21: return EISDIR;
	case 22: return EINVAL;
	case 23: case 24: return EMFILE;
	case 27: return EFBIG;
	case 28: return ENOSPC;
	case 29: return ESPIPE;
	default: return EIO;
	}
}

int sim_syscall(int nr, int a, int b, int c)
{
	register int r0 __asm__("r0");
	register int r1 __asm__("r1");
	register int r4 __asm__("r4") = nr;
	register int r5 __asm__("r5") = a;
	register int r6 __asm__("r6") = b;
	register int r7 __asm__("r7") = c;
	__asm__ volatile ("trapa #34"
		: "=r"(r0), "=r"(r1)
		: "r"(r4), "r"(r5), "r"(r6), "r"(r7)
		: "memory", "t");
	if(r0 < 0) errno = errno_from_newlib(r1);
	return r0;
}

/*--- statistics (sim64 extension) ---------------------------------------------------------------*/

int sim_counters(struct sim_counters *out)
{
	uint32_t w[8] __attribute__((aligned(4)));
	int saved = errno;
	int r = sim_syscall(SIM_SYS_counters, (int)w, 0, 0);
	errno = saved;
	if(r != 0) return -1;
	out->insts    = ((uint64_t)w[0] << 32) | w[1];
	out->cycles   = ((uint64_t)w[2] << 32) | w[3];
	out->stalls   = ((uint64_t)w[4] << 32) | w[5];
	out->memstalls = ((uint64_t)w[6] << 32) | w[7];
	return 0;
}

int sim_mark(int id)
{
	int saved = errno;
	int r = sim_syscall(SIM_SYS_mark, id, 0, 0);
	errno = saved;
	return r;
}

int sim_pcprof(int on)
{
	int saved = errno;
	int r = sim_syscall(SIM_SYS_pcprof, on, 0, 0);
	errno = saved;
	return r;
}

uint32_t sim_micros(void)
{
	struct sim_counters c;
	if(sim_counters(&c) == 0) return (uint32_t)(c.cycles / SIM_CYCLES_PER_US);
	return (uint32_t)sim_syscall(SIM_SYS_time, 0, 0, 0) * 1000000u;
}

/*--- file descriptors ------------------------------------------------------------------------*/

/* newlib O_* values understood by the simulator's open(). */
#define N_O_WRONLY 0x0001
#define N_O_RDWR   0x0002
#define N_O_APPEND 0x0008
#define N_O_CREAT  0x0200
#define N_O_TRUNC  0x0400
#define N_O_EXCL   0x0800

int open(const char *path, int flags, ...)
{
	int nf = flags & 3;                    /* O_RDONLY/O_WRONLY/O_RDWR are 0/1/2 in both */
	if(flags & O_APPEND) nf |= N_O_APPEND;
	if(flags & O_CREAT)  nf |= N_O_CREAT;
	if(flags & O_TRUNC)  nf |= N_O_TRUNC;
	if(flags & O_EXCL)   nf |= N_O_EXCL;
	return sim_syscall(SIM_SYS_open, (int)path, nf, 0);
}

int creat(const char *path, mode_t mode)
{
	(void)mode;
	return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

ssize_t read(int fd, void *buf, size_t n)
{
	return sim_syscall(SIM_SYS_read, fd, (int)buf, (int)n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
	return sim_syscall(SIM_SYS_write, fd, (int)buf, (int)n);
}

off_t lseek(int fd, off_t off, int whence)
{
	/* fxlibc: SEEK_CUR=1, SEEK_END=2, SEEK_SET=3. The simulator wants the POSIX 0/1/2. */
	int w = (whence == SEEK_SET) ? 0 : whence;
	return sim_syscall(SIM_SYS_lseek, fd, off, w);
}

int close(int fd)
{
	if(fd < 3) return 0;      /* never close the host's stdio */
	return sim_syscall(SIM_SYS_close, fd, 0, 0);
}

int unlink(const char *path) { (void)path; errno = ENOSYS; return -1; }
int rmdir(const char *path)  { (void)path; errno = ENOSYS; return -1; }
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int fsync(int fd)            { (void)fd; return 0; }
int isatty(int fd)           { (void)fd; errno = ENOTTY; return 0; }
char *getcwd(char *buf, size_t size) { (void)buf; (void)size; errno = ENOSYS; return NULL; }
int chdir(const char *path)  { (void)path; errno = ENOSYS; return -1; }

/* The simulator fills a newlib-layout stat buffer (big endian):
   0 dev(2) 2 ino(2) 4 mode(4) 8 nlink(2) 10 uid(2) 12 gid(2) 14 rdev(2) 16 size(4) 20 atime(4)
   24 -(4) 28 mtime(4) ...; it writes 64 bytes. */
int stat(const char *restrict path, struct stat *restrict st)
{
	uint8_t raw[96] __attribute__((aligned(4)));
	memset(raw, 0, sizeof raw);
	int r = sim_syscall(SIM_SYS_stat, (int)path, (int)raw, 0);
	if(r < 0) return -1;
	uint32_t mode = *(uint32_t *)(raw + 4);
	memset(st, 0, sizeof *st);
	st->st_size = *(uint32_t *)(raw + 16);
	st->st_mode = ((mode & 0170000) == 0040000) ? (S_IFDIR | 0777) : (S_IFREG | 0777);
	st->st_mtim.tv_sec = *(uint32_t *)(raw + 28);
	return 0;
}

int fstat(int fd, struct stat *st)
{
	/* No fstat in the simulator: derive the size with lseek. */
	off_t cur = lseek(fd, 0, SEEK_CUR);
	if(cur < 0) return -1;
	off_t end = lseek(fd, 0, SEEK_END);
	lseek(fd, cur, SEEK_SET);
	memset(st, 0, sizeof *st);
	st->st_size = end;
	st->st_mode = S_IFREG | 0777;
	return 0;
}

/*--- time ------------------------------------------------------------------------------------*/

/* Host wall clock, 1 second resolution (the simulator only offers time()). */
void fxlibc_hal_rawtime(struct tm *tm)
{
	uint32_t t = (uint32_t)sim_syscall(SIM_SYS_time, 0, 0, 0);
	uint32_t days = t / 86400, rem = t % 86400;
	tm->tm_hour = rem / 3600;
	tm->tm_min = (rem / 60) % 60;
	tm->tm_sec = rem % 60;
	/* civil-from-days (Howard Hinnant) */
	int32_t z = (int32_t)days + 719468;
	int32_t era = z / 146097;
	uint32_t doe = z - era * 146097;
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int32_t y = (int32_t)yoe + era * 400;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;
	uint32_t d = doy - (153 * mp + 2) / 5 + 1;
	uint32_t m = mp < 10 ? mp + 3 : mp - 9;
	tm->tm_year = y + (m <= 2) - 1900;
	tm->tm_mon = m - 1;
	tm->tm_mday = d;
	tm->tm_isdst = 0;
}

clock_t fxlibc_hal_clock(void)
{
	return (clock_t)((uint32_t)sim_syscall(SIM_SYS_time, 0, 0, 0)) * CLOCKS_PER_SEC;
}

/*--- process start and exit ------------------------------------------------------------------*/

void _Exit(int status)
{
	for(;;) sim_syscall(SIM_SYS_exit, status, 0, 0);
}

void exit(int status)
{
	extern void __cxa_finalize(void *);
	__cxa_finalize(NULL);
	sim_exit(status);
}

void sim_exit(int status)
{
	if(stdout) fflush(stdout);
	if(stderr) fflush(stderr);
	_Exit(status);
}

void abort(void)
{
	if(stdout) fflush(stdout);
	static const char msg[] = "abort() called\n";
	write(2, msg, sizeof msg - 1);
	_Exit(134);
}

char *getenv(const char *name) { (void)name; return NULL; }
int system(const char *cmd) { (void)cmd; errno = ENOSYS; return -1; }

#define SIM_MAX_ARGS 64
#define SIM_ARGBUF   16384

int sim_argc;
char **sim_argv;
static char *argv_store[SIM_MAX_ARGS + 1];
static char argbuf[SIM_ARGBUF];

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern int main(int argc, char **argv);

__attribute__((noreturn)) void __sim_start(void)
{
	/* fxlibc relies on constructors (stdin/stdout/stderr) */
	for(void (**f)(void) = __init_array_start; f < __init_array_end; f++)
		(*f)();

	int argc = sim_syscall(SIM_SYS_argc, 0, 0, 0);
	if(argc < 0) argc = 0;
	if(argc > SIM_MAX_ARGS) argc = SIM_MAX_ARGS;
	char *p = argbuf;
	int n = 0;
	for(int i = 0; i < argc; i++) {
		int len = sim_syscall(SIM_SYS_argnlen, i, 0, 0);
		if(len < 0 || p + len + 1 > argbuf + SIM_ARGBUF) break;
		sim_syscall(SIM_SYS_argn, i, (int)p, 0);
		p[len] = 0;
		/* "simrun" protects arguments starting with a digit with a \037 prefix (see simrun) */
		argv_store[n++] = (i > 0 && p[0] == 037) ? p + 1 : p;
		p += len + 1;
	}
	argv_store[n] = NULL;
	sim_argc = n;
	sim_argv = argv_store;

	exit(main(n, argv_store));
}
