// AUTO-GENERATED from proto/schema.json — do not edit by hand.

#include "proto_gen.h"
#include <stdio.h>
#include <string.h>

static size_t pg_esc(char *dst, size_t cap, const char *s) {
    if (!dst || !cap) return 0;
    if (!s) { dst[0] = 0; return 0; }
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *rep = NULL;
        char ubuf[8];
        if (c == '"')  rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c == '\b') rep = "\\b";
        else if (c == '\f') rep = "\\f";
        else if (c < 0x20)   { snprintf(ubuf, sizeof(ubuf), "\\u%04x", c); rep = ubuf; }
        if (rep) {
            size_t rl = strlen(rep);
            if (n + rl + 1 > cap) break;
            memcpy(dst + n, rep, rl); n += rl;
        } else {
            if (n + 2 > cap) break;
            dst[n++] = (char)c;
        }
    }
    dst[n < cap ? n : cap - 1] = 0;
    return n;
}

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
    char e_type[256];
    char e_deviceId[256];
    char e_token[256];
    char e_fwVersion[256];
    char e_gitSha[256];
    char e_bootReason[256];
    pg_esc(e_type, sizeof(e_type), m->type);
    pg_esc(e_deviceId, sizeof(e_deviceId), m->deviceId);
    pg_esc(e_token, sizeof(e_token), m->token);
    pg_esc(e_fwVersion, sizeof(e_fwVersion), m->fwVersion);
    pg_esc(e_gitSha, sizeof(e_gitSha), m->gitSha);
    pg_esc(e_bootReason, sizeof(e_bootReason), m->bootReason);
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"deviceId\":\"%s\",\"token\":\"%s\",\"fwVersion\":\"%s\",\"gitSha\":\"%s\",\"bootReason\":\"%s\"}",
        e_type,
        e_deviceId,
        e_token,
        e_fwVersion,
        e_gitSha,
        e_bootReason);
}

