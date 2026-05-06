// AUTO-GENERATED from proto/schema.json — do not edit by hand.
// Run `python3 proto/codegen.py` from the repo root to regenerate.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICESTATE_BOOT,
    DEVICESTATE_SAFE_MODE,
    DEVICESTATE_IDLE,
    DEVICESTATE_STREAMING,
    DEVICESTATE_CAPTURING,
    DEVICESTATE_TIMELAPSE_ACTIVE,
    DEVICESTATE_DEEP_SLEEP_PREP,
    DEVICESTATE_SHUTDOWN,
} DeviceState_t;

const char *DeviceState_str(DeviceState_t v);

typedef struct {
    const char * mode;
    const char * ssid;
    int32_t rssi;
    const char * ip;
} Wifi_t;

typedef struct {
    uint32_t fps;
    uint32_t w;
    uint32_t h;
    uint32_t quality;
} Visible_t;

typedef struct {
    uint32_t fps;
    const char * gain;
    bool agc;
    uint32_t frames;
    uint32_t totalPackets;
    uint32_t validPackets;
    uint32_t discardPackets;
    uint32_t syncEntries;
    uint32_t lineMismatch;
    uint32_t segNot1;
    uint32_t segMismatch;
    uint32_t segZero;
    uint32_t frameTimeout;
    uint32_t spliceDetected;
    uint32_t hwResets;
    uint32_t lastFFCMs;
    const char * state;
} Thermal_t;

typedef struct {
    const char * type;
    const char * deviceId;
    const char * token;
    const char * fwVersion;
    const char * gitSha;
    const char * bootReason;
} Hello_t;

size_t Hello_to_json(char *buf, size_t bufsz, const Hello_t *m);

typedef struct {
    const char * type;
    const char * fwVersion;
    const char * gitSha;
    const char * deviceId;
    DeviceState_t state;
    uint64_t uptimeMs;
    uint32_t freeHeap;
    uint32_t freePsram;
    Wifi_t wifi;
    bool ntpSynced;
    uint64_t epoch;
    Visible_t visible;
    Thermal_t thermal;
} Init_t;

size_t Init_to_json(char *buf, size_t bufsz, const Init_t *m);

typedef struct {
    const char * type;
    uint64_t uptimeMs;
    uint32_t freeHeap;
    uint32_t freePsram;
    uint64_t epoch;
    DeviceState_t state;
    Thermal_t thermal;
} Tick_t;

size_t Tick_to_json(char *buf, size_t bufsz, const Tick_t *m);

typedef struct {
    const char * type;
    const char * kind;
    const char * msg;
    uint64_t ts;
} Event_t;

size_t Event_to_json(char *buf, size_t bufsz, const Event_t *m);

typedef struct {
    const char * type;
    uint64_t ts;
    const char * level;
    const char * tag;
    const char * msg;
} LogLine_t;

size_t LogLine_to_json(char *buf, size_t bufsz, const LogLine_t *m);

#ifdef __cplusplus
}
#endif
