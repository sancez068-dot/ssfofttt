/*
 * KM 1.4 ARM64 — aim-assist V1.3 trace-slot fix (no recoil compensation).
 *
 * Based on the exact libclient.so from the supplied arm64-v8a.zip:
 *   ClientModeShared::CreateMove wrapper: RVA 0x8B0844
 *   CUserCmd viewangles: +0x10, +0x14, +0x18
 *   m_iHealth: +0xB8; m_lifeState: +0xB7; m_iTeamNum: +0xC0
 *   m_vecOrigin: +0x430; m_vecViewOffset: +0x124
 *
 * Runtime behavior (v1.3 trace-slot fix):
 *   - Calls assist logic after every original CreateMove call with non-null cmd,
 *     regardless of the original function's bool return value.
 *   - Logs CreateMove results and the first eight trace outcomes to diagnose why
 *     targets are selected or rejected. Keeps strict visible-target matching.
 *   - No recoil compensation.
 *   - Experimental head assist uses speed 9.11, magnet 0.70, FOV 11 degrees;
 *     target point is origin + viewOffset as a head/eye approximation.
 *     It only considers an enemy when EngineTraceClient003 reports a clear trace
 *     to that player. This is NOT bone-based; runtime ABI and offsets remain unverified.
 *
 * Hook is installed only if the first four ARM64 instructions at the exact RVA match.
 * Keep the generated library name unique; do not load this and another hook at 0x8B0844.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "KM14 aim assist probe is ARM64-only"
#endif

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif

#define TAG "KM14AimV1"
#define LOG_PATH "/storage/emulated/0/alrm/mod/logger.log"
#define FALLBACK_LOG_PATH "/data/user/0/com.valvesoftware.source.client/files/km14_aimassist_v1.log"
#define CREATE_MOVE_RVA ((uintptr_t)0x8B0844u)
#define CMD_PITCH_OFF   0x10u
#define CMD_YAW_OFF     0x14u
#define CMD_ROLL_OFF    0x18u
#define HEALTH_OFF      0xB8u
#define LIFE_STATE_OFF  0xB7u
#define TEAM_OFF        0xC0u
#define ORIGIN_OFF      0x430u
#define VIEW_OFFSET_OFF 0x124u
#define MAX_ENT_SCAN    2048
#define MAX_PLAYER_SLOTS 65
#define LOG_INTERVAL_MS 1000u
#define DEFAULT_AIM_SPEED       9.11f
#define DEFAULT_AIM_MAGNET      0.70f
#define DEFAULT_HEAD_FOV_DEG    11.0f
#define CONTROL_PORT            28765
#define CONTROL_MAX_REQUEST     8192
#define TRACE_MASK_SHOT 0x46004003u /* Source MASK_SHOT: includes hitboxes */
#define TRACE_RAY_VTABLE_SLOT 5u /* IEngineTrace: slot 4=ClipRayToCollideable, slot 5=TraceRay */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_client_handle, *g_engine_handle;
static void *g_entity_list, *g_engine_client, *g_engine_trace;
static uintptr_t g_client_base;
static volatile int g_interfaces_ready;
static volatile int g_hook_installed;
static volatile int g_hook_busy;
static uint64_t g_last_log_ms;
static int g_last_target = -1;
static int g_trace_test_logged;
static int g_trace_result_logs;
static uint64_t g_last_hook_log_ms;
static uint64_t g_last_skip_log_ms;
static pthread_mutex_t g_config_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cfg_aim_enabled = 1;
static float g_cfg_aim_speed = DEFAULT_AIM_SPEED;
static float g_cfg_aim_magnet = DEFAULT_AIM_MAGNET;
static float g_cfg_head_fov = DEFAULT_HEAD_FOV_DEG;
static pthread_t g_control_thread;
static volatile int g_control_started;

extern void km14_set_radar_enabled(int enabled);
extern int km14_get_radar_enabled(void);

typedef struct {
    int aim_enabled;
    int radar_enabled;
    float speed;
    float magnet;
    float fov;
} RuntimeConfig;

static RuntimeConfig config_snapshot(void) {
    RuntimeConfig c;
    pthread_mutex_lock(&g_config_lock);
    c.aim_enabled = g_cfg_aim_enabled;
    c.speed = g_cfg_aim_speed;
    c.magnet = g_cfg_aim_magnet;
    c.fov = g_cfg_head_fov;
    pthread_mutex_unlock(&g_config_lock);
    c.radar_enabled = km14_get_radar_enabled();
    return c;
}

/* Engine interfaces. */
typedef void *(*CreateInterfaceFn)(const char *, int *);
typedef void *(*GetClientEntityFn)(void *, int);
typedef void *(*GetClientNetworkableFn)(void *, int);
typedef int (*GetHighestEntityIndexFn)(void *);
typedef int (*GetLocalPlayerFn)(void *);
typedef int (*GetMaxClientsFn)(void *);
typedef int (*IsInGameFn)(void *);
typedef void *(*GetClientClassFn)(void *);
typedef int (*CreateMoveFn)(void *, float, void *);
typedef void (*TraceRayFn)(void *, const void *, unsigned int, void *, void *);