size_t Init_to_json(char *buf, size_t bufsz, const Init_t *m) {
    char e_type[256];
    char e_fwVersion[256];
    char e_gitSha[256];
    char e_deviceId[256];
    char e_wifi_mode[256];
    char e_wifi_ssid[256];
    char e_wifi_ip[256];
    char e_visible_sensor[256];
    char e_thermal_gain[256];
    char e_thermal_state[256];
    pg_esc(e_type, sizeof(e_type), m->type);
    pg_esc(e_fwVersion, sizeof(e_fwVersion), m->fwVersion);
    pg_esc(e_gitSha, sizeof(e_gitSha), m->gitSha);
    pg_esc(e_deviceId, sizeof(e_deviceId), m->deviceId);
    pg_esc(e_wifi_mode, sizeof(e_wifi_mode), m->wifi.mode);
    pg_esc(e_wifi_ssid, sizeof(e_wifi_ssid), m->wifi.ssid);
    pg_esc(e_wifi_ip, sizeof(e_wifi_ip), m->wifi.ip);
    pg_esc(e_visible_sensor, sizeof(e_visible_sensor), m->visible.sensor);
    pg_esc(e_thermal_gain, sizeof(e_thermal_gain), m->thermal.gain);
    pg_esc(e_thermal_state, sizeof(e_thermal_state), m->thermal.state);
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"fwVersion\":\"%s\",\"gitSha\":\"%s\",\"deviceId\":\"%s\",\"state\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%lu,\"freePsram\":%lu,\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%ld,\"ip\":\"%s\"},\"ntpSynced\":%s,\"epoch\":%llu,\"visible\":{\"fps\":%lu,\"w\":%lu,\"h\":%lu,\"quality\":%lu,\"ready\":%s,\"sensor\":\"%s\"},\"thermal\":{\"fps\":%lu,\"gain\":\"%s\",\"agc\":%s,\"frames\":%lu,\"totalPackets\":%lu,\"validPackets\":%lu,\"discardPackets\":%lu,\"syncEntries\":%lu,\"lineMismatch\":%lu,\"segNot1\":%lu,\"segMismatch\":%lu,\"segZero\":%lu,\"frameTimeout\":%lu,\"spliceDetected\":%lu,\"hwResets\":%lu,\"lastFFCMs\":%lu,\"state\":\"%s\"},\"storage\":{\"sdMounted\":%s,\"sdTotalKB\":%llu,\"sdUsedKB\":%llu,\"sdFreeKB\":%llu,\"lfsMounted\":%s,\"lfsTotalKB\":%lu,\"lfsUsedKB\":%lu}}",
        e_type,
        e_fwVersion,
        e_gitSha,
        e_deviceId,
        DeviceState_str(m->state),
        (unsigned long long)m->uptimeMs,
        (unsigned long)m->freeHeap,
        (unsigned long)m->freePsram,
        e_wifi_mode,
        e_wifi_ssid,
        (long)m->wifi.rssi,
        e_wifi_ip,
        (m->ntpSynced ? "true" : "false"),
        (unsigned long long)m->epoch,
        (unsigned long)m->visible.fps,
        (unsigned long)m->visible.w,
        (unsigned long)m->visible.h,
        (unsigned long)m->visible.quality,
        (m->visible.ready ? "true" : "false"),
        e_visible_sensor,
        (unsigned long)m->thermal.fps,
        e_thermal_gain,
        (m->thermal.agc ? "true" : "false"),
        (unsigned long)m->thermal.frames,
        (unsigned long)m->thermal.totalPackets,
        (unsigned long)m->thermal.validPackets,
        (unsigned long)m->thermal.discardPackets,
        (unsigned long)m->thermal.syncEntries,
        (unsigned long)m->thermal.lineMismatch,
        (unsigned long)m->thermal.segNot1,
        (unsigned long)m->thermal.segMismatch,
        (unsigned long)m->thermal.segZero,
        (unsigned long)m->thermal.frameTimeout,
        (unsigned long)m->thermal.spliceDetected,
        (unsigned long)m->thermal.hwResets,
        (unsigned long)m->thermal.lastFFCMs,
        e_thermal_state,
        (m->storage.sdMounted ? "true" : "false"),
        (unsigned long long)m->storage.sdTotalKB,
        (unsigned long long)m->storage.sdUsedKB,
        (unsigned long long)m->storage.sdFreeKB,
        (m->storage.lfsMounted ? "true" : "false"),
        (unsigned long)m->storage.lfsTotalKB,
        (unsigned long)m->storage.lfsUsedKB);
}

