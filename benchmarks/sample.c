#define _GNU_SOURCE
/* A sampling profiler for machines without perf: runs a command, samples
 * where its threads spend user time (each thread's task clock, every 50 µs
 * of its CPU time, through perf_event_open: one event a CPU, since the
 * kernel maps no ring for an inherited per-task event), and after SECONDS stops it and
 * writes the functions that took the most samples to REPORT, named from the
 * ELF symbol tables of the files it had mapped. The command's user and
 * system time (from /proc) head the report: time in the kernel is not
 * sampled, only counted. A stripped file is named from its debug file when
 * one is under $SAMPLE_DEBUG (or /usr/lib/debug) by build id. On arm64 each
 * sample also takes the link register,
 * so time in a shared library's leaf (memcpy, strcmp) is shown again by the
 * function that called it.
 *
 * sample SECONDS REPORT COMMAND [ARGS...] */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { RING = 256 << 10, CPUS = 64, MAPS = 256, TOP = 50 };
#if defined(__aarch64__)
#define LINK_SAMPLE PERF_SAMPLE_REGS_USER
#define LINK_MASK (1ull << 30) /* x30, the link register */
#else
#define LINK_SAMPLE 0
#define LINK_MASK 0
#endif

/* Samples by address (and, for the callers' table, by the pair of the
 * address and its link register): an open-addressed table that doubles when
 * half full. */
typedef struct { uint64_t ip, lr, count; } Hit;
typedef struct { Hit *hits; size_t used, size; } Table;
static Table own, called;
static uint64_t samples, lost;

static void add(Table *t, uint64_t ip, uint64_t lr) {
    Hit *hits = t->hits;
    size_t hit_used = t->used, hit_size = t->size;
    if (2 * (hit_used + 1) > hit_size) {
        Hit *old = hits;
        size_t old_size = hit_size;
        hit_size = hit_size ? 2 * hit_size : 4096;
        if (!(hits = calloc(hit_size, sizeof(*hits)))) { perror("sample"); exit(1); }
        hit_used = 0;
        for (size_t i = 0; i < old_size; ++i)
            if (old[i].count) {
                size_t at = ((old[i].ip ^ old[i].lr * 31) * 0x9e3779b97f4a7c15u >> 20) & (hit_size - 1);
                while (hits[at].count) at = (at + 1) & (hit_size - 1);
                hits[at] = old[i];
                ++hit_used;
            }
        free(old);
    }
    size_t at = ((ip ^ lr * 31) * 0x9e3779b97f4a7c15u >> 20) & (hit_size - 1);
    while (hits[at].count && (hits[at].ip != ip || hits[at].lr != lr)) at = (at + 1) & (hit_size - 1);
    if (!hits[at].count) hits[at].ip = ip, hits[at].lr = lr, ++hit_used;
    ++hits[at].count;
    t->hits = hits, t->used = hit_used, t->size = hit_size;
}

/* The ring's records since the last drain: samples of (ip, pid, tid). */
static void drain(struct perf_event_mmap_page *page, const unsigned char *data, size_t size) {
    uint64_t head = __atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE), tail = page->data_tail;
    while (tail < head) {
        unsigned char record[64];
        struct perf_event_header header;
        for (size_t i = 0; i < sizeof(header); ++i) ((unsigned char *)&header)[i] = data[(tail + i) % size];
        for (size_t i = 0; i < header.size && i < sizeof(record); ++i) record[i] = data[(tail + i) % size];
        if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof(header) + 8) {
            uint64_t ip, abi = 0, lr = 0;
            memcpy(&ip, record + sizeof(header), 8);
            if (header.size >= sizeof(header) + 32) { /* ip, pid and tid, the registers' abi, lr */
                memcpy(&abi, record + sizeof(header) + 16, 8);
                if (abi) memcpy(&lr, record + sizeof(header) + 24, 8);
            }
            add(&own, ip, 0);
            add(&called, ip, lr);
            ++samples;
        } else if (header.type == PERF_RECORD_LOST && header.size >= sizeof(header) + 16) {
            uint64_t count;
            memcpy(&count, record + sizeof(header) + 8, 8);
            lost += count;
        }
        tail += header.size ? header.size : sizeof(header);
    }
    __atomic_store_n(&page->data_tail, tail, __ATOMIC_RELEASE);
}

