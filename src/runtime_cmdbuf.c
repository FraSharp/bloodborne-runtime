/* The game's GPU command buffers, written by more than one thread.
 *
 * Guard (on; BB_CB_GUARD=0: off): one writer at a time per command buffer. While a save or an area
 * loads, the game's main thread records clears of new render targets (+0x2168630, a compute job
 * through +0x26b4ef0) into the main graphics command buffer that GXRenderThread is recording its
 * frame into, with no lock: two cursor updates collide and a packet is cut short or overwritten
 * ("Unimplemented PM4 type 0", every few loads; seen with BB_CB_PROBE). On the PS4 the render
 * thread is mostly blocked on the flip then; here, uncapped, it is not. Each of the game's command
 * buffer methods (gpu/shim/bbport_gnm_table.inc: those that take the buffer in rdi), and those two
 * functions as a whole (buffer at rdi + 0x10), run under a recursive lock of their buffer (bbcpu
 * guards: entry and return). Packets of the two threads still alternate, call by call.
 *
 * Probe (BB_CB_PROBE=1, diagnostic): which threads write which command buffers. Each call notes
 * the thread, the buffer object, its memory [begin, end) and the callers. Reported as they happen:
 * - one buffer object used by two threads within BB_CB_PROBE_US microseconds (default 1000):
 *   two writers sharing its write pointer;
 * - two buffer objects whose memory overlaps, used by two threads within that time: one piece of
 *   command memory handed out twice. */
#include "runtime.h"
#include "platform.h"
#include "cpu/bbcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BB_HAVE_BBCPU
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t offset;
    uint8_t prologue_size;
    uint8_t prologue[15];
    unsigned sites;
    int cb_method;
    const char *pm4;
} GnmEntry;
static const GnmEntry table[] = {
#include "gpu/shim/bbport_gnm_table.inc"
};
enum { COUNT = sizeof(table) / sizeof(table[0]), SLOTS = 4096, RECENT = 64, REPORTS = 40 };

typedef struct {
    uint64_t cb, begin, end, cursor;
    uint64_t thread;
    char name[24];
    uint32_t fn;
    uint64_t caller, callers[2], ns;
} Use;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Use slots[SLOTS];
static uint32_t recent[RECENT];
static uint32_t recent_next;
static uint64_t image_base, window_ns, calls, shared, overlapping;
static int reports;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t offset_of(uint64_t address) {
    return address >= image_base ? address - image_base : address;
}

static void report(const char *what, const Use *a, const Use *b) {
    if (reports >= REPORTS) return;
    ++reports;
    printf("CB probe: %s, %.1f us apart\n", what, (double)(b->ns - a->ns) / 1000.0);
    for (const Use *u = a; u; u = u == a ? b : NULL) {
        printf("  %-24s cb %#llx [%#llx, %#llx) at %#llx, %s from +%#llx < +%#llx < +%#llx\n", u->name,
               (unsigned long long)u->cb, (unsigned long long)u->begin, (unsigned long long)u->end,
               (unsigned long long)u->cursor, table[u->fn].pm4, (unsigned long long)offset_of(u->caller),
               (unsigned long long)offset_of(u->callers[0]), (unsigned long long)offset_of(u->callers[1]));
    }
    fflush(stdout);
}

static void probe(const uint64_t regs[16], uint64_t address, uint32_t fn) {
    (void)address;
    const uint64_t cb = regs[7]; /* rdi */
    if (!cb || (cb & 7)) return;
    const uint64_t *fields = (const uint64_t *)(uintptr_t)cb;
    Use use = {.cb = cb, .begin = fields[0], .end = fields[1], .cursor = fields[2],
               .thread = (uint64_t)(uintptr_t)pthread_self(), .fn = fn,
               .caller = *(const uint64_t *)(uintptr_t)regs[4], .ns = now_ns()};
    /* The guest's name for the thread, else the host's ("host:..."; a host thread in guest code). */
    const char *guest = runtime_thread_name();
    if (guest && *guest && strcmp(guest, "host") != 0) {
        snprintf(use.name, sizeof(use.name), "%s", guest);
    } else {
        char host[16] = "";
        pthread_getname_np(pthread_self(), host, sizeof(host));
        snprintf(use.name, sizeof(use.name), "host:%s", *host ? host : (guest && *guest ? guest : "?"));
    }
    /* Two more callers through the frame pointers (the game keeps them): rbp is the caller's. */
    uint64_t frame = regs[5];
    for (int k = 0; k < 2 && frame && !(frame & 7) && frame > 0x10000; ++k) {
        uint64_t f[2];
        if (bb_read_memory(f, (uintptr_t)frame, sizeof(f)) != sizeof(f)) break;
        use.callers[k] = f[1];
        frame = f[0];
    }
    pthread_mutex_lock(&lock);
    ++calls;
    uint32_t i = (uint32_t)((cb * 0x9e3779b97f4a7c15ull) >> 52) & (SLOTS - 1);
    while (slots[i].cb && slots[i].cb != cb) i = (i + 1) & (SLOTS - 1);
    Use *slot = &slots[i];
    if (slot->cb && slot->thread != use.thread && use.ns - slot->ns < window_ns) {
        ++shared;
        report("one command buffer, two threads", slot, &use);
    }
    if (use.end > use.begin) {
        for (int k = 0; k < RECENT; ++k) {
            const Use *other = &slots[recent[k]];
            if (!other->cb || other->cb == cb || other->thread == use.thread) continue;
            if (use.ns - other->ns >= window_ns) continue;
            if (other->begin < use.end && use.begin < other->end) {
                ++overlapping;
                report("two command buffers in the same memory", other, &use);
            }
        }
    }
    const int is_new = slot->cb == 0;
    *slot = use;
    if (is_new || slot->thread != use.thread) recent[recent_next++ % RECENT] = i;
    pthread_mutex_unlock(&lock);
}

