/* NodeMP language host for MicroPython.
 *
 * Callbacks stay on the server worker. node.job runs on one extra thread
 * pinned to a single logical processor. The VM itself is one interpreter
 * with one lock, so a long MicroPython job stalls other MicroPython
 * callbacks the way the GIL stalls the CPython host. The worker thread
 * keeps running Lua and C while it is not inside this interpreter.
 */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <process.h>
#endif

#include "node_int.h"

#include "genhdr/mpversion.h"
#include "py/gc.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/thread_act.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#define JOB_CAP 4096
#define DEFAULT_HEAP_KB 8192

#if defined(_WIN32)
#define STOP_GET() InterlockedCompareExchange(&g_stop, 0, 0)
#define STOP_SET() InterlockedExchange(&g_stop, 1)
static long g_stop;
#else
#define STOP_GET() __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)
#define STOP_SET() __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE)
static long g_stop;
#endif

typedef struct Job {
    int slot;
    struct Job *next;
} Job;

const NodeApi *g_api;
static int g_live;
static void *g_heap;
static size_t g_heap_bytes;

static Job *g_head;
static Job *g_tail;
static int g_count;
static int g_ready_flag;
static int g_lane_up;
static int g_thread_started;
static int g_core = -1;
static int g_ncpu;
static int g_reserve;
static unsigned long long g_reserve_log_at;

#if defined(_WIN32)
static CRITICAL_SECTION g_mu;
static CONDITION_VARIABLE g_cv;
static CRITICAL_SECTION g_vm;
static CRITICAL_SECTION g_reserve_mu;
static HANDLE g_thread;
static DWORD g_lane_tid;
#else
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_vm = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_reserve_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static pthread_t g_lane_thread;
static int g_lane_valid;
#if defined(__linux__)
static long g_lane_os_id;
#endif
#endif

void nodemp_log(const char *msg) {
    if (!g_api || !msg) return;
    if (g_api->log_custom) g_api->log_custom("Mpy", 0x2B5B84u, msg);
    else if (g_api->log_info) g_api->log_info(msg);
}

void nodemp_log_error(const char *msg) {
    if (g_api && g_api->log_error && msg) g_api->log_error(msg);
}

int nodemp_stopping(void) { return STOP_GET() ? 1 : 0; }

void nodemp_vm_lock(void) {
    volatile int top;
#if defined(_WIN32)
    EnterCriticalSection(&g_vm);
#else
    pthread_mutex_lock(&g_vm);
#endif
    mp_stack_set_top((void *)&top);
    mp_stack_set_limit(512 * 1024);
}

void nodemp_vm_unlock(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&g_vm);
#else
    pthread_mutex_unlock(&g_vm);
#endif
}

static void q_lock(void) {
#if defined(_WIN32)
    EnterCriticalSection(&g_mu);
#else
    pthread_mutex_lock(&g_mu);
#endif
}

static void q_unlock(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&g_mu);
#else
    pthread_mutex_unlock(&g_mu);
#endif
}

static void q_wait(void) {
#if defined(_WIN32)
    SleepConditionVariableCS(&g_cv, &g_mu, INFINITE);
#else
    pthread_cond_wait(&g_cv, &g_mu);
#endif
}

static int q_wait_ms(unsigned ms) {
#if defined(_WIN32)
    return SleepConditionVariableCS(&g_cv, &g_mu, ms) ? 1 : 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0;
#endif
}

static void q_broadcast(void) {
#if defined(_WIN32)
    WakeAllConditionVariable(&g_cv);
#else
    pthread_cond_broadcast(&g_cv);
#endif
}

int nodemp_on_lane(void) {
#if defined(_WIN32)
    return g_lane_tid != 0 && GetCurrentThreadId() == g_lane_tid;
#else
    return g_lane_valid && pthread_equal(pthread_self(), g_lane_thread);
#endif
}

static unsigned long long tick_ms(void) {
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static void log_reserve(int moved) {
    unsigned long long now;
    char buf[160];
    if (moved <= 0) return;
    now = tick_ms();
    if (g_reserve_log_at && now - g_reserve_log_at < 10000ull) return;
    g_reserve_log_at = now;
    snprintf(buf, sizeof buf, "moved %d thread(s) off core %d so the micropython lane keeps it", moved, g_core);
    nodemp_log(buf);
}

static void pin_this_thread(void) {
#if defined(_WIN32)
    GROUP_AFFINITY affinity;
    PROCESSOR_NUMBER ideal;
    if (g_core < 0) return;
    ZeroMemory(&affinity, sizeof affinity);
    affinity.Group = (WORD)(g_core / 64);
    affinity.Mask = ((DWORD_PTR)1) << (g_core % 64);
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL)) {
        nodemp_log_error("SetThreadGroupAffinity failed; the micropython lane is not pinned");
        g_core = -1;
        g_reserve = 0;
        return;
    }
    ZeroMemory(&ideal, sizeof ideal);
    ideal.Group = affinity.Group;
    ideal.Number = (BYTE)(g_core % 64);
    SetThreadIdealProcessorEx(GetCurrentThread(), &ideal, NULL);
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy;
    if (g_core < 0) return;
    policy.affinity_tag = g_core + 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
    g_reserve = 0;
    nodemp_log("macOS tags the micropython lane with an affinity hint; other threads are not moved");
