// AUTO-GENERATED from proto/schema.json — do not edit by hand.

#include "proto_gen.h"
#include <stdio.h>

const char *DeviceState_str(DeviceState_t v) {
    switch (v) {
        case DEVICESTATE_BOOT: return "BOOT";
        case DEVICESTATE_SAFE_MODE: return "SAFE_MODE";
        case DEVICESTATE_IDLE: return "IDLE";
        case DEVICESTATE_STREAMING: return "STREAMING";
        case DEVICESTATE_CAPTURING: return "CAPTURING";
        case DEVICESTATE_TIMELAPSE_ACTIVE: return "TIMELAPSE_ACTIVE";
        case DEVICESTATE_DEEP_SLEEP_PREP: return "DEEP_SLEEP_PREP";
        case DEVICESTATE_SHUTDOWN: return "SHUTDOWN";
        default: return "?";
    }
}

size_t Hello_to_json(char *buf, size_t bufsz, const Hello_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"deviceId\":\"%s\",\"token\":\"%s\",\"fwVersion\":\"%s\",\"gitSha\":\"%s\",\"bootReason\":\"%s\"}",
        m->type,
        m->deviceId,
        m->token,
        m->fwVersion,
        m->gitSha,
        m->bootReason);
}

size_t Init_to_json(char *buf, size_t bufsz, const Init_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"fwVersion\":\"%s\",\"gitSha\":\"%s\",\"deviceId\":\"%s\",\"state\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%u,\"freePsram\":%u,\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"},\"ntpSynced\":%s,\"epoch\":%llu,\"visible\":{\"fps\":%u,\"w\":%u,\"h\":%u,\"quality\":%u},\"thermal\":{\"fps\":%u,\"gain\":\"%s\",\"agc\":%s,\"spliceDetected\":%u,\"lastFFCMs\":%u}}",
        m->type,
        m->fwVersion,
        m->gitSha,
        m->deviceId,
        DeviceState_str(m->state),
        (unsigned long long)m->uptimeMs,
        m->freeHeap,
        m->freePsram,
        m->wifi.mode,
        m->wifi.ssid,
        m->wifi.rssi,
        m->wifi.ip,
        (m->ntpSynced ? "true" : "false"),
        (unsigned long long)m->epoch,
        m->visible.fps,
        m->visible.w,
        m->visible.h,
        m->visible.quality,
        m->thermal.fps,
        m->thermal.gain,
        (m->thermal.agc ? "true" : "false"),
        m->thermal.spliceDetected,
        m->thermal.lastFFCMs);
}

size_t Tick_to_json(char *buf, size_t bufsz, const Tick_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%u,\"freePsram\":%u,\"epoch\":%llu,\"state\":\"%s\",\"thermal\":{\"fps\":%u,\"gain\":\"%s\",\"agc\":%s,\"spliceDetected\":%u,\"lastFFCMs\":%u}}",
        m->type,
        (unsigned long long)m->uptimeMs,
        m->freeHeap,
        m->freePsram,
        (unsigned long long)m->epoch,
        DeviceState_str(m->state),
        m->thermal.fps,
        m->thermal.gain,
        (m->thermal.agc ? "true" : "false"),
        m->thermal.spliceDetected,
        m->thermal.lastFFCMs);
}

size_t Event_to_json(char *buf, size_t bufsz, const Event_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"kind\":\"%s\",\"msg\":\"%s\",\"ts\":%llu}",
        m->type,
        m->kind,
        m->msg,
        (unsigned long long)m->ts);
}

size_t LogLine_to_json(char *buf, size_t bufsz, const LogLine_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"ts\":%llu,\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
        m->type,
        (unsigned long long)m->ts,
        m->level,
        m->tag,
        m->msg);
}