/* ---- Names: an executable mapping's file, its loads and its functions. */
typedef struct { uint64_t value, size; const char *name; } Symbol;
typedef struct {
    char path[512];
    Symbol *symbols;
    size_t count;
    Elf64_Phdr loads[16];
    unsigned load_count;
    bool read;
} File;
typedef struct { uint64_t start, end, offset; File *file; } Map;

static File files[MAPS];
static unsigned file_count;
static Map maps[MAPS];
static unsigned map_count;

static int by_value(const void *a, const void *b) {
    const Symbol *x = a, *y = b;
    return x->value < y->value ? -1 : x->value > y->value;
}

static const unsigned char *map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) { if (fd >= 0) close(fd); return NULL; }
    const unsigned char *bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)bytes;
    if (bytes == MAP_FAILED || memcmp(e->e_ident, ELFMAG, SELFMAG) || e->e_ident[EI_CLASS] != ELFCLASS64) return NULL;
    *size = (size_t)st.st_size;
    return bytes;
}

/* The functions of a symbol table (the full one, or else the dynamic one). */
static void read_symbols(File *f, const unsigned char *bytes) {
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)bytes;
    const Elf64_Shdr *sections = (const Elf64_Shdr *)(bytes + e->e_shoff);
    for (int want = SHT_SYMTAB; want; want = want == SHT_SYMTAB ? SHT_DYNSYM : 0) {
        for (unsigned i = 0; i < e->e_shnum; ++i) {
            if (sections[i].sh_type != (unsigned)want) continue;
            const Elf64_Sym *symbols = (const Elf64_Sym *)(bytes + sections[i].sh_offset);
            const char *names = (const char *)(bytes + sections[sections[i].sh_link].sh_offset);
            size_t n = sections[i].sh_size / sizeof(Elf64_Sym);
            f->symbols = realloc(f->symbols, (f->count + n) * sizeof(Symbol));
            for (size_t k = 0; k < n; ++k)
                if (ELF64_ST_TYPE(symbols[k].st_info) == STT_FUNC && symbols[k].st_value)
                    f->symbols[f->count++] = (Symbol){symbols[k].st_value, symbols[k].st_size, names + symbols[k].st_name};
        }
        if (f->count) return; /* a symbol table found: the dynamic one would only repeat part of it */
    }
}

/* A stripped file's separate debug file, by its build id, under $SAMPLE_DEBUG
 * (or /usr/lib/debug): DIR/.build-id/xx/rest.debug. */
static const unsigned char *debug_file(const unsigned char *bytes) {
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)bytes;
    const Elf64_Shdr *sections = (const Elf64_Shdr *)(bytes + e->e_shoff);
    for (unsigned i = 0; i < e->e_shnum; ++i) {
        if (sections[i].sh_type != SHT_NOTE) continue;
        const unsigned char *note = bytes + sections[i].sh_offset, *end = note + sections[i].sh_size;
        while (note + 12 <= end) {
            uint32_t name_size, size, type;
            memcpy(&name_size, note, 4), memcpy(&size, note + 4, 4), memcpy(&type, note + 8, 4);
            const unsigned char *desc = note + 12 + ((name_size + 3) & ~3u);
            if (type == NT_GNU_BUILD_ID && name_size == 4 && !memcmp(note + 12, "GNU", 4) && size > 1 && size <= 64) {
                const char *dir = getenv("SAMPLE_DEBUG");
                char path[512];
                int at = snprintf(path, sizeof(path), "%s/.build-id/%02x/", dir ? dir : "/usr/lib/debug", desc[0]);
                for (uint32_t k = 1; k < size && at < (int)sizeof(path) - 8; ++k) at += snprintf(path + at, 3, "%02x", desc[k]);
                snprintf(path + at, sizeof(path) - (size_t)at, ".debug");
                size_t ignored;
                return map_file(path, &ignored);
            }
            note = desc + ((size + 3) & ~3u);
        }
    }
    return NULL;
}

