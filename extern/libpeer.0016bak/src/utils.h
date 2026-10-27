#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"

#define LEVEL_ERROR 0x00
#define LEVEL_WARN 0x01
#define LEVEL_INFO 0x02
#define LEVEL_DEBUG 0x03

#define ERROR_TAG "ERROR"
#define WARN_TAG "WARN"
#define INFO_TAG "INFO"
#define DEBUG_TAG "DEBUG"

#ifndef LOG_LEVEL
#define LOG_LEVEL LEVEL_INFO
#endif

// ---------------------------------------------------------------------------
// Logging MUST NOT touch stdout/stderr.
//
// On the Switch, stdout is bound to libnx's SOFTWARE CONSOLE RENDERER
// (libnx.a: console.o -> dotab_stdout).  A single printf-family call from
// libpeer therefore reaches ConsoleSwRenderer_drawChar(), which dereferences
// a NULL renderer context and kills the whole app with a Data Abort @ 0x0
// (Atmosphere crash report:
// reports/device/2026-10-22-evening/atmosphere-crash/).
// peer_connection_create() -> agent_create() -> udp_socket_open() hits it,
// because udp_socket_open() logs the receive buffer size.
//
// Every log line goes through peer_log() instead.  peer_log() is implemented by
// the ENGINE, on the far side of the library boundary
// (source/webrtc_backend_libpeer.cc), and routes the line into the engine's
// debug log: stderr, which the engine redirects to sdmc:/switch/nxjs-debug.log.
// stderr has NO libnx console devoptab, so it can never reach the renderer.
//
// There is deliberately NO stdout branch here: the switch must never be
// flipped back, and the #error turns that mistake into a build failure
// instead of a silent runtime crash.
// ---------------------------------------------------------------------------
#if LOG_REDIRECT
#ifdef __cplusplus
extern "C" {
#endif
void peer_log(char* level_tag, const char* file_name, int line_number, const char* fmt, ...);
#ifdef __cplusplus
}
#endif
#define LOG_PRINT(level_tag, fmt, ...) \
  peer_log(level_tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#error "libpeer must not log to stdout on the Switch -- see patches/0022-NOTES.md"
#endif

#if LOG_LEVEL >= LEVEL_DEBUG
#define LOGD(fmt, ...) LOG_PRINT(DEBUG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_INFO
#define LOGI(fmt, ...) LOG_PRINT(INFO_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGI(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_WARN
#define LOGW(fmt, ...) LOG_PRINT(WARN_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGW(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_ERROR
#define LOGE(fmt, ...) LOG_PRINT(ERROR_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGE(fmt, ...)
#endif

#define ALIGN32(num) ((num + 3) & ~3)

void utils_random_string(char* s, const int len);

void utils_get_hmac_sha1(const char* input, size_t input_len, const char* key, size_t key_len, unsigned char* output);

void utils_get_md5(const char* input, size_t input_len, unsigned char* output);

#endif  // UTILS_H_
