#include <bits/syscall.h>
#include <bits/fcntl.h>
#include <syscall.h>

inline static long sysmknod(const char* pathname, int mode, int dev)
{
	return syscall4(__NR_mknodat, AT_FDCWD, (long)pathname, mode, dev);
}
