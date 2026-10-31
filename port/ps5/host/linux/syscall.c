/* Linux host conversion of stat results to the guest kernel layout. */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "host_guest_syscall.h"
/* The Linux x32 host does not create a PS5 HDR10 window. */
int host_ps5_hdr(int request)
{
	(void)request;
	return 0;
}


int host_linux_stat_syscall(long number, long long a, long long b, long long c, long long d, long long *result)
{
	struct stat st;
	struct guest_kstat *out;
	int status;

	if (number == SYS_fstat)
	{
		out = (struct guest_kstat *)(uintptr_t)(uint32_t)b;
		status = fstat((int)a, &st);
	}
	else if (number == SYS_newfstatat)
	{
		out = (struct guest_kstat *)(uintptr_t)(uint32_t)c;
		status = fstatat((int)a, (const char *)(uintptr_t)(uint32_t)b, &st, (int)d);
	}
	else
		return 0;
	if (status)
	{
		*result = -errno;
		return 1;
	}
	if (!out)
	{
		*result = -EFAULT;
		return 1;
	}
	host_guest_stat_out(out, &st);
	*result = 0;
	return 1;
}
