/* token_bench_abi.c — P2 token 走公共 ABI（含前端 dispatch）。 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include "kvspace/kvspace.h"
/* P2 ③: 释放 pin */

/* 计时器：x86 rdtscp / arm64 cntvct_el0 / 其它单调时钟。calib() 把 tick 折算成
 * “等价核心周期”，所以三种计时器都可与 50c 目标（cycles）比较。 */
static inline uint64_t ticks(void){
#if defined(__x86_64__)
 uint32_t lo,hi;__asm__ __volatile__("lfence\n\trdtscp\n\tlfence":"=a"(lo),"=d"(hi)::"rcx");
 return ((uint64_t)hi<<32)|lo;
#elif defined(__aarch64__)
 uint64_t v;__asm__ __volatile__("isb\n\tmrs %0, cntvct_el0\n\tisb":"=r"(v));return v;
#else
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
#endif
}
static uint64_t g_cal_n=100000;
static double calib(void){static volatile uint64_t sink;uint64_t it=g_cal_n/8,best=~0ULL;
 for(int k=0;k<5;k++){uint64_t x=1,t0=ticks();
  for(uint64_t i=0;i<it;i++)__asm__ __volatile__("add $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0":"+r"(x));
  uint64_t t1=ticks();sink=x;if(t1-t0<best)best=t1-t0;}
 return (double)(it*8)/(double)best;}
static volatile uint64_t g_sink;
#define DIE(...) do{fprintf(stderr,"FATAL: "__VA_ARGS__);fputc('\n',stderr);exit(2);}while(0)

int main(int argc,char**argv){
 if(argc<6)DIE("usage: %s op key batches ops cal_n [rel]",argv[0]);
 const char*op=argv[1],*key=argv[2];int batches=atoi(argv[3]);
 uint64_t ops=strtoull(argv[4],NULL,10);g_cal_n=strtoull(argv[5],NULL,10);
 const char*rel=argc>6?argv[6]:"t0";
 const char*store=getenv("TOK_STORE");if(!store)store="/tmp/cycle-fastpath/p2/shm/a.dat";
 uint8_t raw[8]={'0','1','2','3','4','5','6','7'};uint8_t*val=NULL;uint32_t vl=0;
 if(kvspaceNewChar(raw,8,&val,&vl)!=0)DIE("encode");
 char dsn[512];snprintf(dsn,sizeof dsn,"shm://%s",store);
 void*h=kvspaceConnect(dsn);if(!h)DIE("connect");
 char err[256];
 if(kvspaceSetValue(h,key,val,vl,0,0,err,sizeof err)!=0)DIE("setup");
#ifdef ABI_PLAIN_ONLY
 /* 发布回归臂（v0.3.16 公共 ABI，无 View*）：pget / pset / pdel。
  * 口径与 50c 目标不同（Get 含拷贝、Del 只计 kvspaceDel），仅作发布对照。 */
 for(int b=0;b<batches;b++){
  double ca=calib();uint64_t t0=ticks(),sum=0;
  if(strcmp(op,"pget")==0){for(uint64_t i=0;i<ops;i++){uint8_t*o=NULL;uint32_t gl=0;
      if(kvspaceGet(h,key,1,&o,&gl)!=0)DIE("pget");sum+=gl;}}
  else if(strcmp(op,"pset")==0){for(uint64_t i=0;i<ops;i++){
      if(kvspaceSetValue(h,key,val,vl,0,0,err,sizeof err)!=0)DIE("pset");}}
  else if(strcmp(op,"pdel")==0){for(uint64_t i=0;i<ops;i++){
      if(kvspaceSetValue(h,key,val,vl,0,0,err,sizeof err)!=0)DIE("rebuild");
      uint64_t a=ticks();int rc=kvspaceDel(h,&key,1,err,sizeof err);uint64_t c=ticks();
      if(rc!=0)DIE("pdel");sum+=c-a;}}
  else DIE("unknown plain op");
  uint64_t t1=ticks();double cb=calib();uint64_t dt=t1-t0;double drift=(cb-ca)/((cb+ca)*0.5)*100.0;g_sink+=sum;
  double tpo=(strcmp(op,"pdel")==0)?(double)sum/(double)ops:(double)dt/(double)ops;
  printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64 "\n",
         op,0,b,ops,dt,tpo,ca,cb,drift,tpo*(ca+cb)*0.5,(uint64_t)g_sink);
  fflush(stdout);}
 kvspaceClose(h);return 0;
