/* OpenCE's S7 and S8 probes (docs/ps5_port_plan.md, "M0. Feasibility
probes on the console"): sockets and /data/opence from a PS5 title.

S8, the data folder (D7):
  1. /data/opence is a directory the title sees (ShadowMountPlus);
  2. a 4 MiB file is written in 1 MiB writes, synced, renamed over an
     existing file (as port/linux/src/profile_json.c replaces its files),
     read back and compared, then removed with its folder;
  3. the player's disc image there (the first *.iso) is read in place: its
     XDVDFS volume, the maps folder's table, and each map's 2 KB header (a
     cache file's: 'daeh' first, 'toof' last). RANDOM_MAP takes 256 random
     64 KiB reads, then WHOLE_MAP is read whole in 1 MiB reads; both are
     timed, and WHOLE_MAP's CRC-32 is given to compare with the same file
     read out of the image on another computer.

S7, sockets (D8), with tools/ps5_probe.py's s7-peer running on a Linux
machine that reaches the console (PS5_HOST):
  1. a UDP socket binds port 2302, the game's;
  2. the peer's datagram arrives on 2302, the title answers its sender from
     2302, and the peer confirms the answer;
  3. with SO_BROADCAST it sends to 255.255.255.255:2302 and receives its own
     broadcast;
  4. a TCP socket listens on 2302; the peer connects, its line is echoed
     and the peer confirms the echo;
  5. libSceNet's resolver (sceNetResolverStartNtoa, as Forever's titles
     use it) resolves a name, and a TCP connection is made to its port 443;
     libc's getaddrinfo, which the link binds to the platform layer's
     refusal, is only logged;
  6. a DNS query sent over UDP to a public resolver is answered: UDP to the
     internet (DTLS's handshake itself waits for Mbed TLS, M8).

Every line goes to standard output (the klog) and to opence_s78.txt in the
title's folder. The last line counts the failures. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceSystemServiceHideSplashScreen(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverDestroy(int resolver);
int sceNetResolverStartNtoa(int resolver, const char *host, uint32_t *address, int timeout, int retry, int flags);
int *sceNetErrnoLoc(void);

#define LOG_PATH "/app0/opence_s78.txt"
#define DATA_ROOT "/data/opence"
#define PROBE_FOLDER DATA_ROOT "/s8-probe"
#define WRITE_BYTES (4u << 20)
#define BLOCK (1u << 20)
#define SECTOR 2048u
#define TABLE_LIMIT (4u << 20)
#define ENTRY_LIMIT 4096
#define WHOLE_MAP "a10.map"
#define RANDOM_MAP "d40.map"
#define RANDOM_READS 256
#define RANDOM_BYTES (64u << 10)
#define GAME_PORT 2302
#define WAIT_MS 3000
#define PEER_WAIT_MS 20000

static FILE *log_file;
static int failures;

static void say(const char *format, ...)
{
	char line[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	printf("opence S78: %s\n", line);
	fflush(stdout);
	if (log_file)
	{
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}
}

static void verdict(const char *probe, int passed, const char *what)
{
	say("%s %s: %s", probe, passed ? "PASS" : "FAIL", what);
	if (!passed)
		failures++;
}

static uint64_t ticks(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* ---------- CRC-32 (IEEE, reflected: zlib's crc32) */

static uint32_t crc_table[256];

