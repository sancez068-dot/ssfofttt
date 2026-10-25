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
static void *g_snapshot_entity;
static uint8_t g_original_spotted[KM_SPOTTED_ARRAY_COUNT];
static uint8_t g_original_spotted_valid[KM_SPOTTED_ARRAY_COUNT];
static uint8_t g_original_bomb;
static int g_original_bomb_valid;

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

static void km_restore_spotted_flags(void);

static void km_patch_spotted_flags(void)
{
    if (!g_interfaces_ready || !g_get_client_entity || !g_get_max_clients || !g_is_in_game) return;
    if (!g_is_in_game(g_engine_client)) return;

    uint64_t now = km_now_ms();
    (void)km_refresh_resource(now);
    if (!__atomic_load_n(&g_radar_enabled, __ATOMIC_RELAXED)) { km_restore_spotted_flags(); return; }
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

    if (g_snapshot_entity != current) {
        memset(g_original_spotted, 0, sizeof(g_original_spotted));
        memset(g_original_spotted_valid, 0, sizeof(g_original_spotted_valid));
        g_original_bomb_valid = 0;
        g_snapshot_entity = current;
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

    int changed = 0;
    for (int player = 1; player <= max_clients; ++player) {
        if (!g_original_spotted_valid[player]) {
            g_original_spotted[player] = spotted[player];
            g_original_spotted_valid[player] = 1;
        }
        if (spotted[player] != 1) {
            spotted[player] = 1;
            ++changed;
        }
    }
    uint8_t bomb_before = *bomb;
    if (!g_original_bomb_valid) { g_original_bomb = *bomb; g_original_bomb_valid = 1; }
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

static void km_restore_spotted_flags(void)
{
    if (!g_resource_validated || g_resource_index < 0 || !g_resource_entity || !g_snapshot_entity) return;
    void *current = g_get_client_entity(g_entity_list, g_resource_index);
    if (!current || current != g_resource_entity || current != g_snapshot_entity) {
        km_log("RADAR_RESTORE_SKIP resource_changed old=%p current=%p", g_snapshot_entity, current);
        memset(g_original_spotted_valid, 0, sizeof(g_original_spotted_valid));
        g_original_bomb_valid = 0;
        g_snapshot_entity = NULL;
        return;
    }
    uintptr_t base = (uintptr_t)current;
    if (!km_range_mapped(base + KM_BOMB_SPOTTED_OFFSET, 1u + KM_SPOTTED_ARRAY_COUNT, 1)) {
        km_log("RADAR_RESTORE_SKIP reason=field_range_not_rw entity=%p", current);
        return;
    }
    volatile uint8_t *bomb = (volatile uint8_t *)(base + KM_BOMB_SPOTTED_OFFSET);
    volatile uint8_t *spotted = (volatile uint8_t *)(base + KM_SPOTTED_OFFSET);
    int restored = 0;
    for (int player = 1; player < KM_SPOTTED_ARRAY_COUNT; ++player) {
        if (g_original_spotted_valid[player]) {
            if (spotted[player] != g_original_spotted[player]) ++restored;
            spotted[player] = g_original_spotted[player];
            g_original_spotted_valid[player] = 0;
        }
    }
    if (g_original_bomb_valid) {
        *bomb = g_original_bomb;
        g_original_bomb_valid = 0;
    }
    km_log("RADAR_DISABLED restored=%d resource=%p", restored, current);
    g_snapshot_entity = NULL;
    memset(g_original_spotted, 0, sizeof(g_original_spotted));
}

__attribute__((visibility("default"))) void km14_set_radar_enabled(int enabled)
{
    int value = enabled ? 1 : 0;
    int previous = __atomic_exchange_n(&g_radar_enabled, value, __ATOMIC_SEQ_CST);
    if (previous != value) km_log("RADAR_CONFIG enabled=%d", value);
}

__attribute__((visibility("default"))) int km14_get_radar_enabled(void)
{
    return __atomic_load_n(&g_radar_enabled, __ATOMIC_SEQ_CST) ? 1 : 0;
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
    km_log("HOOK_OK target=libclient.so+0xB4492C targetVA=%p trampoline=%p mode=SPOTTED_FLAG_RUNTIME_TOGGLE",
        target, (void *)g_original_paint);
    return 1;
}
#endif

static void *km_worker(void *unused)
{
    (void)unused;
    km_log("START pid=%d mode=SPOTTED_FLAG_RUNTIME_TOGGLE flagArray=CCSPlayerResource+0x1211 count=66 bomb=+0x1210 paintHookRVA=0xB4492C arm64=%d",
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
    return "KM14 spotted-flag patch 1.1; runtime toggle on localhost port 28765";
}