static GetClientEntityFn g_get_entity;
static GetClientNetworkableFn g_get_networkable;
static GetHighestEntityIndexFn g_get_highest;
static GetLocalPlayerFn g_get_local_player;
static GetMaxClientsFn g_get_max_clients;
static IsInGameFn g_is_in_game;
static TraceRayFn g_trace_ray;
static CreateMoveFn g_original_create_move;

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void log_line(const char *fmt, ...) {
    char msg[1000], line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    struct timespec ts; struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    int n = snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] %s\n",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L, msg);
    __android_log_write(ANDROID_LOG_INFO, TAG, msg);
    pthread_mutex_lock(&g_log_lock);
    (void)mkdir("/storage/emulated/0/alrm", 0775);
    (void)mkdir("/storage/emulated/0/alrm/mod", 0775);
    FILE *fp = fopen(LOG_PATH, "a");
    if (!fp) fp = fopen(FALLBACK_LOG_PATH, "a");
    if (fp) {
        if (n > 0) fwrite(line, 1, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line), fp);
        fflush(fp);
        fclose(fp);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static void log_skip_throttled(const char *reason, const char *fmt, ...) {
    uint64_t now = now_ms();
    if (g_last_skip_log_ms && now - g_last_skip_log_ms < LOG_INTERVAL_MS) return;
    g_last_skip_log_ms = now;
    char detail[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    log_line("ASSIST_SKIP reason=%s %s", reason, detail);
}

static int range_mapped(uintptr_t a, size_t n, int write_access) {
    if (!a || !n || a + n < a) return 0;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512], perms[5];
    unsigned long lo, hi;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) != 3) continue;
        if (a >= (uintptr_t)lo && a + n <= (uintptr_t)hi && perms[0] == 'r' &&
            (!write_access || perms[1] == 'w')) { found = 1; break; }
    }
    fclose(fp);
    return found;
}

static void **vtable_of(void *obj) { return obj ? *(void ***)obj : NULL; }
static void *vfunc(void *obj, unsigned slot) {
    void **vt = vtable_of(obj);
    return vt ? vt[slot] : NULL;
}
static void *open_loaded(const char *name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }

static int resolve_interfaces(void) {
    if (g_interfaces_ready) return 1;
    if (!g_client_handle) g_client_handle = open_loaded("libclient.so");
    if (!g_engine_handle) g_engine_handle = open_loaded("libengine.so");
    if (!g_client_handle || !g_engine_handle) return 0;
    CreateInterfaceFn cf = (CreateInterfaceFn)dlsym(g_client_handle, "CreateInterface");
    CreateInterfaceFn ef = (CreateInterfaceFn)dlsym(g_engine_handle, "CreateInterface");
    if (!cf || !ef) { log_line("INTERFACE_FAIL factory client=%p engine=%p", (void *)cf, (void *)ef); return 0; }
    int e1 = -1, e2 = -1, e3 = -1;
    g_entity_list = cf("VClientEntityList003", &e1);
    g_engine_client = ef("VEngineClient014", &e2);
    g_engine_trace = ef("EngineTraceClient003", &e3);
    if (!g_entity_list || !g_engine_client) {
        log_line("INTERFACE_WAIT entityList=%p rc=%d engine=%p rc=%d trace=%p rc=%d",
            g_entity_list, e1, g_engine_client, e2, g_engine_trace, e3);
        return 0;
    }
    g_get_networkable = (GetClientNetworkableFn)vfunc(g_entity_list, 0);
    g_get_entity = (GetClientEntityFn)vfunc(g_entity_list, 3);
    g_get_highest = (GetHighestEntityIndexFn)vfunc(g_entity_list, 6);
    g_get_local_player = (GetLocalPlayerFn)vfunc(g_engine_client, 12);
    g_get_max_clients = (GetMaxClientsFn)vfunc(g_engine_client, 21);
    g_is_in_game = (IsInGameFn)vfunc(g_engine_client, 26);
    g_trace_ray = g_engine_trace ? (TraceRayFn)vfunc(g_engine_trace, TRACE_RAY_VTABLE_SLOT) : NULL;
    if (!g_get_networkable || !g_get_entity || !g_get_highest || !g_get_local_player ||
        !g_get_max_clients || !g_is_in_game) {
        log_line("INTERFACE_FAIL vfunc net=%p ent=%p high=%p local=%p max=%p ingame=%p",
            (void *)g_get_networkable, (void *)g_get_entity, (void *)g_get_highest,
            (void *)g_get_local_player, (void *)g_get_max_clients, (void *)g_is_in_game);
        return 0;
    }
    Dl_info di; memset(&di, 0, sizeof(di));
    if (dladdr((void *)cf, &di) != 0) g_client_base = (uintptr_t)di.dli_fbase;
    g_interfaces_ready = 1;
    log_line("INTERFACES_OK base=%p entityList=%p engine=%p trace=%p traceSlot4_clipRayToCollideable=%p traceSlot5_traceRay=%p traceRaySlot=%u rcTrace=%d localSlot=12 entitySlot=3",
        (void *)g_client_base, g_entity_list, g_engine_client, g_engine_trace,
        g_engine_trace ? vfunc(g_engine_trace, 4) : NULL,
        (void *)g_trace_ray, TRACE_RAY_VTABLE_SLOT, e3);
    return 1;
}

