/* syscalls.c — newlib-nano retargeting.
 *
 * The firmware does not use printf on the reply path: camtrig.c formats
 * integers itself, allocation-free (design D10).  These exist so the link
 * succeeds and so any stray library output goes somewhere visible rather than
 * vanishing.
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>
#include "camtrig.h"

int _write(int file, char *ptr, int len)
{
	int i;

	(void)file;
	for (i = 0; i < len; i++)
		out_putc(SINK_ALL, ptr[i]);
	return len;
}

int _read(int file, char *ptr, int len)   { (void)file; (void)ptr; (void)len; return 0; }
int _close(int file)                      { (void)file; return -1; }
int _fstat(int file, struct stat *st)     { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)                     { (void)file; return 1; }
int _lseek(int file, int ptr, int dir)    { (void)file; (void)ptr; (void)dir; return 0; }
int _getpid(void)                         { return 1; }
int _kill(int pid, int sig)               { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status)                    { (void)status; for (;;) ; }
