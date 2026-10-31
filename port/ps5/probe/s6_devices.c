/* OpenCE's S6 probe (docs/ps5_port_plan.md, "M0. Feasibility probes on the
console", and D6): the PS5's input devices and audio output, from a title
whose frames Zink presents (s5_zink.c built with PROBE_S6):

  - the signed-in user's DualSense through scePad, every record drained
    with scePadRead, oldest first;
  - USB keyboards and mice through sceKeyboard and sceMouse, indexes 0 and
    1 of each;
  - a 440 Hz tone at -12 dBFS through one SceAudioOut main port, 48 kHz
    stereo S16 in 256-frame periods, fed by one thread that sceAudioOutOutput
    paces.

The functions, their arguments and the records' layouts are those
SavageWindRecomp's runtime/ps5/input.c and audio.c use on the same SDK and
console (no SDK header declares them); the code here is the probe's own.

Checked without a person: every library starts and every device opens, no
read or output fails, and the outputs keep pace with 48 kHz. A person at the
console adds what only they can: buttons, sticks, triggers, keys and clicks
are logged as they happen, and the tone is heard. */

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "s6_devices.h"

/* ---------- the system's functions */

int32_t sceUserServiceInitialize(const void *parameters);
int32_t sceUserServiceGetInitialUser(int32_t *user);
int32_t scePadInit(void);
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void *parameters);
int32_t scePadClose(int32_t handle);
int32_t scePadRead(int32_t handle, void *records, int32_t capacity);
int32_t sceSysmoduleLoadModule(uint16_t id);
int32_t sceKeyboardInit(void);
int32_t sceKeyboardOpen(int32_t user, int32_t type, int32_t index, const void *parameters);
int32_t sceKeyboardClose(int32_t handle);
int32_t sceKeyboardRead(int32_t handle, void *records, int32_t capacity);
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t user, int32_t type, int32_t index, const void *parameters);
int32_t sceMouseClose(int32_t handle);
int32_t sceMouseRead(int32_t handle, void *records, int32_t capacity);
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user, int32_t port_type, int32_t index, uint32_t frames, uint32_t rate,
	uint32_t format);
int32_t sceAudioOutOutput(int32_t port, const void *samples);
int32_t sceAudioOutClose(int32_t port);

#define SYSMODULE_KEYBOARD 0x0106
#define SYSMODULE_MOUSE 0x00a9
#define AUDIOOUT_ALREADY_INITIALIZED 0x8026000eu
#define AUDIOOUT_USER_SYSTEM 0xff
#define AUDIOOUT_PORT_MAIN 0
#define AUDIOOUT_S16_STEREO 1

#define AUDIO_RATE 48000
#define AUDIO_PERIOD 256
#define TONE_HZ 440.0
#define TONE_LEVEL 0.25 /* -12 dBFS */
#define PAD_RECORDS 64
#define KEYBOARD_RECORDS 16
#define MOUSE_RECORDS 64
#define HID_INDEXES 2
#define EVENT_LINES 400

/* scePadRead's 120-byte record */
struct pad_record
{
	uint32_t buttons;
	uint8_t left_x, left_y, right_x, right_y; /* 0..255, 128 centred, y down */
	uint8_t left_trigger, right_trigger, reserved0[2];
	float orientation[4];
	float acceleration[3];
	float angular_velocity[3];
	uint8_t touch[24];
	int32_t connected;
	uint64_t timestamp_us;
	uint8_t extension[16];
	uint8_t connection_count;
	uint8_t reserved1[2];
	uint8_t unique_length;
	uint8_t unique[12];
};
_Static_assert(sizeof(struct pad_record) == 120, "scePadRead's record");
_Static_assert(offsetof(struct pad_record, connected) == 0x4c, "scePadRead's connected field");

