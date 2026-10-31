/* Linux x86-64 host entry point for the x32 guest image. */

#include "host.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern char **environ;
void host_install_signal_handlers(void);
/* the guest image linked into this executable (guest_image.S) */
extern const unsigned char halo_guest_image[], halo_guest_image_end[];

void host_logf(int priority, const char *format, ...)
{
	va_list args;

	(void)priority;
	va_start(args, format);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

void host_log(int priority, const char *text)
{
	host_logf(priority, "%s", text);
}

void host_fatal(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
	exit(1);
}

void host_abort(const char *reason)
{
	host_logf(HOST_LOG_ERROR, "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

struct startup
{
	const char *image;
	int argc;
	char **argv;
};

static uint32_t make_boot(int argc, char **argv)
{
	size_t size = sizeof(struct halo_guest_boot) + (argc + 1) * sizeof(uint32_t);
	char **entry;
	int envc = 0, i;
	char *memory, *strings;
	uint32_t *guest_argv, *guest_env;
	struct halo_guest_boot *boot;

	/* The guest's pointers, argument vectors and strings must all be low. */
	for (entry = environ; *entry; entry++)
	{
		size += strlen(*entry) + 1;
		envc++;
	}
	for (i = 0; i < argc; i++)
		size += strlen(argv[i]) + 1;
	size += (envc + 1) * sizeof(uint32_t);
	memory = host_low_map(size, PROT_READ | PROT_WRITE);
	if (!memory)
		host_fatal("cannot allocate guest arguments and environment");
	boot = (struct halo_guest_boot *)memory;
	guest_argv = (uint32_t *)(boot + 1);
	guest_env = guest_argv + argc + 1;
	strings = (char *)(guest_env + envc + 1);
	for (i = 0; i < argc; i++)
	{
		guest_argv[i] = (uint32_t)(uintptr_t)strings;
		strcpy(strings, argv[i]);
		strings += strlen(strings) + 1;
	}
	guest_argv[argc] = 0;
	for (i = 0; i < envc; i++)
	{
		guest_env[i] = (uint32_t)(uintptr_t)strings;
		strcpy(strings, environ[i]);
		strings += strlen(strings) + 1;
	}
	guest_env[envc] = 0;
	boot->argc = argc;
	boot->argv = (uint32_t)(uintptr_t)guest_argv;
	boot->environment = (uint32_t)(uintptr_t)guest_env;
	boot->page_size = (uint32_t)getpagesize();
	return (uint32_t)(uintptr_t)boot;
}

static void *game_main(void *argument)
{
	struct startup *start = argument;

	host_run_guest_main(make_boot(start->argc, start->argv));
}

int main(int argc, char **argv)
{
	struct startup start = { NULL, argc, argv };
	const char *image_override = getenv("HALO_GUEST_IMAGE");
	const void *image;
	void *loaded = NULL;
	size_t image_size;
	int error;

	/* the image linked in, unless --image or HALO_GUEST_IMAGE names
	another (a guest rebuilt without relinking the host) */
	if (argc >= 3 && !strcmp(argv[1], "--image"))
	{
		start.image = argv[2];
		argv[2] = argv[0];
		start.argv = argv + 2;
		start.argc = argc - 2;
	}
	else if (image_override && *image_override)
		start.image = image_override;
	host_memory_reserve_early();
	if (host_memory_fixed_unavailable())
		host_fatal("fixed guest address ranges are unavailable");
	if (start.image)
	{
		loaded = SDL_LoadFile(start.image, &image_size);
		if (!loaded)
			host_fatal("cannot read guest image %s: %s", start.image, SDL_GetError());
		image = loaded;
	}
	else
	{
		start.image = "(linked in)";
		image = halo_guest_image;
		image_size = (size_t)(halo_guest_image_end - halo_guest_image);
	}
	if (host_load_image(image, image_size))
		host_fatal("cannot load guest image %s", start.image);
	SDL_free(loaded);
	host_install_signal_handlers();
	error = host_native_thread_create(game_main, &start, 16 * 1024 * 1024);
	if (error)
		host_fatal("cannot start guest thread: %s", strerror(error));
	for (;;)
		pause();
}