static const char *entity_class_name(int idx) {
    void *nw = g_get_networkable ? g_get_networkable(g_entity_list, idx) : NULL;
    if (!nw) return NULL;
    void *fc = vfunc(nw, 2);
    if (!fc) return NULL;
    void *cl = ((GetClientClassFn)fc)(nw);
    if (!cl) return NULL;
    /* Both pointers are returned by engine virtual methods; avoid /proc/maps parsing in hot path. */
    const char *name = *(const char **)((const uint8_t *)cl + 0x10);
    return name;
}

static int is_player_class(const char *n) {
    return n && (strcmp(n, "C_CSPlayer") == 0 || strcmp(n, "CCSPlayer") == 0 || strcmp(n, "CBasePlayer") == 0);
}

static int read_entity_data(void *ent, int *health, int *team, int *life,
                            float origin[3], float viewoff[3]) {
    if (!ent || (uintptr_t)ent < 0x10000u || ((uintptr_t)ent & 0x7u) != 0) return 0;
    const uint8_t *p = (const uint8_t *)ent;
    uint8_t lv = p[LIFE_STATE_OFF]; *life = (int)lv;
    memcpy(health, p + HEALTH_OFF, sizeof(*health));
    memcpy(team, p + TEAM_OFF, sizeof(*team));
    memcpy(origin, p + ORIGIN_OFF, 3u * sizeof(float));
    memcpy(viewoff, p + VIEW_OFFSET_OFF, 3u * sizeof(float));
    return 1;
}

