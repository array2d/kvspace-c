/* token_bench.c — P2 token 热路径（直连 libkvspace-c，绕过前端 dispatch）。 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvspace_shm.h"
extern int kvspaceNewChar(const uint8_t *, uint32_t, uint8_t **, uint32_t *);

typedef kvspaceViewToken_t tok_t;
extern int kvspaceShmViewResolve(kvspace_t *, const char *, tok_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const tok_t *);
extern int kvspaceShmViewWrite(kvspace_t *, tok_t *, const uint8_t *, uint32_t);
extern int kvspaceShmViewDel(kvspace_t *, tok_t *, const char *);
extern int kvspaceShmViewFlushTomb(kvspace_t *);

static inline uint64_t ticks(void) { uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtscp\n\tlfence" : "=a"(lo), "=d"(hi)::"rcx");
    return ((uint64_t)hi << 32) | lo; }
static uint64_t g_cal_n = 100000;
static double calib(void) { static volatile uint64_t sink; uint64_t it = g_cal_n/8, best=~0ULL;
    for (int k=0;k<5;k++){ uint64_t x=1,t0=ticks();
        for (uint64_t i=0;i<it;i++) __asm__ __volatile__("add $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0":"+r"(x));
        uint64_t t1=ticks(); sink=x; if(t1-t0<best) best=t1-t0; }
    return (double)(it*8)/(double)best; }
static volatile uint64_t g_sink;
#define DIE(...) do{ fprintf(stderr,"FATAL: "__VA_ARGS__); fputc('\n',stderr); exit(2);}while(0)

int main(int argc, char **argv) {
    if (argc < 6) DIE("usage: %s op key batches ops cal_n [run]", argv[0]);
    const char *op = argv[1], *key = argv[2];
    int batches = atoi(argv[3]); uint64_t ops = strtoull(argv[4],NULL,10);
    g_cal_n = strtoull(argv[5],NULL,10); int run = argc>6?atoi(argv[6]):0;
    const char *store = getenv("TOK_STORE"); if(!store) store="/tmp/cycle-fastpath/p2/shm/t.dat";
    uint8_t raw[8]={'0','1','2','3','4','5','6','7'};
    uint8_t *val=NULL; uint32_t vl=0;
    if (kvspaceNewChar(raw,8,&val,&vl)!=0) DIE("encode");
    kvspace_t *kv = kvspaceShmOpen(store, 8ull*64*64*64*64);
    if (!kv) DIE("open");
    if (kvspaceShmSetValue(kv,key,val,(int32_t)vl,0,0)!=0) DIE("setup");
    tok_t t; if (kvspaceShmViewResolve(kv,key,&t)!=0) DIE("view resolve");
    printf("# op=%s key=%s struct_gen=%u value_gen=%u node=%u\n", op,key,t.struct_gen,t.value_gen,t.node_id);
    printf("op,run,batch,ops,ticks,ticks_per_op,cal_before,cal_after,drift_pct,core_cycles_per_op,checksum\n");
    int op_flush=0;
    for (int b=0;b<batches;b++){
        double ca=calib(); uint64_t t0=ticks(), sum=0;
        if (strcmp(op,"resolve")==0){ for(uint64_t i=0;i<ops;i++){ tok_t x; if(kvspaceShmViewResolve(kv,key,&x)!=0) DIE("resolve"); sum+=x.node_id; } }
        else if (strcmp(op,"get")==0){ for(uint64_t i=0;i<ops;i++){ if(kvspaceShmViewCheck(kv,&t)!=0){ if(kvspaceShmViewResolve(kv,key,&t)!=0) DIE("refresh"); } sum += t.body[0]+t.body[7]; } }
        else if (strcmp(op,"set")==0){ for(uint64_t i=0;i<ops;i++){ if(kvspaceShmViewWrite(kv,&t,raw,8)!=0){ if(kvspaceShmViewResolve(kv,key,&t)!=0) DIE("refresh"); if(kvspaceShmViewWrite(kv,&t,raw,8)!=0) DIE("write"); } sum+=t.body[0]; } }
        else if (strcmp(op,"del")==0){ for(uint64_t i=0;i<ops;i++){
                if(kvspaceShmSetValue(kv,key,val,(int32_t)vl,0,0)!=0) DIE("rebuild");
                if(kvspaceShmViewResolve(kv,key,&t)!=0) DIE("resolve2");
                uint64_t a=ticks(); int rc=kvspaceShmViewDel(kv,&t,key); uint64_t c=ticks();
                if(rc!=0) DIE("del"); sum+=c-a; } }
        else if (strcmp(op,"del_pure")==0){ for(uint64_t i=0;i<ops;i++){
                if(kvspaceShmSetValue(kv,key,val,(int32_t)vl,0,0)!=0) DIE("rebuild");
                if(kvspaceShmViewResolve(kv,key,&t)!=0) DIE("resolve2");
                uint64_t a=ticks(); int rc=kvspaceShmViewDel(kv,&t,key); uint64_t c=ticks();
                if(rc!=0) DIE("del"); sum+=c-a; }
            op_flush=1; }
        else DIE("unknown op");
        uint64_t t1=ticks();
        if (op_flush) kvspaceShmViewFlushTomb(kv);   /* 回收放在计时窗口之外 */
        double cb=calib(); uint64_t dt=t1-t0;
        double drift=(cb-ca)/((cb+ca)*0.5)*100.0; g_sink+=sum;
        double tpo = (strcmp(op,"del")==0)? (double)sum/(double)ops : (double)dt/(double)ops;
        printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64 "\n",
               op,run,b,ops,dt,tpo,ca,cb,drift,tpo*(ca+cb)*0.5,(uint64_t)g_sink);
        fflush(stdout);
    }
    kvspaceShmClose(kv); return 0;
}
