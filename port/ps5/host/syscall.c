/* Linux asm-generic guest calls on the PS5's FreeBSD libc. */
#include "host.h"
#include "../../android/guest/libc/arch/arm64_32/bits/syscall.h.in"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <signal.h>
#include <ps5platform/context.h>
#include <unistd.h>
/* The platform layer resolves descriptors opened by its openat shim. */
int ps5p_resolve_at(int directory, const char *name, char *out, size_t size);
int ps5_access(const char *path, int mode);
ssize_t ps5_readlink(const char *path, char *buffer, size_t size);
int host_ps5_random_bytes(void *buffer, size_t size);
static int trace_map_io;
static uint8_t map_descriptors[4096];
static unsigned trace_map_reads;

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

/* SDK errno numbers are FreeBSD's, not musl's asm-generic Linux values. */
static int linux_errno(int error)
{
	static const unsigned char map[] = {
		[0] = 0,
		[EPERM] = 1,
		[ENOENT] = 2,
		[ESRCH] = 3,
		[EINTR] = 4,
		[EIO] = 5,
		[ENXIO] = 6,
		[E2BIG] = 7,
		[ENOEXEC] = 8,
		[EBADF] = 9,
		[ECHILD] = 10,
		[EDEADLK] = 35,
		[ENOMEM] = 12,
		[EACCES] = 13,
		[EFAULT] = 14,
		[ENOTBLK] = 15,
		[EBUSY] = 16,
		[EEXIST] = 17,
		[EXDEV] = 18,
		[ENODEV] = 19,
		[ENOTDIR] = 20,
		[EISDIR] = 21,
		[EINVAL] = 22,
		[ENFILE] = 23,
		[EMFILE] = 24,
		[ENOTTY] = 25,
		[ETXTBSY] = 26,
		[EFBIG] = 27,
		[ENOSPC] = 28,
		[ESPIPE] = 29,
		[EROFS] = 30,
		[EMLINK] = 31,
		[EPIPE] = 32,
		[EDOM] = 33,
		[ERANGE] = 34,
		[EAGAIN] = 11,
		[EINPROGRESS] = 115,
		[EALREADY] = 114,
		[ENOTSOCK] = 88,
		[EDESTADDRREQ] = 89,
		[EMSGSIZE] = 90,
		[EPROTOTYPE] = 91,
		[ENOPROTOOPT] = 92,
		[EPROTONOSUPPORT] = 93,
		[ESOCKTNOSUPPORT] = 94,
		[EOPNOTSUPP] = 95,
		[EPFNOSUPPORT] = 96,
		[EAFNOSUPPORT] = 97,
		[EADDRINUSE] = 98,
		[EADDRNOTAVAIL] = 99,
		[ENETDOWN] = 100,
		[ENETUNREACH] = 101,
		[ENETRESET] = 102,
		[ECONNABORTED] = 103,
		[ECONNRESET] = 104,
		[ENOBUFS] = 105,
		[EISCONN] = 106,
		[ENOTCONN] = 107,
		[ESHUTDOWN] = 108,
		[ETOOMANYREFS] = 109,
		[ETIMEDOUT] = 110,
		[ECONNREFUSED] = 111,
		[ELOOP] = 40,
		[ENAMETOOLONG] = 36,
		[EHOSTDOWN] = 112,
		[EHOSTUNREACH] = 113,
		[ENOTEMPTY] = 39,
		[EPROCLIM] = 11,
		[EUSERS] = 87,
		[EDQUOT] = 122,
		[ESTALE] = 116,
		[EREMOTE] = 66,
		[EBADRPC] = 5,
		[ERPCMISMATCH] = 5,
		[EPROGUNAVAIL] = 5,
		[EPROGMISMATCH] = 5,
		[EPROCUNAVAIL] = 5,
		[ENOLCK] = 37,
		[ENOSYS] = 38,
		[EFTYPE] = 22,
		[EAUTH] = 13,
		[ENEEDAUTH] = 13,
		[EIDRM] = 43,
		[ENOMSG] = 42,
		[EOVERFLOW] = 75,
		[ECANCELED] = 125,
		[EILSEQ] = 84,
		[ENOATTR] = 61,
		[EDOOFUS] = 22,
		[EBADMSG] = 74,
		[EMULTIHOP] = 72,
		[ENOLINK] = 67,
		[EPROTO] = 71,
		[ENOTCAPABLE] = 13,
		[ECAPMODE] = 1,
		[ENOTRECOVERABLE] = 131,
		[EOWNERDEAD] = 130,
	};
	return error >= 0 && (size_t)error < sizeof(map) ? map[error] : 5;
}
static long result(long value)
{
	return value == -1 ? -linux_errno(errno) : value;
}
static long memory_result(long value)
{
	return value < 0 && value >= -4095 ? -linux_errno((int)-value) : value;
}
/* The guest can split one line over several writes or iovecs. Keep a
   separate line for each standard stream, as the Android host does. */
