/* Guest Linux ABI structures and the native stat conversion shared by hosts. */
#ifndef HOST_GUEST_SYSCALL_H
#define HOST_GUEST_SYSCALL_H
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

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
static inline void host_guest_stat_out(struct guest_kstat *out, const struct stat *st)
{
	memset(out, 0, sizeof(*out));
	out->dev = st->st_dev;
	out->ino = st->st_ino;
	out->mode = st->st_mode;
	out->nlink = st->st_nlink;
	out->uid = st->st_uid;
	out->gid = st->st_gid;
	out->rdev = st->st_rdev;
	out->size = st->st_size;
	out->blksize = st->st_blksize;
	out->blocks = st->st_blocks;
	out->atime_sec = st->st_atim.tv_sec;
	out->atime_nsec = st->st_atim.tv_nsec;
	out->mtime_sec = st->st_mtim.tv_sec;
	out->mtime_nsec = st->st_mtim.tv_nsec;
	out->ctime_sec = st->st_ctim.tv_sec;
	out->ctime_nsec = st->st_ctim.tv_nsec;
}
#endif
