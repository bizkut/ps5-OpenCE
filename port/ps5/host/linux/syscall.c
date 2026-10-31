/* Linux host conversion of stat results to the guest kernel layout. */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* The guest musl uses the asm-generic AArch64 kernel stat layout. */
struct guest_kstat
{
	uint64_t dev, ino;
	uint32_t mode, nlink, uid, gid;
	uint64_t rdev, pad;
	int64_t size;
	int32_t blksize, pad2;
	int64_t blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
	uint32_t unused[2];
};

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
	memset(out, 0, sizeof(*out));
	out->dev = st.st_dev;
	out->ino = st.st_ino;
	out->mode = st.st_mode;
	out->nlink = st.st_nlink;
	out->uid = st.st_uid;
	out->gid = st.st_gid;
	out->rdev = st.st_rdev;
	out->size = st.st_size;
	out->blksize = st.st_blksize;
	out->blocks = st.st_blocks;
	out->atime_sec = st.st_atim.tv_sec;
	out->atime_nsec = st.st_atim.tv_nsec;
	out->mtime_sec = st.st_mtim.tv_sec;
	out->mtime_nsec = st.st_mtim.tv_nsec;
	out->ctime_sec = st.st_ctim.tv_sec;
	out->ctime_nsec = st.st_ctim.tv_nsec;
	*result = 0;
	return 1;
}