struct log_stream
{
	char line[1024];
	size_t length;
};
static struct log_stream log_streams[2];
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	pthread_mutex_lock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_logf(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	pthread_mutex_unlock(&log_lock);
}

#include "host_guest_io.h"
#include "host_guest_syscall.h"
struct guest_flock
{
	int16_t type, whence;
	int32_t pad;
	int64_t start, length;
	int32_t pid, pad2;
};
_Static_assert(sizeof(struct guest_kstat) == 128 && offsetof(struct guest_kstat, size) == 48 &&
				   offsetof(struct guest_kstat, blocks) == 64 && offsetof(struct guest_kstat, atime_sec) == 72 &&
				   offsetof(struct guest_kstat, unused) == 120,
			   "guest kstat layout");
_Static_assert(sizeof(struct guest_timespec) == 8 && sizeof(struct guest_iovec) == 8 &&
				   sizeof(struct guest_flock) == 32 && offsetof(struct guest_flock, start) == 8 &&
				   offsetof(struct guest_flock, pid) == 24,
			   "guest musl layouts");
static int time_in(uint64_t p, struct timespec *t)
{
	const struct guest_timespec *g = GUEST(const struct guest_timespec *, p);
	if (!g)
		return 0;
	t->tv_sec = g->seconds;
	t->tv_nsec = g->nanoseconds;
	return 1;
}
static void time_out(uint64_t p, const struct timespec *t)
{
	struct guest_timespec *g = GUEST(struct guest_timespec *, p);
	if (g)
	{
		g->seconds = (int32_t)t->tv_sec;
		g->nanoseconds = (int32_t)t->tv_nsec;
	}
}
static int guest_clock(int id)
{
	switch (id)
	{
	case 0:
		return CLOCK_REALTIME;
	case 1:
		return CLOCK_MONOTONIC;
	default:
		return -1;
	}
}
/* Linux and FreeBSD use different open and at flag bit assignments. */
static int open_flags(int f)
{
	int h = f & 3;
	if (f & 0100)
		h |= O_CREAT;
	if (f & 0200)
		h |= O_EXCL;
	if (f & 0400)
		h |= O_NOCTTY;
	if (f & 01000)
		h |= O_TRUNC;
	if (f & 02000)
		h |= O_APPEND;
	if (f & 04000)
		h |= O_NONBLOCK;
	if (f & 010000)
		h |= O_SYNC;
	if (f & 020000)
		h |= O_ASYNC;
	if (f & 040000)
		h |= O_DIRECTORY;
	if (f & 0100000)
		h |= O_NOFOLLOW;
	if (f & 0200000)
		h |= O_DIRECT;
	if (f & 02000000)
		h |= O_CLOEXEC;
	return h;
}
static int linux_open_flags(int f)
{
	int g = f & O_ACCMODE;
	if (f & O_APPEND)
		g |= 02000;
	if (f & O_NONBLOCK)
		g |= 04000;
	if (f & O_SYNC)
		g |= 04010000;
	if (f & O_ASYNC)
		g |= 020000;
	if (f & O_DIRECT)
		g |= 0200000;
	return g;
}
static int at_flags(int f)
{
	int h = 0;
	if (f & 0x100)
		h |= AT_SYMLINK_NOFOLLOW;
	if (f & 0x200)
		h |= AT_REMOVEDIR;
	if (f & 0x400)
		h |= AT_SYMLINK_FOLLOW;
	if (f & 0x2000)
		h |= AT_EACCESS;
	return h;
}

/* A single bucket lock makes checking the word and sleeping atomic against
   wake. Broadcast also permits musl's REQUEUE to wake spuriously instead. */
