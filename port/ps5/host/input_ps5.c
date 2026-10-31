/* PS5 device declarations and record layouts derived from SavageWindRecomp
 * runtime/ps5/input.c, Copyright the Savage Wind Recomp contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "host.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t sceUserServiceInitialize(const void *);
int32_t sceUserServiceGetInitialUser(int32_t *);
struct login_user_list
{
	int32_t users[4];
};
int32_t sceUserServiceGetLoginUserIdList(struct login_user_list *);
struct user_event
{
	int32_t type, user;
};
int32_t sceUserServiceGetEvent(struct user_event *);
int32_t scePadInit(void);
int32_t scePadOpen(int32_t, int32_t, int32_t, const void *);
int32_t scePadClose(int32_t);
int32_t scePadRead(int32_t, void *, int32_t);
struct vibration
{
	uint8_t large, small;
};
int32_t scePadSetVibration(int32_t, const struct vibration *);
int32_t sceSysmoduleLoadModule(uint16_t);
int32_t sceKeyboardInit(void);
int32_t sceKeyboardOpen(int32_t, int32_t, int32_t, const void *);
int32_t sceKeyboardClose(int32_t);
int32_t sceKeyboardRead(int32_t, void *, int32_t);
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t, int32_t, int32_t, const void *);
int32_t sceMouseClose(int32_t);
int32_t sceMouseRead(int32_t, void *, int32_t);
struct pad_record
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry, l2, r2, pad[2];
	float orientation[4], accel[3], gyro[3];
	uint8_t touch[24];
	int32_t connected;
	uint64_t time;
	uint8_t extension[16], generation, reserved[2], unique_length, unique[12];
};
struct key_record
{
	uint64_t time;
	uint8_t intercepted, pad[7], connected, pad2[3];
	int32_t length;
	uint32_t leds, modifiers;
	uint16_t keys[16];
	uint8_t pad3[32];
};
struct mouse_record
{
	uint64_t time;
	uint8_t connected, pad[3];
	uint32_t buttons;
	int32_t x, y, wheel, tilt;
	uint8_t pad2[8];
};
_Static_assert(sizeof(struct pad_record) == 120 && sizeof(struct key_record) == 96 &&
	sizeof(struct mouse_record) == 40, "device records");
struct pad
{
	int32_t user, handle;
	uint32_t id, buttons;
	int axes[6], present, opened;
	uint8_t generation;
	uint64_t rumble_end, retry;
};
static struct pad pads[4];
static int started, text_enabled, kh[2] = {-1, -1}, mh[2] = {-1, -1};
static uint16_t keys[2][16];
static uint32_t mods[2], mouse_buttons[2];
static uint64_t retry_hid, start_ms, log_second;
static int log_count;
static SDL_Event queue[2048];
static unsigned qread, qwrite, next_id = 1;
static char text_value[8];
extern long long host_sdl_ticks(void);
static const char *const scancode_names[232] = {
	[4]="A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
	"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
	"Return", "Escape", "Backspace", "Tab", "Space", "-", "=", "[", "]", "\\",
	"#", ";", "'", "`", ",", ".", "/", "CapsLock",
	"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
	"PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp", "Delete", "End",
	"PageDown", "Right", "Left", "Down", "Up", "Numlock", "Keypad /", "Keypad *",
	"Keypad -", "Keypad +", "Keypad Enter", "Keypad 1", "Keypad 2", "Keypad 3",
	"Keypad 4", "Keypad 5", "Keypad 6", "Keypad 7", "Keypad 8", "Keypad 9",
	"Keypad 0", "Keypad .", "NonUSBackslash", "Application", "Power", "Keypad =",
	"F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
	[224]="Left Ctrl", "Left Shift", "Left Alt", "Left GUI", "Right Ctrl", "Right Shift",
	"Right Alt", "Right GUI"
};
const char *ps5_scancode_name(int code)
{
	return code >= 0 && code < 232 && scancode_names[code] ? scancode_names[code] : "";
}
int ps5_scancode_from_name(const char *name)
{
	int i;
	for (i = 0; i < 232; i++)
		if (scancode_names[i] && !strcasecmp(scancode_names[i], name)) return i;
	return 0;
}
static void push(SDL_Event *event)
{
	unsigned next = (qwrite + 1) % 2048;
	if (next != qread)
	{
		event->common.timestamp = (uint64_t)host_sdl_ticks() * 1000000;
		queue[qwrite] = *event;
		qwrite = next;
	}
}
void ps5_input_window_created(unsigned int window)
{
	SDL_Event event;
	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_WINDOW_SHOWN;
	event.window.windowID = window;
	push(&event);
	event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
	push(&event);
}
static int log_allowed(void)
{
	uint64_t now = host_sdl_ticks();
	if (now - start_ms >= 60000)
		return 0;
	if (now / 1000 != log_second)
	{
		log_second = now / 1000;
		log_count = 0;
	}
	return log_count++ < 30;
}
static void device(struct pad *pad, int added)
{
	SDL_Event event;
	memset(&event, 0, sizeof(event));
	event.type = added ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
	event.gdevice.which = pad->id;
	push(&event);
}
static void remove_pad(struct pad *pad)
{
	if (pad->present)
	{
		device(pad, 0);
		pad->present = 0;
		pad->buttons = 0;
		memset(pad->axes, 0, sizeof(pad->axes));
	}
}
static void add_user(int32_t user)
{
	int i;
	if (user < 0)
		return;
	for (i = 0; i < 4; i++)
		if (pads[i].user == user)
			return;
	for (i = 0; i < 4; i++)
		if (pads[i].user < 0)
		{
			pads[i].user = user;
			pads[i].retry = 0;
			return;
		}
}
static void remove_user(int32_t user)
{
	int i;
	for (i = 0; i < 4; i++)
		if (pads[i].user == user)
		{
			remove_pad(&pads[i]);
			if (pads[i].handle >= 0)
				scePadClose(pads[i].handle);
			pads[i].handle = -1;
			pads[i].user = -1;
		}
}
static int primary_user(void)
{
	int i;
	for (i = 0; i < 4; i++)
		if (pads[i].user >= 0)
			return pads[i].user;
	return -1;
}
static void init(void)
{
	int i, km, ki, mm, mi;
	int32_t user = -1;
	struct login_user_list users = {{-1, -1, -1, -1}};
	if (started)
		return;
	started = 1;
	start_ms = host_sdl_ticks();
	for (i = 0; i < 4; i++)
		pads[i].user = pads[i].handle = -1;
	sceUserServiceInitialize(NULL);
	sceUserServiceGetInitialUser(&user);
	scePadInit();
	add_user(user);
	if (sceUserServiceGetLoginUserIdList(&users) == 0)
		for (i = 0; i < 4; i++)
			add_user(users.users[i]);
	km = sceSysmoduleLoadModule(0x106);
	ki = sceKeyboardInit();
	mm = sceSysmoduleLoadModule(0xa9);
	mi = sceMouseInit();
	host_logf(HOST_LOG_INFO, "PS5 input user %d keyboard module %#x init %#x", user, (unsigned)km, (unsigned)ki);
	host_logf(HOST_LOG_INFO, "PS5 mouse module %#x init %#x", (unsigned)mm, (unsigned)mi);
}
static const uint32_t button_bits[] = {
	1u << 14, 1u << 13, 1u << 15, 1u << 12, 1u << 0, 0, 1u << 3,
	1u << 1, 1u << 2, 1u << 10, 1u << 11, 1u << 4, 1u << 6,
	1u << 7, 1u << 5, 0, 0, 0, 0, 0, 1u << 20
};
static int stick_axis(uint8_t value)
{
	return value < 128 ? ((int)value - 128) * 256 : ((int)value - 128) * 32767 / 127;
}
static void poll_pads(uint64_t now)
{
	int i, j;
	struct user_event u;
	while (sceUserServiceGetEvent(&u) >= 0)
	{
		if (u.type == 0) add_user(u.user);
		else if (u.type == 1) remove_user(u.user);
	}
	for (i = 0; i < 4; i++)
	{
		struct pad *p = &pads[i];
		struct pad_record records[64];
		int n;
		if (p->user < 0) continue;
		if (p->handle < 0 && now >= p->retry)
		{
			p->handle = scePadOpen(p->user, 0, 0, NULL);
			p->retry = now + 1000;
			host_logf(HOST_LOG_INFO, "PS5 pad user %d open %#x", p->user, (unsigned)p->handle);
		}
		if (p->handle < 0) continue;
		if (p->rumble_end && now >= p->rumble_end)
		{
			struct vibration v = {0, 0};
			scePadSetVibration(p->handle, &v);
			p->rumble_end = 0;
		}
		n = scePadRead(p->handle, records, 64);
		if (n < 0)
		{
			remove_pad(p);
			scePadClose(p->handle);
			p->handle = -1;
			p->retry = now + 1000;
			continue;
		}
		for (j = 0; j < n; j++)
		{
			struct pad_record *v = &records[j];
			int axes[6] = {stick_axis(v->lx), stick_axis(v->ly),
				stick_axis(v->rx), stick_axis(v->ry),
				(int)v->l2*32767/255, (int)v->r2*32767/255};
			if (!v->connected || (v->buttons & 0x80000000u))
			{
				remove_pad(p);
				continue;
			}
			if (p->present && p->generation != v->generation) remove_pad(p);
			if (!p->present)
			{
				p->id = next_id++;
				p->generation = v->generation;
				p->present = 1;
				p->opened = 0;
				device(p, 1);
			}
			if ((p->buttons ^ v->buttons) & (1u << 20))
				host_logf(HOST_LOG_INFO, "PS5 pad %u touchpad %s", p->id,
					v->buttons & (1u << 20) ? "pressed" : "released");
			if ((p->buttons != v->buttons || memcmp(p->axes, axes, sizeof axes)) && log_allowed())
				host_logf(HOST_LOG_INFO, "PS5 pad %u buttons %#x sticks %u,%u %u,%u triggers %u,%u",
					p->id, v->buttons, v->lx, v->ly, v->rx, v->ry, v->l2, v->r2);
			p->buttons = v->buttons;
			memcpy(p->axes, axes, sizeof axes);
		}
	}
}
static int contains(const uint16_t *keys, int key)
{
	int i;
	for (i = 0; i < 16; i++)
		if (keys[i] == key)
			return 1;
	return 0;
}
static SDL_Keymod keymods(uint32_t modifiers)
{
	SDL_Keymod result = 0;
	if (modifiers & 1) result |= SDL_KMOD_LCTRL;
	if (modifiers & 2) result |= SDL_KMOD_LSHIFT;
	if (modifiers & 4) result |= SDL_KMOD_LALT;
	if (modifiers & 8) result |= SDL_KMOD_LGUI;
	if (modifiers & 16) result |= SDL_KMOD_RCTRL;
	if (modifiers & 32) result |= SDL_KMOD_RSHIFT;
	if (modifiers & 64) result |= SDL_KMOD_RALT;
	if (modifiers & 128) result |= SDL_KMOD_RGUI;
	return result;
}
static void key_event(int key, int down, uint32_t mod)
{
	SDL_Event e;
	int ch = 0, shift = (mod & (2 | 32)) != 0;
	static const char plain[] = "1234567890";
	static const char shifted[] = "!@#$%^&*()";
	static const char punctuation[] = "-=[]\\#;'`,./";
	static const char punctuation_shift[] = "_+{}|~:\"~<>?";
	memset(&e, 0, sizeof e);
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.windowID = 1;
	e.key.scancode = (SDL_Scancode)key;
	e.key.mod = keymods(mod);
	e.key.down = down;
	if (key >= 4 && key <= 29) ch = 'a' + key - 4;
	else if (key >= 30 && key <= 39) ch = plain[key - 30];
	else if (key == 44) ch = ' ';
	else if (key >= 45 && key <= 56) ch = punctuation[key - 45];
	e.key.key = ch ? (SDL_Keycode)ch : (SDLK_SCANCODE_MASK | key);
	push(&e);
	if (log_allowed())
		host_logf(HOST_LOG_INFO, "PS5 keyboard key %d %s modifiers %#x", key, down ? "down" : "up", mod);
	if (text_enabled && down && ch && !(mod & (1 | 4 | 8 | 16 | 64 | 128)))
	{
		SDL_Event t;
		if (shift && key >= 4 && key <= 29) ch -= 'a' - 'A';
		else if (shift && key >= 30 && key <= 39) ch = shifted[key - 30];
		else if (shift && key >= 45 && key <= 56) ch = punctuation_shift[key - 45];
		memset(&t, 0, sizeof t);
		t.type = SDL_EVENT_TEXT_INPUT;
		t.text.windowID = 1;
		t.text.text = (const char *)(uintptr_t)(unsigned char)ch;
		push(&t);
	}
}
static void poll_hid(uint64_t now)
{
	int i, j, k, user = primary_user();
	static const uint8_t params[8];
	if (user < 0)
		return;
	if (now >= retry_hid)
	{
		retry_hid = now + 1000;
		for (i = 0; i < 2; i++)
		{
			if (kh[i] < 0) kh[i] = sceKeyboardOpen(user, 0, i, params);
			if (mh[i] < 0) mh[i] = sceMouseOpen(user, 0, i, params);
		}
	}
	for (i = 0; i < 2; i++)
	{
		struct key_record kr[16];
		struct mouse_record mr[64];
		int n = kh[i] >= 0 ? sceKeyboardRead(kh[i], kr, 16) : 0;
		for (j = 0; j < n; j++)
		{
			struct key_record *r = &kr[j];
			uint16_t empty[16] = {0};
			uint16_t *next = r->connected && !r->intercepted ? r->keys : empty;
			uint32_t mod = r->connected && !r->intercepted ? r->modifiers : 0;
			for (k = 0; k < 16; k++)
				if (keys[i][k] > 3 && !contains(next, keys[i][k]))
					key_event(keys[i][k], 0, mod);
			for (k = 0; k < 16; k++)
				if (next[k] > 3 && !contains(keys[i], next[k]))
					key_event(next[k], 1, mod);
			for (k = 0; k < 8; k++)
				if ((mods[i] ^ mod) & (1u << k))
					key_event(224 + k, (mod >> k) & 1, mod);
			memcpy(keys[i], next, sizeof(keys[i]));
			mods[i] = mod;
		}
		n = mh[i] >= 0 ? sceMouseRead(mh[i], mr, 64) : 0;
		for (j = 0; j < n; j++)
		{
			struct mouse_record *r = &mr[j];
			SDL_Event event;
			uint32_t buttons = r->connected && !(r->buttons & 0x80000000u) ? r->buttons : 0;
			for (k = 0; k < 5; k++)
				if ((buttons ^ mouse_buttons[i]) & (1u << k))
				{
					memset(&event, 0, sizeof(event));
					event.type = (buttons & (1u << k)) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
					event.button.windowID = 1;
					event.button.button = (uint8_t)(k == 1 ? 3 : k == 2 ? 2 : k + 1);
					event.button.down = (buttons >> k) & 1;
					push(&event);
				}
			mouse_buttons[i] = buttons;
			if (r->connected && (r->x || r->y))
			{
				memset(&event, 0, sizeof(event));
				event.type = SDL_EVENT_MOUSE_MOTION;
				event.motion.windowID = 1;
				event.motion.xrel = (float)r->x;
				event.motion.yrel = (float)r->y;
				push(&event);
			}
			if (r->connected && (r->wheel || r->tilt))
			{
				memset(&event, 0, sizeof(event));
				event.type = SDL_EVENT_MOUSE_WHEEL;
				event.wheel.windowID = 1;
				event.wheel.x = (float)r->tilt;
				event.wheel.y = (float)r->wheel;
				push(&event);
			}
		}
	}
}
int ps5_input_poll(void *out)
{
	SDL_Event event;
	init();
	if (qread == qwrite)
	{
		uint64_t now = host_sdl_ticks();
		poll_pads(now);
		poll_hid(now);
	}
	if (qread == qwrite)
		return 0;
	event = queue[qread];
	qread = (qread + 1) % 2048;
	memcpy(out, &event, 128);
	if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
		host_logf(HOST_LOG_INFO, "PS5 window %u focus gained event delivered", event.window.windowID);
	if (event.type == SDL_EVENT_TEXT_INPUT)
	{
		uint32_t pointer;
		text_value[0] = (char)(uintptr_t)event.text.text;
		text_value[1] = 0;
		pointer = (uint32_t)(uintptr_t)text_value;
		memcpy((char *)out + 20, &pointer, sizeof(pointer));
	}
	return 1;
}
int ps5_input_text(int type)
{
	text_enabled = type >= 0;
	return text_enabled;
}
int host_sdl_get_gamepads(unsigned int *ids, int capacity)
{
	int i, n = 0;
	init();
	poll_pads(host_sdl_ticks());
	for (i = 0; i < 4; i++)
		if (pads[i].present && n < capacity)
			ids[n++] = pads[i].id;
	return n;
}
static struct pad *find(unsigned id)
{
	int i;
	for (i = 0; i < 4; i++)
		if (pads[i].present && pads[i].id == id)
			return &pads[i];
	return NULL;
}
unsigned host_sdl_open_gamepad(unsigned id)
{
	struct pad *pad = find(id);
	if (!pad)
		return 0;
	pad->opened = 1;
	return id;
}
unsigned host_sdl_gamepad_from_id(unsigned id)
{
	struct pad *pad = find(id);
	return pad && pad->opened ? id : 0;
}
int host_sdl_gamepad_axis(unsigned id, int axis)
{
	struct pad *pad = find(id);
	return pad && pad->opened && axis >= 0 && axis < 6 ? pad->axes[axis] : 0;
}
int host_sdl_gamepad_button(unsigned id, int button)
{
	struct pad *pad = find(id);
	return pad && pad->opened && button >= 0 && button < (int)(sizeof(button_bits) / sizeof(button_bits[0])) &&
		(pad->buttons & button_bits[button]) != 0;
}
int host_sdl_gamepad_type(unsigned id)
{
	return find(id) ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
int host_sdl_rumble_gamepad(unsigned id, unsigned low, unsigned high, unsigned ms)
{
	struct pad *pad = find(id);
	struct vibration vibration = {(uint8_t)(low >> 8), (uint8_t)(high >> 8)};
	if (!pad || !pad->opened || scePadSetVibration(pad->handle, &vibration) < 0)
		return 0;
	pad->rumble_end = host_sdl_ticks() + ms;
	return 1;
}
void host_sdl_close_gamepad(unsigned id)
{
	struct pad *pad = find(id);
	if (pad)
		pad->opened = 0;
}
