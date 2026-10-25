/*
 * KM14 V2.3 — performance-tuned single-source Aim Assist + spotted-flag radar.
 * Uses animated Studio head-bone matrices (including crouched poses), and a
 * fail-closed single-pass world/entity/static-prop visibility test. Keeps the
 * existing panel API on 127.0.0.1:28765; the panel APK does not need rebuilding.
 * Static addresses and signatures are tied to the supplied libclient.so build.
 * SetupBones is signature-validated; visibility fails closed unless world and filtered entity/prop rays are both clear.
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

#define TAG "KM14AimV22"
#define LOG_PATH "/storage/emulated/0/alrm/mod/logger.log"
#define FALLBACK_LOG_PATH "/data/user/0/com.valvesoftware.source.client/files/km14_aimassist_v2_2.log"
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
#define TRACE_MASK_VISIBLE      0xFFFFFFFFu /* MASK_ALL: include every collision-content class for occlusion */
#define TRACE_RAY_VTABLE_SLOT   4u
#define RVA_SETUP_BONES         0x0081D334UL /* confirmed SetupBones entry in supplied libclient.so */
#define ENGINE_TIME_SLOT        14u
#define STUDIOHDR_WRAPPER_OFF   0x0BD0UL
#define STUDIOHDR_MAGIC         0x54534449u
#define STUDIO_BONE_LIMIT       128
#define STUDIO_BONE_SIZE        0xD8UL
#define BONE_USED_BY_ANYTHING   0x0007FF00
#define HEAD_NAME_LIMIT         64
#define TRACE_EVERYTHING        0
#define TRACE_WORLD_ONLY        1
#define TRACE_EVERYTHING_FILTER_PROPS 3
#define HEAD_BONE_CACHE_CAP 64u
#define TRACE_RESULT_SIZE       0x80UL
#define TRACE_FRACTION_OFF      0x2CUL
#define TRACE_SOLID_OFF         0x36UL
#define TRACE_STARTSOLID_OFF    0x37UL
#define TRACE_ENTITY_OFF        0x58UL
#define LOCAL_EYE_HEIGHT        64.0f

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_client_handle, *g_engine_handle;
static void *g_entity_list, *g_engine_client, *g_engine_trace;
static uintptr_t g_client_base;
static volatile int g_interfaces_ready;
static volatile int g_hook_installed;
static volatile int g_hook_busy;
static uint64_t g_last_log_ms;
static uint64_t g_last_hook_log_ms;
static uint64_t g_last_skip_log_ms;
static pthread_mutex_t g_config_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cfg_aim_enabled = 1;
static float g_cfg_aim_speed = DEFAULT_AIM_SPEED;
static float g_cfg_aim_magnet = DEFAULT_AIM_MAGNET;
static float g_cfg_head_fov = DEFAULT_HEAD_FOV_DEG;
static pthread_t g_control_thread;
static volatile int g_control_started;

/* Radar API is implemented later in this same translation unit; strong symbols
 * ensure the existing overlay can still resolve both functions at dlopen time. */
extern void km14_set_radar_enabled(int enabled);
extern int km14_get_radar_enabled(void);

static void radar_api_set_enabled(int enabled) {
    km14_set_radar_enabled(enabled ? 1 : 0);
}

static int radar_api_get_enabled(void) {
    return km14_get_radar_enabled() ? 1 : 0;
}

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
    c.radar_enabled = radar_api_get_enabled();
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
typedef struct { float m[3][4]; } matrix3x4_t;
typedef int (*SetupBonesFn)(void *, matrix3x4_t *, int, int, float);
typedef float (*EngineTimeFn)(void *);

static GetClientEntityFn g_get_entity;
static GetClientNetworkableFn g_get_networkable;
static GetHighestEntityIndexFn g_get_highest;
static GetLocalPlayerFn g_get_local_player;
static GetMaxClientsFn g_get_max_clients;
static IsInGameFn g_is_in_game;
static TraceRayFn g_trace_ray;
static SetupBonesFn g_setup_bones;
static EngineTimeFn g_engine_time;
static unsigned g_head_position_logs;
static unsigned g_visibility_logs;
static unsigned g_world_probe_logs;
static uint64_t g_world_probe_last_ms;
static float g_world_probe_last_eye[3];
static int g_world_probe_last_result = -1;
typedef struct {
    uintptr_t entity;
    uintptr_t wrapper;
    uintptr_t hdr;
    int bone_index;
    int bone_count;
    char bone_name[HEAD_NAME_LIMIT];
} HeadBoneCacheEntry;
static HeadBoneCacheEntry g_head_bone_cache[HEAD_BONE_CACHE_CAP];
static unsigned g_head_bone_cache_next;
static CreateMoveFn g_original_create_move;
static int km_range_mapped(uintptr_t address, size_t length, int need_write);

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

static int range_executable(uintptr_t address, size_t length) {
    if (!address || !length || address + length < address) return 0;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512], perms[5];
    unsigned long lo, hi;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) != 3) continue;
        if (address >= (uintptr_t)lo && address + length <= (uintptr_t)hi &&
            perms[0] == 'r' && perms[2] == 'x') { found = 1; break; }
    }
    fclose(fp);
    return found;
}

/* Verified against the uploaded arm64-v8a/libclient.so. At RVA 0x81D334,
 * the entry begins with these four instructions and accepts:
 * x0=this, x1=matrix3x4_t*, w2=maxBones, w3=boneMask, s0=currentTime.
 * The former RVA 0x8BCF20 was inside a matrix-multiply helper, not SetupBones. */
static int setup_bones_entry_valid(uintptr_t address) {
    static const uint32_t expected[4] = {
        0xFC190FE8u, 0xA9017BFDu, 0xA9026FFCu, 0xA90367FAu
    };
    uint32_t actual[4] = {0};
    if (!range_executable(address, sizeof(actual))) {
        log_line("SETUP_BONES_REJECT address=%p reason=not_executable", (void *)address);
        return 0;
    }
    memcpy(actual, (const void *)address, sizeof(actual));
    if (memcmp(actual, expected, sizeof(expected)) != 0) {
        log_line("SETUP_BONES_REJECT address=%p reason=signature_mismatch actual=%08x,%08x,%08x,%08x expected=%08x,%08x,%08x,%08x",
            (void *)address, actual[0], actual[1], actual[2], actual[3],
            expected[0], expected[1], expected[2], expected[3]);
        return 0;
    }
    return 1;
}

