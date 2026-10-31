/* M2 has no display, input or audio backend. */
#include "../../android/guest/runtime/guest_host.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static void copy_text(char *out, unsigned int size, const char *text)
{
	if (size) { size_t n = strlen(text); if (n >= size) n = size - 1; memcpy(out, text, n); out[n] = 0; }
}
int host_sdl_init(unsigned int flags) { (void)flags; return 1; }
int host_sdl_set_hint(const char *name, const char *value) { (void)name; (void)value; return 0; }
void host_sdl_get_error(char *buffer, unsigned int size) { copy_text(buffer, size, "no window until M3"); }
long long host_sdl_ticks(void) { struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0; return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
long long host_sdl_thread_id(void) { return (long long)(uintptr_t)pthread_self(); }
unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags) { (void)title; (void)width; (void)height; (void)flags; return 0; }
void host_sdl_window_size_in_pixels(unsigned int window, int *width, int *height) { (void)window; if (width) *width = 0; if (height) *height = 0; }
int host_sdl_set_relative_mouse(unsigned int window, int enabled) { (void)window; (void)enabled; return 0; }
int host_sdl_gl_set_attribute(int attribute, int value) { (void)attribute; (void)value; return 0; }
unsigned int host_sdl_gl_create_context(unsigned int window) { (void)window; return 0; }
int host_sdl_gl_make_current(unsigned int window, unsigned int context) { (void)window; (void)context; return 0; }
int host_sdl_gl_set_swap_interval(int interval) { (void)interval; return 0; }
int host_sdl_gl_swap_window(unsigned int window) { (void)window; return 0; }
int host_sdl_poll_event(void *event) { (void)event; return 0; }
int host_sdl_text_input(unsigned int window, int type) { (void)window; (void)type; return 0; }
int host_sdl_set_clipboard_text(const char *text) { (void)text; return 0; }
void host_sdl_get_clipboard_text(char *buffer, unsigned int size) { copy_text(buffer, size, ""); }
int host_sdl_open_url(const char *url) { (void)url; return 0; }
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size) { (void)scancode; copy_text(buffer, size, ""); }
int host_sdl_scancode_from_name(const char *name) { (void)name; return 0; }
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y) { (void)message; (void)duration; (void)gravity; (void)x; (void)y; return 0; }
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message) { (void)flags; (void)title; (void)message; return 0; }
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int default_index, const char *button0, const char *button1, const char *button2) { (void)flags; (void)title; (void)message; (void)default_index; (void)button0; (void)button1; (void)button2; return -1; }
int host_sdl_get_gamepads(unsigned int *ids, int capacity) { (void)ids; (void)capacity; return 0; }
unsigned int host_sdl_open_gamepad(unsigned int id) { (void)id; return 0; }
unsigned int host_sdl_gamepad_from_id(unsigned int id) { (void)id; return 0; }
int host_sdl_gamepad_axis(unsigned int gamepad, int axis) { (void)gamepad; (void)axis; return 0; }
int host_sdl_gamepad_button(unsigned int gamepad, int button) { (void)gamepad; (void)button; return 0; }
int host_sdl_gamepad_type(unsigned int gamepad) { (void)gamepad; return 0; }
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds) { (void)gamepad; (void)low; (void)high; (void)milliseconds; return 0; }
void host_sdl_close_gamepad(unsigned int gamepad) { (void)gamepad; }
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback, unsigned int userdata) { (void)device; (void)spec; (void)callback; (void)userdata; return 0; }
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length) { (void)stream; (void)data; (void)length; return 0; }
int host_sdl_resume_audio_stream_device(unsigned int stream) { (void)stream; return 0; }
int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length) { (void)stream; (void)data; (void)length; return -1; }
int host_sdl_get_audio_stream_available(unsigned int stream) { (void)stream; return -1; }
void host_sdl_destroy_audio_stream(unsigned int stream) { (void)stream; }