#define FUTEX_BUCKETS 64
struct futex_bucket
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int waiters;
};
static struct futex_bucket buckets[FUTEX_BUCKETS];
static pthread_once_t futex_once = PTHREAD_ONCE_INIT;
static void futex_init(void)
{
	unsigned i;
	for (i = 0; i < FUTEX_BUCKETS; i++)
	{
		pthread_mutex_init(&buckets[i].mutex, NULL);
		pthread_cond_init(&buckets[i].cond, NULL);
	}
}
static long futex_call(uint64_t address, int op, uint32_t value, uint64_t timeout, uint32_t value3)
{
	volatile uint32_t *word = GUEST(volatile uint32_t *, address);
	struct futex_bucket *bucket;
	int command = op & ~(128 | 256), rc = 0;
	struct timespec until, rel;
	if (!word || ((uintptr_t)word & 3))
		return -22;
	if (command != 0 && command != 1 && command != 3 && command != 4)
		return -38;
	if (command == 0 && timeout)
	{
		time_in(timeout, &rel);
		if (rel.tv_sec < 0 || rel.tv_nsec < 0 || rel.tv_nsec >= 1000000000)
			return -22;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += rel.tv_sec;
		until.tv_nsec += rel.tv_nsec;
		if (until.tv_nsec >= 1000000000)
		{
			until.tv_nsec -= 1000000000;
			until.tv_sec++;
		}
	}
	pthread_once(&futex_once, futex_init);
	bucket = &buckets[((uintptr_t)word >> 2) % FUTEX_BUCKETS];
	pthread_mutex_lock(&bucket->mutex);
	if (command == 0)
	{
		if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != value)
			rc = -11;
		else
		{
			bucket->waiters++;
			rc = timeout ? pthread_cond_timedwait(&bucket->cond, &bucket->mutex, &until)
						 : pthread_cond_wait(&bucket->cond, &bucket->mutex);
			bucket->waiters--;
			if (rc)
				rc = -linux_errno(rc);
		}
	}
	else if (command == 4 && __atomic_load_n(word, __ATOMIC_SEQ_CST) != value3)
	{
		rc = -11;
	}
	else
	{
		int count = bucket->waiters;
		if (command == 1 && value < (uint32_t)count)
			count = (int)value;
		if (bucket->waiters && (command == 3 || command == 4 || value))
			pthread_cond_broadcast(&bucket->cond);
		rc = count;
	}
	pthread_mutex_unlock(&bucket->mutex);
	return rc;
}
static long vector_io(int fd, uint64_t address, int count, int64_t offset, int write_op, int positional)
{
	struct iovec v[64];
	const struct guest_iovec *g = GUEST(const struct guest_iovec *, address);
	int i;
	if (count < 0 || count > 64)
		return -22;
	for (i = 0; i < count; i++)
	{
		v[i].iov_base = GUEST(void *, g[i].base);
		v[i].iov_len = g[i].length;
	}
	if (write_op && (fd == 1 || fd == 2))
	{
		long n = 0;
		for (i = 0; i < count; i++)
		{
			log_bytes(fd, v[i].iov_base, v[i].iov_len);
			n += (long)v[i].iov_len;
		}
		return n;
	}
	if (positional)
		return result(write_op ? pwritev(fd, v, count, offset) : preadv(fd, v, count, offset));
	return result(write_op ? writev(fd, v, count) : readv(fd, v, count));
}
static long guest_fcntl(int fd, int command, uint64_t argument)
{
	switch (command)
	{
	case 0:
		return result(fcntl(fd, F_DUPFD, argument));
	case 1:
		return result(fcntl(fd, F_GETFD));
	case 2:
		return result(fcntl(fd, F_SETFD, argument));
	case 3:
	{
		int r = fcntl(fd, F_GETFL);
		return r < 0 ? -linux_errno(errno) : linux_open_flags(r);
	}
	case 4:
		return result(fcntl(fd, F_SETFL, open_flags(argument)));
	case 5:
	case 6:
	case 7:
	{
		struct guest_flock *g = GUEST(struct guest_flock *, argument);
		struct flock h;
		int r;
		if (!g)
			return -14;
		memset(&h, 0, sizeof(h));
		h.l_start = g->start;
		h.l_len = g->length;
		h.l_pid = g->pid;
		h.l_whence = g->whence;
		switch (g->type)
		{
		case 0:
			h.l_type = F_RDLCK;
			break;
		case 1:
			h.l_type = F_WRLCK;
			break;
		case 2:
			h.l_type = F_UNLCK;
			break;
		default:
			return -22;
		}
		r = fcntl(fd, command == 5 ? F_GETLK : command == 6 ? F_SETLK : F_SETLKW, &h);
		if (r < 0)
			return -linux_errno(errno);
		if (command == 5)
		{
			g->type = h.l_type == F_RDLCK ? 0 : h.l_type == F_WRLCK ? 1 : 2;
			g->whence = h.l_whence;
			g->start = h.l_start;
			g->length = h.l_len;
			g->pid = h.l_pid;
		}
		return r;
	}
	case 8:
		return result(fcntl(fd, F_SETOWN, argument));
	case 9:
		return result(fcntl(fd, F_GETOWN));
	case 1030:
		return result(fcntl(fd, F_DUPFD_CLOEXEC, argument));
	default:
		return -38;
	}
}

/* The title kernel's getdents records have a 32-bit inode and no d_off.
   Keep unread records across guest calls because Linux records are larger. */
