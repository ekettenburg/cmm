#define _POSIX_C_SOURCE 199309L
#include "cmm_runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
int main(void){
    cmm_init();
    size_t N=200000, cap=128u<<20, len=0; char *buf=(char*)malloc(cap), tmp[256];
    buf[len++]='[';
    for(size_t i=0;i<N;i++){ if(i) buf[len++]=',';
        int n=snprintf(tmp,sizeof tmp,
          "{\"id\":%zu,\"name\":\"user_%zu\",\"active\":%s,\"score\":%.3f,\"tags\":[\"x\",\"y\",\"z\"]}",
          i,i,(i&1)?"true":"false",(double)(i%100)/7.0);
        memcpy(buf+len,tmp,(size_t)n); len+=(size_t)n; }
    buf[len++]=']'; buf[len]=0;
    double mb=len/1e6; printf("doc: %.1f MB, %zu objects\n",mb,N);
    CmmValue jstr=cmm_str_n(buf,len);
    int iters=5; double t0=now(); CmmValue parsed=cmm_empty();
    for(int it=0; it<iters; it++){ cmm_frame_enter(); CmmValue v=cmm_json_decode(jstr);
        if(it==iters-1) parsed=cmm_frame_leave(v); else cmm_frame_leave(cmm_empty()); }
    double t1=now(); printf("decode: %6.1f MB/s\n", mb*iters/(t1-t0));
    double t2=now(); size_t outlen=0;
    for(int it=0; it<iters; it++){ cmm_frame_enter(); CmmValue s=cmm_json_encode(parsed); outlen=s.s->len; cmm_frame_leave(cmm_empty()); }
    double t3=now(); printf("encode: %6.1f MB/s\n", (outlen/1e6)*iters/(t3-t2));
    return 0;
}
