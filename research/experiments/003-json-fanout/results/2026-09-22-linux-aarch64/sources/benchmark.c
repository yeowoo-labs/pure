#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vendor/yyjson/yyjson.h"

enum { BRANCHES=3, CACHE=16, READ_SIZE=16384, MAX_JSON=1024*1024, HIST=512 };
enum { SOURCE, OUTPUT, PARSER, KINDS };
typedef struct Item Item;
typedef struct Block {
    size_t capacity, refs;
    uint64_t since;
    unsigned kind;
    Item *views;
    _Alignas(max_align_t) unsigned char data[];
} Block;
struct Item {
    Block *block;
    Item *prev, *next;
    size_t offset, length;
    uint64_t sequence, start, ready;
};
typedef struct Queue {
    Item *items;
    size_t head, count, peak;
    const char *key;
    uint64_t hold, service, last, emitted, bytes, latency_sum, histogram[HIST];
} Queue;
typedef struct Engine {
    Queue q[BRANCHES];
    Block *cache[CACHE];
    unsigned char read_buffer[READ_SIZE];
    size_t read_at, read_end;
    size_t budget, fixed, slots, fanout, reserved, active, cached, peak;
    size_t by_kind[KINDS], peak_kind[KINDS];
    long double byte_ns[KINDS];
    uint64_t allocations[KINDS], reuses, accepted, emitted, input_bytes;
    uint64_t selection_copies, reader_copies, parser_copies, parse_ns;
    uint64_t budget_waits, slot_waits, compacted, compact_skipped, dedup_hits;
    uint64_t sleep_ns, elapsed_ns, cpu_ns, cancel_after;
    unsigned ratio;
    int pool;
    const char *policy, *status;
} Engine;
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped=1; }
static uint64_t ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock,&t)) abort();
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static size_t rounded(size_t n) { size_t c=64; while(c<n)c*=2; return c; }
static size_t charge(const Block *b) { return sizeof(*b)+b->capacity; }
static void check(Engine *e) {
    assert(e->active+e->cached==e->reserved);
    assert(e->by_kind[0]+e->by_kind[1]+e->by_kind[2]==e->active);
    assert(e->fixed+e->reserved<=e->budget);
    if(e->reserved>e->peak)e->peak=e->reserved;
    for(unsigned k=0;k<KINDS;++k)if(e->by_kind[k]>e->peak_kind[k])e->peak_kind[k]=e->by_kind[k];
}
static void evict(Engine *e,size_t i) {
    Block *b=e->cache[i]; if(!b)return;
    assert(!b->refs && !b->views);
    e->reserved-=charge(b); e->cached-=charge(b); free(b); e->cache[i]=NULL;
}
static Block *take(Engine *e,size_t n,unsigned kind) {
    size_t capacity=rounded(n),need=sizeof(Block)+capacity;
    Block *b=NULL;
    if(e->pool)for(size_t i=0;i<CACHE;++i)if(e->cache[i] && e->cache[i]->capacity==capacity) {
        b=e->cache[i]; e->cache[i]=NULL; e->cached-=need; ++e->reuses; break;
    }
    if(!b) {
        for(size_t i=0;i<CACHE && need>e->budget-e->fixed-e->reserved;++i)evict(e,i);
        if(need>e->budget-e->fixed-e->reserved)return NULL;
        b=malloc(need);
        if(!b){e->status="allocation_failed";return NULL;}
        b->capacity=capacity; e->reserved+=need; ++e->allocations[kind];
    }
    b->kind=kind;b->refs=1;b->views=NULL;b->since=ns(CLOCK_MONOTONIC);
    e->active+=need;e->by_kind[kind]+=need;check(e);return b;
}
static void drop(Engine *e,Block *b) {
    if(!b)return;
    assert(b->refs);
    if(--b->refs)return;
    assert(!b->views);
    size_t bytes=charge(b);
    e->byte_ns[b->kind]+=(long double)bytes*(ns(CLOCK_MONOTONIC)-b->since);
    e->active-=bytes;e->by_kind[b->kind]-=bytes;
    if(e->pool && bytes<=e->budget/4-e->cached)for(size_t i=0;i<CACHE;++i)if(!e->cache[i]) {
        e->cache[i]=b;e->cached+=bytes;check(e);return;
    }
    e->reserved-=bytes;free(b);check(e);
}
static void attach(Item *item) {
    Block *b=item->block;
    item->prev=NULL;item->next=b->views;
    if(b->views)b->views->prev=item;
    b->views=item;
}
static void detach(Item *item) {
    if(item->prev)item->prev->next=item->next;else item->block->views=item->next;
    if(item->next)item->next->prev=item->prev;
    item->prev=item->next=NULL;
}
static void compact(Engine *e,Block *source) {
    if(strcmp(e->policy,"compact") || source->kind!=SOURCE || source->refs!=1 || !source->views)return;
    Item *item=source->views;
    assert(!item->next);
    if(source->capacity/rounded(item->length)<e->ratio)return;
    /* Opportunistic only: never block or recurse from a completion callback. */
    Block *small=take(e,item->length,OUTPUT);
    if(!small){++e->compact_skipped;return;}
    memcpy(small->data,source->data+item->offset,item->length);
    e->selection_copies+=item->length;++e->compacted;
    detach(item);item->block=small;item->offset=0;attach(item);drop(e,source);
}
static uint64_t deadline(Queue *q) {
    uint64_t ready=q->items[q->head].ready;
    return ready>q->last+q->service?ready:q->last+q->service;
}
static int next_queue(Engine *e) {
    int selected=-1;uint64_t soon=UINT64_MAX;
    for(size_t k=0;k<e->fanout;++k)if(e->q[k].count && deadline(&e->q[k])<soon) {
        selected=(int)k;soon=deadline(&e->q[k]);
    }
    return selected;
}
static int sleep_to(Engine *e,uint64_t until) {
    uint64_t start=ns(CLOCK_MONOTONIC),now=start;
    while(now<until && !stopped) {
        uint64_t left=until-now;
        struct timespec t={(time_t)(left/1000000000),(long)(left%1000000000)};
        if(nanosleep(&t,NULL) && errno!=EINTR){e->status="sleep_failed";return 0;}
        now=ns(CLOCK_MONOTONIC);
    }
    e->sleep_ns+=now-start;return !stopped;
}
static unsigned bucket(uint64_t n) {
    unsigned k=0;for(uint64_t x=n;x>1;x>>=1)++k;
    return k<3?(unsigned)n:k*8+(unsigned)((n-(UINT64_C(1)<<k))>>(k-3));
}
static uint64_t upper(unsigned b) {
    unsigned k=b/8;
    if(b<24)return b+1;
    if(k>=63)return UINT64_MAX;
    return (UINT64_C(1)<<k)+(uint64_t)(b%8+1)*(UINT64_C(1)<<(k-3));
}
static int write_all(Engine *e,const void *data,size_t n) {
    const unsigned char *p=data;
    while(n && !stopped) {
        ssize_t w=write(STDOUT_FILENO,p,n);
        if(w<0 && errno==EINTR)continue;
        if(w<=0){e->status="output_failed";return 0;}
        p+=w;n-=(size_t)w;
    }
    return !stopped;
}
static int emit(Engine *e) {
    int k=next_queue(e);assert(k>=0);
    Queue *q=&e->q[k];Item *item=&q->items[q->head];
    if(!sleep_to(e,deadline(q)))return 0;
    unsigned char header[16];
    for(unsigned i=0;i<4;++i){header[i]=(unsigned char)((unsigned)k>>(8*i));header[4+i]=(unsigned char)(item->length>>(8*i));}
    for(unsigned i=0;i<8;++i)header[8+i]=(unsigned char)(item->sequence>>(8*i));
    if(!write_all(e,header,16) || !write_all(e,item->block->data+item->offset,item->length))return 0;
    uint64_t now=ns(CLOCK_MONOTONIC),latency=now-item->start;
    q->last=now;q->latency_sum+=latency;++q->histogram[bucket(latency)];
    ++q->emitted;q->bytes+=item->length;++e->emitted;
    Block *b=item->block;size_t old_refs=b->refs;
    detach(item);drop(e,b);item->block=NULL;
    q->head=(q->head+1)%e->slots;--q->count;
    if(old_refs>1)compact(e,b);
    if(e->cancel_after && e->emitted>=e->cancel_after){e->status="cancelled";return 0;}
    return !strcmp(e->status,"ok");
}
static Block *acquire(Engine *e,size_t n,unsigned kind) {
    for(;;) {
        Block *b=take(e,n,kind);if(b)return b;
        if(strcmp(e->status,"ok"))return NULL;
        if(next_queue(e)<0){e->status="budget_too_small";return NULL;}
        ++e->budget_waits;if(!emit(e))return NULL;
    }
}
static void *parser_alloc(void *ctx,size_t n) {
    Block *b=acquire(ctx,n,PARSER);return b?b->data:NULL;
}
static Block *from_data(void *p) {return (Block *)((unsigned char *)p-sizeof(Block));}
static void parser_free(void *ctx,void *p) {if(p)drop(ctx,from_data(p));}
static void *parser_realloc(void *ctx,void *p,size_t old,size_t n) {
    if(!p)return parser_alloc(ctx,n);
    Block *b=from_data(p);assert(b->refs==1 && b->kind==PARSER && old<=b->capacity);
    if(n<=b->capacity)return p;
    void *fresh=parser_alloc(ctx,n);if(!fresh)return NULL;
    memcpy(fresh,p,old);((Engine *)ctx)->parser_copies+=old;parser_free(ctx,p);return fresh;
}
/* Bounded NDJSON reader, accepting a final complete JSON without a newline. */
static Block *read_record(Engine *e,size_t *length) {
    Block *b=acquire(e,256,SOURCE);if(!b)return NULL;
    *length=0;
    for(;;) {
        if(stopped){drop(e,b);return NULL;}
        if(e->read_at==e->read_end) {
            ssize_t got=read(STDIN_FILENO,e->read_buffer,READ_SIZE);
            if(got<0 && errno==EINTR)continue;
            if(got<0){e->status="input_failed";drop(e,b);return NULL;}
            if(!got){if(*length)break;drop(e,b);return NULL;}
            e->read_at=0;e->read_end=(size_t)got;e->input_bytes+=(uint64_t)got;
        }
        unsigned char *p=e->read_buffer+e->read_at;
        size_t available=e->read_end-e->read_at;
        unsigned char *newline=memchr(p,'\n',available);
        size_t amount=newline?(size_t)(newline-p):available;
        if(amount>MAX_JSON-*length){e->status="record_too_large";drop(e,b);return NULL;}
        size_t need=*length+amount+YYJSON_PADDING_SIZE;
        if(need>b->capacity) {
            Block *larger=acquire(e,need,SOURCE);
            if(!larger){drop(e,b);return NULL;}
            memcpy(larger->data,b->data,*length);e->reader_copies+=*length;
            drop(e,b);b=larger;
        }
        memcpy(b->data+*length,p,amount);e->reader_copies+=amount;
        *length+=amount;e->read_at+=amount+(newline?1:0);
        if(newline)break;
    }
    memset(b->data+*length,0,YYJSON_PADDING_SIZE);return b;
}
static int submit(Engine *e,Block *source,size_t length,uint64_t start) {
    size_t offsets[BRANCHES]={0},lengths[BRANCHES]={0};
    yyjson_alc alc={parser_alloc,parser_realloc,parser_free,e};
    yyjson_read_err error;
    uint64_t before=ns(CLOCK_MONOTONIC);
    yyjson_doc *doc=yyjson_read_opts((char *)source->data,length,YYJSON_READ_INSITU,&alc,&error);
    e->parse_ns+=ns(CLOCK_MONOTONIC)-before;
    if(!doc){if(!strcmp(e->status,"ok"))e->status="invalid_json";drop(e,source);return 0;}
    yyjson_val *root=yyjson_doc_get_root(doc);
    int valid=yyjson_is_obj(root);
    for(size_t k=0;valid && k<e->fanout;++k) {
        yyjson_val *value=yyjson_obj_get(root,e->q[k].key);
        const char *str=yyjson_get_str(value);size_t size=yyjson_get_len(value);
        uintptr_t address=(uintptr_t)str,base=(uintptr_t)source->data;
        if(!yyjson_is_str(value) || address<base || address-base>length || size>length-(address-base))valid=0;
        else{offsets[k]=(size_t)(address-base);lengths[k]=size;}
    }
    /* Only decoded string offsets/lengths survive. No DOM nodes escape. */
    yyjson_doc_free(doc);
    if(!valid){e->status="invalid_schema";drop(e,source);return 0;}
    Item pending[BRANCHES]={{0}};
    for(size_t k=0;k<e->fanout;++k) {
        int copy=!strcmp(e->policy,"copy") || !strcmp(e->policy,"shared_copy") ||
            (!strcmp(e->policy,"threshold") && source->capacity/rounded(lengths[k])>=e->ratio);
        int reuse=copy && (!strcmp(e->policy,"shared_copy") || !strcmp(e->policy,"threshold"));
        Block *b=NULL;size_t offset=offsets[k];
        if(reuse)for(size_t j=0;j<k;++j)if(offsets[j]==offsets[k] && lengths[j]==lengths[k] && pending[j].block!=source) {
            b=pending[j].block;++b->refs;offset=0;++e->dedup_hits;break;
        }
        if(!b && copy) {
            b=acquire(e,lengths[k],OUTPUT);
            if(!b) {
                for(size_t j=0;j<k;++j)drop(e,pending[j].block);
                drop(e,source);return 0;
            }
            memcpy(b->data,source->data+offset,lengths[k]);e->selection_copies+=lengths[k];offset=0;
        } else if(!b){b=source;++b->refs;}
        pending[k]=(Item){.block=b,.offset=offset,.length=lengths[k],.sequence=e->accepted,.start=start};
    }
    uint64_t now=ns(CLOCK_MONOTONIC);
    for(size_t k=0;k<e->fanout;++k) {
        Queue *q=&e->q[k];assert(q->count<e->slots);
        Item *item=&q->items[(q->head+q->count)%e->slots];
        *item=pending[k];item->ready=now+q->hold;attach(item);++q->count;
        if(q->count>q->peak)q->peak=q->count;
    }
    ++e->accepted;drop(e,source);return 1;
}
static void run(Engine *e) {
    while(!stopped && !strcmp(e->status,"ok")) {
        int k=next_queue(e);
        if(k>=0 && deadline(&e->q[k])<=ns(CLOCK_MONOTONIC)){if(!emit(e))break;continue;}
        int full=0;for(size_t j=0;j<e->fanout;++j)if(e->q[j].count==e->slots)full=1;
        if(full){++e->slot_waits;if(!emit(e))break;continue;}
        size_t length;uint64_t start=ns(CLOCK_MONOTONIC);
        Block *source=read_record(e,&length);
        if(!source) {
            while(!stopped && !strcmp(e->status,"ok") && next_queue(e)>=0)if(!emit(e))break;
            break;
        }
        if(!submit(e,source,length,start))break;
    }
    if(stopped)e->status="cancelled";
}
static uint64_t number(const char *s,uint64_t max) {
    char *end;errno=0;unsigned long long n=strtoull(s,&end,10);
    if(!*s || *s=='-' || *end || errno || n>max){fprintf(stderr,"invalid number\n");exit(2);}
    return n;
}
static uint64_t rss(void) {
#if defined(__linux__)
    FILE *f=fopen("/proc/self/status","r");if(!f)return 0;
    char line[256];unsigned long long kib=0;
    while(fgets(line,sizeof(line),f))if(sscanf(line,"VmHWM: %llu kB",&kib)==1)break;
    fclose(f);return (uint64_t)kib*1024;
#else
    return 0;
#endif
}
int main(int argc,char **argv) {
    _Static_assert(sizeof(Block)==offsetof(Block,data),"aligned block header");
    Engine storage={0},*e=&storage;
    e->policy="borrow";e->status="ok";e->slots=64;e->fanout=3;e->budget=8*1024*1024;e->ratio=2;e->pool=1;
    e->q[0].key="body";e->q[1].key=e->q[2].key="tag";
    e->q[1].hold=5000000;e->q[2].hold=8000000;
    for(int i=1;i<argc;i+=2) {
        if(i+1==argc){fprintf(stderr,"option needs value\n");return 2;}
        const char *key=argv[i],*value=argv[i+1];
        if(!strcmp(key,"--policy"))e->policy=value;
        else if(!strcmp(key,"--budget"))e->budget=(size_t)number(value,1073741824);
        else if(!strcmp(key,"--slots"))e->slots=(size_t)number(value,4096);
        else if(!strcmp(key,"--fanout"))e->fanout=(size_t)number(value,BRANCHES);
        else if(!strcmp(key,"--ratio"))e->ratio=(unsigned)number(value,1024);
        else if(!strcmp(key,"--pool"))e->pool=(int)number(value,1);
        else if(!strcmp(key,"--cancel-after"))e->cancel_after=number(value,UINT64_MAX);
        else {
            int recognized=0;
            for(size_t k=0;k<BRANCHES;++k) {
                char option[32];
                snprintf(option,sizeof(option),"--key%zu",k);
                if(!strcmp(key,option)){e->q[k].key=value;recognized=1;}
                snprintf(option,sizeof(option),"--hold%zu-us",k);
                if(!strcmp(key,option)){e->q[k].hold=number(value,10000000)*1000;recognized=1;}
                snprintf(option,sizeof(option),"--service%zu-us",k);
                if(!strcmp(key,option)){e->q[k].service=number(value,10000000)*1000;recognized=1;}
            }
            if(!recognized){fprintf(stderr,"unknown option\n");return 2;}
        }
    }
    if(!e->slots || !e->fanout || e->ratio<2 || (strcmp(e->policy,"borrow") && strcmp(e->policy,"copy") &&
       strcmp(e->policy,"shared_copy") && strcmp(e->policy,"threshold") && strcmp(e->policy,"compact"))) {
        fprintf(stderr,"invalid configuration\n");return 2;
    }
    e->fixed=sizeof(*e)+e->fanout*e->slots*sizeof(Item);
    if(e->fixed>e->budget){fprintf(stderr,"budget below metadata\n");return 2;}
    for(size_t k=0;k<e->fanout;++k) {
        e->q[k].items=calloc(e->slots,sizeof(Item));
        if(!e->q[k].items){for(size_t j=0;j<k;++j)free(e->q[j].items);return 2;}
    }
    signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGPIPE,SIG_IGN);
    uint64_t start=ns(CLOCK_MONOTONIC),cpu=ns(CLOCK_PROCESS_CPUTIME_ID);
    run(e);
    size_t live_at_stop=e->active;
    for(size_t k=0;k<e->fanout;++k) {
        Queue *q=&e->q[k];
        while(q->count){Item *item=&q->items[q->head];detach(item);drop(e,item->block);q->head=(q->head+1)%e->slots;--q->count;}
    }
    for(size_t i=0;i<CACHE;++i)evict(e,i);
    assert(!e->active && !e->reserved);
    e->elapsed_ns=ns(CLOCK_MONOTONIC)-start;e->cpu_ns=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    fprintf(stderr,"{\"status\":\"%s\",\"policy\":\"%s\",\"parser_version\":\"%s\",\"budget_bytes\":%zu,"
        "\"fixed_bytes\":%zu,\"peak_reserved_bytes\":%zu,\"source_peak_bytes\":%zu,\"output_peak_bytes\":%zu,"
        "\"parser_peak_bytes\":%zu,\"rss_peak_bytes\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ","
        "\"parse_call_ns\":%" PRIu64 ",\"accepted\":%" PRIu64 ",\"emitted\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ","
        "\"selection_copy_bytes\":%" PRIu64 ",\"reader_copy_bytes\":%" PRIu64 ",\"parser_realloc_copy_bytes\":%" PRIu64 ","
        "\"source_byte_seconds\":%.6Lf,\"output_byte_seconds\":%.6Lf,\"parser_byte_seconds\":%.6Lf,"
        "\"source_allocations\":%" PRIu64 ",\"output_allocations\":%" PRIu64 ",\"parser_allocations\":%" PRIu64 ","
        "\"reuses\":%" PRIu64 ",\"budget_waits\":%" PRIu64 ",\"slot_waits\":%" PRIu64 ","
        "\"compacted\":%" PRIu64 ",\"compact_skipped\":%" PRIu64 ",\"dedup_hits\":%" PRIu64 ",\"sleep_ns\":%" PRIu64 ","
        "\"live_at_stop_bytes\":%zu,\"unreleased_buffer_bytes\":%zu,\"queues\":[",
        e->status,e->policy,YYJSON_VERSION_STRING,e->budget,e->fixed,e->fixed+e->peak,e->peak_kind[SOURCE],
        e->peak_kind[OUTPUT],e->peak_kind[PARSER],rss(),e->elapsed_ns,e->cpu_ns,e->parse_ns,e->accepted,e->emitted,
        e->input_bytes,e->selection_copies,e->reader_copies,e->parser_copies,e->byte_ns[SOURCE]/1e9L,
        e->byte_ns[OUTPUT]/1e9L,e->byte_ns[PARSER]/1e9L,e->allocations[SOURCE],e->allocations[OUTPUT],
        e->allocations[PARSER],e->reuses,e->budget_waits,e->slot_waits,e->compacted,e->compact_skipped,
        e->dedup_hits,e->sleep_ns,live_at_stop,e->reserved);
    for(size_t k=0;k<e->fanout;++k) {
        Queue *q=&e->q[k];uint64_t target=q->emitted-q->emitted/100,sum=0,p99=0;
        for(unsigned b=0;q->emitted && b<HIST;++b){sum+=q->histogram[b];if(sum>=target){p99=upper(b);break;}}
        fprintf(stderr,"%s{\"emitted\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"peak_count\":%zu,"
            "\"latency_mean_ns\":%.3f,\"latency_p99_upper_ns\":%" PRIu64 "}",k?",":"",q->emitted,q->bytes,q->peak,
            q->emitted?(double)q->latency_sum/q->emitted:0,p99);
        free(q->items);
    }
    fprintf(stderr,"]}\n");return !strcmp(e->status,"ok")?0:!strcmp(e->status,"cancelled")?3:2;
}