struct dir_buffer
{
	int fd;
	size_t used, offset;
	unsigned char data[65536];
	struct dir_buffer *next;
};
static struct dir_buffer *directories;
static pthread_mutex_t directories_lock = PTHREAD_MUTEX_INITIALIZER;
static void directory_forget(int fd)
{
	struct dir_buffer **p = &directories;
	pthread_mutex_lock(&directories_lock);
	while (*p)
	{
		if ((*p)->fd == fd)
		{
			struct dir_buffer *old = *p;
			*p = old->next;
			free(old);
			break;
		}
		p = &(*p)->next;
	}
	pthread_mutex_unlock(&directories_lock);
}
static long directory_read(int fd, void *output, size_t capacity)
{
	struct dir_buffer *s;
	size_t done = 0;
	long answer;
	pthread_mutex_lock(&directories_lock);
	for (s = directories; s && s->fd != fd; s = s->next)
	{
	}
	if (!s)
	{
		s = calloc(1, sizeof(*s));
		if (!s)
		{
			answer = -12;
			goto out;
		}
		s->fd = fd;
		s->next = directories;
		directories = s;
	}
	for (;;)
	{
		uint32_t ino;
		uint16_t len, outlen;
		unsigned char *p, *q;
		size_t namesize;
		if (s->offset == s->used)
		{
			int n = getdents(fd, (char *)s->data, sizeof(s->data));
			if (n < 0)
			{
				answer = done ? (long)done : -linux_errno(errno);
				goto out;
			}
			if (!n)
				break;
			s->offset = 0;
			s->used = (size_t)n;
		}
		p = s->data + s->offset;
		if (s->used - s->offset < 8)
		{
			answer = done ? (long)done : -5;
			goto out;
		}
		memcpy(&ino, p, 4);
		memcpy(&len, p + 4, 2);
		namesize = (size_t)p[7] + 1;
		if (len < 8 + namesize || len > s->used - s->offset || p[8 + namesize - 1])
		{
			answer = done ? (long)done : -5;
			goto out;
		}
		if (!ino)
		{
			s->offset += len;
			continue;
		}
		outlen = (uint16_t)((19 + namesize + 7) & ~7u);
		if (capacity - done < outlen)
		{
			if (!done)
			{
				answer = -22;
				goto out;
			}
			break;
		}
		q = (unsigned char *)output + done;
		memset(q, 0, outlen);
		{
			uint64_t id = ino;
			memcpy(q, &id, 8);
		}
		memcpy(q + 16, &outlen, 2);
		q[18] = p[6];
		memcpy(q + 19, p + 8, namesize);
		done += outlen;
		s->offset += len;
	}
	answer = (long)done;
out:
	pthread_mutex_unlock(&directories_lock);
	return answer;
}
static long long dispatch_syscall(long long number, long long a, long long b, long long c, long long d, long long e,
								  long long f)
{
	(void)e;
	(void)f;
	switch (number)
	{
	case __NR_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (uint32_t)c);
			return (uint32_t)c;
		}
		return result(write((int)a, GUEST(const void *, b), (uint32_t)c));
	case __NR_read:
	{
		long r = result(read((int)a, GUEST(void *, b), (uint32_t)c));
		if (trace_map_io && a >= 0 && a < 4096 && map_descriptors[a] && __sync_fetch_and_add(&trace_map_reads, 1) < 100)
			host_logf(HOST_LOG_INFO, "opence: map read fd=%lld size=%llu result=%ld", a, (unsigned long long)c, r);
		return r;
	}
	case __NR_writev:
		return vector_io(a, b, c, 0, 1, 0);
	case __NR_readv:
		return vector_io(a, b, c, 0, 0, 0);
	case __NR_pwritev:
		return vector_io(a, b, c, d, 1, 1);
	case __NR_preadv:
		return vector_io(a, b, c, d, 0, 1);
	case __NR_pread64:
	{
		long r = result(pread(a, GUEST(void *, b), (uint32_t)c, d));
		if (trace_map_io && a >= 0 && a < 4096 && map_descriptors[a] && __sync_fetch_and_add(&trace_map_reads, 1) < 100)
			host_logf(HOST_LOG_INFO, "opence: map pread fd=%lld size=%llu offset=%lld result=%ld", a,
					  (unsigned long long)c, d, r);
		return r;
	}
	case __NR_pwrite64:
		return result(pwrite(a, GUEST(const void *, b), (uint32_t)c, d));
	case __NR_openat:
	{
		const char *path = GUEST(const char *, b);
		long r = result(openat(a, path, open_flags(c), d));
		if (trace_map_io && path && strstr(path, ".map"))
		{
			if (r >= 0 && r < 4096)
				map_descriptors[r] = strstr(path, "bloodgulch") ? 2 : 1;
			host_logf(HOST_LOG_INFO, "opence: map open path=%s flags=%llo result=%ld", path, (unsigned long long)c, r);
		}
		return r;
	}
	case __NR_close:
		if (a >= 0 && a < 4096)
			map_descriptors[a] = 0;
		directory_forget(a);
		return result(close(a));
	case __NR_lseek:
	{
		long r;
		directory_forget(a);
		r = result(lseek(a, b, c));
		if (trace_map_io && a >= 0 && a < 4096 && map_descriptors[a])
			host_logf(HOST_LOG_INFO, "opence: map lseek fd=%lld offset=%lld whence=%lld result=%ld", a, b, c, r);
		return r;
	}
	case __NR_getdents64:
		return directory_read(a, GUEST(void *, b), (uint32_t)c);
	case __NR_unlinkat:
		return result(unlinkat(a, GUEST(const char *, b), at_flags(c)));
	case __NR_renameat:
		return result(renameat(a, GUEST(const char *, b), c, GUEST(const char *, d)));
	case __NR_mkdirat:
		return result(mkdirat(a, GUEST(const char *, b), c));
	case __NR_fchmod:
		return result(fchmod(a, b));
	case __NR_fchmodat:
		return result(fchmodat(a, GUEST(const char *, b), c, at_flags(d)));
	case __NR_ftruncate:
		return result(ftruncate(a, b));
	case __NR_fsync:
	case __NR_fdatasync:
		return result(fsync(a));
	case __NR_getcwd:
	{
		char *p = GUEST(char *, a);
		return getcwd(p, (uint32_t)b) ? (long)strlen(p) + 1 : -linux_errno(errno);
	}
	case __NR_chdir:
		return result(chdir(GUEST(const char *, a)));
	case __NR_readlinkat:
	{
		char path[1024];
		if (ps5p_resolve_at(a, GUEST(const char *, b), path, sizeof(path)))
			return -linux_errno(errno);
		return result(ps5_readlink(path, GUEST(char *, c), (uint32_t)d));
	}
	case __NR_faccessat:
	{
		char path[1024];
		if (ps5p_resolve_at(a, GUEST(const char *, b), path, sizeof(path)))
			return -linux_errno(errno);
		return result(ps5_access(path, c));
	}
	case __NR_dup:
		return result(dup(a));
	case __NR_dup3:
	{
		int r;
		if (a == b || (c & ~02000000))
			return -22;
		r = dup2(a, b);
		if (r < 0)
			return -linux_errno(errno);
		if (c && fcntl(r, F_SETFD, FD_CLOEXEC) < 0)
		{
			int err = errno;
			close(r);
			return -linux_errno(err);
		}
		return r;
	}
	case __NR_pipe2:
	{
		int *p = GUEST(int *, a);
		if (!p)
			return -14;
		if (b & ~(02000000 | 04000))
			return -22;
		if (pipe(p))
			return -linux_errno(errno);
		if ((b & 02000000) && (fcntl(p[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) < 0))
		{
			int err = errno;
			close(p[0]);
			close(p[1]);
			return -linux_errno(err);
		}
		if ((b & 04000) && (fcntl(p[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(p[1], F_SETFL, O_NONBLOCK) < 0))
		{
			int err = errno;
			close(p[0]);
			close(p[1]);
			return -linux_errno(err);
		}
		return 0;
	}
	case __NR_fstat:
	case __NR_newfstatat:
	{
		struct stat st;
		struct guest_kstat *g = GUEST(struct guest_kstat *, number == __NR_fstat ? b : c);
		int r = number == __NR_fstat ? fstat(a, &st) : fstatat(a, GUEST(const char *, b), &st, at_flags(d));
		int err = errno;
		if (trace_map_io && number == __NR_fstat && a >= 0 && a < 4096 && map_descriptors[a])
			host_logf(HOST_LOG_INFO, "opence: map fstat fd=%lld result=%d size=%lld", a, r,
					  r ? 0LL : (long long)st.st_size);
		if (r)
			return -linux_errno(err);
		if (!g)
			return -14;
		host_guest_stat_out(g, &st);
		return 0;
	}
	case __NR_clock_gettime:
	case __NR_clock_getres:
	{
		struct timespec t;
		int id = guest_clock(a), r;
		if (id < 0)
			return -22;
		r = number == __NR_clock_gettime ? clock_gettime(id, &t) : clock_getres(id, &t);
		if (r)
			return -linux_errno(errno);
		time_out(b, &t);
		return 0;
	}
	case __NR_gettimeofday:
	{
		struct timespec t;
		if (clock_gettime(CLOCK_REALTIME, &t))
			return -linux_errno(errno);
		t.tv_nsec /= 1000;
		time_out(a, &t);
		return 0;
	}
	case __NR_nanosleep:
	{
		struct timespec t, r;
		int x;
		if (!time_in(a, &t))
			return -14;
		x = nanosleep(&t, &r);
		if (x && errno == EINTR)
			time_out(b, &r);
		return result(x);
	}
	case __NR_clock_nanosleep:
	{
		struct timespec t, r;
		int id = guest_clock(a), x;
		if (id < 0)
			return -22;
		if (!time_in(c, &t))
			return -14;
		x = clock_nanosleep(id, b, &t, &r);
		if (x == EINTR)
			time_out(d, &r);
		return x ? -linux_errno(x) : 0;
	}
	case __NR_futex:
		return futex_call(a, b, c, d, f);
	case __NR_ppoll:
	{
		struct timespec t;
		int ms = -1;
		if (time_in(c, &t))
		{
			if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000)
				return -22;
			ms = t.tv_sec > 2147483 ? 2147483647 : (int)(t.tv_sec * 1000 + (t.tv_nsec + 999999) / 1000000);
		}
		return result(poll(GUEST(struct pollfd *, a), (uint32_t)b, ms));
	}
	case __NR_utimensat:
	{
		struct timespec t[2];
		const struct guest_timespec *g = GUEST(const struct guest_timespec *, c);
		int i;
		if (g)
		{
			time_in(c, &t[0]);
			time_in(c + sizeof(*g), &t[1]);
			for (i = 0; i < 2; i++)
			{
				if (t[i].tv_nsec == 0x3fffffff)
					t[i].tv_nsec = UTIME_NOW;
				else if (t[i].tv_nsec == 0x3ffffffe)
					t[i].tv_nsec = UTIME_OMIT;
			}
		}
		return result(utimensat(a, GUEST(const char *, b), g ? t : NULL, at_flags(d)));
	}
	case __NR_mmap:
		return memory_result(host_guest_mmap(a, b, c, d, e, f));
	case __NR_munmap:
		return memory_result(host_guest_munmap(a, b));
	case __NR_mprotect:
		return memory_result(host_guest_mprotect(a, b, c));
	case __NR_madvise:
		return (uint64_t)a + (uint64_t)b > 0x100000000ULL ? -22 : 0;
	case __NR_mremap:
	case __NR_brk:
		return -12;
	case __NR_exit:
	case __NR_exit_group:
		host_exit(a);
	case __NR_set_tid_address:
	case __NR_gettid:
		return (uint32_t)(uintptr_t)pthread_self();
	case __NR_rt_sigaction:
	case __NR_sigaltstack:
		return 0;
	case __NR_rt_sigprocmask:
		if (c)
			memset(GUEST(void *, c), 0, 8);
		return 0;
	case __NR_ioctl:
		return -25;
	case __NR_getpid:
		return getpid();
	case __NR_getppid:
		return getppid();
	case __NR_getuid:
		return getuid();
	case __NR_geteuid:
		return geteuid();
	case __NR_getgid:
		return getgid();
	case __NR_getegid:
		return getegid();
	case __NR_sched_yield:
		return result(sched_yield());
	case __NR_getrandom:
		if (c & ~3)
			return -22;
		return host_ps5_random_bytes(GUEST(void *, a), (uint32_t)b) ? -5 : (uint32_t)b;
	case __NR_flock:
		return result(flock(a, b));
	case __NR_uname:
	{
		struct utsname u;
		char (*out)[65] = GUEST(char (*)[65], a);
		const char *values[5];
		int i;
		if (!out)
			return -14;
		if (uname(&u))
			return -linux_errno(errno);
		values[0] = u.sysname;
		values[1] = u.nodename;
		values[2] = u.release;
		values[3] = u.version;
		values[4] = u.machine;
		memset(out, 0, 6 * 65);
		for (i = 0; i < 5; i++)
		{
			size_t n = strnlen(values[i], 64);
			memcpy(out[i], values[i], n);
		}
		return 0;
	}
	case __NR_getrlimit:
	case __NR_prlimit64:
	{
		static const int resources[] = {RLIMIT_CPU, RLIMIT_FSIZE, RLIMIT_DATA,	 RLIMIT_STACK,	 RLIMIT_CORE,
										RLIMIT_RSS, RLIMIT_NPROC, RLIMIT_NOFILE, RLIMIT_MEMLOCK, RLIMIT_AS};
		int resource = (int)(number == __NR_getrlimit ? a : b);
		uint64_t *old = GUEST(uint64_t *, number == __NR_getrlimit ? b : d);
		const uint64_t *newlim = number == __NR_getrlimit ? NULL : GUEST(const uint64_t *, c);
		struct rlimit lim;
		if (number == __NR_prlimit64 && a && a != getpid())
			return -3;
		if (resource < 0 || (size_t)resource >= sizeof(resources) / sizeof(resources[0]))
			return -22;
		if (old)
		{
			if (getrlimit(resources[resource], &lim))
				return -linux_errno(errno);
			if (number == __NR_getrlimit)
			{
				uint32_t *old32 = (uint32_t *)old;
				old32[0] = lim.rlim_cur >= UINT32_MAX ? UINT32_MAX : (uint32_t)lim.rlim_cur;
				old32[1] = lim.rlim_max >= UINT32_MAX ? UINT32_MAX : (uint32_t)lim.rlim_max;
			}
			else
			{
				old[0] = lim.rlim_cur == RLIM_INFINITY ? UINT64_MAX : lim.rlim_cur;
				old[1] = lim.rlim_max == RLIM_INFINITY ? UINT64_MAX : lim.rlim_max;
			}
		}
		if (newlim)
		{
			lim.rlim_cur = newlim[0] == UINT64_MAX ? RLIM_INFINITY : newlim[0];
			lim.rlim_max = newlim[1] == UINT64_MAX ? RLIM_INFINITY : newlim[1];
			if (setrlimit(resources[resource], &lim))
				return -linux_errno(errno);
		}
		return 0;
	}
	case __NR_umask:
		return umask(a);
	case __NR_fcntl:
		return guest_fcntl(a, b, c);
	case __NR_renameat2:
		if (e)
			return -38;
		return result(renameat(a, GUEST(const char *, b), c, GUEST(const char *, d)));
	/* These are already rejected by the Android dispatcher. */
	case __NR_statx:
	case __NR_statfs:
	case __NR_fstatfs:
	case __NR_clone:
	case __NR_clone3:
	case __NR_execve:
	case __NR_rt_sigtimedwait:
	case __NR_pselect6:
	case __NR_epoll_pwait:
	case __NR_sysinfo:
	case __NR_sendmsg:
	case __NR_recvmsg:
	case __NR_timer_create:
	case __NR_timer_settime:
	case __NR_timer_gettime:
	case __NR_setitimer:
	case __NR_getitimer:
	case __NR_times:
	case __NR_getrusage:
	case __NR_wait4:
	case __NR_waitid:
		return -38;
	/* Linux signal numbers and thread IDs do not identify SDK threads. */
	case __NR_kill:
	case __NR_tkill:
	case __NR_tgkill:
		return -38;
	/* There is no SDK process-wide Linux membarrier operation. */
	case __NR_membarrier:
		return -38;
	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -38;
	}
}

