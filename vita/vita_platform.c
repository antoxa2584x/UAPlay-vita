#include "vita_platform.h"

#include <string.h>
#include <sys/stat.h>

#include <mpv/client.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <SDL.h>
#include <vitaGL.h>

#include "../source/platform.h"

/* Newlib heap for FFmpeg buffers, fonts and the catalogue. vitaGL takes
   the rest of the user RAM, leaving its threshold for thread stacks. */
int _newlib_heap_size_user = 160 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;

static int enter_circle;
static int network_ready;

int vita_platform_init(void) {
    sceAppUtilInit(&(SceAppUtilInitParam){0}, &(SceAppUtilBootParam){0});
    SceCommonDialogConfigParam config;
    sceCommonDialogConfigParamInit(&config);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&config.language);
    int button = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &button);
    config.enterButtonAssign = button;
    enter_circle = button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    sceCommonDialogSetConfigParam(&config);

    mkdir(UAPLAY_DATA_DIR, 0777);
    mpv_vita_preinit();
    /* The result only reports whether the resolution had to be lowered. */
    vglInitExtended(0, VITA_SCREEN_W, VITA_SCREEN_H, 16 * 1024 * 1024,
                    SCE_GXM_MULTISAMPLE_NONE);
    return 1;
}

int vita_network_init(void) {
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return 0;
    static char memory[1024 * 1024];
    SceNetInitParam param = { memory, sizeof(memory), 0 };
    int result = sceNetInit(&param);
    if (result < 0 && result != (int)SCE_NET_ERROR_EBUSY) return 0;
    sceNetCtlInit();
    network_ready = 1;
    return 1;
}

void vita_network_exit(void) {
    if (!network_ready) return;
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    network_ready = 0;
}

int vita_enter_is_circle(void) { return enter_circle; }

static size_t utf8_to_utf16(const char *in, SceWChar16 *out, size_t capacity) {
    size_t n = 0;
    const unsigned char *u = (const unsigned char *)in;
    while (*u && n + 1 < capacity) {
        unsigned cp = *u++;
        if (cp >= 0xC0 && cp < 0xE0 && *u) cp = ((cp & 31) << 6) | (*u++ & 63);
        else if (cp >= 0xE0 && cp < 0xF0 && u[0] && u[1]) {
            cp = ((cp & 15) << 12) | ((u[0] & 63) << 6) | (u[1] & 63);
            u += 2;
        } else if (cp >= 0xF0 && u[0] && u[1] && u[2]) { u += 3; continue; }
        out[n++] = (SceWChar16)cp;
    }
    out[n] = 0;
    return n;
}

static void utf16_to_utf8(const SceWChar16 *in, char *out, size_t capacity) {
    size_t n = 0;
    for (; *in; in++) {
        unsigned cp = *in;
        if (cp < 0x80) {
            if (n + 1 >= capacity) break;
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            if (n + 2 >= capacity) break;
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 63));
        } else {
            if (n + 3 >= capacity) break;
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        }
    }
    out[n] = 0;
}

int vita_keyboard(char *value, size_t size) {
    static SceWChar16 title[32], initial[256], input[256];
    utf8_to_utf16("Пошук", title, 32);
    utf8_to_utf16(value, initial, 256);
    memset(input, 0, sizeof(input));
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    /* The system IME has no Ukrainian layout; Russian covers most Cyrillic. */
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_RUSSIAN;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title;
    param.maxTextLength = 128;
    param.initialText = initial;
    param.inputTextBuffer = input;
    if (sceImeDialogInit(&param) < 0) return 0;
    for (;;) {
        glClearColor(.035f, .044f, .059f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
        if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) break;
    }
    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER) return 0;
    utf16_to_utf8(input, value, size);
    return value[0] != 0;
}

void vita_media_tick(int watching) {
    static unsigned frames;
    if (watching && ++frames % 60 == 0) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
    }
}

void vita_swap(void) {
    vglSwapBuffers(GL_FALSE);
    /* Not every screen paints the whole display (menus over a paused
       video, partial overlays); start each frame from black so the
       previous screen never shows through. */
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void vita_platform_exit(void) {
    /* vitaGL has no teardown; the kernel reclaims its pools on exit. */
    sceAppUtilShutdown();
}

/* SDL's Vita backend only reads the touch panels while an SDL window
   exists, and vitaGL owns the display here. Poll the front panel directly
   and post the same finger events SDL would; the rear pad stays unused so
   holding the console never triggers taps. */
void vita_poll_touch(void) {
    static int ready, previous_count;
    static SceTouchReport previous[SCE_TOUCH_MAX_REPORT];
    if (!ready) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        ready = 1;
    }
    SceTouchData data;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1) < 1) return;
    int count = data.reportNum > SCE_TOUCH_MAX_REPORT ? SCE_TOUCH_MAX_REPORT : (int)data.reportNum;
    for (int i = 0; i < count; i++) {
        int was_down = 0, moved = 1;
        for (int j = 0; j < previous_count; j++)
            if (previous[j].id == data.report[i].id) {
                was_down = 1;
                moved = previous[j].x != data.report[i].x || previous[j].y != data.report[i].y;
            }
        /* Like SDL, report motion only when the finger moves: the player
           treats motion as continued input, so a resting finger on the
           pause button would toggle it every frame. */
        if (was_down && !moved) continue;
        SDL_Event event;
        SDL_zero(event);
        event.type = was_down ? SDL_FINGERMOTION : SDL_FINGERDOWN;
        event.tfinger.touchId = 1;
        event.tfinger.fingerId = data.report[i].id;
        /* The front panel reports 1920x1088 for the 960x544 screen. */
        event.tfinger.x = data.report[i].x / 1920.f;
        event.tfinger.y = data.report[i].y / 1088.f;
        event.tfinger.pressure = 1.f;
        SDL_PushEvent(&event);
    }
    for (int j = 0; j < previous_count; j++) {
        int still_down = 0;
        for (int i = 0; i < count; i++)
            if (previous[j].id == data.report[i].id) still_down = 1;
        if (still_down) continue;
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_FINGERUP;
        event.tfinger.touchId = 1;
        event.tfinger.fingerId = previous[j].id;
        event.tfinger.x = previous[j].x / 1920.f;
        event.tfinger.y = previous[j].y / 1088.f;
        SDL_PushEvent(&event);
    }
    memcpy(previous, data.report, sizeof(SceTouchReport) * (size_t)count);
    previous_count = count;
}

int vita_battery(int *charging) {
    if (charging) *charging = scePowerIsBatteryCharging() > 0;
    int percent = scePowerGetBatteryLifePercent();
    return percent >= 0 && percent <= 100 ? percent : -1;
}