static int finite_vec3(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
        fabsf(v[0]) < 32768.0f && fabsf(v[1]) < 32768.0f && fabsf(v[2]) < 32768.0f;
}
static float normalize_yaw(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
static float clampf_local(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static void calc_angle(const float from[3], const float to[3], float out[2]) {
    float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    float horiz = sqrtf(dx * dx + dy * dy);
    out[0] = atan2f(-dz, horiz) * (180.0f / 3.14159265358979323846f);
    out[1] = atan2f(dy, dx) * (180.0f / 3.14159265358979323846f);
}

/* Ray_t layout in Source SDK 2013 (four 16-byte aligned vectors + transform + flags). */
typedef struct { float x, y, z, pad; } VecAligned16;
typedef struct {
    VecAligned16 start;
    VecAligned16 delta;
    VecAligned16 start_offset;
    VecAligned16 extents;
    const void *world_axis_transform;
    uint8_t is_ray;
    uint8_t is_swept;
    uint8_t pad[6];
} Ray_t_KM;
_Static_assert(sizeof(Ray_t_KM) == 0x50, "Ray_t size mismatch");

typedef int (*ShouldHitEntityFn)(void *, void *, int);
typedef int (*GetTraceTypeFn)(void *);
typedef struct { ShouldHitEntityFn should_hit; GetTraceTypeFn get_type; } TraceFilterVTable;
typedef struct { const TraceFilterVTable *vtable; void *skip_entity; } TraceFilterSimple;
static int filter_should_hit(void *self, void *entity, int mask) {
    (void)mask;
    TraceFilterSimple *f = (TraceFilterSimple *)self;
    return entity != f->skip_entity;
}
static int filter_get_type(void *self) { (void)self; return 0; } /* TRACE_EVERYTHING */
static const TraceFilterVTable g_filter_vtable = { filter_should_hit, filter_get_type };

/* CGameTrace fields used here: fraction @ +0x2C, m_pEnt @ +0x58 on ARM64. */
static int trace_visible(void *local, void *target, const float from[3], const float to[3],
                         float *fraction_out, void **hit_out) {
    if (fraction_out) *fraction_out = -1.0f;
    if (hit_out) *hit_out = NULL;
    if (!g_trace_ray || !g_engine_trace) return -1;
    Ray_t_KM ray; memset(&ray, 0, sizeof(ray));
    ray.start.x = from[0]; ray.start.y = from[1]; ray.start.z = from[2];
    ray.delta.x = to[0] - from[0]; ray.delta.y = to[1] - from[1]; ray.delta.z = to[2] - from[2];
    ray.is_ray = 1;
    ray.is_swept = (fabsf(ray.delta.x) + fabsf(ray.delta.y) + fabsf(ray.delta.z)) > 0.001f;
    TraceFilterSimple filter = { &g_filter_vtable, local };
    __attribute__((aligned(16))) uint8_t tr[0x80];
    memset(tr, 0, sizeof(tr));
    if (!g_trace_test_logged) {
        log_line("TRACE_BEGIN iface=%p traceRay=%p local=%p target=%p start=(%.1f,%.1f,%.1f) end=(%.1f,%.1f,%.1f) raySize=%zu traceSize=%zu trace_slot=%u",
            g_engine_trace, (void *)g_trace_ray, local, target, from[0], from[1], from[2],
            to[0], to[1], to[2], sizeof(ray), sizeof(tr), TRACE_RAY_VTABLE_SLOT);
    }
    g_trace_ray(g_engine_trace, &ray, TRACE_MASK_SHOT, &filter, tr);
    if (!g_trace_test_logged) { log_line("TRACE_RETURN iface=%p traceRay=%p", g_engine_trace, (void *)g_trace_ray); g_trace_test_logged = 1; }
    float fraction = -1.0f;
    void *hit = NULL;
    memcpy(&fraction, tr + 0x2C, sizeof(fraction));
    memcpy(&hit, tr + 0x58, sizeof(hit));
    if (fraction_out) *fraction_out = fraction;
    if (hit_out) *hit_out = hit;
    int valid_fraction = isfinite(fraction) && fraction >= 0.0f && fraction <= 1.001f;
    int visible = valid_fraction && hit == target && fraction > 0.0f;
    /* Keep the strict occlusion rule; log early results to expose ABI/layout issues. */
    if (g_trace_result_logs < 8) {
        log_line("TRACE_RESULT n=%d target=%p hit=%p fraction=%.5f valid_fraction=%d hit_equals_target=%d visible=%d mask=0x%X trace_slot=%u fraction_off=0x2C entity_off=0x58",
            g_trace_result_logs + 1, target, hit, fraction, valid_fraction,
            hit == target, visible, TRACE_MASK_SHOT, TRACE_RAY_VTABLE_SLOT);
        ++g_trace_result_logs;
    }
    if (!valid_fraction) return -1;
    /* Fail closed: a high fraction alone is not proof of visibility. */
    return visible ? 1 : 0;
}

static void assist_command(void *cmd, float sample_time) {
    if (!g_interfaces_ready) { log_skip_throttled("interfaces_not_ready", "cmd=%p", cmd); return; }
    if (!cmd) { log_skip_throttled("cmd_null", "sample=%.5f", sample_time); return; }
    if (!g_engine_client || !g_is_in_game) {
        log_skip_throttled("engine_state_fn_missing", "engine=%p isInGame=%p", g_engine_client, (void *)g_is_in_game);
        return;
    }
    int in_game = g_is_in_game(g_engine_client);
    if (!in_game) { log_skip_throttled("not_in_game", "engine=%p", g_engine_client); return; }
    RuntimeConfig cfg = config_snapshot();
    if (!cfg.aim_enabled) {
        uint64_t now = now_ms();
        if (now - g_last_log_ms >= LOG_INTERVAL_MS) {
            log_line("ASSIST_DISABLED runtime_config=1 radar=%d speed=%.2f magnet=%.2f fov=%.1f",
                cfg.radar_enabled, cfg.speed, cfg.magnet, cfg.fov);
            g_last_log_ms = now;
        }
        return;
    }

    int local_index = g_get_local_player(g_engine_client);
    int max_clients = g_get_max_clients(g_engine_client);
    if (local_index < 1 || local_index > MAX_PLAYER_SLOTS) {
        log_skip_throttled("local_index_invalid", "local=%d", local_index); return;
    }
    if (max_clients < 1) {
        log_skip_throttled("max_clients_invalid", "maxClients=%d", max_clients); return;
    }
    if (max_clients > MAX_PLAYER_SLOTS) max_clients = MAX_PLAYER_SLOTS;
    void *local = g_get_entity(g_entity_list, local_index);
    if (!local) { log_skip_throttled("local_entity_null", "index=%d", local_index); return; }

    int local_health = 0, local_team = 0, local_life = 0;
    float local_origin[3] = {0}, local_view[3] = {0};
    if (!read_entity_data(local, &local_health, &local_team, &local_life,
                          local_origin, local_view)) {
        log_line("LOCAL_REJECT index=%d ptr=%p range_check_failed", local_index, local);
        return;
    }
    if (local_life != 0 || local_health <= 0 || local_health > 200 || !finite_vec3(local_origin) || !finite_vec3(local_view)) {
        log_skip_throttled("local_state_invalid", "life=%d hp=%d team=%d originFinite=%d viewFinite=%d",
            local_life, local_health, local_team, finite_vec3(local_origin), finite_vec3(local_view));
        return;
    }

    float before_pitch = 0.0f, before_yaw = 0.0f, roll = 0.0f;
    memcpy(&before_pitch, (uint8_t *)cmd + CMD_PITCH_OFF, sizeof(float));
    memcpy(&before_yaw, (uint8_t *)cmd + CMD_YAW_OFF, sizeof(float));
    memcpy(&roll, (uint8_t *)cmd + CMD_ROLL_OFF, sizeof(float));
    if (!isfinite(before_pitch) || !isfinite(before_yaw) || !isfinite(roll) || fabsf(before_pitch) > 180.0f || fabsf(before_yaw) > 720.0f) {
        log_line("CMD_REJECT cmd=%p angles=(%.3f,%.3f,%.3f)", cmd, before_pitch, before_yaw, roll);
        return;
    }

    /* Recoil compensation intentionally removed. The camera/command is only changed
       when a strictly visible enemy is selected inside the configured FOV. */
    float cp = before_pitch, cy = before_yaw;
    float local_eye[3] = {
        local_origin[0] + local_view[0],
        local_origin[1] + local_view[1],
        local_origin[2] + local_view[2]
    };
    int best_idx = -1, best_team = 0, best_health = 0;
    void *best_ent = NULL, *best_hit = NULL;
    float best_error = 999.0f, best_fraction = -1.0f;
    float best_target[3] = {0}, best_angles[2] = {0};
    int scanned = 0, visible_count = 0, blocked_count = 0, invalid_trace_count = 0;
    int highest = g_get_highest(g_entity_list);
    if (highest < 0) highest = 0;
    if (highest >= MAX_ENT_SCAN) highest = MAX_ENT_SCAN - 1;
    if (highest > max_clients + 128) highest = max_clients + 128;

    if (!g_trace_ray) log_skip_throttled("trace_interface_null", "trace=%p engineTrace=%p", (void *)g_trace_ray, g_engine_trace);
    if (g_trace_ray) {
        for (int i = 1; i <= highest; ++i) {
            if (i == local_index || i > max_clients) continue;
            const char *name = entity_class_name(i);
            if (!is_player_class(name)) continue;
            void *ent = g_get_entity(g_entity_list, i);
            if (!ent || ent == local) continue;
            int hp = 0, team = 0, life = 0;
            float org[3] = {0}, vo[3] = {0};
            if (!read_entity_data(ent, &hp, &team, &life, org, vo)) continue;
            if (life != 0 || hp <= 0 || hp > 200 || team < 2 || team > 3 || local_team < 2 || local_team > 3 || team == local_team) continue;
            if (!finite_vec3(org) || !finite_vec3(vo)) continue;
            ++scanned;
            float point[3] = { org[0] + vo[0], org[1] + vo[1], org[2] + vo[2] };
            float target_angle[2]; calc_angle(local_eye, point, target_angle);
            float dp = clampf_local(normalize_yaw(target_angle[0] - cp), -180.0f, 180.0f);
            float dy = normalize_yaw(target_angle[1] - cy);
            float err = sqrtf(dp * dp + dy * dy);
            if (err > cfg.fov || err >= best_error) continue;
            float frac = -1.0f; void *hit = NULL;
            int vis = trace_visible(local, ent, local_eye, point, &frac, &hit);
            if (vis != 1) {
                if (vis == 0) ++blocked_count;
                else ++invalid_trace_count;
                continue;
            }
            ++visible_count;
            best_idx = i; best_ent = ent; best_team = team; best_health = hp;
            best_error = err; best_fraction = frac; best_hit = hit;
            memcpy(best_target, point, sizeof(best_target));
            memcpy(best_angles, target_angle, sizeof(best_angles));
        }
    }

    int aim_applied = 0;
    float after_pitch = cp, after_yaw = cy;
    if (best_idx >= 1) {
        float dp = normalize_yaw(best_angles[0] - cp);
        float dy = normalize_yaw(best_angles[1] - cy);
        float error_deg = sqrtf(dp * dp + dy * dy);
        float dt = (isfinite(sample_time) && sample_time >= 0.001f && sample_time <= 0.1f)
                     ? sample_time : (1.0f / 60.0f);
        float max_step_deg = cfg.speed * dt * 60.0f;
        float requested_step_deg = error_deg * cfg.magnet;
        float step_deg = fminf(requested_step_deg, max_step_deg);
        float alpha = (error_deg > 0.0001f) ? (step_deg / error_deg) : 0.0f;
        after_pitch = clampf_local(cp + dp * alpha, -89.0f, 89.0f);
        after_yaw = normalize_yaw(cy + dy * alpha);
        aim_applied = (fabsf(dp) > 0.01f || fabsf(dy) > 0.01f) && alpha > 0.0f;
    }

    float d_pitch = after_pitch - before_pitch;
    float d_yaw = normalize_yaw(after_yaw - before_yaw);
    if (isfinite(after_pitch) && isfinite(after_yaw) && fabsf(d_pitch) <= 25.0f && fabsf(d_yaw) <= 25.0f) {
        if (aim_applied) {
            memcpy((uint8_t *)cmd + CMD_PITCH_OFF, &after_pitch, sizeof(float));
            memcpy((uint8_t *)cmd + CMD_YAW_OFF, &after_yaw, sizeof(float));
        }
    } else {
        log_line("WRITE_SKIP unreasonable_delta dp=%.3f dy=%.3f before=(%.3f,%.3f) after=(%.3f,%.3f)",
            d_pitch, d_yaw, before_pitch, before_yaw, after_pitch, after_yaw);
        after_pitch = before_pitch; after_yaw = before_yaw;
        aim_applied = 0;
    }

    uint64_t now = now_ms();
    if (now - g_last_log_ms >= LOG_INTERVAL_MS || best_idx != g_last_target) {
        log_line("FRAME local=%d hp=%d team=%d cmd=(%.2f,%.2f)->(%.2f,%.2f) scanned=%d visible=%d blocked=%d invalidTrace=%d selected=%d hit=%p fraction=%.4f error=%.2f aim=%d speed=%.2f magnet=%.2f fov=%.1f recoilControl=OFF",
            local_index, local_health, local_team, before_pitch, before_yaw, after_pitch, after_yaw,
            scanned, visible_count, blocked_count, invalid_trace_count, best_idx, best_hit,
            best_fraction, best_error, aim_applied, cfg.speed, cfg.magnet, cfg.fov);
        log_line("TARGET_SCAN highest=%d maxClients=%d trace=%p target=%p health=%d team=%d point=(%.1f,%.1f,%.1f) targetAngle=(%.2f,%.2f) pointSource=origin+viewOffset headBone=UNVERIFIED visibilityRule=hit_must_equal_target",
            highest, max_clients, (void *)g_trace_ray, best_ent, best_health, best_team,
            best_target[0], best_target[1], best_target[2], best_angles[0], best_angles[1]);
        g_last_log_ms = now;
        g_last_target = best_idx;
    }
}

static char *json_value(char *body, const char *key) {
    char quoted[80];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    char *p = strstr(body, quoted);
    if (!p) return NULL;
    p = strchr(p + strlen(quoted), ':');
    if (!p) return NULL;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static int json_read_bool(char *body, const char *key, int *value) {
    char *p = json_value(body, key);
    if (!p) return 0;
    if (!strncmp(p, "true", 4)) { *value = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *value = 0; return 1; }
    if (*p == '0' || *p == '1') { *value = (*p == '1'); return 1; }
    return 0;
}

static int json_read_float(char *body, const char *key, float *value) {
    char *p = json_value(body, key);
    if (!p) return 0;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p || !isfinite(v)) return 0;
    *value = v;
    return 1;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int read_http_request(int fd, char *buf, size_t cap) {
    size_t used = 0, header_end = 0, content_len = 0;
    buf[0] = '\0';
    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        if (!header_end) {
            char *mark = strstr(buf, "\r\n\r\n");
            if (mark) {
                header_end = (size_t)(mark - buf) + 4u;
                char *cl = strcasestr(buf, "content-length:");
                if (cl && cl < mark) {
                    cl += strlen("content-length:");
                    while (*cl && isspace((unsigned char)*cl)) ++cl;
                    content_len = (size_t)strtoul(cl, NULL, 10);
                    if (content_len > 4096u) return -1;
                }
            }
        }
        if (header_end && used >= header_end + content_len) return (int)used;
    }
    return -1;
}

static void send_json_response(int fd, const char *body) {
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(body));
    if (hn > 0) { (void)send_all(fd, hdr, (size_t)hn); (void)send_all(fd, body, strlen(body)); }
}

static void send_error_response(int fd, int code, const char *reason) {
    char body[256], hdr[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", reason);
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        code, code == 404 ? "Not Found" : "Bad Request", strlen(body));
    if (hn > 0) { (void)send_all(fd, hdr, (size_t)hn); (void)send_all(fd, body, strlen(body)); }
}

static void current_state_json(char *out, size_t cap) {
    RuntimeConfig c = config_snapshot();
    snprintf(out, cap,
        "{\"ok\":true,\"aim_enabled\":%s,\"radar_enabled\":%s,\"speed\":%.2f,\"magnet\":%.2f,\"fov\":%.1f,\"port\":%d,\"recoil_control\":false}",
        c.aim_enabled ? "true" : "false", c.radar_enabled ? "true" : "false",
        c.speed, c.magnet, c.fov, CONTROL_PORT);
}

static void handle_control_client(int fd) {
    char req[CONTROL_MAX_REQUEST];
    int n = read_http_request(fd, req, sizeof(req));
    if (n <= 0) { send_error_response(fd, 400, "bad_request"); return; }
    char method[16] = {0}, path[128] = {0};
    if (sscanf(req, "%15s %127s", method, path) != 2) {
        send_error_response(fd, 400, "bad_request_line"); return;
    }
    if (!strcmp(method, "GET") && (!strcmp(path, "/state") || !strcmp(path, "/health"))) {
        char body[512];
        if (!strcmp(path, "/health")) snprintf(body, sizeof(body), "{\"ok\":true,\"service\":\"km14-control\"}");
        else current_state_json(body, sizeof(body));
        send_json_response(fd, body);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/config")) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_error_response(fd, 400, "missing_body"); return; }
        body += 4;
        int aim = -1, radar = -1;
        float speed, magnet, fov;
        pthread_mutex_lock(&g_config_lock);
        speed = g_cfg_aim_speed; magnet = g_cfg_aim_magnet; fov = g_cfg_head_fov;
        pthread_mutex_unlock(&g_config_lock);
        int got_aim = json_read_bool(body, "aim_enabled", &aim);
        int got_radar = json_read_bool(body, "radar_enabled", &radar);
        int got_speed = json_read_float(body, "speed", &speed);
        int got_magnet = json_read_float(body, "magnet", &magnet);
        int got_fov = json_read_float(body, "fov", &fov);
        if (got_speed) { if (speed < 0.10f) speed = 0.10f; if (speed > 30.0f) speed = 30.0f; }
        if (got_magnet) { if (magnet < 0.0f) magnet = 0.0f; if (magnet > 1.0f) magnet = 1.0f; }
        if (got_fov) { if (fov < 1.0f) fov = 1.0f; if (fov > 45.0f) fov = 45.0f; }
        pthread_mutex_lock(&g_config_lock);
        if (got_aim) g_cfg_aim_enabled = aim;
        if (got_speed) g_cfg_aim_speed = speed;
        if (got_magnet) g_cfg_aim_magnet = magnet;
        if (got_fov) g_cfg_head_fov = fov;
        pthread_mutex_unlock(&g_config_lock);
        if (got_radar) km14_set_radar_enabled(radar);
        RuntimeConfig c = config_snapshot();
        log_line("CONFIG_UPDATE aim=%d radar=%d speed=%.2f magnet=%.2f fov=%.1f updated=%d%d%d%d%d",
            c.aim_enabled, c.radar_enabled, c.speed, c.magnet, c.fov,
            got_aim, got_radar, got_speed, got_magnet, got_fov);
        char out[512]; current_state_json(out, sizeof(out)); send_json_response(fd, out);
        return;
    }
    send_error_response(fd, 404, "not_found");
}

static void *control_server_worker(void *unused) {
    (void)unused;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { log_line("CONTROL_SOCKET_FAIL errno=%d", errno); return NULL; }
    int one = 1; (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(CONTROL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_line("CONTROL_BIND_FAIL port=%d errno=%d", CONTROL_PORT, errno); close(server); return NULL;
    }
    if (listen(server, 8) != 0) {
        log_line("CONTROL_LISTEN_FAIL port=%d errno=%d", CONTROL_PORT, errno); close(server); return NULL;
    }
    log_line("CONTROL_LISTEN_OK address=127.0.0.1 port=%d api=GET:/state,POST:/config", CONTROL_PORT);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; usleep(25000); continue; }
        struct timeval tv = {2, 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_control_client(client);
        close(client);
    }
    close(server);
    return NULL;
}

static void write_abs_branch(uint8_t *out, uintptr_t dst) {
    const uint32_t ldr_x17 = 0x58000051u; /* ldr x17, #8 */
    const uint32_t br_x17 = 0xD61F0220u;  /* br x17 */
    memcpy(out, &ldr_x17, 4); memcpy(out + 4, &br_x17, 4); memcpy(out + 8, &dst, 8);
}

static int create_move_hook(void *self, float sample_time, void *cmd) {
    CreateMoveFn orig = g_original_create_move;
    int result = orig ? orig(self, sample_time, cmd) : 0;

    /* The return value describes CreateMove's status, not whether cmd is valid.
       The previous version incorrectly skipped all assist work when result==0. */
    uint64_t now = now_ms();
    if (!g_last_hook_log_ms || now - g_last_hook_log_ms >= LOG_INTERVAL_MS) {
        int in_game = (g_interfaces_ready && g_engine_client && g_is_in_game)
            ? g_is_in_game(g_engine_client) : -1;
        log_line("CREATE_MOVE_CALL result=%d cmd=%p sample=%.5f orig=%p interfaces_ready=%d inGame=%d hook_busy=%d",
            result, cmd, sample_time, (void *)orig, g_interfaces_ready, in_game, g_hook_busy);
        g_last_hook_log_ms = now;
    }
    if (cmd && !__sync_lock_test_and_set(&g_hook_busy, 1)) {
        assist_command(cmd, sample_time);
        __sync_lock_release(&g_hook_busy);
    }
    return result;
}

static int install_create_move_hook(void) {
    if (g_hook_installed) return 1;
    if (!g_client_base) return 0;
    uint8_t *target = (uint8_t *)(g_client_base + CREATE_MOVE_RVA);
    static const uint32_t expected[4] = { 0xFC1E0FE8u, 0xA900FBFDu, 0xF9000FF3u, 0x910023FDu };
    if (!range_mapped((uintptr_t)target, 16, 0)) {
        log_line("HOOK_SKIP target_unmapped target=%p", target); return -1;
    }
    uint32_t actual[4]; memcpy(actual, target, sizeof(actual));
    if (memcmp(actual, expected, sizeof(expected)) != 0) {
        log_line("HOOK_SKIP signature_mismatch base=%p target=%p actual=%08x,%08x,%08x,%08x expected=%08x,%08x,%08x,%08x",
            (void *)g_client_base, target, actual[0], actual[1], actual[2], actual[3],
            expected[0], expected[1], expected[2], expected[3]);
        return -1;
    }
    long ps = sysconf(_SC_PAGESIZE); if (ps <= 0) ps = 4096;
    void *tramp = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) { log_line("HOOK_FAIL trampoline_mmap errno=%d", errno); return 0; }
    memcpy(tramp, target, 16);
    write_abs_branch((uint8_t *)tramp + 16, (uintptr_t)target + 16);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 32);
    if (mprotect(tramp, (size_t)ps, PROT_READ | PROT_EXEC) != 0) {
        log_line("HOOK_FAIL trampoline_mprotect errno=%d", errno); munmap(tramp, (size_t)ps); return 0;
    }
    g_original_create_move = (CreateMoveFn)tramp;
    uint8_t patch[16]; write_abs_branch(patch, (uintptr_t)create_move_hook);
    uintptr_t page = (uintptr_t)target & ~((uintptr_t)ps - 1u);
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        int err = errno;
        if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
            log_line("HOOK_FAIL target_mprotect rwx=%d rw=%d", err, errno);
            munmap(tramp, (size_t)ps); g_original_create_move = NULL; return 0;
        }
        log_line("HOOK_INFO page_temporarily_rw errnoRWX=%d", err);
    }
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char *)target, (char *)target + sizeof(patch));
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_EXEC) != 0)
        log_line("HOOK_WARN restore_rx errno=%d", errno);
    __sync_synchronize();
    g_hook_installed = 1;
    log_line("HOOK_OK target=libclient.so+0x8B0844 targetVA=%p trampoline=%p mode=STRICT_VISIBLE_HEAD_ASSIST_NO_RCS_V1_3_TRACE_SLOT5",
        target, tramp);
    return 1;
}