struct syscall_record
{
	struct syscall_record *next;
	uintptr_t thread, caller;
	long long number, a, b, c;
	uint64_t frames[8];
	unsigned wake_count;
	long long last_wake_result;
	struct timespec entered;
	int active;
};
static pthread_mutex_t watchdog_lock = PTHREAD_MUTEX_INITIALIZER;
static struct syscall_record *watchdog_records;
static __thread struct syscall_record *this_record;
static unsigned watchdog_seconds;
static pthread_t watchdog_main_thread;
static volatile sig_atomic_t watchdog_sample_ready;
static volatile uint64_t watchdog_rip, watchdog_rbp, watchdog_frames[8];

static void watchdog_signal(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uintptr_t frame = uc->uc_mcontext.mc_rbp, sp = uc->uc_mcontext.mc_rsp;
	unsigned i;
	(void)sig;
	(void)info;
	watchdog_rip = uc->uc_mcontext.mc_rip;
	watchdog_rbp = frame;
	for (i = 0; i < 8; i++)
		watchdog_frames[i] = 0;
	for (i = 0; i < 8 && frame >= sp && frame < 0x100000000ULL && !(frame & 7) && frame - sp < 16 * 1024 * 1024; i++)
	{
		const uint64_t *pair = (const uint64_t *)frame;
		uint64_t next = pair[0];
		watchdog_frames[i] = pair[1];
		if (next <= frame)
			break;
		frame = (uintptr_t)next;
	}
	__atomic_store_n(&watchdog_sample_ready, 1, __ATOMIC_RELEASE);
}
void host_syscall_watchdog_main_thread(void)
{
	if (watchdog_seconds)
		__atomic_store_n(&watchdog_main_thread, pthread_self(), __ATOMIC_RELEASE);
}