size_t Tick_to_json(char *buf, size_t bufsz, const Tick_t *m) {
    char e_type[256];
    char e_wifi_mode[256];
    char e_wifi_ssid[256];
    char e_wifi_ip[256];
    char e_visible_sensor[256];
    char e_thermal_gain[256];
    char e_thermal_state[256];
    pg_esc(e_type, sizeof(e_type), m->type);
    pg_esc(e_wifi_mode, sizeof(e_wifi_mode), m->wifi.mode);
    pg_esc(e_wifi_ssid, sizeof(e_wifi_ssid), m->wifi.ssid);
    pg_esc(e_wifi_ip, sizeof(e_wifi_ip), m->wifi.ip);
    pg_esc(e_visible_sensor, sizeof(e_visible_sensor), m->visible.sensor);
    pg_esc(e_thermal_gain, sizeof(e_thermal_gain), m->thermal.gain);
    pg_esc(e_thermal_state, sizeof(e_thermal_state), m->thermal.state);
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"uptimeMs\":%llu,\"freeHeap\":%lu,\"freePsram\":%lu,\"epoch\":%llu,\"state\":\"%s\",\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%ld,\"ip\":\"%s\"},\"visible\":{\"fps\":%lu,\"w\":%lu,\"h\":%lu,\"quality\":%lu,\"ready\":%s,\"sensor\":\"%s\"},\"thermal\":{\"fps\":%lu,\"gain\":\"%s\",\"agc\":%s,\"frames\":%lu,\"totalPackets\":%lu,\"validPackets\":%lu,\"discardPackets\":%lu,\"syncEntries\":%lu,\"lineMismatch\":%lu,\"segNot1\":%lu,\"segMismatch\":%lu,\"segZero\":%lu,\"frameTimeout\":%lu,\"spliceDetected\":%lu,\"hwResets\":%lu,\"lastFFCMs\":%lu,\"state\":\"%s\"},\"storage\":{\"sdMounted\":%s,\"sdTotalKB\":%llu,\"sdUsedKB\":%llu,\"sdFreeKB\":%llu,\"lfsMounted\":%s,\"lfsTotalKB\":%lu,\"lfsUsedKB\":%lu}}",
        e_type,
        (unsigned long long)m->uptimeMs,
        (unsigned long)m->freeHeap,
        (unsigned long)m->freePsram,
        (unsigned long long)m->epoch,
        DeviceState_str(m->state),
        e_wifi_mode,
        e_wifi_ssid,
        (long)m->wifi.rssi,
        e_wifi_ip,
        (unsigned long)m->visible.fps,
        (unsigned long)m->visible.w,
        (unsigned long)m->visible.h,
        (unsigned long)m->visible.quality,
        (m->visible.ready ? "true" : "false"),
        e_visible_sensor,
        (unsigned long)m->thermal.fps,
        e_thermal_gain,
        (m->thermal.agc ? "true" : "false"),
        (unsigned long)m->thermal.frames,
        (unsigned long)m->thermal.totalPackets,
        (unsigned long)m->thermal.validPackets,
        (unsigned long)m->thermal.discardPackets,
        (unsigned long)m->thermal.syncEntries,
        (unsigned long)m->thermal.lineMismatch,
        (unsigned long)m->thermal.segNot1,
        (unsigned long)m->thermal.segMismatch,
        (unsigned long)m->thermal.segZero,
        (unsigned long)m->thermal.frameTimeout,
        (unsigned long)m->thermal.spliceDetected,
        (unsigned long)m->thermal.hwResets,
        (unsigned long)m->thermal.lastFFCMs,
        e_thermal_state,
        (m->storage.sdMounted ? "true" : "false"),
        (unsigned long long)m->storage.sdTotalKB,
        (unsigned long long)m->storage.sdUsedKB,
        (unsigned long long)m->storage.sdFreeKB,
        (m->storage.lfsMounted ? "true" : "false"),
        (unsigned long)m->storage.lfsTotalKB,
        (unsigned long)m->storage.lfsUsedKB);
}

size_t Event_to_json(char *buf, size_t bufsz, const Event_t *m) {
    char e_type[256];
    char e_kind[256];
    char e_msg[256];
    pg_esc(e_type, sizeof(e_type), m->type);
    pg_esc(e_kind, sizeof(e_kind), m->kind);
    pg_esc(e_msg, sizeof(e_msg), m->msg);
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"kind\":\"%s\",\"msg\":\"%s\",\"ts\":%llu}",
        e_type,
        e_kind,
        e_msg,
        (unsigned long long)m->ts);
}

size_t LogLine_to_json(char *buf, size_t bufsz, const LogLine_t *m) {
    char e_type[256];
    char e_level[256];
    char e_tag[256];
    char e_msg[256];
    pg_esc(e_type, sizeof(e_type), m->type);
    pg_esc(e_level, sizeof(e_level), m->level);
    pg_esc(e_tag, sizeof(e_tag), m->tag);
    pg_esc(e_msg, sizeof(e_msg), m->msg);
    return snprintf(buf, bufsz,
        "{\"type\":\"%s\",\"ts\":%llu,\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
        e_type,
        (unsigned long long)m->ts,
        e_level,
        e_tag,
        e_msg);
}