static void read_file(File *f) {
    f->read = true;
    size_t size;
    const unsigned char *bytes = map_file(f->path, &size);
    if (!bytes) return;
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)bytes;
    for (unsigned i = 0; i < e->e_phnum && f->load_count < 16; ++i) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(bytes + e->e_phoff + (size_t)i * e->e_phentsize);
        if (p->p_type == PT_LOAD) f->loads[f->load_count++] = *p;
    }
    const unsigned char *debug = debug_file(bytes);
    read_symbols(f, debug ? debug : bytes);
    qsort(f->symbols, f->count, sizeof(Symbol), by_value);
}

static void read_maps(pid_t pid) {
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *in = fopen(path, "r");
    while (in && fgets(line, sizeof(line), in) && map_count < MAPS) {
        unsigned long long start, end, offset;
        char perms[8], name[512] = "";
        if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %511[^\n]", &start, &end, perms, &offset, name) < 4) continue;
        if (perms[2] != 'x' || name[0] != '/') continue;
        File *f = NULL;
        for (unsigned i = 0; i < file_count; ++i) if (!strcmp(files[i].path, name)) f = &files[i];
        if (!f && file_count < MAPS) f = &files[file_count++], snprintf(f->path, sizeof(f->path), "%s", name);
        if (f) maps[map_count++] = (Map){start, end, offset, f};
    }
    if (in) fclose(in);
}

/* The function an address is in, as "name" and the file it came from. */
static const char *name_of(uint64_t ip, const File **file) {
    *file = NULL;
    for (unsigned i = 0; i < map_count; ++i) {
        const Map *m = &maps[i];
        if (ip < m->start || ip >= m->end) continue;
        File *f = m->file;
        *file = f;
        if (!f->read) read_file(f);
        uint64_t at = ip - m->start + m->offset, vaddr = 0;
        bool placed = false;
        for (unsigned k = 0; k < f->load_count && !placed; ++k)
            if (at >= f->loads[k].p_offset && at < f->loads[k].p_offset + f->loads[k].p_filesz)
                vaddr = at - f->loads[k].p_offset + f->loads[k].p_vaddr, placed = true;
        if (!placed || !f->count) return NULL;
        size_t low = 0, high = f->count;
        while (high - low > 1) {
            size_t mid = (low + high) / 2;
            if (f->symbols[mid].value <= vaddr) low = mid; else high = mid;
        }
        const Symbol *s = &f->symbols[low];
        if (s->value <= vaddr && (vaddr < s->value + s->size || !s->size)) return s->name;
        return NULL;
    }
    return NULL;
}

typedef struct { char name[160]; uint64_t count; } Row;

static int by_count(const void *a, const void *b) {
    const Row *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : strcmp(x->name, y->name);
}

static void cpu_times(pid_t pid, double *user, double *system) {
    char path[64], line[2048];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *in = fopen(path, "r");
    *user = *system = 0;
    if (!in || !fgets(line, sizeof(line), in)) { if (in) fclose(in); return; }
    fclose(in);
    const char *after = strrchr(line, ')');
    unsigned long long utime, stime;
    /* fields 14 and 15, counted after the command's closing parenthesis (field 2) */
    if (after && sscanf(after + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) == 2) {
        long ticks = sysconf(_SC_CLK_TCK);
        *user = (double)utime / (double)ticks, *system = (double)stime / (double)ticks;
    }
}

/* The table's samples by function (or by file when the address has no
 * name); by_caller keeps only samples in another file's function than the
 * one that called it, named "function <- caller". */