#else
    cpu_set_t set;
    if (g_core < 0) return;
    CPU_ZERO(&set);
    CPU_SET((size_t)g_core, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        nodemp_log_error("sched_setaffinity failed; the micropython lane is not pinned");
        g_core = -1;
        g_reserve = 0;
    }
#endif
}

static void name_this_thread(void) {
#if defined(_WIN32)
    typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    SetThreadDescriptionFn fn = kernel ? (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription") : NULL;
    if (fn) fn(GetCurrentThread(), L"nodemp-mpy-core");
#elif defined(__APPLE__)
    pthread_setname_np("nodemp-mpy-core");
#else
    pthread_setname_np(pthread_self(), "nodemp-mpy-core");
#endif
}

static void core_reserve(void) {
#if defined(_WIN32)
    HANDLE snap;
    THREADENTRY32 entry;
    int moved = 0;
    DWORD pid;
    DWORD_PTR bit;
    WORD group;
    if (!g_reserve || g_core < 0 || g_lane_tid == 0) return;
    EnterCriticalSection(&g_reserve_mu);
    bit = ((DWORD_PTR)1) << (g_core % 64);
    group = (WORD)(g_core / 64);
    pid = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_reserve_mu);
        return;
    }
    entry.dwSize = sizeof entry;
    if (Thread32First(snap, &entry)) {
        do {
            HANDLE thread;
            GROUP_AFFINITY affinity;
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == g_lane_tid) continue;
            thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            if (GetThreadGroupAffinity(thread, &affinity) && affinity.Group == group &&
                (affinity.Mask & bit) && affinity.Mask != bit) {
                affinity.Mask &= ~bit;
                if (SetThreadGroupAffinity(thread, &affinity, NULL)) moved++;
            }
            CloseHandle(thread);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    log_reserve(moved);
    LeaveCriticalSection(&g_reserve_mu);
#elif defined(__linux__)
    DIR *dir;
    struct dirent *ent;
    int moved = 0;
    if (!g_reserve || g_core < 0 || g_lane_os_id == 0) return;
    pthread_mutex_lock(&g_reserve_mu);
    dir = opendir("/proc/self/task");
    if (!dir) {
        pthread_mutex_unlock(&g_reserve_mu);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        long tid = strtol(ent->d_name, &end, 10);
        cpu_set_t set;
        if (end == ent->d_name || tid <= 0 || tid == g_lane_os_id) continue;
        if (sched_getaffinity((pid_t)tid, sizeof set, &set) != 0) continue;
        if (!CPU_ISSET(g_core, &set) || CPU_COUNT(&set) <= 1) continue;
        CPU_CLR(g_core, &set);
        if (sched_setaffinity((pid_t)tid, sizeof set, &set) == 0) moved++;
    }
    closedir(dir);
    log_reserve(moved);
    pthread_mutex_unlock(&g_reserve_mu);
#else
    (void)0;
#endif
}

void nodemp_reserve_now(void) { core_reserve(); }


int nodemp_enqueue(int slot) {
    Job *job;
    if (!g_lane_up || STOP_GET()) return -1;
    job = (Job *)calloc(1, sizeof *job);
    if (!job) return -1;
    job->slot = slot;
    q_lock();
    if (!g_lane_up || STOP_GET() || g_count >= JOB_CAP) {
        q_unlock();
        free(job);
        return -1;
    }
    if (g_tail) g_tail->next = job;
    else g_head = job;
    g_tail = job;
    g_count++;
    q_broadcast();
    q_unlock();
    return 0;
}

static void lane_body(void) {
    pin_this_thread();
    name_this_thread();
#if defined(_WIN32)
    g_lane_tid = GetCurrentThreadId();
#elif defined(__linux__)
    g_lane_thread = pthread_self();
    g_lane_valid = 1;
    g_lane_os_id = (long)syscall(SYS_gettid);
#else
    g_lane_thread = pthread_self();
    g_lane_valid = 1;
#endif
    core_reserve();
    q_lock();
    g_ready_flag = 1;
    q_broadcast();
    for (;;) {
        Job *job;
        int stopping;
        while (!STOP_GET() && g_head == NULL) {
            if (q_wait_ms(5000)) continue;
            q_unlock();
            core_reserve();
            q_lock();
        }
        if (g_head == NULL) {
            q_unlock();
            return;
        }
        job = g_head;
        g_head = job->next;
        if (g_head == NULL) g_tail = NULL;
        g_count--;
        stopping = STOP_GET() ? 1 : 0;
        q_unlock();
        if (stopping) {
            nodemp_vm_lock();
            nodemp_discard_slot(job->slot);
            nodemp_vm_unlock();
        } else {
            nodemp_run_slot(job->slot);
        }
        free(job);
        q_lock();
    }
}

