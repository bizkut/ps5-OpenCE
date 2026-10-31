/* AudioOut declarations from SavageWindRecomp runtime/ps5/audio.c,
 * Copyright the Savage Wind Recomp contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "host.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
int32_t sceAudioOutOutput(int32_t, const void *);
int32_t sceAudioOutClose(int32_t);
/* AudioIn interface and port parameters as used by
 * ps5-wow/ps5/title/voice.cpp (Forever). */
int32_t sceUserServiceGetInitialUser(int32_t *);
int32_t sceAudioInInit(void);
int32_t sceAudioInOpen(int32_t, unsigned, unsigned, unsigned, unsigned, unsigned);
int32_t sceAudioInInput(int32_t, void *);
int32_t sceAudioInClose(int32_t);
extern long long host_sdl_ticks(void);
#define FRAMES 256
#define CAPACITY (FRAMES * 2 * 32)
static struct
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int active, paused, quit, finished, port;
	uint32_t callback, userdata;
	float samples[CAPACITY];
	unsigned count;
} audio = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void *audio_thread(void *unused)
{
	int16_t output[FRAMES * 2];
	unsigned periods = 0, underruns = 0, errors = 0;
	uint64_t last = host_sdl_ticks();
	(void)unused;
	for (;;)
	{
		unsigned i, n;
		int result;
		pthread_mutex_lock(&audio.lock);
		while (audio.paused && !audio.quit)
			pthread_cond_wait(&audio.cond, &audio.lock);
		if (audio.quit)
		{
			pthread_mutex_unlock(&audio.lock);
			break;
		}
		n = audio.count;
		pthread_mutex_unlock(&audio.lock);
		if (n < FRAMES * 2)
			host_call_guest(audio.callback, audio.userdata, 1,
				(FRAMES * 2 - n) * sizeof(float), (FRAMES * 2 - n) * sizeof(float));
		pthread_mutex_lock(&audio.lock);
		n = audio.count < FRAMES * 2 ? audio.count : FRAMES * 2;
		if (n < FRAMES * 2)
			underruns++;
		for (i = 0; i < n; i++)
		{
			float value = audio.samples[i];
			if (!(value <= 1.0f)) value = 1.0f;
			if (value < -1.0f) value = -1.0f;
			output[i] = (int16_t)(value < 0 ? value * 32768.0f : value * 32767.0f);
		}
		memset(output + n, 0, (FRAMES * 2 - n) * sizeof(*output));
		audio.count -= n;
		memmove(audio.samples, audio.samples + n, audio.count * sizeof(float));
		pthread_mutex_unlock(&audio.lock);
		result = sceAudioOutOutput(audio.port, output);
		periods++;
		if (result < 0) errors++;
		if ((uint64_t)host_sdl_ticks() - last >= 10000)
		{
			host_logf(HOST_LOG_INFO, "PS5 audio periods %u underruns %u errors %u", periods, underruns, errors);
			periods = underruns = errors = 0;
			last = host_sdl_ticks();
		}
	}
	sceAudioOutClose(audio.port);
	pthread_mutex_lock(&audio.lock);
	audio.finished = 1;
	pthread_cond_broadcast(&audio.cond);
	pthread_mutex_unlock(&audio.lock);
	return NULL;
}
/* One second of native 16 kHz mono. Read position uses units of 1/freq
 * native samples, retaining one sample for linear interpolation. */
#define CAPTURE_RATE 16000
#define CAPTURE_RING 16384
static struct
{
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_t thread;
	int active, paused, quit, port, freq, channels, format;
	uint64_t end, position;
	int16_t samples[CAPTURE_RING];
} capture = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
static int capture_initialized, capture_init_result;

