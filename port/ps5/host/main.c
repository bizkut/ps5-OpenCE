/* PS5 title entry and guest bootstrap. */
#include "host.h"
#include <ps5platform/klog.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef OPENCE_BUILD_VERSION
#define OPENCE_BUILD_VERSION "unknown"
#endif
#ifndef OPENCE_IMAGE_HASH
#define OPENCE_IMAGE_HASH "unknown"
#endif

int sceSystemServiceLoadExec(const char *, const char *const *);
extern char **environ;
extern const unsigned char halo_guest_image[], halo_guest_image_end[];
void host_syscall_watchdog_start(unsigned seconds);
void host_syscall_watchdog_main_thread(void);

static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void host_logf(int priority, const char *format, ...)
{
	char line[4096];
	va_list args;
	(void)priority;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	pthread_mutex_lock(&log_lock);
	strncat(line, "\n", sizeof(line) - strlen(line) - 1);
	sceKernelDebugOutText(0, line);
	if (log_file)
	{
		fputs(line, log_file);
		fflush(log_file);
	}
	pthread_mutex_unlock(&log_lock);
}
void host_log(int priority, const char *text)
{
	host_logf(priority, "%s", text);
}
void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "opence: LoadExec exit (%d)", code);
	sceSystemServiceLoadExec("exit", NULL);
	for (;;)
		usleep(100000);
}
void host_fatal(const char *format, ...)
{
	char line[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	host_logf(HOST_LOG_ERROR, "opence: fatal: %s", line);
	host_exit(1);
}
void host_abort(const char *reason)
{
	host_fatal("guest abort: %s", reason);
}
int host_errno(void)
{
	return errno;
}
void host_debug_thread_started(void)
{
}
void host_debug_thread_exited(void)
{
}
void host_debug_start_sampler(const char *setting)
{
	(void)setting;
}

static int guest_argc = 1;
static char *guest_argv[128] = {"opence"};
static char args_line[4096];

static void read_environment(void)
{
	char line[8192];
	FILE *file;
	setenv("HALO_DATA_ROOT", "/data/opence", 1);
	setenv("HALO_SAVE_ROOT", "/data/opence/save", 1);
	setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
	setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
	setenv("HALO_PS5_TOUCHPAD_TALK", "1", 1);
	file = fopen("/app0/opence-env.txt", "r");
	if (file)
	{
		while (fgets(line, sizeof(line), file))
		{
			char *equals, *name = line;
			line[strcspn(line, "\r\n")] = 0;
			if (!*name || *name == '#')
				continue;
			equals = strchr(name, '=');
			if (!equals || equals == name)
				continue;
			*equals++ = 0;
			if (!strcmp(name, "ARGS"))
			{
				char *save, *word;
				snprintf(args_line, sizeof(args_line), "%s", equals);
				guest_argc = 1;
				for (word = strtok_r(args_line, " \t", &save); word && guest_argc < 127;
					 word = strtok_r(NULL, " \t", &save))
					guest_argv[guest_argc++] = word;
			}
			else
				setenv(name, equals, 1);
		}
		fclose(file);
	}
	setenv("HALO_WINDOW_FLOOR", "0x80120000", 1);
}

static uint32_t make_boot(void)
{
	size_t size = sizeof(struct halo_guest_boot) + (guest_argc + 1) * sizeof(uint32_t);
	char **entry, *memory, *strings;
	uint32_t *argv, *env;
	struct halo_guest_boot *boot;
	int envc = 0, i;
	for (entry = environ; *entry; entry++)
	{
		size += strlen(*entry) + 1;
		envc++;
	}
	for (i = 0; i < guest_argc; i++)
		size += strlen(guest_argv[i]) + 1;
	size += (envc + 1) * sizeof(uint32_t);
	memory = host_low_map(size, PROT_READ | PROT_WRITE);
	if (!memory)
		host_fatal("cannot allocate guest arguments and environment");
	boot = (struct halo_guest_boot *)memory;
	argv = (uint32_t *)(boot + 1);
	env = argv + guest_argc + 1;
	strings = (char *)(env + envc + 1);
	for (i = 0; i < guest_argc; i++)
	{
		argv[i] = (uint32_t)(uintptr_t)strings;
		strcpy(strings, guest_argv[i]);
		strings += strlen(strings) + 1;
	}
	argv[guest_argc] = 0;
	for (i = 0; i < envc; i++)
	{
		env[i] = (uint32_t)(uintptr_t)strings;
		strcpy(strings, environ[i]);
		strings += strlen(strings) + 1;
	}
	env[envc] = 0;
	boot->argc = guest_argc;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)env;
	boot->page_size = 0x4000;
	return (uint32_t)(uintptr_t)boot;
}
static void *game_main(void *unused)
{
	(void)unused;
	if (getenv("OPENCE_HOST_WATCHDOG"))
		host_logf(HOST_LOG_INFO, "opence: guest main thread=%llx", (unsigned long long)(uintptr_t)pthread_self());
	host_syscall_watchdog_main_thread();
	host_run_guest_main(make_boot());
}
int main(void)
{
	char first[512], path[4096];
	int error;
	snprintf(first, sizeof(first), "opence: PPSA99117 %s image %s\n", OPENCE_BUILD_VERSION, OPENCE_IMAGE_HASH);
	sceKernelDebugOutText(0, first);
	ps5_klog_capture_stderr("opence: ");
	read_environment();
	{
		const char *watchdog = getenv("OPENCE_HOST_WATCHDOG");
		if (watchdog && *watchdog)
			host_syscall_watchdog_start((unsigned)strtoul(watchdog, NULL, 10));
	}
	mkdir(getenv("HALO_DATA_ROOT"), 0777);
	mkdir(getenv("HALO_SAVE_ROOT"), 0777);
	snprintf(path, sizeof(path), "%s/opence.log", getenv("HALO_DATA_ROOT"));
	log_file = fopen(path, "a");
	if (log_file)
	{
		fputs(first, log_file);
		fflush(log_file);
	}
	host_memory_reserve_early();
	if (host_memory_fixed_unavailable())
		host_fatal("fixed guest address ranges are unavailable");
	if (host_load_image(halo_guest_image, (size_t)(halo_guest_image_end - halo_guest_image)))
		host_fatal("cannot load guest image");
	error = host_native_thread_create(game_main, NULL, 16 * 1024 * 1024);
	if (error)
		host_fatal("cannot start guest thread: %s", strerror(error));
	for (;;)
		usleep(100000);
}
