#include <bits/syscall.h>
#include <syscall.h>

inline static long sysumount(const char* target, int flags)
{
	return syscall2(__NR_umount2, (long)target, flags);
}
