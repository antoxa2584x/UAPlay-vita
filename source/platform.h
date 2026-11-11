#ifndef UAPLAY_PLATFORM_H
#define UAPLAY_PLATFORM_H

/* Storage roots per console. Host-side tests keep the Switch layout. */
#ifdef __vita__
#define UAPLAY_CONSOLE 1
#define UAPLAY_DATA_ROOT "ux0:data"
#define UAPLAY_ROMFS "app0:romfs/"
/* OpenSSL on the Vita has no system trust store; ship Mozilla's bundle. */
#define UAPLAY_CA_BUNDLE UAPLAY_ROMFS "cacert.pem"
#else
#ifdef __SWITCH__
#define UAPLAY_CONSOLE 1
#endif
#define UAPLAY_DATA_ROOT "sdmc:/switch"
#define UAPLAY_ROMFS "romfs:/"
#endif
#define UAPLAY_DATA_DIR UAPLAY_DATA_ROOT "/uaplay"

#endif