#if defined(_WIN32)
static unsigned __stdcall lane_main(void *arg) {
    (void)arg;
    lane_body();
    return 0;
}
#else
static void *lane_main(void *arg) {
    (void)arg;
    lane_body();
    return NULL;
}
#endif

static int processor_count(void) {
#if defined(_WIN32)
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 ? (int)count : 1;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
#endif
}

int nodemp_processor_count(void) { return g_ncpu > 0 ? g_ncpu : 1; }
int nodemp_core_index(void) { return g_core; }
int nodemp_reserve_flag(void) { return g_reserve; }
int nodemp_pinned(void) { return g_core >= 0; }

unsigned long nodemp_lane_tid(void) {
#if defined(_WIN32)
    return g_lane_tid;
#elif defined(__linux__)
    return (unsigned long)g_lane_os_id;
#else
    return g_lane_valid ? (unsigned long)(uintptr_t)pthread_self() : 0;
#endif
}

static int choose_core(void) {
    const char *env = getenv("NODEMP_MPY_CORE");
    char *end = NULL;
    long value;
    char buf[180];
    if (g_ncpu < 1) g_ncpu = 1;
    if (env && (strcmp(env, "off") == 0 || strcmp(env, "any") == 0)) return -1;
    if (env && strcmp(env, "last") == 0) return g_ncpu <= 1 ? -1 : g_ncpu - 1;
    /* Python takes the last core. Use the one before it unless asked otherwise. */
    if (!env || !env[0]) return g_ncpu <= 1 ? -1 : (g_ncpu > 2 ? g_ncpu - 2 : g_ncpu - 1);
    value = strtol(env, &end, 10);
    if (end == env || *end != '\0' || value < 0 || value >= g_ncpu) {
        snprintf(buf, sizeof buf, "NODEMP_MPY_CORE=%s is not a core on this machine; using the last core", env);
        nodemp_log(buf);
        return g_ncpu <= 1 ? -1 : g_ncpu - 1;
    }
    return (int)value;
}

static int reserve_enabled(void) {
    const char *env = getenv("NODEMP_MPY_RESERVE");
    if (env && strcmp(env, "0") == 0) return 0;
    return 1;
}

static int core_start(void) {
#if defined(_WIN32)
    InitializeCriticalSection(&g_mu);
    InitializeConditionVariable(&g_cv);
    InitializeCriticalSection(&g_reserve_mu);
#endif
    g_ncpu = processor_count();
    g_core = choose_core();
    g_reserve = reserve_enabled();
    if (g_core < 0) g_reserve = 0;
#if defined(_WIN32)
    g_thread = (HANDLE)_beginthreadex(NULL, 2 * 1024 * 1024, lane_main, NULL, 0, NULL);
    if (!g_thread) return -1;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (pthread_create(&g_thread, &attr, lane_main, NULL) != 0) {
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
#endif
    g_thread_started = 1;
    q_lock();
    while (!g_ready_flag) q_wait();
    q_unlock();
    g_lane_up = 1;
    return 0;
}

static void core_stop(void) {
    if (!g_thread_started) return;
    STOP_SET();
    q_lock();
    q_broadcast();
    q_unlock();
#if defined(_WIN32)
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
#else
    pthread_join(g_thread, NULL);
#endif
    g_thread_started = 0;
    g_lane_up = 0;
}

static int absolute_path(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || path[0] == '\\') return 1;
    return path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static int join_entry(char *out, size_t n, const char *dir, const char *entry) {
    size_t ld;
    size_t le;
    int slash;
    if (!entry || !entry[0]) return 0;
    if (absolute_path(entry)) {
        if (strlen(entry) + 1 > n) return 0;
        memcpy(out, entry, strlen(entry) + 1);
        return 1;
    }
    if (!dir) return 0;
    ld = strlen(dir);
    le = strlen(entry);
    slash = ld > 0 && (dir[ld - 1] == '/' || dir[ld - 1] == '\\');
    if (ld + (slash ? 0 : 1) + le + 1 > n) return 0;
    memcpy(out, dir, ld);
    if (!slash) out[ld++] = '/';
    memcpy(out + ld, entry, le + 1);
    return 1;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file;
    long size;
    char *buf;
#if defined(_WIN32)
    wchar_t wide[1024];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)(sizeof wide / sizeof wide[0]));
    if (n <= 0) return NULL;
    file = _wfopen(wide, L"rb");
