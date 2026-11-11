#ifndef UAPLAY_VITA_PLATFORM_H
#define UAPLAY_VITA_PLATFORM_H

#include <stddef.h>

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

/* Reserve decoder memory, start vitaGL at native resolution and prepare
   the system dialogs. Returns 0 on failure. */
int vita_platform_init(void);
void vita_platform_exit(void);
int vita_network_init(void);
void vita_network_exit(void);
/* System IME dialog. Returns 1 with UTF-8 text when confirmed. */
int vita_keyboard(char *value, size_t size);
/* Japanese-region consoles confirm with Circle instead of Cross. */
int vita_enter_is_circle(void);
/* Keep the screen on and the console awake while a video plays. */
void vita_media_tick(int watching);
/* Keep the console out of auto standby (the screen may still dim). */
void vita_keep_awake_tick(int awake);
void vita_swap(void);
/* Feed front touch panel input into the SDL event queue. */
void vita_poll_touch(void);
/* Battery charge 0-100 (or -1 when unknown) and whether it is charging. */
int vita_battery(int *charging);
/* One profiler line: clocks, free heap and free GPU memory. */
void vita_profile_info(char *out, size_t size);
/* Timings of the steps inside vita_platform_init. */
void vita_boot_info(char *out, size_t size);

#endif
