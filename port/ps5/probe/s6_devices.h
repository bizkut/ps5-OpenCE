/* S6's devices (s6_devices.c): the pad, keyboards, mice and a tone, beside
s5_zink.c's presentation (built with PROBE_S6) */

#ifndef S6_DEVICES_H
#define S6_DEVICES_H

typedef void (*s6_log_function)(const char *format, ...);

/* starts the user service, the pad, keyboard, mouse and AudioOut
libraries, opens the devices, and starts the tone's thread */
void s6_start(s6_log_function log);
/* reads every device once (each presented frame), logging what changed */
void s6_frame(void);
/* stops the tone, logs the counters and the checks over `seconds`; the
number of checks failed */
int s6_finish(double seconds);

#endif