#else
    file = fopen(path, "rb");
#endif
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    if (size && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return NULL;
    }
    buf[size] = '\0';
    fclose(file);
    *length = (size_t)size;
    return buf;
}

static size_t heap_bytes(void) {
    const char *env = getenv("NODEMP_MPY_HEAP_KB");
    long kb = DEFAULT_HEAP_KB;
    char *end = NULL;
    if (env && env[0]) {
        kb = strtol(env, &end, 10);
        if (end == env || *end != '\0' || kb < 256) kb = DEFAULT_HEAP_KB;
        if (kb > 262144) kb = 262144;
    }
    return (size_t)kb * 1024u;
}

static int host_load(const char *name, const char *dir, const char *entry, void *user) {
    char path[1024];
    char *source;
    size_t length = 0;
    int rc;
    (void)user;
    if (!entry || !entry[0]) {
        nodemp_log_error("micropython resource has no server.main");
        return -1;
    }
    if (!join_entry(path, sizeof path, dir, entry)) {
        nodemp_log_error("micropython resource path is too long");
        return -1;
    }
    source = read_file(path, &length);
    if (!source) {
        nodemp_log_error("micropython resource file could not be read");
        return -1;
    }
    nodemp_vm_lock();
    rc = nodemp_load(name, dir, source, length, path);
    nodemp_vm_unlock();
    free(source);
    return rc;
}

static void host_unload(const char *name, void *user) {
    (void)user;
    nodemp_vm_lock();
    nodemp_unload(name);
    nodemp_vm_unlock();
}

static void shutdown_vm(void) {
    nodemp_vm_lock();
    mp_deinit();
    nodemp_vm_unlock();
    free(g_heap);
    g_heap = NULL;
}

NODE_PLUGIN_ABI()

NODE_EXPORT int node_plugin_init(const NodeApi *api) {
    static NodeLanguageHost host;
    char banner[320];
    g_api = api;
    if (!api || (api->abi_version >> 16) != NODE_ABI_VERSION_MAJOR) return -1;
    if (api->struct_size < (uint32_t)(offsetof(NodeApi, register_language_host) + sizeof(void *))) {
        nodemp_log_error("mpyhost needs a server that provides register_language_host");
        return -1;
    }
#if defined(_WIN32)
    InitializeCriticalSection(&g_vm);
#endif
    g_heap_bytes = heap_bytes();
    g_heap = malloc(g_heap_bytes);
    if (!g_heap) {
        nodemp_log_error("micropython heap allocation failed");
        return -1;
    }
    nodemp_vm_lock();
    gc_init(g_heap, (uint8_t *)g_heap + g_heap_bytes);
    mp_init();
    {
        volatile int top;
        mp_stack_set_top((void *)&top);
        mp_stack_set_limit(512 * 1024);
    }
    nodemp_boot();
    nodemp_vm_unlock();
    if (core_start() != 0) {
        nodemp_log_error("micropython core lane failed to start");
        shutdown_vm();
        return -1;
    }
    memset(&host, 0, sizeof host);
    host.struct_size = (uint32_t)sizeof host;
    host.type_name = "micropython";
    host.load = host_load;
    host.unload = host_unload;
    if (api->register_language_host(&host) != 0) {
        nodemp_log_error("resource type \"micropython\" is already registered");
        core_stop();
        shutdown_vm();
        return -1;
    }
    if (g_core < 0) {
        snprintf(banner, sizeof banner,
            "resources with type = \"micropython\" run on MicroPython " MICROPY_GIT_TAG "; background lane is not pinned");
    } else if (g_reserve) {
        snprintf(banner, sizeof banner,
            "resources with type = \"micropython\" run on MicroPython " MICROPY_GIT_TAG
            "; background lane is pinned to core %d and other threads are moved off it",
            g_core);
    } else {
        snprintf(banner, sizeof banner,
            "resources with type = \"micropython\" run on MicroPython " MICROPY_GIT_TAG
            "; background lane is pinned to core %d",
            g_core);
    }
    nodemp_log(banner);
    g_live = 1;
    return 0;
}

NODE_EXPORT void node_plugin_shutdown(void) {
    if (!g_live) return;
    g_live = 0;
    core_stop();
    if (g_api && g_api->unregister_language_host &&
        g_api->struct_size >= (uint32_t)(offsetof(NodeApi, unregister_language_host) + sizeof(void *))) {
        g_api->unregister_language_host("micropython");
    }
    shutdown_vm();
    g_api = NULL;
}