static void *capture_thread(void *unused)
{
	_Alignas(64) int16_t input[FRAMES];
	int last_error = 0, zero_periods = 0, silent = 0;
	int peak = 0;
	uint64_t last_report = host_sdl_ticks();
	(void)unused;
	for (;;)
	{
		int result, i;
		pthread_mutex_lock(&capture.lock);
		while (capture.paused && !capture.quit)
			pthread_cond_wait(&capture.changed, &capture.lock);
		if (capture.quit)
		{
			pthread_mutex_unlock(&capture.lock);
			break;
		}
		pthread_mutex_unlock(&capture.lock);
		result = sceAudioInInput(capture.port, input);
		if (result < 0)
		{
			struct timespec delay = {0, 16000000};
			if (result != last_error)
				host_logf(HOST_LOG_ERROR, "PS5 microphone input %#x; delivering silence", (unsigned)result);
			last_error = result;
			memset(input, 0, sizeof(input));
			nanosleep(&delay, NULL);
		}
		else if (last_error)
		{
			host_logf(HOST_LOG_INFO, "PS5 microphone input recovered");
			last_error = 0;
		}
		for (i = 0; i < FRAMES && !input[i]; i++);
		if (i == FRAMES)
		{
			if (zero_periods < 30) zero_periods++;
			if (zero_periods == 30 && !silent)
			{
				host_logf(HOST_LOG_INFO, "PS5 microphone input is all zero");
				silent = 1;
			}
		}
		else
		{
			zero_periods = 0;
			if (silent)
				host_logf(HOST_LOG_INFO, "PS5 microphone input is no longer all zero");
			silent = 0;
		}
		for (i = 0; i < FRAMES; i++)
		{
			int magnitude = input[i] < 0 ? -(int)input[i] : input[i];
			if (magnitude > peak) peak = magnitude;
		}
		if ((uint64_t)host_sdl_ticks() - last_report >= 1000)
		{
			host_logf(HOST_LOG_INFO, "PS5 microphone peak %d/32768 all-zero=%d input-error=%#x",
				peak, silent, (unsigned)last_error);
			peak = 0;
			last_report = host_sdl_ticks();
		}
		pthread_mutex_lock(&capture.lock);
		for (i = 0; i < FRAMES; i++)
			capture.samples[capture.end++ % CAPTURE_RING] = input[i];
		if (capture.end > CAPTURE_RING && capture.position / capture.freq < capture.end - CAPTURE_RING)
			capture.position = (capture.end - CAPTURE_RING) * capture.freq;
		pthread_mutex_unlock(&capture.lock);
	}
	return NULL;
}

static unsigned capture_open(const SDL_AudioSpec *spec)
{
	int32_t user = -1;
	int result;
	if (!spec || (spec->format != SDL_AUDIO_F32 && spec->format != SDL_AUDIO_S16) ||
		(spec->channels != 1 && spec->channels != 2) || spec->freq < 8000 || spec->freq > 192000)
		return 0;
	pthread_mutex_lock(&capture.lock);
	if (capture.active)
	{
		pthread_mutex_unlock(&capture.lock);
		return 0;
	}
	if (!capture_initialized)
	{
		capture_init_result = sceAudioInInit();
		capture_initialized = 1;
	}
	if (capture_init_result < 0 || sceUserServiceGetInitialUser(&user) < 0)
	{
		host_logf(HOST_LOG_ERROR, "PS5 microphone initialization %#x user %d",
			(unsigned)capture_init_result, user);
		pthread_mutex_unlock(&capture.lock);
		return 0;
	}
	capture.port = sceAudioInOpen(user, 1, 0, FRAMES, CAPTURE_RATE, 0);
	if (capture.port < 0)
	{
		host_logf(HOST_LOG_ERROR, "PS5 microphone open %#x", (unsigned)capture.port);
		pthread_mutex_unlock(&capture.lock);
		return 0;
	}
	capture.active = 1;
	capture.paused = 1;
	capture.quit = 0;
	capture.end = capture.position = 0;
	capture.freq = spec->freq;
	capture.channels = spec->channels;
	capture.format = spec->format;
	result = pthread_create(&capture.thread, NULL, capture_thread, NULL);
	if (result)
	{
		sceAudioInClose(capture.port);
		capture.active = 0;
		pthread_mutex_unlock(&capture.lock);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "PS5 microphone port %d open user %d", capture.port, user);
	pthread_mutex_unlock(&capture.lock);
	return 2;
}