static void *summary(void *unused) {
    (void)unused;
    for (;;) {
        sleep(10);
        pthread_mutex_lock(&lock);
        printf("CB probe: %llu calls; %llu shared-buffer and %llu overlapping-memory uses so far\n",
               (unsigned long long)calls, (unsigned long long)shared, (unsigned long long)overlapping);
        pthread_mutex_unlock(&lock);
        fflush(stdout);
    }
    return NULL;
}


/* Guard: recursive locks by buffer address (exact keys: no lock shared by two buffers). */
enum { LOCKS = 1024 };
static uint64_t lock_keys[LOCKS];
static pthread_mutex_t locks[LOCKS];
static uint64_t guarded_calls, contended;

static uint64_t guard_lock(uint64_t cb) {
    if (!cb || (cb & 7)) return 0;
    uint32_t i = (uint32_t)((cb * 0x9e3779b97f4a7c15ull) >> 54) & (LOCKS - 1);
    for (uint32_t n = 0; n < LOCKS; ++n, i = (i + 1) & (LOCKS - 1)) {
        uint64_t key = __atomic_load_n(&lock_keys[i], __ATOMIC_ACQUIRE);
        if (key == 0) {
            uint64_t empty = 0;
            if (__atomic_compare_exchange_n(&lock_keys[i], &empty, cb, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                key = cb;
            else
                key = empty;
        }
        if (key != cb) continue;
        if (pthread_mutex_trylock(&locks[i]) != 0) {
            __atomic_add_fetch(&contended, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&locks[i]);
        }
        __atomic_add_fetch(&guarded_calls, 1, __ATOMIC_RELAXED);
        return (uint64_t)i + 1;
    }
    return 0; /* table full: unguarded */
}
static void guard_unlock(uint64_t cookie) { pthread_mutex_unlock(&locks[cookie - 1]); }
void runtime_cmdbuf_fault_report(void) {
    uint64_t cookies[64];
    const size_t count = bbcpu_guard_cookies(guard_unlock, cookies, 64);
    char line[128];
    int n = snprintf(line, sizeof(line), "  command-buffer guards held: %zu\n", count);
    { ssize_t written_ = write(2, line, (size_t)n); (void)written_; }
    for (size_t i = 0; i < count; ++i) {
        if (!cookies[i] || cookies[i] > LOCKS) continue;
        const uint64_t key = __atomic_load_n(&lock_keys[cookies[i] - 1], __ATOMIC_RELAXED);
        n = snprintf(line, sizeof(line), "    guard %zu buffer %#llx\n", i, (unsigned long long)key);
        { ssize_t written_ = write(2, line, (size_t)n); (void)written_; }
    }
}

static uint64_t guard_method(const uint64_t regs[16], uint32_t tag) { (void)tag; return guard_lock(regs[7]); }
/* The two functions that record a whole compute job: the command buffer is rdi + 0x10. */
static uint64_t guard_context(const uint64_t regs[16], uint32_t tag) { (void)tag; return guard_lock(regs[7] + 0x10); }

/* A movie frame snapshots, changes and restores the graphics input state. The render task
 * list resets that same state between draw groups. Packet-method guards do not cover either
 * transaction's direct stores or memset. Both scopes use the context's existing buffer lock. */
static uint64_t guard_movie_state(const uint64_t regs[16], uint32_t tag) {
    uint64_t context = regs[6];
    if (tag) {
        /* Empty lists do not access the context; preserve that contract. */
        if ((uint32_t)regs[1] == 0 || !context) return 0;
        context = __atomic_load_n((const uint64_t *)(uintptr_t)(context + 8), __ATOMIC_RELAXED);
    }
    return context ? guard_lock(context + 0x10) : 0;
}

static int prologue_matches(uint64_t address, const uint8_t *bytes, size_t n) {
    return memcmp((const void *)(uintptr_t)address, bytes, n) == 0;
}

static void *guard_report(void *unused) {
    (void)unused;
    uint64_t last = 0;
    for (;;) {
        sleep(30);
        const uint64_t c = __atomic_load_n(&contended, __ATOMIC_RELAXED);
        if (c != last) {
            printf("Command buffers: %llu calls waited for another thread's call on the same buffer "
                   "(%llu guarded calls)\n", (unsigned long long)c,
                   (unsigned long long)__atomic_load_n(&guarded_calls, __ATOMIC_RELAXED));
            fflush(stdout);
            last = c;
        }
    }
    return NULL;
}

static void guard_load(uint64_t base) {
    const char *env = getenv("BB_CB_GUARD");
    if (env && env[0] == '0') {
        printf("Command buffers: no guard (BB_CB_GUARD=0)\n");
        return;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < LOCKS; ++i) pthread_mutex_init(&locks[i], &attr);
    pthread_mutexattr_destroy(&attr);
    size_t used = 0, mismatched = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        if (!table[i].cb_method) continue;
        if (!prologue_matches(base + table[i].offset, table[i].prologue, table[i].prologue_size)) {
            ++mismatched;
            continue;
        }
        bbcpu_set_guard(base + table[i].offset, guard_method, guard_unlock, (uint32_t)i);
        ++used;
    }
    /* push rbp; mov rbp, rsp (1.09). */
    static const struct { uint64_t offset; uint8_t prologue[4]; } jobs[] = {
        {0x2168630, {0x55, 0x48, 0x89, 0xe5}}, {0x26b4ef0, {0x55, 0x48, 0x89, 0xe5}}};
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); ++i) {
        if (!prologue_matches(base + jobs[i].offset, jobs[i].prologue, 4)) {
            ++mismatched;
            continue;
        }
        bbcpu_set_guard(base + jobs[i].offset, guard_context, guard_unlock, 0);
        ++used;
    }
    const char *movie_guard = getenv("BB_MOVIE_STATE_GUARD");
    if (!movie_guard || movie_guard[0] != '0') {
        static const struct { uint64_t offset; uint32_t tag; } scopes[] = {
            {0x28fe7c0, 0}, /* movie frame: context in rsi */
            {0x21b4d00, 1}, /* render task list: context at [rsi + 8] */
        };
        static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5};
        size_t protected_scopes = 0;
        for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); ++i) {
            if (!prologue_matches(base + scopes[i].offset, prologue, sizeof(prologue))) {
                ++mismatched;
                continue;
            }
            bbcpu_set_guard(base + scopes[i].offset, guard_movie_state, guard_unlock, scopes[i].tag);
            ++used;
            ++protected_scopes;
        }
        printf("Movie graphics state: %zu/2 transactions guarded (BB_MOVIE_STATE_GUARD=0: off)\n",
               protected_scopes);
    }
    printf("Command buffers: %zu game functions write one buffer at a time (%zu do not match the 1.09 "
           "eboot; BB_CB_GUARD=0: off)\n", used, mismatched);
    pthread_t thread;
    if (pthread_create(&thread, NULL, guard_report, NULL) == 0) pthread_detach(thread);
}

void runtime_cmdbuf_load(uint64_t base) {
    guard_load(base);
    const char *env = getenv("BB_CB_PROBE");
    if (!env || env[0] != '1') return;
    image_base = base;
    const char *us = getenv("BB_CB_PROBE_US");
    window_ns = (us && *us ? strtoull(us, NULL, 10) : 1000) * 1000;
    size_t used = 0;
    for (size_t i = 0; i < COUNT; ++i) {
        if (!table[i].cb_method) continue;
        bbcpu_set_probe(base + table[i].offset, probe, (uint32_t)i);
        ++used;
    }
    printf("CB probe: %zu command buffer methods probed (window %llu us)\n", used,
           (unsigned long long)(window_ns / 1000));
    pthread_t thread;
    if (pthread_create(&thread, NULL, summary, NULL) == 0) pthread_detach(thread);
}
#else
void runtime_cmdbuf_load(uint64_t base) { (void)base; }
void runtime_cmdbuf_fault_report(void) {}
#endif