static void *worker(void *unused) {
    (void)unused;
    RuntimeConfig c = config_snapshot();
    log_line("START pid=%ld mode=STRICT_VISIBLE_HEAD_ASSIST_NO_RCS_V1_3_TRACE_SLOT5 aim=%d radar=%d speed=%.2f magnet=%.2f fov=%.1f controlPort=%d traceMask=0x%X",
        (long)getpid(), c.aim_enabled, c.radar_enabled, c.speed, c.magnet, c.fov, CONTROL_PORT, TRACE_MASK_SHOT);
    for (int i = 0; i < 240; ++i) {
        if (resolve_interfaces()) {
            int r = install_create_move_hook();
            if (r == 1) return NULL;
            if (r < 0) { log_line("STOP no_patch_due_to_signature_or_mapping_mismatch"); return NULL; }
        }
        usleep(250000);
    }
    log_line("HOOK_TIMEOUT interfaces=%d base=%p", g_interfaces_ready, (void *)g_client_base);
    return NULL;
}

__attribute__((constructor)) static void on_load(void) {
    log_line("CONSTRUCTOR pid=%ld uid=%ld", (long)getpid(), (long)getuid());
    if (!g_control_started) {
        int cr = pthread_create(&g_control_thread, NULL, control_server_worker, NULL);
        if (cr == 0) { (void)pthread_detach(g_control_thread); g_control_started = 1; }
        else log_line("CONTROL_THREAD_FAIL rc=%d", cr);
    }
    pthread_t t;
    pthread_attr_t a;
    if (pthread_attr_init(&a) == 0) {
        (void)pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&t, &a, worker, NULL);
        pthread_attr_destroy(&a);
        if (rc != 0) log_line("THREAD_FAIL rc=%d", rc);
    } else {
        int rc = pthread_create(&t, NULL, worker, NULL);
        if (rc == 0) (void)pthread_detach(t); else log_line("THREAD_FAIL rc=%d", rc);
    }
}

__attribute__((visibility("default"))) const char *km14_aimassist_version(void) {
    return "KM14 AimAssist v1.2: strict target-hit visibility, no RCS, localhost control port 28765";
}