static void report(FILE *out, const Table *t, bool by_caller) {
    Row *rows = calloc(t->used + 1, sizeof(Row));
    size_t row_count = 0;
    for (size_t i = 0; i < t->size; ++i) {
        const Hit *h = &t->hits[i];
        if (!h->count) continue;
        const File *f, *caller_file = NULL;
        const char *name = name_of(h->ip, &f), *caller = by_caller ? name_of(h->lr, &caller_file) : NULL;
        if (by_caller && (!h->lr || f == caller_file)) continue;
        const char *base = f ? strrchr(f->path, '/') + 1 : "?";
        char label[160];
        if (by_caller) snprintf(label, sizeof(label), "%s %s <- %s", name ? name : "?", base, caller ? caller : "?");
        else snprintf(label, sizeof(label), "%s %s", name ? name : "?", base);
        size_t k = 0;
        while (k < row_count && strcmp(rows[k].name, label)) ++k;
        if (k == row_count) snprintf(rows[row_count++].name, sizeof(rows[k].name), "%s", label);
        rows[k].count += h->count;
    }
    qsort(rows, row_count, sizeof(Row), by_count);
    for (size_t i = 0; i < row_count && i < TOP; ++i)
        fprintf(out, "%6.2f%% %8llu  %s\n", 100.0 * (double)rows[i].count / (double)(samples ? samples : 1),
                (unsigned long long)rows[i].count, rows[i].name);
    free(rows);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fputs("sample SECONDS REPORT COMMAND [ARGS...]\n", stderr);
        return 2;
    }
    double seconds = atof(argv[1]);
    int go[2];
    if (pipe(go)) { perror("pipe"); return 1; }
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (!child) { /* waits until the sampling is in place, then becomes the command */
        close(go[1]);
        char c;
        if (read(go[0], &c, 1) != 1) _exit(127);
        close(go[0]);
        execvp(argv[3], argv + 3);
        perror(argv[3]);
        _exit(127);
    }
    close(go[0]);
    struct perf_event_attr attr = {
        .size = sizeof(attr), .type = PERF_TYPE_SOFTWARE, .config = PERF_COUNT_SW_TASK_CLOCK,
        .sample_period = 50000, .sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | LINK_SAMPLE, .sample_regs_user = LINK_MASK,
        .disabled = 1, .enable_on_exec = 1, .inherit = 1, .exclude_kernel = 1, .exclude_hv = 1,
        .wakeup_events = 256,
    };
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE), size = RING > page_size ? RING : page_size;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    struct pollfd fds[CPUS];
    struct perf_event_mmap_page *pages[CPUS];
    for (long cpu = 0; cpu < cpus && cpu < CPUS; ++cpu) {
        int fd = (int)syscall(SYS_perf_event_open, &attr, child, (int)cpu, -1, PERF_FLAG_FD_CLOEXEC);
        void *ring = fd < 0 ? MAP_FAILED : mmap(NULL, size + page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ring == MAP_FAILED) {
            perror(fd < 0 ? "perf_event_open" : "mmap");
            kill(child, SIGKILL);
            return 1;
        }
        fds[cpu] = (struct pollfd){fd, POLLIN, 0};
        pages[cpu] = ring;
    }
    unsigned count = (unsigned)(cpus < CPUS ? cpus : CPUS);
    if (write(go[1], "x", 1) != 1) return 1;
    close(go[1]);
    struct timespec now, end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += (time_t)seconds;
    end.tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1e9);
    if (end.tv_nsec >= 1000000000) end.tv_sec += 1, end.tv_nsec -= 1000000000;
    int status;
    bool exited = false;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) break;
        poll(fds, count, 50);
        for (unsigned i = 0; i < count; ++i) drain(pages[i], (const unsigned char *)pages[i] + page_size, size);
        if (waitpid(child, &status, WNOHANG) == child) { exited = true; break; }
    }
    double user = 0, system = 0;
    if (!exited) {
        read_maps(child);
        cpu_times(child, &user, &system);
        kill(child, SIGTERM);
        waitpid(child, &status, 0);
    }
    for (unsigned i = 0; i < count; ++i) drain(pages[i], (const unsigned char *)pages[i] + page_size, size);
    FILE *out = fopen(argv[2], "w");
    if (!out) { perror(argv[2]); return 1; }
    fprintf(out, "user %.2f s, system %.2f s; %llu samples of 50 us user time, %llu lost\n", user, system,
            (unsigned long long)samples, (unsigned long long)lost);
    report(out, &own, false);
    fputs("\nin another file's function, by the caller (the link register):\n", out);
    report(out, &called, true);
    fclose(out);
    return 0;
}
