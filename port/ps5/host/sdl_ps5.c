/* PS5 SDL bridge: EGL/Zink presentation, with input and audio still absent. */
#include "../../android/guest/runtime/guest_host.h"
#include "host.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

int sceSystemServiceHideSplashScreen(void);
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLConfig config;
static uint32_t window_size[2] = {3840, 2160};
static int context_major = 3, context_minor = 2;
static int window_created, splash_hidden;
static uint64_t window_start, first_swap, previous_swap, interval_sum, interval_max;
static unsigned int interval_count, summaries;
static char error_text[160];

static uint64_t nanoseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static int egl_error(const char *operation)
{
	snprintf(error_text, sizeof(error_text), "%s: EGL error %#x", operation, eglGetError());
	host_logf(HOST_LOG_ERROR, "opence: %s", error_text);
	return 0;
}

static void copy_text(char *out, unsigned int size, const char *text)
{
	if (size)
	{
		size_t n = strlen(text);
		if (n >= size)
			n = size - 1;
		memcpy(out, text, n);
		out[n] = 0;
	}
}
int host_sdl_init(unsigned int flags)
{
	(void)flags;
	return 1;
}
int host_sdl_set_hint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return 0;
}
void host_sdl_get_error(char *buffer, unsigned int size)
{
	copy_text(buffer, size, error_text);
}
long long host_sdl_ticks(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
long long host_sdl_thread_id(void)
{
	return (long long)(uintptr_t)pthread_self();
}
unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags)
{
	static const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE
	};
	const char *resolution = getenv("HALO_PS5_RESOLUTION");
	EGLint major, minor, count;
	uint64_t start = nanoseconds();
	window_start = start;
	(void)title;
	(void)flags;
	if (window_created)
	{
		snprintf(error_text, sizeof(error_text), "only one PS5 window per run");
		return 0;
	}
	if ((width == 3840 && height == 2160) || (width == 2560 && height == 1440) ||
		(width == 1920 && height == 1080))
	{
		window_size[0] = width;
		window_size[1] = height;
	}
	if (resolution && *resolution)
	{
		unsigned int w, h;
		char extra;
		if (sscanf(resolution, "%ux%u%c", &w, &h, &extra) != 2 ||
			!((w == 3840 && h == 2160) || (w == 2560 && h == 1440) || (w == 1920 && h == 1080)))
		{
			snprintf(error_text, sizeof(error_text), "invalid HALO_PS5_RESOLUTION: %s", resolution);
			return 0;
		}
		window_size[0] = w;
		window_size[1] = h;
	}
	window_created = 1;
	display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
		return egl_error("eglInitialize");
	host_logf(HOST_LOG_INFO, "opence: EGL %d.%d vendor %s version %s", major, minor,
		eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_VERSION));
	if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, attributes, &config, 1, &count) || !count)
		return egl_error("eglChooseConfig");
	surface = eglCreatePlatformWindowSurface(display, config, window_size, NULL);
	if (surface == EGL_NO_SURFACE)
		return egl_error("eglCreatePlatformWindowSurface");
	host_logf(HOST_LOG_INFO, "opence: window %ux%u created in %.1f ms", window_size[0], window_size[1],
		(nanoseconds() - start) / 1e6);
	return 1;
}
void host_sdl_window_size_in_pixels(unsigned int window, int *width, int *height)
{
	if (width)
		*width = window == 1 ? (int)window_size[0] : 0;
	if (height)
		*height = window == 1 ? (int)window_size[1] : 0;
}
int host_sdl_set_relative_mouse(unsigned int window, int enabled)
{
	(void)window;
	(void)enabled;
	return 0;
}
int host_sdl_gl_set_attribute(int attribute, int value)
{
	/* SDL_GLAttr values for the version requested by the guest. */
	if (attribute == 17)
		context_major = value;
	else if (attribute == 18)
		context_minor = value;
	return 1;
}
unsigned int host_sdl_gl_create_context(unsigned int window)
{
	EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, context_major,
		EGL_CONTEXT_MINOR_VERSION, context_minor, EGL_NONE};
	uint64_t start = nanoseconds();
	if (window != 1 || surface == EGL_NO_SURFACE)
		return 0;
	egl_context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
	if (egl_context == EGL_NO_CONTEXT)
		return egl_error("eglCreateContext");
	if (!eglMakeCurrent(display, surface, surface, egl_context))
		return egl_error("eglMakeCurrent");
	{
		PFNGLGETSTRINGPROC get_string = (PFNGLGETSTRINGPROC)eglGetProcAddress("glGetString");
		if (get_string)
			host_logf(HOST_LOG_INFO, "opence: GL %s renderer %s", get_string(GL_VERSION), get_string(GL_RENDERER));
	}
	host_logf(HOST_LOG_INFO, "opence: context current in %.1f ms", (nanoseconds() - start) / 1e6);
	return 1;
}
int host_sdl_gl_make_current(unsigned int window, unsigned int context)
{
	if (window != 1 || context != 1)
		return 0;
	return eglMakeCurrent(display, surface, surface, egl_context);
}
int host_sdl_gl_set_swap_interval(int interval)
{
	if (!eglSwapInterval(display, interval))
		return egl_error("eglSwapInterval");
	return 1;
}
int host_sdl_gl_swap_window(unsigned int window)
{
	uint64_t now, delta;
	if (window != 1 || !eglSwapBuffers(display, surface))
		return egl_error("eglSwapBuffers");
	now = nanoseconds();
	if (!splash_hidden)
	{
		splash_hidden = 1;
		sceSystemServiceHideSplashScreen();
		host_logf(HOST_LOG_INFO, "opence: first frame presented %.1f ms after window creation began",
			(now - window_start) / 1e6);
		first_swap = now;
	}
	if (previous_swap)
	{
		delta = now - previous_swap;
		interval_sum += delta;
		if (delta > interval_max)
			interval_max = delta;
		interval_count++;
	}
	previous_swap = now;
	if (now - first_swap >= 10000000000ull)
	{
		host_logf(HOST_LOG_INFO, "opence: swap intervals %s frames=%u mean=%.3f ms max=%.3f ms",
			summaries++ ? "after first 10s" : "first 10s",
			interval_count, interval_count ? interval_sum / (double)interval_count / 1e6 : 0.0,
			interval_max / 1e6);
		first_swap = now;
		interval_count = 0;
		interval_sum = interval_max = 0;
	}
	return 1;
}
int host_sdl_poll_event(void *event)
{
	(void)event;
	return 0;
}
int host_sdl_text_input(unsigned int window, int type)
{
	(void)window;
	(void)type;
	return 0;
}
int host_sdl_set_clipboard_text(const char *text)
{
	(void)text;
	return 0;
}
void host_sdl_get_clipboard_text(char *buffer, unsigned int size)
{
	copy_text(buffer, size, "");
}
int host_sdl_open_url(const char *url)
{
	(void)url;
	return 0;
}
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size)
{
	(void)scancode;
	copy_text(buffer, size, "");
}
int host_sdl_scancode_from_name(const char *name)
{
	(void)name;
	return 0;
}
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)message;
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	return 0;
}
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message)
{
	(void)flags;
	(void)title;
	(void)message;
	return 0;
}
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int default_index,
							  const char *button0, const char *button1, const char *button2)
{
	(void)flags;
	(void)title;
	(void)message;
	(void)default_index;
	(void)button0;
	(void)button1;
	(void)button2;
	return -1;
}
int host_sdl_get_gamepads(unsigned int *ids, int capacity)
{
	(void)ids;
	(void)capacity;
	return 0;
}
unsigned int host_sdl_open_gamepad(unsigned int id)
{
	(void)id;
	return 0;
}
unsigned int host_sdl_gamepad_from_id(unsigned int id)
{
	(void)id;
	return 0;
}
int host_sdl_gamepad_axis(unsigned int gamepad, int axis)
{
	(void)gamepad;
	(void)axis;
	return 0;
}
int host_sdl_gamepad_button(unsigned int gamepad, int button)
{
	(void)gamepad;
	(void)button;
	return 0;
}
int host_sdl_gamepad_type(unsigned int gamepad)
{
	(void)gamepad;
	return 0;
}
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds)
{
	(void)gamepad;
	(void)low;
	(void)high;
	(void)milliseconds;
	return 0;
}
void host_sdl_close_gamepad(unsigned int gamepad)
{
	(void)gamepad;
}
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback,
										unsigned int userdata)
{
	(void)device;
	(void)spec;
	(void)callback;
	(void)userdata;
	return 0;
}
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length)
{
	(void)stream;
	(void)data;
	(void)length;
	return 0;
}
int host_sdl_resume_audio_stream_device(unsigned int stream)
{
	(void)stream;
	return 0;
}
int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length)
{
	(void)stream;
	(void)data;
	(void)length;
	return -1;
}
int host_sdl_get_audio_stream_available(unsigned int stream)
{
	(void)stream;
	return -1;
}
void host_sdl_destroy_audio_stream(unsigned int stream)
{
	(void)stream;
}
