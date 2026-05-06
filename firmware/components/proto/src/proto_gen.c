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
        "{\"type\":\"%s\",\"fwVersion\":\"%s\",\"gitSha\":\"%s\",\"deviceId\":\"%s\",\"state\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%lu,\"freePsram\":%lu,\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%ld,\"ip\":\"%s\"},\"ntpSynced\":%s,\"epoch\":%llu,\"visible\":{\"fps\":%lu,\"w\":%lu,\"h\":%lu,\"quality\":%lu},\"thermal\":{\"fps\":%lu,\"gain\":\"%s\",\"agc\":%s,\"spliceDetected\":%lu,\"lastFFCMs\":%lu}}",
        m->type,
        m->fwVersion,
        m->gitSha,
        m->deviceId,
        DeviceState_str(m->state),
        (unsigned long long)m->uptimeMs,
        (unsigned long)m->freeHeap,
        (unsigned long)m->freePsram,
        m->wifi.mode,
        m->wifi.ssid,
        (long)m->wifi.rssi,
        m->wifi.ip,
        (m->ntpSynced ? "true" : "false"),
        (unsigned long long)m->epoch,
        (unsigned long)m->visible.fps,
        (unsigned long)m->visible.w,
        (unsigned long)m->visible.h,
        (unsigned long)m->visible.quality,
        (unsigned long)m->thermal.fps,
        m->thermal.gain,
        (m->thermal.agc ? "true" : "false"),
        (unsigned long)m->thermal.spliceDetected,
        (unsigned long)m->thermal.lastFFCMs);
}

size_t Tick_to_json(char *buf, size_t bufsz, const Tick_t *m) {
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%lu,\"freePsram\":%lu,\"epoch\":%llu,\"state\":\"%s\",\"thermal\":{\"fps\":%lu,\"gain\":\"%s\",\"agc\":%s,\"spliceDetected\":%lu,\"lastFFCMs\":%lu}}",
        m->type,
        (unsigned long long)m->uptimeMs,
        (unsigned long)m->freeHeap,
        (unsigned long)m->freePsram,
        (unsigned long long)m->epoch,
        DeviceState_str(m->state),
        (unsigned long)m->thermal.fps,
        m->thermal.gain,
        (m->thermal.agc ? "true" : "false"),
        (unsigned long)m->thermal.spliceDetected,
        (unsigned long)m->thermal.lastFFCMs);
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

