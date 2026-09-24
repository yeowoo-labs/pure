#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* A measurement apparatus, not the Pure runtime or a general document parser. */
enum { CACHE_SLOTS = 16, SCRATCH = 65536, HIST_SIZE = 512, MAX_FRAME = 8*1024*1024 };
typedef struct Block { size_t capacity; uint64_t since; unsigned char bytes[]; } Block;
typedef struct Item {
    Block *block;
    size_t offset, length;
    uint64_t start, ready;
} Item;
typedef struct Engine {
    Item *queue;
    Block *cache[CACHE_SLOTS];
    unsigned char scratch[SCRATCH];
    uint64_t histogram[HIST_SIZE];
    size_t slots, head, count, fixed, budget, reserved, active, cached;
    size_t peak_reserved, peak_active, peak_queue;
    uint64_t allocations, reuses, copies, read_bytes, written_bytes;
    uint64_t accepted, emitted, copied_records, borrowed_records, pressure_copies;
    uint64_t budget_waits, slot_waits, sleep_ns, hold_ns, service_ns, last_emit;
    uint64_t elapsed_ns, cpu_ns, latency_sum, latency_max, start, cancel_after;
    long double active_byte_ns;
    unsigned ratio;
    bool pool;
    const char *policy, *status;
} Engine;
static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }
static uint64_t ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static size_t charge(const Block *b) { return sizeof(*b) + b->capacity; }
static size_t rounded(size_t n) {
    size_t r = 64;
    while (r < n) r *= 2;
    return r;
}
static void peaks(Engine *e) {
    assert(e->active + e->cached == e->reserved);
    assert(e->fixed + e->reserved <= e->budget);
    if (e->reserved > e->peak_reserved) e->peak_reserved = e->reserved;
    if (e->active > e->peak_active) e->peak_active = e->active;
}
static void evict(Engine *e, size_t i) {
    Block *b = e->cache[i];
    if (!b) return;
    e->cached -= charge(b); e->reserved -= charge(b);
    free(b); e->cache[i] = NULL;
}
static Block *take(Engine *e, size_t n) {
    size_t capacity = rounded(n), need = sizeof(Block) + capacity;
    if (e->pool) {
        for (size_t i = 0; i < CACHE_SLOTS; ++i) {
            Block *b = e->cache[i];
            if (b && b->capacity == capacity) {
                e->cache[i] = NULL; e->cached -= need; e->active += need;
                ++e->reuses; b->since = ns(CLOCK_MONOTONIC); peaks(e); return b;
            }
        }
    }
    for (size_t i = 0; i < CACHE_SLOTS && need > e->budget-e->fixed-e->reserved; ++i)
        evict(e, i);
    if (need > e->budget-e->fixed-e->reserved) return NULL;
    Block *b = malloc(need);
    if (!b) { e->status = "allocation_failed"; return NULL; }
    b->capacity = capacity; b->since = ns(CLOCK_MONOTONIC);
    e->reserved += need; e->active += need; ++e->allocations; peaks(e); return b;
}
static void give(Engine *e, Block *b) {
    if (!b) return;
    size_t bytes = charge(b);
    e->active_byte_ns += (long double)bytes * (ns(CLOCK_MONOTONIC)-b->since);
    e->active -= bytes;
    if (e->pool && bytes <= e->budget/4-e->cached) {
        for (size_t i = 0; i < CACHE_SLOTS; ++i) if (!e->cache[i]) {
            e->cache[i] = b; e->cached += bytes; peaks(e); return;
        }
    }
    e->reserved -= bytes; free(b); peaks(e);
}
static bool sleep_until(Engine *e, uint64_t deadline) {
    uint64_t t = ns(CLOCK_MONOTONIC), start = t;
    while (t < deadline && !interrupted) {
        uint64_t delta = deadline-t;
        struct timespec request = {(time_t)(delta/1000000000), (long)(delta%1000000000)};
        if (nanosleep(&request, NULL) && errno != EINTR) { e->status="sleep_failed"; return false; }
        t = ns(CLOCK_MONOTONIC);
    }
    e->sleep_ns += t-start;
    return !interrupted;
}
/* floor(log2(ns)) with eight subdivisions: upper bound error <= 12.5%. */
static unsigned bucket(uint64_t n) {
    unsigned exponent = 0;
    for (uint64_t x = n; x > 1; x >>= 1) ++exponent;
    if (exponent < 3) return (unsigned)n;
    return exponent*8 + (unsigned)((n-(UINT64_C(1)<<exponent))>>(exponent-3));
}
static uint64_t upper(unsigned b) {
    if (b < 24) return b+1;
    unsigned exponent = b/8;
    if (exponent >= 63) return UINT64_MAX;
    return (UINT64_C(1)<<exponent) + (uint64_t)(b%8+1)*(UINT64_C(1)<<(exponent-3));
}
static bool write_all(Engine *e, const void *src, size_t n) {
    const unsigned char *p = src;
    while (n && !interrupted) {
        ssize_t written = write(STDOUT_FILENO, p, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { e->status = "output_failed"; return false; }
        p += written; n -= (size_t)written; e->written_bytes += (uint64_t)written;
    }
    return !interrupted;
}
static bool emit(Engine *e) {
    assert(e->count);
    Item *item = &e->queue[e->head];
    uint64_t ready = item->ready;
    if (e->last_emit + e->service_ns > ready) ready = e->last_emit + e->service_ns;
    if (!sleep_until(e, ready)) return false;
    uint32_t len = (uint32_t)item->length;
    unsigned char header[4] = {(unsigned char)len, (unsigned char)(len>>8),
                              (unsigned char)(len>>16), (unsigned char)(len>>24)};
    if (!write_all(e, header, sizeof header) ||
        !write_all(e, item->block->bytes+item->offset, item->length)) return false;
    uint64_t now = ns(CLOCK_MONOTONIC), latency = now-item->start;
    ++e->histogram[bucket(latency)]; e->latency_sum += latency;
    if (latency > e->latency_max) e->latency_max = latency;
    e->last_emit = now; ++e->emitted;
    give(e, item->block); item->block = NULL;
    e->head = (e->head+1)%e->slots; --e->count;
    if (e->cancel_after && e->emitted >= e->cancel_after) {
        e->status = "cancelled"; return false;
    }
    return true;
}
static Block *acquire(Engine *e, size_t n) {
    for (;;) {
        Block *b = take(e, n);
        if (b) return b;
        if (strcmp(e->status, "ok")) return NULL;
        if (!e->count) { e->status = "budget_too_small"; return NULL; }
        ++e->budget_waits;
        if (!emit(e)) return NULL;
    }
}
/* Returns 1 on complete read, 0 on clean EOF before any byte, -1 on failure. */
static int read_exact(Engine *e, void *dst, size_t n) {
    unsigned char *p = dst; size_t done = 0;
    while (done < n && !interrupted) {
        ssize_t got = read(STDIN_FILENO, p+done, n-done);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { e->status="input_failed"; return -1; }
        if (!got) { if (!done) return 0; e->status="truncated_input"; return -1; }
        done += (size_t)got; e->read_bytes += (uint64_t)got;
    }
    return interrupted ? -1 : 1;
}
static bool discard(Engine *e, size_t n) {
    while (n) {
        size_t chunk = n < SCRATCH ? n : SCRATCH;
        if (read_exact(e, e->scratch, chunk) != 1) {
            if (!strcmp(e->status,"ok")) e->status="truncated_input";
            return false;
        }
        n -= chunk;
    }
    return true;
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
/* Linux's rusage high-water mark survives exec. Read the current mm's HWM
 * separately; proc RSS accounting is approximate, not allocator accounting. */
static uint64_t image_peak_rss(void) {
#if defined(__linux__)
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return 0;
    char line[256]; unsigned long long kib = 0;
    while (fgets(line, sizeof line, status))
        if (sscanf(line, "VmHWM: %llu kB", &kib) == 1) break;
    fclose(status);
    return (uint64_t)kib * 1024;
#else
    return 0; /* No current-image peak implemented for other OSes. */
#endif
}
static void run(Engine *e) {
    for (;;) {
        if (interrupted) break;
        if (e->count) {
            Item *head = &e->queue[e->head];
            uint64_t now = ns(CLOCK_MONOTONIC);
            if (now >= head->ready && now >= e->last_emit+e->service_ns) {
                if (!emit(e)) break;
                continue;
            }
        }
        if (e->count == e->slots) { ++e->slot_waits; if (!emit(e)) break; }
        unsigned char header[12];
        uint64_t start = ns(CLOCK_MONOTONIC);
        int got = read_exact(e, header, sizeof header);
        if (got != 1) {
            if (!got) while (e->count && !interrupted) if (!emit(e)) break;
            break;
        }
        size_t payload = le32(header), offset = le32(header+4), length = le32(header+8);
        if (!payload || payload > MAX_FRAME || !length || offset > payload || length > payload-offset) {
            e->status="invalid_frame"; break;
        }
        bool project = !strcmp(e->policy,"project");
        Block *source = acquire(e, project ? length : payload);
        if (!source) break;
        bool valid;
        if (project) {
            valid = discard(e, offset) && read_exact(e, source->bytes, length)==1 &&
                    discard(e, payload-offset-length);
            offset = 0;
        } else valid = read_exact(e, source->bytes, payload)==1;
        if (!valid) {
            if (!strcmp(e->status,"ok")) e->status="truncated_input";
            give(e, source); break;
        }
        bool copy = !strcmp(e->policy,"copy") ||
            (!strcmp(e->policy,"threshold") && rounded(payload)/rounded(length) >= e->ratio);
        if (!strcmp(e->policy,"pressure")) {
            /* Online rule using current charged live buffers; no future lifetime. */
            copy = rounded(length) < source->capacity &&
                   e->active > (e->budget-e->fixed)/2;
            if (copy) ++e->pressure_copies;
        }
        if (!project && copy) {
            Block *small = acquire(e, length);
            if (!small) { give(e, source); break; }
            memcpy(small->bytes, source->bytes+offset, length);
            e->copies += length; ++e->copied_records;
            give(e, source); source = small; offset = 0;
        } else if (!project) ++e->borrowed_records;
        size_t tail = (e->head+e->count)%e->slots;
        e->queue[tail] = (Item){source, offset, length, start, ns(CLOCK_MONOTONIC)+e->hold_ns};
        ++e->count; ++e->accepted;
        if (e->count > e->peak_queue) e->peak_queue=e->count;
    }
    if (interrupted) e->status="cancelled";
}
static uint64_t number(const char *s, uint64_t maximum) {
    char *end; errno=0;
    unsigned long long n = strtoull(s, &end, 10);
    if (!*s || *s=='-' || *end || errno || n>maximum) { fprintf(stderr,"invalid number\n"); exit(2); }
    return n;
}
int main(int argc, char **argv) {
    Engine *e = calloc(1, sizeof *e);
    if (!e) return 2;
    e->policy="borrow"; e->status="ok"; e->budget=8*1024*1024; e->slots=512;
    e->ratio=4; e->pool=true;
    for (int i=1; i<argc; i+=2) {
        if (i+1==argc) { fprintf(stderr,"options require values\n"); free(e); return 2; }
        const char *key=argv[i], *value=argv[i+1];
        if (!strcmp(key,"--policy")) e->policy=value;
        else if (!strcmp(key,"--budget")) e->budget=(size_t)number(value,1073741824);
        else if (!strcmp(key,"--slots")) e->slots=(size_t)number(value,4096);
        else if (!strcmp(key,"--hold-us")) e->hold_ns=number(value,10000000)*1000;
        else if (!strcmp(key,"--service-us")) e->service_ns=number(value,10000000)*1000;
        else if (!strcmp(key,"--ratio")) e->ratio=(unsigned)number(value,UINT_MAX);
        else if (!strcmp(key,"--pool")) e->pool=number(value,1)!=0;
        else if (!strcmp(key,"--cancel-after")) e->cancel_after=number(value,UINT64_MAX);
        else { fprintf(stderr,"unknown option\n"); free(e); return 2; }
    }
    bool known = !strcmp(e->policy,"borrow") || !strcmp(e->policy,"copy") ||
        !strcmp(e->policy,"threshold") || !strcmp(e->policy,"pressure") || !strcmp(e->policy,"project");
    if (!known || !e->slots || e->slots>4096 || e->ratio<2 || e->budget>UINT64_C(1073741824) ||
        e->hold_ns>UINT64_C(10000000000) || e->service_ns>UINT64_C(10000000000)) {
        fprintf(stderr,"invalid configuration\n"); free(e); return 2;
    }
    e->fixed=sizeof(*e)+e->slots*sizeof(Item);
    if (e->budget < e->fixed) { fprintf(stderr,"budget below fixed metadata\n"); free(e); return 2; }
    e->queue=calloc(e->slots,sizeof(Item));
    if (!e->queue) { free(e); return 2; }
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGPIPE,SIG_IGN);
    e->start=ns(CLOCK_MONOTONIC); uint64_t cpu=ns(CLOCK_PROCESS_CPUTIME_ID);
    run(e);
    size_t live_at_stop=e->active, cached_at_stop=e->cached;
    while (e->count) {
        give(e,e->queue[e->head].block); e->head=(e->head+1)%e->slots; --e->count;
    }
    for (size_t i=0;i<CACHE_SLOTS;++i) evict(e,i);
    assert(!e->active && !e->reserved && !e->cached);
    e->elapsed_ns=ns(CLOCK_MONOTONIC)-e->start; e->cpu_ns=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    uint64_t target=e->emitted-e->emitted/100, sum=0, p99=0;
    for (unsigned b=0; e->emitted && b<HIST_SIZE; ++b) {
        sum+=e->histogram[b]; if (sum>=target) { p99=upper(b); break; }
    }
    struct rusage usage; long rss=0;
    if (!getrusage(RUSAGE_SELF,&usage)) rss=usage.ru_maxrss;
#if defined(__APPLE__)
    uint64_t rusage_bytes=(uint64_t)rss;
#else
    uint64_t rusage_bytes=(uint64_t)rss*1024;
#endif
    uint64_t rss_bytes=image_peak_rss();
    fprintf(stderr,"{\"status\":\"%s\",\"policy\":\"%s\",\"pool\":%s,"
        "\"budget_bytes\":%zu,\"fixed_bytes\":%zu,\"peak_reserved_bytes\":%zu,"
        "\"peak_active_bytes\":%zu,\"peak_queue\":%zu,\"rss_peak_bytes\":%" PRIu64 ","
        "\"rusage_maxrss_bytes\":%" PRIu64 ",\"rss_peak_source\":\"%s\","
        "\"elapsed_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ","
        "\"output_bytes\":%" PRIu64 ",\"accepted\":%" PRIu64 ",\"emitted\":%" PRIu64 ","
        "\"copy_bytes\":%" PRIu64 ",\"copied_records\":%" PRIu64 ",\"borrowed_records\":%" PRIu64 ","
        "\"pressure_copies\":%" PRIu64 ",\"allocations\":%" PRIu64 ",\"reuses\":%" PRIu64 ","
        "\"budget_waits\":%" PRIu64 ",\"slot_waits\":%" PRIu64 ",\"sleep_ns\":%" PRIu64 ","
        "\"latency_mean_ns\":%.3f,\"latency_p99_upper_ns\":%" PRIu64 ",\"latency_max_ns\":%" PRIu64 ","
        "\"active_byte_seconds\":%.6Lf,\"live_at_stop_bytes\":%zu,\"cached_at_stop_bytes\":%zu,"
        "\"unreleased_buffer_bytes\":%zu}\n",
        e->status,e->policy,e->pool?"true":"false",e->budget,e->fixed,e->fixed+e->peak_reserved,
        e->fixed+e->peak_active,e->peak_queue,rss_bytes,rusage_bytes,
        rss_bytes?"linux_proc_vmhwm_approximate":"unavailable",e->elapsed_ns,e->cpu_ns,e->read_bytes,
        e->written_bytes,e->accepted,e->emitted,e->copies,e->copied_records,e->borrowed_records,
        e->pressure_copies,e->allocations,e->reuses,e->budget_waits,e->slot_waits,e->sleep_ns,
        e->emitted?(double)e->latency_sum/(double)e->emitted:0,p99,e->latency_max,
        e->active_byte_ns/1e9L,live_at_stop,cached_at_stop,e->reserved);
    int result=!strcmp(e->status,"ok")?0:!strcmp(e->status,"cancelled")?3:2;
    free(e->queue); free(e); return result;
}