static int resolve_interfaces(void) {
    if (g_interfaces_ready) return 1;
    if (!g_client_handle) g_client_handle = open_loaded("libclient.so");
    if (!g_engine_handle) g_engine_handle = open_loaded("libengine.so");
    if (!g_client_handle || !g_engine_handle) return 0;
    CreateInterfaceFn cf = (CreateInterfaceFn)dlsym(g_client_handle, "CreateInterface");
    CreateInterfaceFn ef = (CreateInterfaceFn)dlsym(g_engine_handle, "CreateInterface");
    if (!cf || !ef) { log_line("INTERFACE_FAIL factory client=%p engine=%p", (void *)cf, (void *)ef); return 0; }
    int e1 = -1, e2 = -1;
    int e3 = -1;
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
    /* Use the EngineTraceClient003 slot from the reference v11 implementation.
       The mobile Ray_t layout below is essential; the stock transform-pointer layout is wrong here. */
    g_trace_ray = g_engine_trace ? (TraceRayFn)vfunc(g_engine_trace, TRACE_RAY_VTABLE_SLOT) : NULL;
    if (!g_get_networkable || !g_get_entity || !g_get_highest || !g_get_local_player ||
        !g_get_max_clients || !g_is_in_game) {
        log_line("INTERFACE_FAIL vfunc net=%p ent=%p high=%p local=%p max=%p ingame=%p",
            (void *)g_get_networkable, (void *)g_get_entity, (void *)g_get_highest,
            (void *)g_get_local_player, (void *)g_get_max_clients, (void *)g_is_in_game);
        return 0;
    }
    if (!g_engine_trace || !g_trace_ray) {
        log_line("INTERFACE_WAIT trace=%p traceRay=%p slot=%u rcTrace=%d",
            g_engine_trace, (void *)g_trace_ray, TRACE_RAY_VTABLE_SLOT, e3);
        return 0;
    }
    Dl_info di; memset(&di, 0, sizeof(di));
    if (dladdr((void *)cf, &di) != 0) g_client_base = (uintptr_t)di.dli_fbase;
    g_setup_bones = NULL;
    if (g_client_base) {
        uintptr_t setup_address = g_client_base + RVA_SETUP_BONES;
        if (setup_bones_entry_valid(setup_address)) {
            g_setup_bones = (SetupBonesFn)setup_address;
            log_line("SETUP_BONES_API_OK module=libclient.so rva=0x%lx address=%p validated_signature=1",
                (unsigned long)RVA_SETUP_BONES, (void *)setup_address);
        }
    }
    g_engine_time = (EngineTimeFn)vfunc(g_engine_client, ENGINE_TIME_SLOT);
    /* Validate these pointers once at interface setup, not in every target scan. */
    if (g_engine_time && !range_executable((uintptr_t)g_engine_time, 4)) g_engine_time = NULL;
    g_interfaces_ready = 1;
    log_line("INTERFACES_OK base=%p entityList=%p engine=%p trace=%p traceRay=%p traceSlot=%u localSlot=12 entitySlot=3 teamOffset=0x%X setupBones=%p engineTime=%p timeSlot=%u",
        (void *)g_client_base, g_entity_list, g_engine_client, g_engine_trace,
        (void *)g_trace_ray, TRACE_RAY_VTABLE_SLOT, TEAM_OFF, (void *)g_setup_bones,
        (void *)g_engine_time, ENGINE_TIME_SLOT);
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

/* This mobile engine build stores Ray_t flags at +0x40/+0x41 and has no
 * world-axis pointer in that area. Keep this layout from the working v11 source. */
typedef struct __attribute__((aligned(16))) {
    float start[4];
    float delta[4];
    float start_offset[4];
    float extents[4];
    uint8_t is_ray;
    uint8_t is_swept;
    uint8_t pad[14];
} SourceRay;
_Static_assert(sizeof(SourceRay) == 0x50, "mobile Ray_t layout mismatch");
_Static_assert(__builtin_offsetof(SourceRay, is_ray) == 0x40, "Ray_t is_ray offset mismatch");
_Static_assert(__builtin_offsetof(SourceRay, is_swept) == 0x41, "Ray_t is_swept offset mismatch");

typedef int (*ShouldHitEntityFn)(void *, void *, int);
typedef int (*GetTraceTypeFn)(void *);
typedef struct { ShouldHitEntityFn should_hit; GetTraceTypeFn get_type; } TraceFilterVTable;
typedef struct {
    uintptr_t vtable;
    uintptr_t skip_entity;
    uintptr_t skip_target;
    int trace_type;
    int reserved;
} TraceFilterObject;
_Static_assert(sizeof(TraceFilterObject) == 0x20, "TraceFilter layout mismatch");
_Static_assert(__builtin_offsetof(TraceFilterObject, skip_target) == 0x10, "TraceFilter target offset mismatch");
_Static_assert(__builtin_offsetof(TraceFilterObject, trace_type) == 0x18, "TraceFilter type offset mismatch");

static int filter_should_hit(void *self, void *entity, int mask) {
    (void)mask;
    const TraceFilterObject *filter = (const TraceFilterObject *)self;
    uintptr_t candidate = (uintptr_t)entity;
    return !filter || (candidate != filter->skip_entity && candidate != filter->skip_target);
}
static int filter_get_type(void *self) {
    const TraceFilterObject *filter = (const TraceFilterObject *)self;
    return filter ? filter->trace_type : TRACE_EVERYTHING;
}
static const TraceFilterVTable g_filter_vtable = { filter_should_hit, filter_get_type };

static int read_ptr_checked(uintptr_t address, uintptr_t *out) {
    if (!out || !km_range_mapped(address, sizeof(uintptr_t), 0)) return 0;
    memcpy(out, (const void *)address, sizeof(*out));
    return 1;
}
static int read_i32_checked(uintptr_t address, int32_t *out) {
    if (!out || !km_range_mapped(address, sizeof(int32_t), 0)) return 0;
    memcpy(out, (const void *)address, sizeof(*out));
    return 1;
}
static int read_u32_checked(uintptr_t address, uint32_t *out) {
    if (!out || !km_range_mapped(address, sizeof(uint32_t), 0)) return 0;
    memcpy(out, (const void *)address, sizeof(*out));
    return 1;
}

static int copy_studio_string(uintptr_t address, uintptr_t header_end, char out[HEAD_NAME_LIMIT]) {
    if (!out || address < 0x10000UL || address >= header_end) return 0;
    memset(out, 0, HEAD_NAME_LIMIT);
    for (size_t i = 0; i < HEAD_NAME_LIMIT - 1; ++i) {
        uintptr_t current = address + i;
        /* find_head_bone validates the complete studio header range before reading strings. */
        if (current < address || current >= header_end) return 0;
        char c = 0;
        memcpy(&c, (const void *)current, 1);
        out[i] = c;
        if (c == '\0') return i > 0;
    }
    out[HEAD_NAME_LIMIT - 1] = '\0';
    return 0;
}

static int head_name_score(const char *name) {
    if (!name || !*name) return 0;
    char lower[HEAD_NAME_LIMIT];
    size_t n = 0;
    for (; name[n] && n < sizeof(lower) - 1; ++n) {
        char c = name[n];
        lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    if (strcmp(lower, "head") == 0) return 120;
    if (strstr(lower, "bip01_head1") || strstr(lower, "bip_head1")) return 110;
    if (strstr(lower, "head") && !strstr(lower, "attachment") &&
        !strstr(lower, "head_end") && !strstr(lower, "headend") &&
        !strstr(lower, "eyelid") && !strstr(lower, "camera")) return 90;
    return 0;
}

static int find_head_bone(uintptr_t entity, int *index_out, int *count_out,
                          char name_out[HEAD_NAME_LIMIT]) {
    if (!entity || !index_out || !count_out || !name_out) return 0;

    /* Fast path: the engine-provided entity and its studio wrapper remain stable
       between frames. Cache the expensive studio-header walk per entity/model. */
    uintptr_t wrapper_fast = 0;
    memcpy(&wrapper_fast, (const void *)(entity + STUDIOHDR_WRAPPER_OFF), sizeof(wrapper_fast));
    if (wrapper_fast >= 0x10000UL && (wrapper_fast & 0x7u) == 0) {
        for (unsigned c = 0; c < HEAD_BONE_CACHE_CAP; ++c) {
            HeadBoneCacheEntry *entry = &g_head_bone_cache[c];
            if (entry->entity != entity) continue;
            if (entry->wrapper == wrapper_fast) {
                uintptr_t hdr_fast = 0;
                memcpy(&hdr_fast, (const void *)wrapper_fast, sizeof(hdr_fast));
                if (hdr_fast == entry->hdr && entry->bone_index >= 0 &&
                    entry->bone_index < entry->bone_count) {
                    *index_out = entry->bone_index;
                    *count_out = entry->bone_count;
                    memcpy(name_out, entry->bone_name, HEAD_NAME_LIMIT);
                    name_out[HEAD_NAME_LIMIT - 1] = '\0';
                    return 1;
                }
            }
            memset(entry, 0, sizeof(*entry));
            break;
        }
    }

    uintptr_t wrapper = 0, hdr = 0;
    int32_t length = 0, count = 0, bone_index = 0;
    if (!read_ptr_checked(entity + STUDIOHDR_WRAPPER_OFF, &wrapper) || !wrapper ||
        !read_ptr_checked(wrapper, &hdr) || !hdr || !km_range_mapped(hdr, 0xA8, 0)) return 0;

    uint32_t id = 0;
    if (!read_u32_checked(hdr, &id) || id != STUDIOHDR_MAGIC ||
        !read_i32_checked(hdr + 0x4C, &length) ||
        !read_i32_checked(hdr + 0x9C, &count) ||
        !read_i32_checked(hdr + 0xA0, &bone_index)) return 0;
    if (length < 0xA8 || length > 0x1000000 || count <= 0 ||
        count > STUDIO_BONE_LIMIT || bone_index <= 0) return 0;
    uint64_t bone_bytes = (uint64_t)(uint32_t)count * STUDIO_BONE_SIZE;
    if ((uint64_t)(uint32_t)bone_index + bone_bytes > (uint64_t)(uint32_t)length) return 0;
    uintptr_t records = hdr + (uintptr_t)bone_index;
    uintptr_t header_end = hdr + (uintptr_t)length;
    if (records < hdr || header_end < hdr || !km_range_mapped(hdr, (size_t)length, 0)) return 0;

    int best_index = -1, best_score = 0;
    char best_name[HEAD_NAME_LIMIT] = {0};
    for (int i = 0; i < count; ++i) {
        uintptr_t record = records + (uintptr_t)i * STUDIO_BONE_SIZE;
        int32_t name_relative = 0;
        memcpy(&name_relative, (const void *)record, sizeof(name_relative));
        if (name_relative <= 0) continue;
        uintptr_t name_address = (uintptr_t)((int64_t)record + (int64_t)name_relative);
        if (name_address < hdr || name_address >= header_end) continue;
        char bone_name[HEAD_NAME_LIMIT];
        if (!copy_studio_string(name_address, header_end, bone_name)) continue;
        int score = head_name_score(bone_name);
        if (score > best_score) {
            best_score = score;
            best_index = i;
            memcpy(best_name, bone_name, sizeof(best_name));
            if (score >= 120) break;
        }
    }
    if (best_index < 0) return 0;
    *index_out = best_index;
    *count_out = count;
    memcpy(name_out, best_name, HEAD_NAME_LIMIT);
    name_out[HEAD_NAME_LIMIT - 1] = '\0';

    HeadBoneCacheEntry *entry = &g_head_bone_cache[g_head_bone_cache_next++ % HEAD_BONE_CACHE_CAP];
    memset(entry, 0, sizeof(*entry));
    entry->entity = entity;
    entry->wrapper = wrapper;
    entry->hdr = hdr;
    entry->bone_index = best_index;
    entry->bone_count = count;
    memcpy(entry->bone_name, best_name, HEAD_NAME_LIMIT);
    entry->bone_name[HEAD_NAME_LIMIT - 1] = '\0';
    return 1;
}

static int get_head_position(uintptr_t entity, const float entity_origin[3],
                             float out[3], int *bone_index_out,
                             char name_out[HEAD_NAME_LIMIT]) {
    if (!g_setup_bones || !g_engine_time || !g_engine_client || !entity || !entity_origin || !out) return 0;
    int bone_index = -1, bone_count = 0;
    char bone_name[HEAD_NAME_LIMIT] = {0};
    if (!find_head_bone(entity, &bone_index, &bone_count, bone_name)) return 0;
    float now = g_engine_time(g_engine_client);
    if (!isfinite(now) || now < 0.0f || now > 1.0e8f) return 0;
    matrix3x4_t matrices[STUDIO_BONE_LIMIT];
    memset(matrices, 0, sizeof(matrices));
    int ok = g_setup_bones((void *)entity, matrices, STUDIO_BONE_LIMIT, BONE_USED_BY_ANYTHING, now);
    if (!ok || bone_index < 0 || bone_index >= bone_count) return 0;
    out[0] = matrices[bone_index].m[0][3];
    out[1] = matrices[bone_index].m[1][3];
    out[2] = matrices[bone_index].m[2][3];
    for (int k = 0; k < 3; ++k)
        if (!isfinite(out[k]) || fabsf(out[k]) > 1000000.0f) return 0;
    float dx = out[0] - entity_origin[0];
    float dy = out[1] - entity_origin[1];
    float dz = out[2] - entity_origin[2];
    float bone_offset_len = sqrtf(dx * dx + dy * dy + dz * dz);
    /* This validates the head-bone offset relative to the target origin, NOT player distance.
       There is intentionally no local-to-target distance cap. */
    if (!isfinite(bone_offset_len) || bone_offset_len < 8.0f || bone_offset_len > 180.0f || dz < 5.0f || dz > 150.0f) return 0;
    if (bone_index_out) *bone_index_out = bone_index;
    if (name_out) {
        memcpy(name_out, bone_name, HEAD_NAME_LIMIT);
        name_out[HEAD_NAME_LIMIT - 1] = '\0';
    }
    if (g_head_position_logs < 2) {
        log_line("HEAD_POS n=%u entity=%p bone=%d/%d name=%s point=(%.2f,%.2f,%.2f) delta_z=%.2f time=%.3f",
            g_head_position_logs + 1, (void *)entity, bone_index, bone_count, bone_name,
            out[0], out[1], out[2], dz, now);
        ++g_head_position_logs;
    }
    return 1;
}

static int trace_line(void *skip_entity, void *skip_target, int trace_type,
                      const float start[3], const float end[3],
                      float *fraction_out, void **hit_out, int *solid_out) {
    if (fraction_out) *fraction_out = -1.0f;
    if (hit_out) *hit_out = NULL;
    if (solid_out) *solid_out = 0;
    if (!g_engine_trace || !g_trace_ray || !start || !end) return 0;
    SourceRay ray;
    memset(&ray, 0, sizeof(ray));
    for (int k = 0; k < 3; ++k) {
        ray.start[k] = start[k];
        ray.delta[k] = end[k] - start[k];
    }
    ray.is_ray = 1;
    ray.is_swept = (ray.delta[0] != 0.0f || ray.delta[1] != 0.0f || ray.delta[2] != 0.0f) ? 1 : 0;
    TraceFilterObject filter;
    memset(&filter, 0, sizeof(filter));
    filter.vtable = (uintptr_t)&g_filter_vtable;
    filter.skip_entity = (uintptr_t)skip_entity;
    filter.skip_target = (uintptr_t)skip_target;
    filter.trace_type = trace_type;
    uint8_t raw[TRACE_RESULT_SIZE] __attribute__((aligned(16)));
    memset(raw, 0, sizeof(raw));
    g_trace_ray(g_engine_trace, &ray, TRACE_MASK_VISIBLE, &filter, raw);

    float fraction = -1.0f;
    void *hit = NULL;
    uint8_t allsolid = 0, startsolid = 0;
    memcpy(&fraction, raw + TRACE_FRACTION_OFF, sizeof(fraction));
    memcpy(&hit, raw + TRACE_ENTITY_OFF, sizeof(hit));
    memcpy(&allsolid, raw + TRACE_SOLID_OFF, sizeof(allsolid));
    memcpy(&startsolid, raw + TRACE_STARTSOLID_OFF, sizeof(startsolid));
    if (!isfinite(fraction) || fraction < -0.01f || fraction > 1.01f) return 0;
    if (fraction_out) *fraction_out = fraction;
    if (hit_out) *hit_out = hit;
    if (solid_out) *solid_out = (allsolid || startsolid) ? 1 : 0;
    return 1;
}

static int world_trace_probe(void *local, const float local_eye[3]) {
    if (!local || !local_eye) return 0;
    uint64_t now = now_ms();
    float dx = local_eye[0] - g_world_probe_last_eye[0];
    float dy = local_eye[1] - g_world_probe_last_eye[1];
    float dz = local_eye[2] - g_world_probe_last_eye[2];
    float moved2 = dx * dx + dy * dy + dz * dz;
    if (g_world_probe_last_result >= 0 && now >= g_world_probe_last_ms &&
        now - g_world_probe_last_ms < 1000u && moved2 < (96.0f * 96.0f)) {
        return g_world_probe_last_result;
    }

    float end[3] = { local_eye[0], local_eye[1], local_eye[2] - 2048.0f };
    float fraction = 1.0f;
    void *hit = NULL;
    int solid = 0;
    int trace_ok = trace_line(local, NULL, TRACE_WORLD_ONLY, local_eye, end, &fraction, &hit, &solid);
    int verified = trace_ok && !solid && fraction >= 0.0f && fraction < 0.999f;
    g_world_probe_last_ms = now;
    memcpy(g_world_probe_last_eye, local_eye, sizeof(g_world_probe_last_eye));
    g_world_probe_last_result = verified;
    if (g_world_probe_logs < 3) {
        log_line("TRACE_WORLD_PROBE n=%u ok=%d verified=%d fraction=%.6f solid=%d start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f)",
            g_world_probe_logs + 1, trace_ok, verified, fraction, solid,
            local_eye[0], local_eye[1], local_eye[2], end[0], end[1], end[2]);
        ++g_world_probe_logs;
    }
    return verified;
}

static int trace_visible(void *local, void *target, const float from[3], const float to[3],
                         float *fraction_out, void **hit_out) {
    if (fraction_out) *fraction_out = -1.0f;
    if (hit_out) *hit_out = NULL;
    if (!local || !target || !from || !to) return -1;

    /* One strict full-content ray, with the local player excluded but NOT the
       candidate. An unobstructed ray must either hit that candidate or finish
       without any hit; another entity, world brush, or prop before the endpoint
       blocks the candidate. This eliminates the old two-ray ambiguity. */
    float fraction = -1.0f;
    void *hit = NULL;
    int solid = 0;
    int trace_ok = trace_line(local, NULL, TRACE_EVERYTHING_FILTER_PROPS,
                              from, to, &fraction, &hit, &solid);
    int fraction_valid = isfinite(fraction) && fraction >= 0.0f && fraction <= 1.001f;
    if (fraction_out) *fraction_out = fraction;
    if (hit_out) *hit_out = hit;
    if (!trace_ok || !fraction_valid) return -1;

    int target_hit = (hit == target);
    int clear_endpoint = (!hit && fraction >= 0.9995f);
    int visible = !solid && (target_hit || clear_endpoint);
    if (g_visibility_logs < 8) {
        log_line("TRACE_VIS n=%u mode=SINGLE_MASK_ALL target=%p hit=%p fraction=%.6f solid=%d targetHit=%d clearEndpoint=%d visible=%d mask=0x%08x",
            g_visibility_logs + 1, target, hit, fraction, solid,
            target_hit, clear_endpoint, visible, TRACE_MASK_VISIBLE);
        ++g_visibility_logs;
    }
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
    if (local_team < 2 || local_team > 3) {
        log_skip_throttled("local_team_invalid", "local=%d team=%d", local_index, local_team);
        return;
    }
    if (!g_engine_trace || !g_trace_ray) {
        log_skip_throttled("trace_unavailable", "trace=%p traceRay=%p slot=%u",
            g_engine_trace, (void *)g_trace_ray, TRACE_RAY_VTABLE_SLOT);
        return;
    }

    float local_eye[3] = {
        local_origin[0] + local_view[0],
        local_origin[1] + local_view[1],
        local_origin[2] + local_view[2]
    };
    if (!g_setup_bones || !g_engine_time) {
        log_skip_throttled("head_api_unavailable", "setupBones=%p engineTime=%p clientBase=%p",
            (void *)g_setup_bones, (void *)g_engine_time, (void *)g_client_base);
        return;
    }
    /* Do not enable aim if world geometry cannot be observed by this trace API. */
    if (!world_trace_probe(local, local_eye)) {
        log_skip_throttled("world_trace_unverified", "trace=%p traceRay=%p slot=%u eye=(%.2f,%.2f,%.2f)",
            g_engine_trace, (void *)g_trace_ray, TRACE_RAY_VTABLE_SLOT,
            local_eye[0], local_eye[1], local_eye[2]);
        return;
    }
    int best_idx = -1, best_team = 0, best_health = 0;
    void *best_ent = NULL, *best_hit = NULL;
    float best_error = 999.0f, best_fraction = -1.0f;
    float best_target[3] = {0}, best_angles[2] = {0};
    int enemy_candidates = 0, visible_count = 0, blocked_count = 0, invalid_trace_count = 0;
    int highest = g_get_highest(g_entity_list);
    if (highest < 0) highest = 0;
    if (highest >= MAX_ENT_SCAN) highest = MAX_ENT_SCAN - 1;
    if (highest > max_clients) highest = max_clients;

    for (int i = 1; i <= highest; ++i) {
        if (i == local_index || i > max_clients) continue;
        const char *name = entity_class_name(i);
        if (!is_player_class(name)) continue;
        void *ent = g_get_entity(g_entity_list, i);
        if (!ent || ent == local) continue;
        int hp = 0, team = 0, life = 0;
        float org[3] = {0}, vo[3] = {0};
        if (!read_entity_data(ent, &hp, &team, &life, org, vo)) continue;
        if (life != 0 || hp <= 0 || hp > 200 || team < 2 || team > 3 || team == local_team) continue;
        if (!finite_vec3(org)) continue;
        ++enemy_candidates;

        /* Cheap yaw-only prefilter: head/crouch animation cannot materially change
           the target's horizontal bearing, so reject clearly off-FOV entities before
           parsing model data or calling SetupBones. A generous margin avoids false skips. */
        float coarse_yaw = atan2f(org[1] - local_eye[1], org[0] - local_eye[0]) *
                           (180.0f / 3.14159265358979323846f);
        float coarse_yaw_error = fabsf(normalize_yaw(coarse_yaw - cy));
        if (!isfinite(coarse_yaw_error) ||
            (coarse_yaw_error > cfg.fov + 14.0f && coarse_yaw_error > best_error + 14.0f))
            continue;

        /* Resolve the animated head bone; the model's bone index is cached. */
        float point[3] = {0.0f, 0.0f, 0.0f};
        int head_bone = -1;
        char head_name[HEAD_NAME_LIMIT] = {0};
        if (!get_head_position((uintptr_t)ent, org, point, &head_bone, head_name)) continue;
        float target_angle[2];
        calc_angle(local_eye, point, target_angle);
        float dp = normalize_yaw(target_angle[0] - cp);
        float dy = normalize_yaw(target_angle[1] - cy);
        float err = sqrtf(dp * dp + dy * dy);
        if (!isfinite(err) || err > cfg.fov || err >= best_error) continue;

        float frac = -1.0f;
        void *hit = NULL;
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

    int aim_applied = 0;
    float after_pitch = cp, after_yaw = cy;
    if (best_idx >= 1) {
        float dp = normalize_yaw(best_angles[0] - cp);
        float dy = normalize_yaw(best_angles[1] - cy);
        float error_deg = sqrtf(dp * dp + dy * dy);
        float dt = (isfinite(sample_time) && sample_time >= 0.001f && sample_time <= 0.1f)
                     ? sample_time : (1.0f / 60.0f);
        /* The panel maximum (30) means immediate snap. At lower speeds, magnet
           scales the exponentially smoothed response as before. */
        float alpha;
        if (cfg.speed >= 29.99f) {
            alpha = 1.0f;
        } else {
            float response = 1.0f - expf(-cfg.speed * dt * 12.0f);
            alpha = clampf_local(response * cfg.magnet, 0.0f, 1.0f);
            float step_deg = error_deg * alpha;
            if (step_deg > 22.0f && error_deg > 0.0001f) alpha = 22.0f / error_deg;
        }
        after_pitch = clampf_local(cp + dp * alpha, -89.0f, 89.0f);
        after_yaw = normalize_yaw(cy + dy * alpha);
        aim_applied = (fabsf(dp) > 0.01f || fabsf(dy) > 0.01f) && alpha > 0.0f;
    }

    float d_pitch = after_pitch - before_pitch;
    float d_yaw = normalize_yaw(after_yaw - before_yaw);
    if (isfinite(after_pitch) && isfinite(after_yaw) && fabsf(after_pitch) <= 89.0f && fabsf(d_pitch) <= 180.0f && fabsf(d_yaw) <= 180.0f) {
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
    if (now - g_last_log_ms >= LOG_INTERVAL_MS) {
        log_line("FRAME local=%d hp=%d team=%d cmd=(%.2f,%.2f)->(%.2f,%.2f) scanned=%d visible=%d blocked=%d invalidTrace=%d selected=%d targetTeam=%d error=%.2f aimApplied=%d aimEnabled=%d speed=%.2f magnet=%.2f fov=%.1f recoilControl=OFF targetMode=visible_enemy occlusion=MASK_VISIBLE",
            local_index, local_health, local_team, before_pitch, before_yaw, after_pitch, after_yaw,
            enemy_candidates, visible_count, blocked_count, invalid_trace_count, best_idx,
            best_team, best_error, aim_applied, cfg.aim_enabled, cfg.speed, cfg.magnet, cfg.fov);
        log_line("TARGET_SCAN highest=%d maxClients=%d trace=%p traceRay=%p target=%p hit=%p fraction=%.4f health=%d team=%d point=(%.1f,%.1f,%.1f) targetAngle=(%.2f,%.2f) pointSource=studio_head_bone teamRule=enemy_only",
            highest, max_clients, g_engine_trace, (void *)g_trace_ray, best_ent, best_hit,
            best_fraction, best_health, best_team, best_target[0], best_target[1], best_target[2],
            best_angles[0], best_angles[1]);
        g_last_log_ms = now;
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
        if (got_fov) { if (fov < 1.0f) fov = 1.0f; if (fov > 360.0f) fov = 360.0f; }
        pthread_mutex_lock(&g_config_lock);
        if (got_aim) g_cfg_aim_enabled = aim;
        if (got_speed) g_cfg_aim_speed = speed;
        if (got_magnet) g_cfg_aim_magnet = magnet;
        if (got_fov) g_cfg_head_fov = fov;
        pthread_mutex_unlock(&g_config_lock);
        if (got_radar) radar_api_set_enabled(radar);
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
    log_line("HOOK_OK target=libclient.so+0x8B0844 targetVA=%p trampoline=%p mode=V2_3_CACHED_HEAD_SINGLE_TRACE",
        target, tramp);
    return 1;
}

static void *worker(void *unused) {
    (void)unused;
    RuntimeConfig c = config_snapshot();
    log_line("START pid=%ld mode=VISIBLE_ENEMY_STUDIO_HEAD_ASSIST_V2_4 aim=%d radar=%d speed=%.2f magnet=%.2f fov=%.1f controlPort=%d teamOffset=0x%X",
        (long)getpid(), c.aim_enabled, c.radar_enabled, c.speed, c.magnet, c.fov, CONTROL_PORT, TEAM_OFF);
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
    return "KM14 AimAssist v2.3: cached Studio head bone, single MASK_ALL visibility trace, no RCS, localhost control port 28765";
}


/* Namespace private symbols from the radar module to avoid static collisions. */
#define g_client_base radar_g_client_base
#define g_get_max_clients radar_g_get_max_clients
#define g_hook_busy radar_g_hook_busy
#define g_hook_installed radar_g_hook_installed
#define g_interfaces_ready radar_g_interfaces_ready
#define g_is_in_game radar_g_is_in_game
#define g_last_log_ms radar_g_last_log_ms

/* ---- Radar module ---- */
/*
 * KM 1.4 — штатный radar spotted-flag probe/patch, ARM64.
 *
 * Без собственной отрисовки, VGUI вызовов и подмены текстуры карты.
 * На игровом потоке (хук кандидата overview Paint, ранее подтверждённого логом)
 * находит сетевой объект CCSPlayerResource и поддерживает включёнными:
 *   m_bBombSpotted      +0x1210 (bool)
 *   m_bPlayerSpotted[]  +0x1211 (66 byte elements; player indices 1..maxClients)
 *
 * Статическая опора из конкретного arm64-v8a/libclient.so:
 *   registration init around RVA 0xAC61A4: m_bBombSpotted, offset 0x1210
 *   registration init around RVA 0xAC61C0: m_bPlayerSpotted[0], offset 0x1211
 *   registration init around RVA 0xAC61F0: m_bPlayerSpotted array, offset 0x1211, count 0x42=66
 *   class string "CCSPlayerResource" at file/RVA 0x643AF6
 *
 * Все значения относятся только к бинарнику из предоставленного arm64-v8a.zip.
 * Хук устанавливается только при точном совпадении 4 инструкций пролога.
 * Если объект/диапазон не проходит проверки — ничего не патчит, оставляет лог.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004
#endif

#if defined(__aarch64__)
#define KM_ARM64 1
#else
#define KM_ARM64 0
#endif

#define KM_TAG                    "KM14Spotted"
#define KM_LOG_PATH               "/storage/emulated/0/alrm/mod/logger.log"
#define KM_FALLBACK_LOG_PATH      "/data/user/0/com.valvesoftware.source.client/files/km14_spotted_flag.log"
#define KM_PAINT_RVA              ((uintptr_t)0xB4492C)
#define KM_SPOTTED_OFFSET         ((uintptr_t)0x1211)
#define KM_BOMB_SPOTTED_OFFSET    ((uintptr_t)0x1210)
#define KM_SPOTTED_ARRAY_COUNT    66
#define KM_MAX_PLAYER_SLOTS       65
#define KM_MAX_ENTITY_SCAN        2048
#define KM_REFRESH_MS             1000u
#define KM_LOG_INTERVAL_MS        1000u

/* Client-side Source interfaces for the exact audited build. */
typedef void *(*CreateInterfaceFn)(const char *, int *);
typedef void *(*GetClientNetworkableFn)(void *, int);
typedef void *(*GetClientEntityFn)(void *, int);
typedef int (*GetHighestEntityIndexFn)(void *);
typedef int (*GetMaxClientsFn)(void *);
typedef int (*IsInGameFn)(void *);
typedef void *(*GetClientClassFn)(void *);
typedef void (*PaintFn)(void *);

static void km_log(const char *fmt, ...);

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *g_client_handle;
static void *g_engine_handle;
static void *g_entity_list;
static void *g_engine_client;
static GetClientNetworkableFn g_get_client_networkable;
static GetClientEntityFn g_get_client_entity;
static GetHighestEntityIndexFn g_get_highest_entity_index;
static GetMaxClientsFn g_get_max_clients;
static IsInGameFn g_is_in_game;
static uintptr_t g_client_base;
static PaintFn g_original_paint;
static volatile int g_interfaces_ready;
static volatile int g_hook_installed;
static volatile int g_hook_busy;
static int g_resource_index = -1;
static void *g_resource_entity;
static int g_resource_validated;
static uint64_t g_last_refresh_ms;
static uint64_t g_last_log_ms;
static int g_first_patch_logged;
static volatile int g_radar_enabled = 1;

/* These exported functions are the API consumed by the aim-assist control server. */
__attribute__((visibility("default"))) void km14_set_radar_enabled(int enabled)
{
    int value = enabled ? 1 : 0;
    __atomic_store_n(&g_radar_enabled, value, __ATOMIC_SEQ_CST);
    km_log("RADAR_API enabled=%d", value);
}

__attribute__((visibility("default"))) int km14_get_radar_enabled(void)
{
    return __atomic_load_n(&g_radar_enabled, __ATOMIC_SEQ_CST) ? 1 : 0;
}

static uint64_t km_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void km_log(const char *fmt, ...)
{
    char msg[1200];
    char line[1500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t secs = ts.tv_sec;
    localtime_r(&secs, &tmv);
    int n = snprintf(line, sizeof(line),
        "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] %s\n",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L, msg);

    __android_log_write(ANDROID_LOG_INFO, KM_TAG, msg);
    pthread_mutex_lock(&g_log_mutex);
    (void)mkdir("/storage/emulated/0/alrm", 0775);
    (void)mkdir("/storage/emulated/0/alrm/mod", 0775);
    int fd = open(KM_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0664);
    if (fd < 0) {
        (void)mkdir("/data/user/0/com.valvesoftware.source.client/files", 0770);
        fd = open(KM_FALLBACK_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    }
    if (fd >= 0) {
        size_t len = n > 0 ? (size_t)n : 0;
        if (len > sizeof(line)) len = sizeof(line);
        if (len) (void)write(fd, line, len);
        (void)fsync(fd);
        close(fd);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

static void **km_vtable(void *object)
{
    if (!object) return NULL;
    return *(void ***)object;
}

static void *km_vfunc(void *object, size_t slot)
{
    void **vt = km_vtable(object);
    return vt ? vt[slot] : NULL;
}

static void *km_open_loaded(const char *name)
{
    return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
}

/* Confirms that [address,address+length) lies in one mapped range with the
 * requested read/write permissions before touching the player-resource bytes. */
static int km_range_mapped(uintptr_t address, size_t length, int need_write)
{
    if (!address || !length || address + length < address) return 0;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    int ok = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (address >= (uintptr_t)start && address + length <= (uintptr_t)end &&
            perms[0] == 'r' && (!need_write || perms[1] == 'w')) {
            ok = 1;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static int km_resolve_interfaces(void)
{
    if (g_interfaces_ready) return 1;
    if (!g_client_handle) g_client_handle = km_open_loaded("libclient.so");
    if (!g_engine_handle) g_engine_handle = km_open_loaded("libengine.so");
    if (!g_client_handle || !g_engine_handle) return 0;

    CreateInterfaceFn client_factory = (CreateInterfaceFn)dlsym(g_client_handle, "CreateInterface");
    CreateInterfaceFn engine_factory = (CreateInterfaceFn)dlsym(g_engine_handle, "CreateInterface");
    if (!client_factory || !engine_factory) {
        km_log("INTERFACE_FAIL factories client=%p engine=%p", (void *)client_factory, (void *)engine_factory);
        return 0;
    }

    int rc_entity = -1, rc_engine = -1;
    g_entity_list = client_factory("VClientEntityList003", &rc_entity);
    g_engine_client = engine_factory("VEngineClient014", &rc_engine);
    if (!g_entity_list || !g_engine_client) {
        km_log("INTERFACE_WAIT entityList=%p rc=%d engine=%p rc=%d",
            g_entity_list, rc_entity, g_engine_client, rc_engine);
        return 0;
    }

    /* Audited VClientEntityList003 slots: 0 networkable, 3 entity, 6 highest. */
    g_get_client_networkable = (GetClientNetworkableFn)km_vfunc(g_entity_list, 0);
    g_get_client_entity = (GetClientEntityFn)km_vfunc(g_entity_list, 3);
    g_get_highest_entity_index = (GetHighestEntityIndexFn)km_vfunc(g_entity_list, 6);
    /* Audited VEngineClient014 slots: 21 max clients, 26 IsInGame. */
    g_get_max_clients = (GetMaxClientsFn)km_vfunc(g_engine_client, 21);
    g_is_in_game = (IsInGameFn)km_vfunc(g_engine_client, 26);

    if (!g_get_client_networkable || !g_get_client_entity || !g_get_highest_entity_index ||
        !g_get_max_clients || !g_is_in_game) {
        km_log("INTERFACE_FAIL vfunc networkable=%p entity=%p highest=%p max=%p ingame=%p",
            (void *)g_get_client_networkable, (void *)g_get_client_entity,
            (void *)g_get_highest_entity_index, (void *)g_get_max_clients, (void *)g_is_in_game);
        return 0;
    }

    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *)client_factory, &info) != 0) g_client_base = (uintptr_t)info.dli_fbase;
    __sync_synchronize();
    g_interfaces_ready = 1;
    km_log("INTERFACES_OK base=%p entityList=%p engine=%p getNetworkable=%p getEntity=%p highest=%p max=%p inGame=%p",
        (void *)g_client_base, g_entity_list, g_engine_client,
        (void *)g_get_client_networkable, (void *)g_get_client_entity,
        (void *)g_get_highest_entity_index, (void *)g_get_max_clients, (void *)g_is_in_game);
    return 1;
}

/* IClientNetworkable::GetClientClass is virtual slot 2. ClientClass::m_pNetworkName
 * is at +0x10 in this 64-bit ABI. GetClientNetworkable comes from entity-list slot 0. */
static const char *km_entity_network_name(int index, void **networkable_out)
{
    if (networkable_out) *networkable_out = NULL;
    void *networkable = g_get_client_networkable(g_entity_list, index);
    if (networkable_out) *networkable_out = networkable;
    if (!networkable) return NULL;
    void *get_class_ptr = km_vfunc(networkable, 2);
    if (!get_class_ptr) return NULL;
    GetClientClassFn get_class = (GetClientClassFn)get_class_ptr;
    void *client_class = get_class(networkable);
    if (!client_class) return NULL;
    /* GetClientNetworkable/GetClientClass are engine interfaces; don't parse
     * /proc/self/maps for every entity during the paint callback. */
    const char *name = *(const char **)((const uint8_t *)client_class + 0x10);
    return name;
}

static int km_is_resource_name(const char *name)
{
    if (!name) return 0;
    return strcmp(name, "CCSPlayerResource") == 0 ||
           strcmp(name, "CSPlayerResource") == 0 ||
           strcmp(name, "DT_CSPlayerResource") == 0;
}

static int km_validate_resource(int index, void *entity, const char *name)
{
    if (!entity || !km_is_resource_name(name)) return 0;
    uintptr_t start = (uintptr_t)entity + KM_BOMB_SPOTTED_OFFSET;
    size_t size = 1u + KM_SPOTTED_ARRAY_COUNT;
    if (!km_range_mapped(start, size, 1)) {
        km_log("RESOURCE_REJECT idx=%d ptr=%p name=%s reason=range_not_rw field=%p len=%zu",
            index, entity, name, (void *)start, size);
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)start;
    unsigned invalid = bytes[0] > 1 ? 1u : 0u;
    for (size_t i = 0; i < KM_SPOTTED_ARRAY_COUNT; ++i)
        if (bytes[1 + i] > 1) ++invalid;
    if (invalid != 0) {
        km_log("RESOURCE_REJECT idx=%d ptr=%p name=%s reason=bool_sanity invalid=%u bomb=%u spotted0..15=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            index, entity, name, invalid, bytes[0],
            bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15], bytes[16]);
        return 0;
    }

    km_log("RESOURCE_FOUND idx=%d entity=%p networkName=%s bombOff=0x%lx bomb=%u spottedOff=0x%lx count=%d values[1..8]=%u,%u,%u,%u,%u,%u,%u,%u",
        index, entity, name, (unsigned long)KM_BOMB_SPOTTED_OFFSET, bytes[0],
        (unsigned long)KM_SPOTTED_OFFSET, KM_SPOTTED_ARRAY_COUNT,
        bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9]);
    return 1;
}

static int km_refresh_resource(uint64_t now)
{
    if (now - g_last_refresh_ms < KM_REFRESH_MS) return g_resource_validated;
    g_last_refresh_ms = now;

    /* Revalidate a cached entity index first; entity indexes can be reused after a map change. */
    if (g_resource_index >= 0) {
        void *networkable = NULL;
        const char *name = km_entity_network_name(g_resource_index, &networkable);
        void *entity = g_get_client_entity(g_entity_list, g_resource_index);
        if (entity && entity == g_resource_entity && km_is_resource_name(name)) {
            g_resource_validated = km_validate_resource(g_resource_index, entity, name);
            return g_resource_validated;
        }
        km_log("RESOURCE_CACHE_INVALID idx=%d old=%p new=%p name=%s", g_resource_index,
            g_resource_entity, entity, name ? name : "(null)");
        g_resource_index = -1;
        g_resource_entity = NULL;
        g_resource_validated = 0;
    }

    int highest = g_get_highest_entity_index(g_entity_list);
    if (highest < 0) highest = 0;
    if (highest >= KM_MAX_ENTITY_SCAN) highest = KM_MAX_ENTITY_SCAN - 1;
    int scanned = 0;
    for (int index = 0; index <= highest; ++index) {
        void *networkable = NULL;
        const char *name = km_entity_network_name(index, &networkable);
        if (!name) continue;
        ++scanned;
        if (strstr(name, "PlayerResource") || strstr(name, "PlayerManager"))
            km_log("CLASS_CANDIDATE idx=%d networkable=%p name=%s", index, networkable, name);
        if (!km_is_resource_name(name)) continue;

        void *entity = g_get_client_entity(g_entity_list, index);
        if (!entity) {
            km_log("RESOURCE_NAME_NO_ENTITY idx=%d networkable=%p name=%s", index, networkable, name);
            continue;
        }
        g_resource_index = index;
        g_resource_entity = entity;
        g_resource_validated = km_validate_resource(index, entity, name);
        km_log("RESOURCE_CACHE idx=%d entity=%p validated=%d highest=%d scanned=%d",
            index, entity, g_resource_validated, highest, scanned);
        return g_resource_validated;
    }
    km_log("RESOURCE_NOT_FOUND highest=%d scanned_networkables=%d", highest, scanned);
    return 0;
}

static void km_patch_spotted_flags(void)
{
    if (!g_interfaces_ready || !g_get_client_entity || !g_get_max_clients || !g_is_in_game) return;
    if (!g_is_in_game(g_engine_client)) return;

    uint64_t now = km_now_ms();
    (void)km_refresh_resource(now);
    if (!g_resource_validated || g_resource_index < 0 || !g_resource_entity) return;

    /* Always reacquire through EntityList before writing; do not use a stale cached pointer. */
    void *current = g_get_client_entity(g_entity_list, g_resource_index);
    if (!current || current != g_resource_entity) {
        km_log("RESOURCE_POINTER_CHANGED idx=%d old=%p new=%p; refusing write this frame",
            g_resource_index, g_resource_entity, current);
        g_resource_index = -1;
        g_resource_entity = NULL;
        g_resource_validated = 0;
        return;
    }

    uintptr_t base = (uintptr_t)current;
    if (!km_range_mapped(base + KM_BOMB_SPOTTED_OFFSET,
                         1u + KM_SPOTTED_ARRAY_COUNT, 1)) {
        km_log("PATCH_ABORT idx=%d ptr=%p reason=field_range_no_longer_rw", g_resource_index, current);
        g_resource_validated = 0;
        return;
    }

    volatile uint8_t *bomb = (volatile uint8_t *)(base + KM_BOMB_SPOTTED_OFFSET);
    volatile uint8_t *spotted = (volatile uint8_t *)(base + KM_SPOTTED_OFFSET);
    int max_clients = g_get_max_clients(g_engine_client);
    if (max_clients < 1) max_clients = 1;
    if (max_clients > KM_MAX_PLAYER_SLOTS) max_clients = KM_MAX_PLAYER_SLOTS;

    if (!__atomic_load_n(&g_radar_enabled, __ATOMIC_SEQ_CST)) {
        int cleared = 0;
        for (int player = 1; player <= max_clients; ++player) {
            if (spotted[player] != 0) {
                spotted[player] = 0;
                ++cleared;
            }
        }
        if (*bomb != 0) {
            *bomb = 0;
            ++cleared;
        }
        if (cleared > 0 || !g_last_log_ms || now - g_last_log_ms >= KM_LOG_INTERVAL_MS) {
            km_log("RADAR_DISABLED_CLEAR idx=%d cleared=%d maxClients=%d", g_resource_index, cleared, max_clients);
            g_last_log_ms = now;
        }
        return;
    }

    int changed = 0;
    for (int player = 1; player <= max_clients; ++player) {
        if (spotted[player] != 1) {
            spotted[player] = 1;
            ++changed;
        }
    }
    uint8_t bomb_before = *bomb;
    if (*bomb != 1) *bomb = 1;

    if (!g_first_patch_logged || now - g_last_log_ms >= KM_LOG_INTERVAL_MS) {
        uint8_t sample[8];
        for (int i = 0; i < 8; ++i) sample[i] = spotted[i + 1];
        km_log("PATCH_OK idx=%d entity=%p maxClients=%d changed=%d spotted[1..8]=%u,%u,%u,%u,%u,%u,%u,%u bomb=%u->%u source=CCSPlayerResource+0x1211",
            g_resource_index, current, max_clients, changed,
            sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7],
            bomb_before, *bomb);
        g_last_log_ms = now;
        g_first_patch_logged = 1;
    }
}

#if KM_ARM64
static void km_write_abs_branch(uint8_t *out, uintptr_t destination)
{
    /* ldr x17, #8 ; br x17 ; .quad destination */
    const uint32_t ldr_x17_literal_8 = 0x58000051u;
    const uint32_t br_x17 = 0xD61F0220u;
    memcpy(out, &ldr_x17_literal_8, sizeof(ldr_x17_literal_8));
    memcpy(out + 4, &br_x17, sizeof(br_x17));
    memcpy(out + 8, &destination, sizeof(destination));
}

static void km_overview_paint_hook(void *self)
{
    /* Set the flags before the original overview/radar paint for this frame. */
    if (!__sync_lock_test_and_set(&g_hook_busy, 1)) {
        km_patch_spotted_flags();
        __sync_lock_release(&g_hook_busy);
    }
    PaintFn original = g_original_paint;
    if (original) original(self);
}

static int km_install_paint_hook(void)
{
    if (g_hook_installed) return 1;
    if (!g_client_base) return 0;
    uint8_t *target = (uint8_t *)(g_client_base + KM_PAINT_RVA);
    static const uint32_t expected[4] = {
        0x6db63befu, 0x6d0133edu, 0x6d022bebu, 0x6d0323e9u
    };
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        uint32_t actual[4] = {0, 0, 0, 0};
        memcpy(actual, target, sizeof(actual));
        km_log("HOOK_SKIP signature_mismatch base=%p target=%p actual=%08x,%08x,%08x,%08x",
            (void *)g_client_base, target, actual[0], actual[1], actual[2], actual[3]);
        return -1;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    void *trampoline = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) {
        km_log("HOOK_FAIL trampoline_mmap errno=%d", errno);
        return 0;
    }
    memcpy(trampoline, target, 16);
    km_write_abs_branch((uint8_t *)trampoline + 16, (uintptr_t)target + 16);
    __builtin___clear_cache((char *)trampoline, (char *)trampoline + 32);
    if (mprotect(trampoline, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        km_log("HOOK_FAIL trampoline_mprotect errno=%d", errno);
        munmap(trampoline, (size_t)page_size);
        return 0;
    }
    g_original_paint = (PaintFn)trampoline;

    uint8_t patch[16];
    km_write_abs_branch(patch, (uintptr_t)km_overview_paint_hook);
    uintptr_t page_start = (uintptr_t)target & ~((uintptr_t)page_size - 1u);
    if (mprotect((void *)page_start, (size_t)page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        int rwx_error = errno;
        if (mprotect((void *)page_start, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) {
            km_log("HOOK_FAIL target_mprotect errnoRWX=%d errnoRW=%d", rwx_error, errno);
            munmap(trampoline, (size_t)page_size);
            g_original_paint = NULL;
            return 0;
        }
        km_log("HOOK_INFO target_code_page_temporarily_rw errnoRWX=%d", rwx_error);
    }
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char *)target, (char *)target + sizeof(patch));
    if (mprotect((void *)page_start, (size_t)page_size, PROT_READ | PROT_EXEC) != 0)
        km_log("HOOK_WARN target_page_restore_rx errno=%d", errno);
    __sync_synchronize();
    g_hook_installed = 1;
    km_log("HOOK_OK target=libclient.so+0xB4492C targetVA=%p trampoline=%p mode=SPOTTED_FLAG_ONLY",
        target, (void *)g_original_paint);
    return 1;
}
#endif

static void *km_worker(void *unused)
{
    (void)unused;
    km_log("START pid=%d mode=SPOTTED_FLAG_ONLY flagArray=CCSPlayerResource+0x1211 count=66 bomb=+0x1210 paintHookRVA=0xB4492C arm64=%d",
        (int)getpid(), KM_ARM64);
#if !KM_ARM64
    km_log("HOOK_UNAVAILABLE build_not_AArch64");
    return NULL;
#else
    for (int attempt = 0; attempt < 240; ++attempt) {
        if (km_resolve_interfaces()) {
            int installed = km_install_paint_hook();
            if (installed == 1) return NULL;
            if (installed < 0) {
                km_log("STOP target_signature_mismatch_no_patch");
                return NULL;
            }
        }
        usleep(250000);
    }
    km_log("HOOK_TIMEOUT interfaces=%d base=%p", g_interfaces_ready, (void *)g_client_base);
    return NULL;
#endif
}

__attribute__((constructor)) static void km14_spotted_ctor(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&thread, &attr, km_worker, NULL);
        pthread_attr_destroy(&attr);
        if (rc != 0) km_log("THREAD_FAIL rc=%d", rc);
    } else {
        int rc = pthread_create(&thread, NULL, km_worker, NULL);
        if (rc == 0) (void)pthread_detach(thread);
        else km_log("THREAD_FAIL rc=%d", rc);
    }
}

__attribute__((visibility("default"))) const char *km14_spotted_flag_version(void)
{
    return "KM14 spotted-flag patch 1.0; no custom drawing";
}