#else
 { int q=kvspaceViewQuiesce(h,0); if(q!=0 && q!=-2) DIE("quiesce off"); }
 kvspaceRef_t ref;if(kvspaceResolveRef(h,key,&ref)!=0)DIE("resolve ref");
 kvspaceRef_t pref={0};
 const char*ppfx=getenv("TOK_PREFIX");
 if(ppfx && kvspaceViewResolvePrefix(h,ppfx,&pref)!=0) DIE("prefix resolve");
 kvspaceViewToken_t t;if(kvspaceViewResolve(h,key,&t)!=0)DIE("view resolve");
 printf("# op=%s key=%s sg=%u vg=%u node=%u parent=%u depth=%u\n",op,key,t.struct_gen,t.value_gen,t.node_id,ref.parent_id,ref.depth);
 printf("op,run,batch,ops,ticks,ticks_per_op,cal_before,cal_after,drift_pct,core_cycles_per_op,checksum\n");
 if(strcmp(op,"del_tokens")==0){
  enum { KMAX = 4096 }; int K = getenv("TOK_K") ? atoi(getenv("TOK_K")) : KMAX;
  static char keys[KMAX][128]; static kvspaceViewToken_t toks[KMAX];
  const char*base = getenv("TOK_PREFIX"); if(!base) base="/vthread/1/[1]/";
  { size_t bl=strlen(base); int slash = bl>0 && base[bl-1]=='/';
    for(int i=0;i<K;i++) snprintf(keys[i],sizeof keys[i],"%s%sk%d",base,slash?"":"/",i); }
  /* 两阶段：先完成全部结构构造，再解析 token（避免插入过程使 token 失效）。 */
  for(int i=0;i<K;i++){ if(kvspaceSetValue(h,keys[i],val,vl,0,0,err,sizeof err)!=0) DIE("setup k"); }
  for(int i=0;i<K;i++){ if(kvspaceViewResolve(h,keys[i],&toks[i])!=0) DIE("resolve k"); }
  for(int b=0;b<batches;b++){
    static uint64_t samp[4096];
    double ca=calib(); uint64_t t0=ticks(), s2=0;
    for(int i=0;i<K;i++){ uint64_t a=ticks(); int rc=kvspaceViewDel(h,&toks[i],keys[i]); uint64_t c=ticks();
      if(rc!=0) DIE("tokdel"); samp[i]=c-a; s2+=c-a; }
    for(int i=1;i<K;i++){ uint64_t v=samp[i]; int j=i-1; while(j>=0&&samp[j]>v){samp[j+1]=samp[j];j--;} samp[j+1]=v; }
    uint64_t p50=samp[K/2], p95=samp[(int)(K*0.95)], p99=samp[(int)(K*0.99)], mx=samp[K-1];
    int mxi=0; for(int i=0;i<K;i++) if(samp[i]==mx){ mxi=i; break; }
    uint64_t t1=ticks();
    /* 重建 + 回收都放在计时窗口之外 */
    int (*sw)(void*) = (int(*)(void*))dlsym(RTLD_DEFAULT,"kvspaceViewSweep");
    if(sw) sw(h);   /* 后台回收：窗口外 */
    /* token 无 pin：无需 release */
    for(int i=0;i<K;i++){ if(kvspaceSetValue(h,keys[i],val,vl,0,0,err,sizeof err)!=0) DIE("reset k"); }
    for(int i=0;i<K;i++){ int rr=kvspaceViewResolve(h,keys[i],&toks[i]);
      if(rr!=0){ uint8_t *o=NULL; uint32_t gl=0; (void)kvspaceGet(h,keys[i],1,&o,&gl);
        fprintf(stderr,"DBG re-resolve i=%d rc=%d key=%s get=%d\n",i,rr,keys[i],o!=NULL); DIE("re-resolve k"); } }
    double cb=calib(); uint64_t dt=t1-t0; double drift=(cb-ca)/((cb+ca)*0.5)*100.0; g_sink+=s2;
    double tpo=(double)s2/(double)K;
    printf("del_tokens,%d,%d,%d,%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64 ",%llu,%llu,%llu,%llu\n",
           b,b,K,dt,tpo,ca,cb,drift,tpo*(ca+cb)*0.5,(uint64_t)g_sink,
           (unsigned long long)p50,(unsigned long long)p95,(unsigned long long)p99,(unsigned long long)mx);
    fprintf(stderr,"DBG max_idx=%d (sorted pos %d/%d)\n", mxi, 0, K);
    fflush(stdout); }
  kvspaceClose(h); return 0; }
 for(int b=0;b<batches;b++){
  double ca=calib();uint64_t t0=ticks(),sum=0;
  if(strcmp(op,"resolve")==0){for(uint64_t i=0;i<ops;i++){kvspaceViewToken_t x;if(kvspaceViewResolve(h,key,&x)!=0)DIE("r");sum+=x.node_id;}}
  else if(strcmp(op,"resolve_from")==0){for(uint64_t i=0;i<ops;i++){kvspaceViewToken_t x;if(kvspaceViewResolveFrom(h,&pref,rel,&x)!=0)DIE("rf");sum+=x.node_id;}}
  else if(strcmp(op,"from_ref")==0){for(uint64_t i=0;i<ops;i++){kvspaceViewToken_t x;if(kvspaceViewFromRef(h,&ref,key,&x)!=0)DIE("fr");sum+=x.node_id;}}
  else if(strcmp(op,"get")==0){for(uint64_t i=0;i<ops;i++){if(kvspaceViewCheck(h,&t)!=0){if(kvspaceViewResolve(h,key,&t)!=0)DIE("rf");}sum+=t.body[0]+t.body[7];}}
  else if(strcmp(op,"set")==0){for(uint64_t i=0;i<ops;i++){if(kvspaceViewWrite(h,&t,raw,8)!=0){if(kvspaceViewResolve(h,key,&t)!=0)DIE("rf");if(kvspaceViewWrite(h,&t,raw,8)!=0)DIE("w");}sum+=t.body[0];}}
  else if(strcmp(op,"del")==0){for(uint64_t i=0;i<ops;i++){
      if(kvspaceSetValue(h,key,val,vl,0,0,err,sizeof err)!=0)DIE("rebuild");
      if(kvspaceViewResolve(h,key,&t)!=0)DIE("r2");
      uint64_t a=ticks();int rc=kvspaceViewDel(h,&t,key);uint64_t c=ticks();if(rc!=0)DIE("del");sum+=c-a;}}
  else DIE("unknown");
  uint64_t t1=ticks();double cb=calib();uint64_t dt=t1-t0;double drift=(cb-ca)/((cb+ca)*0.5)*100.0;g_sink+=sum;
  double tpo=(strcmp(op,"del")==0)?(double)sum/(double)ops:(double)dt/(double)ops;
  printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64 "\n",op,0,b,ops,dt,tpo,ca,cb,drift,tpo*(ca+cb)*0.5,(uint64_t)g_sink);
  fflush(stdout);}
 kvspaceClose(h);return 0;
#endif
}