static uint64_t capture_available_frames(void)
{
	uint64_t limit;
	if (capture.end < 2)
		return 0;
	limit = (capture.end - 1) * (uint64_t)capture.freq;
	return capture.position < limit ? (limit - capture.position + CAPTURE_RATE - 1) / CAPTURE_RATE : 0;
}
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback,
	unsigned int userdata)
{
	const SDL_AudioSpec *format = spec;
	int result;
	(void)device;
	if (!callback)
		return capture_open(format);
	if (!format || format->format != SDL_AUDIO_F32 || format->channels != 2 || format->freq != 48000)
		return 0;
	pthread_mutex_lock(&audio.lock);
	if (audio.active)
	{
		pthread_mutex_unlock(&audio.lock);
		return 0;
	}
	result = sceAudioOutInit();
	if (result < 0 && (uint32_t)result != 0x8026000eU)
	{
		pthread_mutex_unlock(&audio.lock);
		return 0;
	}
	audio.port = sceAudioOutOpen(0xff, 0, 0, FRAMES, 48000, 1);
	if (audio.port < 0)
	{
		host_logf(HOST_LOG_ERROR, "PS5 audio open %#x", (unsigned)audio.port);
		pthread_mutex_unlock(&audio.lock);
		return 0;
	}
	audio.active = audio.paused = 1;
	audio.quit = audio.finished = 0;
	audio.count = 0;
	audio.callback = callback;
	audio.userdata = userdata;
	host_logf(HOST_LOG_INFO, "PS5 audio port %d open", audio.port);
	result = host_native_thread_create(audio_thread, NULL, 256 * 1024);
	if (result)
	{
		sceAudioOutClose(audio.port);
		audio.active = 0;
		pthread_mutex_unlock(&audio.lock);
		return 0;
	}
	pthread_mutex_unlock(&audio.lock);
	return 1;
}
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length)
{
	unsigned count;
	if (stream != 1 || length < 0 || length % (int)sizeof(float))
		return 0;
	pthread_mutex_lock(&audio.lock);
	count = (unsigned)length / sizeof(float);
	if (!audio.active || count > CAPACITY - audio.count)
	{
		pthread_mutex_unlock(&audio.lock);
		return 0;
	}
	memcpy(audio.samples + audio.count, data, (size_t)length);
	audio.count += count;
	pthread_mutex_unlock(&audio.lock);
	return 1;
}
int host_sdl_resume_audio_stream_device(unsigned int stream)
{
	int ok;
	if (stream == 2)
	{
		pthread_mutex_lock(&capture.lock);
		ok = capture.active && !capture.quit;
		if (ok)
		{
			capture.paused = 0;
			pthread_cond_signal(&capture.changed);
		}
		pthread_mutex_unlock(&capture.lock);
		return ok;
	}
	pthread_mutex_lock(&audio.lock);
	ok = stream == 1 && audio.active;
	if (ok)
	{
		audio.paused = 0;
		pthread_cond_signal(&audio.cond);
	}
	pthread_mutex_unlock(&audio.lock);
	return ok;
}
int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length)
{
	unsigned char *out = data;
	uint64_t frames, available, i;
	int frame_bytes, channel;
	if (stream != 2 || !data || length < 0)
		return -1;
	pthread_mutex_lock(&capture.lock);
	if (!capture.active || capture.quit)
	{
		pthread_mutex_unlock(&capture.lock);
		return -1;
	}
	frame_bytes = capture.channels * (capture.format == SDL_AUDIO_F32 ? 4 : 2);
	frames = (unsigned)length / frame_bytes;
	available = capture_available_frames();
	if (frames > available) frames = available;
	for (i = 0; i < frames; i++)
	{
		uint64_t index = capture.position / capture.freq;
		unsigned fraction = (unsigned)(capture.position % capture.freq);
		float a = capture.samples[index % CAPTURE_RING], b = capture.samples[(index + 1) % CAPTURE_RING];
		float value = (a + (b - a) * (float)fraction / (float)capture.freq) / 32768.0f;
		for (channel = 0; channel < capture.channels; channel++)
		{
			if (capture.format == SDL_AUDIO_F32)
			{
				memcpy(out, &value, 4);
				out += 4;
			}
			else
			{
				int16_t sample = (int16_t)(value * 32768.0f);
				memcpy(out, &sample, 2);
				out += 2;
			}
		}
		capture.position += CAPTURE_RATE;
	}
	pthread_mutex_unlock(&capture.lock);
	return (int)(frames * frame_bytes);
}
int host_sdl_get_audio_stream_available(unsigned int stream)
{
	uint64_t bytes;
	if (stream != 2)
		return -1;
	pthread_mutex_lock(&capture.lock);
	if (!capture.active || capture.quit)
	{
		pthread_mutex_unlock(&capture.lock);
		return -1;
	}
	bytes = capture_available_frames() * capture.channels * (capture.format == SDL_AUDIO_F32 ? 4 : 2);
	pthread_mutex_unlock(&capture.lock);
	return (int)bytes;
}
void host_sdl_destroy_audio_stream(unsigned int stream)
{
	if (stream == 2)
	{
		pthread_mutex_lock(&capture.lock);
		if (!capture.active || capture.quit)
		{
			pthread_mutex_unlock(&capture.lock);
			return;
		}
		capture.quit = 1;
		pthread_cond_signal(&capture.changed);
		pthread_mutex_unlock(&capture.lock);
		pthread_join(capture.thread, NULL);
		sceAudioInClose(capture.port);
		pthread_mutex_lock(&capture.lock);
		capture.active = 0;
		pthread_mutex_unlock(&capture.lock);
		host_logf(HOST_LOG_INFO, "PS5 microphone closed");
		return;
	}
	pthread_mutex_lock(&audio.lock);
	if (stream == 1 && audio.active)
	{
		audio.quit = 1;
		pthread_cond_signal(&audio.cond);
		while (!audio.finished)
			pthread_cond_wait(&audio.cond, &audio.lock);
		audio.active = 0;
	}
	pthread_mutex_unlock(&audio.lock);
}
