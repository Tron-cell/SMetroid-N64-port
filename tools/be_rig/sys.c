// newlib syscall stubs for the Unicorn big-endian rig
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
long hostcall(long code, long a1, long a2, long a3);
extern char __heap_start[];
static char *brk_ptr;
void *sbrk(ptrdiff_t incr) {
  if (!brk_ptr) brk_ptr = __heap_start;
  char *p = brk_ptr;
  if ((uintptr_t)(brk_ptr + incr) > 0x80F00000u) { errno = ENOMEM; return (void *)-1; }
  brk_ptr += incr; return p;
}
int write(int fd, const void *buf, size_t n) { return hostcall(2, (long)buf, n, fd); }
int read(int fd, void *buf, size_t n) { return 0; }
int close(int fd) { return -1; }
int open(const char *n, int f, int m) { return -1; }
long lseek(int fd, long o, int w) { return 0; }
int fstat(int fd, struct stat *st) { st->st_mode = S_IFCHR; return 0; }
int isatty(int fd) { return 1; }
void _exit(int c) { hostcall(3, c, 0, 0); for (;;) {} }
int kill(int p, int s) { return -1; }
int getpid(void) { return 1; }
int gettimeofday(void *tv, void *tz) { return 0; }
int unlink(const char *n) { return -1; }
int link(const char *a, const char *b) { return -1; }
int rename(const char *a, const char *b) { return -1; }
int mkdir(const char *p, mode_t m) { return -1; }
long times(void *b) { return 0; }
#include <stdio.h>
void __assert_func(const char *file, int line, const char *fn, const char *expr) {
  printf("ASSERT %s:%d %s: %s\n", file, line, fn ? fn : "", expr);
  _exit(98);
}