static void *watchdog_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct syscall_record *record;
		struct timespec now;
		sleep(watchdog_seconds);
		pthread_t target = __atomic_load_n(&watchdog_main_thread, __ATOMIC_ACQUIRE);
		if (target)
		{
			unsigned sample;
			for (sample = 0; sample < 5; sample++)
			{
				unsigned i;
				__atomic_store_n(&watchdog_sample_ready, 0, __ATOMIC_RELAXED);
				pthread_kill(target, SIGUSR1);
				sleep(1);
				if (!__atomic_load_n(&watchdog_sample_ready, __ATOMIC_ACQUIRE))
					continue;
				host_logf(HOST_LOG_INFO, "opence: watchdog main rip=%llx rbp=%llx %s", (unsigned long long)watchdog_rip,
						  (unsigned long long)watchdog_rbp,
						  watchdog_rip >= host_image.base && watchdog_rip < host_image.end ? "guest" : "host");
				for (i = 0; i < 8 && watchdog_frames[i]; i++)
					host_logf(HOST_LOG_INFO, "opence: watchdog main frame %u=%llx", i,
							  (unsigned long long)watchdog_frames[i]);
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		pthread_mutex_lock(&watchdog_lock);
		for (record = watchdog_records; record; record = record->next)
		{
			double elapsed;
			if (!record->active)
				continue;
			elapsed = (double)(now.tv_sec - record->entered.tv_sec) +
					  (double)(now.tv_nsec - record->entered.tv_nsec) / 1000000000.0;
			host_logf(HOST_LOG_INFO,
					  "opence: watchdog thread=%llx syscall=%lld%s a=%llx b=%llx c=%llx caller=%llx elapsed=%.3fs",
					  (unsigned long long)record->thread, record->number, record->number == __NR_futex ? " futex" : "",
					  (unsigned long long)record->a, (unsigned long long)record->b, (unsigned long long)record->c,
					  (unsigned long long)record->caller, elapsed);
			if (record->number == __NR_futex && elapsed >= 1.0)
			{
				host_logf(HOST_LOG_INFO, "opence: watchdog futex addr=%llx wakes=%u last_result=%lld",
						  (unsigned long long)record->a, record->wake_count, record->last_wake_result);
				unsigned i;
				for (i = 0; i < 8 && record->frames[i]; i++)
					host_logf(HOST_LOG_INFO, "opence: watchdog frame thread=%llx %u=%llx",
							  (unsigned long long)record->thread, i, (unsigned long long)record->frames[i]);
			}
		}
		pthread_mutex_unlock(&watchdog_lock);
	}
	return NULL;
}
void host_syscall_watchdog_start(unsigned seconds)
{
	pthread_t thread;
	if (!seconds)
		return;
	{
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_sigaction = watchdog_signal;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGUSR1, &action, NULL))
			host_fatal("cannot install watchdog signal");
	}
	watchdog_seconds = seconds;
	trace_map_io = 1;
	if (pthread_create(&thread, NULL, watchdog_thread, NULL))
		host_fatal("cannot start syscall watchdog");
	pthread_detach(thread);
}
long long host_syscall(long long number, long long a, long long b, long long c, long long d, long long e, long long f)
{
	long long result;
	struct syscall_record *record;
	if (!watchdog_seconds)
		return dispatch_syscall(number, a, b, c, d, e, f);
	record = this_record;
	if (!record)
	{
		record = calloc(1, sizeof(*record));
		if (!record)
			host_fatal("cannot allocate syscall watchdog record");
		record->thread = (uintptr_t)pthread_self();
		pthread_mutex_lock(&watchdog_lock);
		record->next = watchdog_records;
		watchdog_records = record;
		pthread_mutex_unlock(&watchdog_lock);
		this_record = record;
	}
	pthread_mutex_lock(&watchdog_lock);
	record->number = number;
	record->a = a;
	record->b = b;
	record->c = c;
	record->wake_count = 0;
	record->last_wake_result = 0;
	record->caller = (uintptr_t)__builtin_return_address(0);
	memset(record->frames, 0, sizeof(record->frames));
	if (number == __NR_futex && ((b & 0x7f) == 0))
	{
		uintptr_t frame = (uintptr_t)__builtin_frame_address(0), initial = frame;
		unsigned i;
		for (i = 0; i < 8 && frame && frame < 0x100000000ULL && !(frame & 7) && frame >= initial &&
					frame - initial < 16 * 1024 * 1024;
			 i++)
		{
			const uint64_t *pair = (const uint64_t *)frame;
			uint64_t next = pair[0];
			record->frames[i] = pair[1];
			if (next <= frame)
				break;
			frame = (uintptr_t)next;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &record->entered);
	record->active = 1;
	pthread_mutex_unlock(&watchdog_lock);
	result = dispatch_syscall(number, a, b, c, d, e, f);
	pthread_mutex_lock(&watchdog_lock);
	if (number == __NR_futex && ((b & 0x7f) == 1 || (b & 0x7f) == 3 || (b & 0x7f) == 4))
	{
		struct syscall_record *waiter;
		for (waiter = watchdog_records; waiter; waiter = waiter->next)
			if (waiter != record && waiter->active && waiter->number == __NR_futex && ((waiter->b & 0x7f) == 0) &&
				(uint32_t)waiter->a == (uint32_t)a)
			{
				waiter->wake_count++;
				waiter->last_wake_result = result;
			}
	}
	record->active = 0;
	pthread_mutex_unlock(&watchdog_lock);
	return result;
}