/* sceKeyboardRead's 96-byte record */
struct keyboard_record
{
	uint64_t timestamp_us;
	uint8_t intercepted, reserved0[7];
	uint8_t connected, reserved1[3];
	int32_t length;
	uint32_t leds;
	uint32_t modifiers;
	uint16_t keys[16]; /* USB HID usages held */
	uint8_t reserved2[32];
};
_Static_assert(sizeof(struct keyboard_record) == 96, "sceKeyboardRead's record");

/* sceMouseRead's 40-byte record */
struct mouse_record
{
	uint64_t timestamp_us;
	uint8_t connected, reserved0[3];
	uint32_t buttons;
	int32_t x, y, wheel, tilt;
	uint8_t reserved1[8];
};
_Static_assert(sizeof(struct mouse_record) == 40, "sceMouseRead's record");

static const struct
{
	uint32_t bit;
	const char *name;
} pad_buttons[] = {
	{1u << 0, "Create"}, {1u << 1, "L3"}, {1u << 2, "R3"}, {1u << 3, "Options"}, {1u << 4, "Up"},
	{1u << 5, "Right"}, {1u << 6, "Down"}, {1u << 7, "Left"}, {1u << 8, "L2"}, {1u << 9, "R2"},
	{1u << 10, "L1"}, {1u << 11, "R1"}, {1u << 12, "Triangle"}, {1u << 13, "Circle"}, {1u << 14, "Cross"},
	{1u << 15, "Square"}, {1u << 20, "Touchpad"}, {1u << 31, "(intercepted by the system)"},
};

enum { KEYBOARD, MOUSE, HID_KINDS };

static struct
{
	s6_log_function log;
	int event_lines;
	int32_t user;
	int32_t pad;
	int32_t pad_init, user_result, keyboard_module, keyboard_init, mouse_module, mouse_init;
	uint32_t pad_buttons;
	int pad_zones[4];
	bool pad_connected_seen;
	uint64_t pad_reads, pad_records, pad_errors, pad_events;
	int32_t hid[HID_KINDS][HID_INDEXES];
	uint64_t hid_reads[HID_KINDS], hid_errors[HID_KINDS], hid_events[HID_KINDS];
	bool hid_connected_seen[HID_KINDS];
	uint16_t keys_held[HID_INDEXES][16];
	uint32_t mouse_buttons[HID_INDEXES];
	int64_t mouse_motion[2];

	int32_t audio_init, audio_port;
	pthread_t audio_thread;
	bool audio_started;
	volatile bool audio_running;
	uint64_t audio_outputs, audio_errors, audio_late, audio_start, audio_end;
	double audio_slowest_ms;
} s6 = {.user = -1, .pad = -1, .audio_port = -1};