static void crc_start(void)
{
	uint32_t index, bit, value;

	for (index = 0; index < 256; index++)
	{
		value = index;
		for (bit = 0; bit < 8; bit++)
			value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
		crc_table[index] = value;
	}
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size)
{
	crc = ~crc;
	while (size--)
		crc = crc_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* ---------- S8: writing */

static uint8_t pattern_byte(uint32_t offset)
{
	return (uint8_t)((offset * 2654435761u) >> 24);
}

static int write_whole(const char *path, const uint8_t *data, uint32_t size, uint64_t *nanoseconds)
{
	uint64_t start = ticks();
	int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	uint32_t done = 0;

	if (file < 0)
	{
		say("S8 open %s for writing: errno %d", path, errno);
		return 0;
	}
	while (done < size)
	{
		uint32_t chunk = size - done < BLOCK ? size - done : BLOCK;
		ssize_t written = write(file, data + done, chunk);

		if (written <= 0)
		{
			say("S8 write %s at %u: errno %d", path, done, errno);
			close(file);
			return 0;
		}
		done += (uint32_t)written;
	}
	if (fsync(file) != 0)
		say("S8 fsync %s: errno %d", path, errno);
	close(file);
	if (nanoseconds)
		*nanoseconds = ticks() - start;
	return 1;
}

static void s8_write(void)
{
	static const char old_path[] = PROBE_FOLDER "/settings.bin";
	static const char new_path[] = PROBE_FOLDER "/settings.bin.new";
	uint8_t *data = malloc(WRITE_BYTES), *back = malloc(WRITE_BYTES);
	struct stat status;
	uint64_t nanoseconds = 0;
	uint32_t index;
	int ok = 0, file;
	ssize_t got = 0;

	if (!data || !back)
	{
		verdict("S8", 0, "memory for the write test");
		return;
	}
	for (index = 0; index < WRITE_BYTES; index++)
		data[index] = pattern_byte(index);
	if (mkdir(PROBE_FOLDER, 0777) != 0 && errno != EEXIST)
		say("S8 mkdir %s: errno %d", PROBE_FOLDER, errno);
	if (write_whole(old_path, (const uint8_t *)"an older file\n", 14, NULL)
		&& write_whole(new_path, data, WRITE_BYTES, &nanoseconds))
	{
		say("S8 4 MiB written and synced in %.1f ms (%.1f MiB/s)", nanoseconds / 1e6,
			4.0 / (nanoseconds / 1e9));
		if (rename(new_path, old_path) != 0)
			say("S8 rename over the older file: errno %d", errno);
		else if (stat(old_path, &status) != 0 || status.st_size != WRITE_BYTES)
			say("S8 stat after rename: errno %d, size %lld", errno, (long long)status.st_size);
		else if ((file = open(old_path, O_RDONLY)) < 0)
			say("S8 open to read back: errno %d", errno);
		else
		{
			got = pread(file, back, WRITE_BYTES, 0);
			close(file);
			ok = got == (ssize_t)WRITE_BYTES && !memcmp(data, back, WRITE_BYTES);
			say("S8 read back %zd bytes, mode %o, %s", got, (unsigned)(status.st_mode & 07777),
				ok ? "identical" : "different");
		}
	}
	unlink(new_path);
	if (unlink(old_path) != 0)
		say("S8 unlink: errno %d", errno);
	if (rmdir(PROBE_FOLDER) != 0)
		say("S8 rmdir: errno %d", errno);
	verdict("S8", ok, "a file written, synced, renamed over another, read back identical, removed");
	free(data);
	free(back);
}

/* ---------- S8: the disc image, read in place */

struct xdvdfs_file
{
	char name[256];
	uint32_t sector;
	uint32_t size;
	uint8_t attributes;
};

struct disc
{
	int file;
	uint64_t partition;
};

/* the entries of one directory table (at most ENTRY_LIMIT), each offset
visited once */
static int xdvdfs_table(const struct disc *disc, uint32_t sector, uint32_t size, struct xdvdfs_file *entries)
{
	uint8_t *table, *visited;
	uint32_t pending[ENTRY_LIMIT], pending_count = 1;
	int count = 0;

	if (size == 0 || size > TABLE_LIMIT)
		return -1;
	table = malloc(size);
	visited = calloc(size / 4 + 1, 1);
	if (!table || !visited
		|| pread(disc->file, table, size, (off_t)(disc->partition + (uint64_t)sector * SECTOR)) != (ssize_t)size)
	{
		free(table);
		free(visited);
		return -1;
	}
	pending[0] = 0;
	while (pending_count && count < ENTRY_LIMIT)
	{
		uint32_t offset = pending[--pending_count];
		uint16_t left, right;
		uint8_t name_length;

		if (offset + 14 > size || visited[offset / 4])
			continue;
		visited[offset / 4] = 1;
		memcpy(&left, table + offset, 2);
		memcpy(&right, table + offset + 2, 2);
		if (left == 0xFFFF)
			continue;
		name_length = table[offset + 13];
		if (offset + 14 + name_length > size)
			continue;
		memcpy(&entries[count].sector, table + offset + 4, 4);
		memcpy(&entries[count].size, table + offset + 8, 4);
		entries[count].attributes = table[offset + 12];
		memcpy(entries[count].name, table + offset + 14, name_length);
		entries[count].name[name_length] = 0;
		count++;
		if (left && pending_count < ENTRY_LIMIT)
			pending[pending_count++] = (uint32_t)left * 4;
		if (right && right != 0xFFFF && pending_count < ENTRY_LIMIT)
			pending[pending_count++] = (uint32_t)right * 4;
	}
	free(table);
	free(visited);
	return count;
}

static int find_image(char *path, size_t size)
{
	DIR *folder = opendir(DATA_ROOT);
	struct dirent *entry;
	int found = 0;

	if (!folder)
	{
		say("S8 opendir %s: errno %d", DATA_ROOT, errno);
		return 0;
	}
	while (!found && (entry = readdir(folder)))
	{
		size_t length = strlen(entry->d_name);

		say("S8 %s/%s", DATA_ROOT, entry->d_name);
		if (length > 4 && !strcasecmp(entry->d_name + length - 4, ".iso"))
		{
			snprintf(path, size, "%s/%s", DATA_ROOT, entry->d_name);
			found = 1;
		}
	}
	closedir(folder);
	return found;
}

static void read_whole_map(const struct disc *disc, const struct xdvdfs_file *map)
{
	uint8_t *buffer = malloc(BLOCK);
	uint64_t base = disc->partition + (uint64_t)map->sector * SECTOR, read_time = 0, slowest = 0;
	uint32_t done = 0, crc = 0;

	if (!buffer)
		return;
	while (done < map->size)
	{
		uint32_t chunk = map->size - done < BLOCK ? map->size - done : BLOCK;
		uint64_t start = ticks(), elapsed;
		ssize_t got = pread(disc->file, buffer, chunk, (off_t)(base + done));

		elapsed = ticks() - start;
		read_time += elapsed;
		if (elapsed > slowest)
			slowest = elapsed;
		if (got != (ssize_t)chunk)
		{
			say("S8 %s: read at %u returned %zd, errno %d", map->name, done, got, errno);
			break;
		}
		crc = crc_update(crc, buffer, chunk);
		done += chunk;
	}
	say("S8 %s read whole: %u bytes in %.0f ms of reads (%.1f MiB/s), slowest 1 MiB read %.1f ms, CRC-32 %08x",
		map->name, done, read_time / 1e6, done / 1048576.0 / (read_time / 1e9), slowest / 1e6, crc);
	verdict("S8", done == map->size, "a map read whole out of the disc image in place");
	free(buffer);
}

static void read_random(const struct disc *disc, const struct xdvdfs_file *map)
{
	uint8_t *buffer = malloc(RANDOM_BYTES);
	uint64_t base = disc->partition + (uint64_t)map->sector * SECTOR, total = 0, slowest = 0;
	uint32_t blocks = map->size / RANDOM_BYTES, seed = 0x5EED1234u, index;
	int ok = 1;

	if (!buffer || !blocks)
		return;
	for (index = 0; index < RANDOM_READS; index++)
	{
		uint64_t start, elapsed;
		uint32_t block;

		seed = seed * 1664525u + 1013904223u;
		block = seed % blocks;
		start = ticks();
		ok &= pread(disc->file, buffer, RANDOM_BYTES, (off_t)(base + (uint64_t)block * RANDOM_BYTES))
			== (ssize_t)RANDOM_BYTES;
		elapsed = ticks() - start;
		total += elapsed;
		if (elapsed > slowest)
			slowest = elapsed;
	}
	say("S8 %s: %d random 64 KiB reads, mean %.2f ms, slowest %.2f ms (%.1f MiB/s)", map->name, RANDOM_READS,
		total / 1e6 / RANDOM_READS, slowest / 1e6, RANDOM_READS * 64.0 / 1024.0 / (total / 1e9));
	verdict("S8", ok, "random 64 KiB reads of a map in the disc image");
	free(buffer);
}

static void s8_image(void)
{
	static const uint64_t partitions[] = {0, 0x18300000ull, 0x30600000ull, 0x2080000ull, 0xFD90000ull};
	struct xdvdfs_file *root = calloc(ENTRY_LIMIT, sizeof *root), *maps = calloc(ENTRY_LIMIT, sizeof *maps);
	const struct xdvdfs_file *whole = NULL, *random_map = NULL;
	char path[512], magic[20];
	struct disc disc = {-1, 0};
	struct stat status;
	uint32_t root_sector = 0, root_size = 0, index, map_count = 0, headers_ok = 0;
	uint64_t map_bytes = 0;
	int root_count, maps_count = -1, found = 0, partition;

	if (!root || !maps || !find_image(path, sizeof path))
	{
		verdict("S8", 0, "a disc image (*.iso) in " DATA_ROOT);
		goto done;
	}
	disc.file = open(path, O_RDONLY);
	if (disc.file < 0 || fstat(disc.file, &status) != 0)
	{
		say("S8 open %s: errno %d", path, errno);
		verdict("S8", 0, "the disc image opened for reading");
		goto done;
	}
	say("S8 disc image %s: %lld bytes", path, (long long)status.st_size);
	for (partition = 0; partition < (int)(sizeof partitions / sizeof *partitions) && !found; partition++)
	{
		disc.partition = partitions[partition];
		found = pread(disc.file, magic, sizeof magic, (off_t)(disc.partition + 0x10000)) == (ssize_t)sizeof magic
			&& !memcmp(magic, "MICROSOFT*XBOX*MEDIA", sizeof magic)
			&& pread(disc.file, &root_sector, 4, (off_t)(disc.partition + 0x10014)) == 4
			&& pread(disc.file, &root_size, 4, (off_t)(disc.partition + 0x10018)) == 4;
	}
	verdict("S8", found, "the disc image's XDVDFS volume descriptor");
	if (!found)
		goto done;
	root_count = xdvdfs_table(&disc, root_sector, root_size, root);
	say("S8 partition at %#llx, root table sector %u, %u bytes, %d entries", (unsigned long long)disc.partition,
		root_sector, root_size, root_count);
	for (index = 0; root_count > 0 && index < (uint32_t)root_count; index++)
		if (!strcasecmp(root[index].name, "maps") && (root[index].attributes & 0x10))
			maps_count = xdvdfs_table(&disc, root[index].sector, root[index].size, maps);
	for (index = 0; maps_count > 0 && index < (uint32_t)maps_count; index++)
	{
		uint8_t header[SECTOR];

		if (maps[index].attributes & 0x10)
			continue;
		map_count++;
		map_bytes += maps[index].size;
		if (pread(disc.file, header, SECTOR, (off_t)(disc.partition + (uint64_t)maps[index].sector * SECTOR))
				== (ssize_t)SECTOR
			&& !memcmp(header, "daeh", 4) && !memcmp(header + SECTOR - 4, "toof", 4))
			headers_ok++;
		if (!strcasecmp(maps[index].name, WHOLE_MAP))
			whole = &maps[index];
		if (!strcasecmp(maps[index].name, RANDOM_MAP))
			random_map = &maps[index];
	}
	say("S8 maps folder: %u files, %llu bytes, %u with a cache file header", map_count,
		(unsigned long long)map_bytes, headers_ok);
	verdict("S8", map_count > 0 && headers_ok == map_count, "every file of the maps folder has a cache header");
	if (random_map)
		read_random(&disc, random_map);
	if (whole)
		read_whole_map(&disc, whole);
	verdict("S8", whole && random_map, "the maps " WHOLE_MAP " and " RANDOM_MAP " found in the image");
done:
	if (disc.file >= 0)
		close(disc.file);
	free(root);
	free(maps);
}

static void s8(void)
{
	struct stat status;
	int seen = stat(DATA_ROOT, &status) == 0 && S_ISDIR(status.st_mode);

	say("S8 stat %s: %s (errno %d), mode %o", DATA_ROOT, seen ? "a directory" : "not seen", seen ? 0 : errno,
		seen ? (unsigned)(status.st_mode & 07777) : 0u);
	verdict("S8", seen, DATA_ROOT " is a directory the title sees");
	if (!seen)
		return;
	s8_write();
	s8_image();
}

/* ---------- S7 */

static int wait_readable(int socket_fd, int milliseconds)
{
	struct pollfd poller = {socket_fd, POLLIN, 0};

	return poll(&poller, 1, milliseconds) == 1 && (poller.revents & POLLIN);
}

/* a datagram whose text begins with `expected`, within `milliseconds` */
static int receive_text(int socket_fd, const char *expected, struct sockaddr_in *from, int milliseconds)
{
	uint64_t deadline = ticks() + (uint64_t)milliseconds * 1000000ull;
	char text[512];

	while (ticks() < deadline)
	{
		socklen_t from_size = sizeof *from;
		ssize_t got;

		if (!wait_readable(socket_fd, (int)((deadline - ticks()) / 1000000ull) + 1))
			break;
		got = recvfrom(socket_fd, text, sizeof text - 1, 0, (struct sockaddr *)from, &from_size);
		if (got <= 0)
			continue;
		text[got] = 0;
		if (!strncmp(text, expected, strlen(expected)))
			return 1;
		say("S7 other datagram from %s:%u: %.60s", inet_ntoa(from->sin_addr), ntohs(from->sin_port), text);
	}
	return 0;
}

static int tcp_connect(const struct sockaddr *address, socklen_t size, int milliseconds)
{
	int socket_fd = socket(address->sa_family, SOCK_STREAM, 0), flags, error = 0;
	socklen_t error_size = sizeof error;
	struct pollfd poller;

	if (socket_fd < 0)
		return -1;
	flags = fcntl(socket_fd, F_GETFL, 0);
	fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
	if (connect(socket_fd, address, size) != 0 && errno != EINPROGRESS)
	{
		say("S7 connect: errno %d", errno);
		close(socket_fd);
		return -1;
	}
	poller.fd = socket_fd;
	poller.events = POLLOUT;
	poller.revents = 0;
	if (poll(&poller, 1, milliseconds) != 1
		|| getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error)
	{
		say("S7 connect did not complete: socket error %d", error);
		close(socket_fd);
		return -1;
	}
	fcntl(socket_fd, F_SETFL, flags);
	return socket_fd;
}

/* the peer's datagram on 2302, the answer to its sender, the peer's
confirmation; then the same over a TCP connection the peer opens to 2302 */
static void s7_peer(int game_socket)
{
	static const char pong[] = "opence-s7 pong";
	struct sockaddr_in from, local;
	char line[64], confirm[64];
	int heard, answered = 0, listener, listening, connection = -1, confirmed = 0, yes = 1;
	ssize_t got = 0;
	uint64_t start = ticks();

	heard = receive_text(game_socket, "opence-s7 hello", &from, PEER_WAIT_MS);
	if (heard)
	{
		say("S7 hello from %s:%u after %.1f s", inet_ntoa(from.sin_addr), ntohs(from.sin_port), (ticks() - start) / 1e9);
		answered = sendto(game_socket, pong, sizeof pong - 1, 0, (struct sockaddr *)&from, sizeof from)
				== (ssize_t)(sizeof pong - 1)
			&& receive_text(game_socket, "opence-s7 confirmed", &from, WAIT_MS);
	}
	verdict("S7", heard, "a datagram from the Linux peer received on port 2302");
	verdict("S7", answered, "the title's answer from port 2302 reached the peer, which confirmed it");

	listener = socket(AF_INET, SOCK_STREAM, 0);
	memset(&local, 0, sizeof local);
	local.sin_family = AF_INET;
	local.sin_port = htons(GAME_PORT);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	listening = listener >= 0 && bind(listener, (struct sockaddr *)&local, sizeof local) == 0 && listen(listener, 4) == 0;
	if (!listening)
		say("S7 TCP listen on 2302: errno %d", errno);
	verdict("S7", listening, "a TCP socket listening on port 2302");
	start = ticks();
	if (listening && wait_readable(listener, PEER_WAIT_MS))
		connection = accept(listener, NULL, NULL);
	if (connection >= 0)
	{
		memset(line, 0, sizeof line);
		memset(confirm, 0, sizeof confirm);
		if (wait_readable(connection, WAIT_MS))
			got = recv(connection, line, sizeof line - 1, 0);
		if (got > 0 && send(connection, line, (size_t)got, 0) == got && wait_readable(connection, WAIT_MS))
			confirmed = recv(connection, confirm, sizeof confirm - 1, 0) > 0 && !strncmp(confirm, "confirmed", 9);
		say("S7 TCP connection accepted after %.1f s: line %zd bytes, echo %s", (ticks() - start) / 1e9, got,
			confirmed ? "confirmed" : "not confirmed");
		close(connection);
	}
	if (listener >= 0)
		close(listener);
	verdict("S7", confirmed, "a TCP connection from the Linux peer accepted, its line echoed, the echo confirmed");
}

static void s7_broadcast(int game_socket)
{
	struct sockaddr_in everyone, from;
	char text[64];
	int yes = 1, set, sent, received;

	snprintf(text, sizeof text, "opence-s7 broadcast %llu", (unsigned long long)ticks());
	set = setsockopt(game_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes) == 0;
	memset(&everyone, 0, sizeof everyone);
	everyone.sin_family = AF_INET;
	everyone.sin_port = htons(GAME_PORT);
	everyone.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sent = sendto(game_socket, text, strlen(text), 0, (struct sockaddr *)&everyone, sizeof everyone)
		== (ssize_t)strlen(text);
	if (!sent)
		say("S7 broadcast sendto: errno %d", errno);
	received = sent && receive_text(game_socket, text, &from, WAIT_MS);
	say("S7 SO_BROADCAST %s; broadcast sent %d; own broadcast received %s", set ? "set" : "refused", sent,
		received ? inet_ntoa(from.sin_addr) : "no");
	verdict("S7", set && sent, "a broadcast to 255.255.255.255:2302 sent");
	verdict("S7", received, "the title's own broadcast received on port 2302");
}

static void s7_internet(void)
{
	static const uint8_t query[] = {
		0x4F, 0x43, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		6, 'g', 'i', 't', 'h', 'u', 'b', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01};
	struct addrinfo hints, *found = NULL;
	struct sockaddr_in resolver, server;
	uint8_t answer[512];
	uint32_t address = 0;
	int result, tcp, udp, answered = 0, pool, lookup = -1, attempt;
	uint64_t start = ticks();

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	result = getaddrinfo("github.com", "443", &hints, &found);
	say("S7 getaddrinfo github.com (the platform layer's): %d (%.1f ms)", result, (ticks() - start) / 1e6);
	if (found)
		freeaddrinfo(found);

	start = ticks();
	pool = sceNetPoolCreate("opence_s7", 0x4000, 0);
	if (pool >= 0)
	{
		lookup = sceNetResolverCreate("opence_s7", pool, 0);
		if (lookup >= 0)
		{
			result = sceNetResolverStartNtoa(lookup, "github.com", &address, 0, 0, 0);
			sceNetResolverDestroy(lookup);
		}
		sceNetPoolDestroy(pool);
	}
	memset(&server, 0, sizeof server);
	server.sin_family = AF_INET;
	server.sin_port = htons(443);
	server.sin_addr.s_addr = address;
	say("S7 sceNetResolverStartNtoa github.com: pool %#x, resolver %#x, result %#x, net errno %d, %s (%.1f ms)",
		(unsigned)pool, (unsigned)lookup, (unsigned)result, *sceNetErrnoLoc(), inet_ntoa(server.sin_addr),
		(ticks() - start) / 1e6);
	verdict("S7", pool >= 0 && lookup >= 0 && result >= 0 && address, "a name resolved with libSceNet's resolver");
	if (address)
	{
		start = ticks();
		tcp = tcp_connect((struct sockaddr *)&server, sizeof server, 5000);
		say("S7 TCP to github.com:443: %s (%.1f ms)", tcp >= 0 ? "connected" : "failed", (ticks() - start) / 1e6);
		verdict("S7", tcp >= 0, "a TCP connection to the internet");
		if (tcp >= 0)
			close(tcp);
	}

	/* two public resolvers, two tries each: a datagram can be lost */
	udp = socket(AF_INET, SOCK_DGRAM, 0);
	for (attempt = 0; udp >= 0 && attempt < 4 && !answered; attempt++)
	{
		const char *server_name = attempt & 1 ? "8.8.8.8" : "1.1.1.1";
		ssize_t sent, got = 0;

		memset(&resolver, 0, sizeof resolver);
		resolver.sin_family = AF_INET;
		resolver.sin_port = htons(53);
		inet_pton(AF_INET, server_name, &resolver.sin_addr);
		start = ticks();
		sent = sendto(udp, query, sizeof query, 0, (struct sockaddr *)&resolver, sizeof resolver);
		if (sent == (ssize_t)sizeof query && wait_readable(udp, WAIT_MS))
			got = recv(udp, answer, sizeof answer, 0);
		answered = got > 12 && answer[0] == query[0] && answer[1] == query[1] && (answer[2] & 0x80)
			&& (answer[6] << 8 | answer[7]) > 0;
		say("S7 DNS over UDP to %s, try %d: sent %zd (errno %d), received %zd bytes, %d answer(s) (%.1f ms)",
			server_name, attempt, sent, sent < 0 ? errno : 0, got, got > 12 ? answer[6] << 8 | answer[7] : 0,
			(ticks() - start) / 1e6);
	}
	if (udp >= 0)
		close(udp);
	verdict("S7", answered, "a UDP query to the internet answered");
}

static void s7(void)
{
	struct sockaddr_in local;
	int game_socket = socket(AF_INET, SOCK_DGRAM, 0), yes = 1, bound;

	memset(&local, 0, sizeof local);
	local.sin_family = AF_INET;
	local.sin_port = htons(GAME_PORT);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	setsockopt(game_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	bound = game_socket >= 0 && bind(game_socket, (struct sockaddr *)&local, sizeof local) == 0;
	if (!bound)
		say("S7 socket %d, bind: errno %d", game_socket, errno);
	verdict("S7", bound, "a UDP socket bound to port 2302");
	if (bound)
	{
		s7_peer(game_socket);
		s7_broadcast(game_socket);
	}
	if (game_socket >= 0)
		close(game_socket);
	s7_internet();
}

int main(void)
{
	log_file = fopen(LOG_PATH, "w");
	say("OpenCE PS5 S7/S8 probe: sockets and " DATA_ROOT " (docs/ps5_port_plan.md), log %s", LOG_PATH);
	sceSystemServiceHideSplashScreen();
	crc_start();
	s8();
	s7();
	say("done: %d failed check(s)", failures);
	if (log_file)
		fclose(log_file);
	sceSystemServiceLoadExec("exit", NULL);
	for (;;)
		usleep(100000);
	return 0;
}