static uint64_t ticks(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static bool event_line(void)
{
	return s6.event_lines++ < EVENT_LINES;
}

/* ---------- the tone */

static void *audio_main(void *unused)
{
	static int16_t samples[AUDIO_PERIOD * 2];
	double phase = 0.0, step = 2.0 * M_PI * TONE_HZ / AUDIO_RATE;
	uint64_t previous = 0;
	int frame;

	(void)unused;
	s6.audio_start = ticks();
	while (s6.audio_running)
	{
		uint64_t now;
		int32_t result;

		for (frame = 0; frame < AUDIO_PERIOD; frame++)
		{
			int16_t value = (int16_t)lrint(sin(phase) * TONE_LEVEL * 32767.0);

			samples[frame * 2] = samples[frame * 2 + 1] = value;
			phase += step;
			if (phase >= 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		result = sceAudioOutOutput(s6.audio_port, samples);
		now = ticks();
		if (result < 0)
			s6.audio_errors++;
		else
			s6.audio_outputs++;
		/* AudioOut takes the next period when the last one starts playing:
		two periods' time between returns means the port ran dry */
		if (previous)
		{
			double milliseconds = (now - previous) / 1e6;

			if (milliseconds > s6.audio_slowest_ms)
				s6.audio_slowest_ms = milliseconds;
			if (milliseconds > 2.0 * 1000.0 * AUDIO_PERIOD / AUDIO_RATE)
				s6.audio_late++;
		}
		previous = now;
	}
	s6.audio_end = ticks();
	return NULL;
}

static void audio_start(void)
{
	s6.audio_init = sceAudioOutInit();
	if (s6.audio_init < 0 && (uint32_t)s6.audio_init != AUDIOOUT_ALREADY_INITIALIZED)
		return;
	s6.audio_port = sceAudioOutOpen(AUDIOOUT_USER_SYSTEM, AUDIOOUT_PORT_MAIN, 0, AUDIO_PERIOD, AUDIO_RATE,
		AUDIOOUT_S16_STEREO);
	if (s6.audio_port < 0)
		return;
	s6.audio_running = true;
	s6.audio_started = pthread_create(&s6.audio_thread, NULL, audio_main, NULL) == 0;
	if (!s6.audio_started)
		s6.audio_running = false;
}

/* ---------- the pad */

static int stick_zone(uint8_t value)
{
	return ((int)value - 128) / 52;
}

static void pad_record(const struct pad_record *record)
{
	static const char *const sticks[4] = {"left x", "left y", "right x", "right y"};
	uint8_t axes[4];
	uint32_t changed;
	size_t index;

	if (!record->connected)
		return;
	s6.pad_connected_seen = true;
	changed = record->buttons ^ s6.pad_buttons;
	for (index = 0; changed && index < sizeof pad_buttons / sizeof *pad_buttons; index++)
		if (changed & pad_buttons[index].bit)
		{
			s6.pad_events++;
			if (event_line())
				s6.log("S6 pad: %s %s", pad_buttons[index].name,
					record->buttons & pad_buttons[index].bit ? "pressed" : "released");
		}
	s6.pad_buttons = record->buttons;
	axes[0] = record->left_x;
	axes[1] = record->left_y;
	axes[2] = record->right_x;
	axes[3] = record->right_y;
	for (index = 0; index < 4; index++)
	{
		int zone = stick_zone(axes[index]);

		if (zone != s6.pad_zones[index])
		{
			s6.pad_zones[index] = zone;
			s6.pad_events++;
			if (event_line())
				s6.log("S6 pad: %s stick %u (zone %d), triggers %u %u", sticks[index], axes[index], zone,
					record->left_trigger, record->right_trigger);
		}
	}
}

static void pad_start(void)
{
	s6.user_result = sceUserServiceInitialize(NULL); /* (an error if already started; the service works) */
	s6.user_result = sceUserServiceGetInitialUser(&s6.user);
	s6.pad_init = scePadInit();
	if (s6.user >= 0)
		s6.pad = scePadOpen(s6.user, 0, 0, NULL);
}

static void pad_read(void)
{
	static struct pad_record records[PAD_RECORDS];
	int32_t count, index;

	if (s6.pad < 0)
		return;
	count = scePadRead(s6.pad, records, PAD_RECORDS);
	s6.pad_reads++;
	if (count < 0)
	{
		if (s6.pad_errors++ < 4)
			s6.log("S6 scePadRead: %#x", (unsigned)count);
		return;
	}
	s6.pad_records += (uint64_t)count;
	for (index = 0; index < count; index++)
		pad_record(&records[index]);
}

/* ---------- keyboards and mice */

static void hid_start(void)
{
	static const uint8_t parameters[8];
	int index;

	s6.keyboard_module = sceSysmoduleLoadModule(SYSMODULE_KEYBOARD);
	s6.keyboard_init = sceKeyboardInit();
	s6.mouse_module = sceSysmoduleLoadModule(SYSMODULE_MOUSE);
	s6.mouse_init = sceMouseInit();
	for (index = 0; index < HID_INDEXES; index++)
	{
		s6.hid[KEYBOARD][index] = s6.user >= 0 && s6.keyboard_init >= 0
			? sceKeyboardOpen(s6.user, 0, index, parameters) : -1;
		s6.hid[MOUSE][index] = s6.user >= 0 && s6.mouse_init >= 0
			? sceMouseOpen(s6.user, 0, index, parameters) : -1;
	}
}

static bool key_in(const uint16_t *keys, uint16_t key)
{
	int index;

	for (index = 0; index < 16; index++)
		if (keys[index] == key)
			return true;
	return false;
}

static void keyboard_read(int index)
{
	static struct keyboard_record records[KEYBOARD_RECORDS];
	int32_t count = sceKeyboardRead(s6.hid[KEYBOARD][index], records, KEYBOARD_RECORDS), record, key;

	s6.hid_reads[KEYBOARD]++;
	if (count < 0)
	{
		if (s6.hid_errors[KEYBOARD]++ < 4)
			s6.log("S6 sceKeyboardRead(index %d): %#x", index, (unsigned)count);
		return;
	}
	for (record = 0; record < count; record++)
	{
		if (!records[record].connected)
			continue;
		s6.hid_connected_seen[KEYBOARD] = true;
		for (key = 0; key < 16; key++)
		{
			uint16_t usage = records[record].keys[key];

			if (usage > 3 && !key_in(s6.keys_held[index], usage))
			{
				s6.hid_events[KEYBOARD]++;
				if (event_line())
					s6.log("S6 keyboard %d: usage %#x pressed (modifiers %#x)", index, usage,
						records[record].modifiers);
			}
		}
		memcpy(s6.keys_held[index], records[record].keys, sizeof s6.keys_held[index]);
	}
}

static void mouse_read(int index)
{
	static struct mouse_record records[MOUSE_RECORDS];
	int32_t count = sceMouseRead(s6.hid[MOUSE][index], records, MOUSE_RECORDS), record;

	s6.hid_reads[MOUSE]++;
	if (count < 0)
	{
		if (s6.hid_errors[MOUSE]++ < 4)
			s6.log("S6 sceMouseRead(index %d): %#x", index, (unsigned)count);
		return;
	}
	for (record = 0; record < count; record++)
	{
		if (!records[record].connected)
			continue;
		s6.hid_connected_seen[MOUSE] = true;
		s6.mouse_motion[0] += records[record].x;
		s6.mouse_motion[1] += records[record].y;
		if (records[record].buttons != s6.mouse_buttons[index])
		{
			s6.hid_events[MOUSE]++;
			if (event_line())
				s6.log("S6 mouse %d: buttons %#x, motion so far %lld %lld", index, records[record].buttons,
					(long long)s6.mouse_motion[0], (long long)s6.mouse_motion[1]);
			s6.mouse_buttons[index] = records[record].buttons;
		}
	}
}

/* ---------- the probe's calls */

void s6_start(s6_log_function log)
{
	s6.log = log;
	pad_start();
	hid_start();
	audio_start();
	log("S6 user service %#x, initial user %d; scePadInit %#x, scePadOpen %#x", (unsigned)s6.user_result, s6.user,
		(unsigned)s6.pad_init, (unsigned)s6.pad);
	log("S6 keyboard module %#x, init %#x, opened %#x %#x; mouse module %#x, init %#x, opened %#x %#x",
		(unsigned)s6.keyboard_module, (unsigned)s6.keyboard_init, (unsigned)s6.hid[KEYBOARD][0],
		(unsigned)s6.hid[KEYBOARD][1], (unsigned)s6.mouse_module, (unsigned)s6.mouse_init,
		(unsigned)s6.hid[MOUSE][0], (unsigned)s6.hid[MOUSE][1]);
	log("S6 sceAudioOutInit %#x, sceAudioOutOpen %#x, tone thread %s; a 440 Hz tone should be heard",
		(unsigned)s6.audio_init, (unsigned)s6.audio_port, s6.audio_started ? "started" : "not started");
}

void s6_frame(void)
{
	int index;

	pad_read();
	for (index = 0; index < HID_INDEXES; index++)
	{
		if (s6.hid[KEYBOARD][index] >= 0)
			keyboard_read(index);
		if (s6.hid[MOUSE][index] >= 0)
			mouse_read(index);
	}
}

static int check(bool passed, const char *what)
{
	s6.log("S6 %s: %s", passed ? "PASS" : "FAIL", what);
	return passed ? 0 : 1;
}

int s6_finish(double seconds)
{
	double audio_seconds, expected;
	int failed = 0, index;
	bool opened = true;

	if (s6.audio_started)
	{
		s6.audio_running = false;
		pthread_join(s6.audio_thread, NULL);
	}
	if (s6.audio_port >= 0)
		sceAudioOutClose(s6.audio_port);
	if (s6.pad >= 0)
		scePadClose(s6.pad);
	for (index = 0; index < HID_INDEXES; index++)
	{
		if (s6.hid[KEYBOARD][index] >= 0)
			sceKeyboardClose(s6.hid[KEYBOARD][index]);
		if (s6.hid[MOUSE][index] >= 0)
			sceMouseClose(s6.hid[MOUSE][index]);
		opened = opened && s6.hid[KEYBOARD][index] >= 0 && s6.hid[MOUSE][index] >= 0;
	}
	audio_seconds = s6.audio_end > s6.audio_start ? (s6.audio_end - s6.audio_start) / 1e9 : 0.0;
	expected = audio_seconds * AUDIO_RATE / AUDIO_PERIOD;
	s6.log("S6 over %.1f s: pad %llu reads, %llu records, %llu errors, connected %s, %llu events", seconds,
		(unsigned long long)s6.pad_reads, (unsigned long long)s6.pad_records, (unsigned long long)s6.pad_errors,
		s6.pad_connected_seen ? "yes" : "never", (unsigned long long)s6.pad_events);
	s6.log("S6 keyboards %llu reads, %llu errors, connected %s, %llu keys; mice %llu reads, %llu errors, "
		"connected %s, %llu button changes, motion %lld %lld",
		(unsigned long long)s6.hid_reads[KEYBOARD], (unsigned long long)s6.hid_errors[KEYBOARD],
		s6.hid_connected_seen[KEYBOARD] ? "yes" : "never", (unsigned long long)s6.hid_events[KEYBOARD],
		(unsigned long long)s6.hid_reads[MOUSE], (unsigned long long)s6.hid_errors[MOUSE],
		s6.hid_connected_seen[MOUSE] ? "yes" : "never", (unsigned long long)s6.hid_events[MOUSE],
		(long long)s6.mouse_motion[0], (long long)s6.mouse_motion[1]);
	s6.log("S6 audio over %.2f s: %llu periods output (%.0f at 48 kHz), %llu errors, %llu late returns, "
		"slowest %.2f ms", audio_seconds, (unsigned long long)s6.audio_outputs, expected,
		(unsigned long long)s6.audio_errors, (unsigned long long)s6.audio_late, s6.audio_slowest_ms);
	failed += check(s6.user >= 0 && s6.pad_init >= 0 && s6.pad >= 0, "the signed-in user's pad opened");
	failed += check(s6.pad_reads > 0 && s6.pad_errors == 0, "every scePadRead succeeded");
	failed += check(s6.keyboard_init >= 0 && s6.mouse_init >= 0 && opened,
		"keyboard and mouse libraries started, indexes 0 and 1 of each opened");
	failed += check(s6.hid_errors[KEYBOARD] == 0 && s6.hid_errors[MOUSE] == 0,
		"every keyboard and mouse read succeeded");
	failed += check(s6.audio_port >= 0 && s6.audio_started && s6.audio_errors == 0, "the tone output with no error");
	failed += check(expected > 0 && s6.audio_outputs >= expected * 0.98 && s6.audio_outputs <= expected * 1.02
			&& s6.audio_late == 0,
		"AudioOut took the tone at 48 kHz, with no late period");
	s6.log("S6 hands-on (a person at the console): pad events %llu, keys %llu, mouse button changes %llu; "
		"the tone heard: ask", (unsigned long long)s6.pad_events, (unsigned long long)s6.hid_events[KEYBOARD],
		(unsigned long long)s6.hid_events[MOUSE]);
	return failed;
}
