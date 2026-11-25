// membench: times the kernel's memcpy/memset/memset32/memcmp implementations
// one by one through the MemBench syscall (162, src/cpu/syscall/mem_bench.cpp)
// and prints MB/s per size, for the dispatch the kernel uses ("auto") and for
// each variant: rep string instructions (the byte loop for memcmp), SSE2, AVX2.
// Freestanding like the feature suite; runs as /bin/login of the suite image:
//
//   RUNNER_SRC=tools/membench/membench.c OUT_DIR=build/membench \
//     tools/os-testsuite/build-testsuite-iso.sh
//
// then boot it and read the "[membench]" lines (tools/membench/run-membench-win.sh).
#include "../os-testsuite/ostest.h"

enum { N_MEMBENCH = 162 };
enum { OP_MEMCPY = 0, OP_MEMSET = 1, OP_MEMSET32 = 2, OP_MEMCMP = 3, OP_INFO = 255 };
static const char* const OP_NAME[] = {"memcpy", "memset", "memset32", "memcmp"};
static const char* const VAR_NAME[] = {"auto", "rep", "sse2", "avx2"};

static i64 bench(int op, int variant, int dstOff, int srcOff, u64 size, u64 reps){
    u64 spec = (u64)op | (u64)variant << 8 | (u64)dstOff << 16 | (u64)srcOff << 24;
    return SC3(N_MEMBENCH, spec, size, reps);
}

// Each row is built here and written with one serial call, so the log lines of
// services starting meanwhile cannot land in the middle of it.
static char line[256];
static unsigned len;
static void put(const char* s){ while(*s && len < sizeof(line)-1) line[len++] = *s++; }
static void pad(const char* s, int width){ put(s); for(int i=(int)slen(s); i<width; i++) put(" "); }
static void dec(i64 v){ char b[24]; int i=23; b[23]=0; int n=v<0; u64 u=n?(u64)(-v):(u64)v; if(u==0) b[--i]='0';
    while(u){ b[--i]=(char)('0'+u%10); u/=10; } if(n) b[--i]='-'; put(&b[i]); }
static void pad_dec(i64 v, int width){
    char b[24]; int i=23; b[23]=0; if(v==0) b[--i]='0';
    while(v>0){ b[--i]=(char)('0'+v%10); v/=10; }
    for(int k=23-i; k<width; k++) put(" ");
    put(&b[i]);
}

static i64 cpu_us;

// One row: MB/s for each variant (decimal MB, 10^6 bytes/s); "-" when the CPU
// lacks it, "ERR" when the kernel found its result wrong.
static void row(int op, u64 size, int dstOff, int srcOff){
    u64 reps = (8u * 1024 * 1024) / size; if(reps < 1) reps = 1; if(reps > (1u << 20)) reps = 1u << 20;
    len = 0;
    put("[membench] "); pad(OP_NAME[op], 9);
    pad_dec((i64)size, 8); put(" off "); dec(dstOff); put("/"); dec(srcOff); put(" ");
    for(int v=0; v<4; v++){
        i64 c = bench(op, v, dstOff, srcOff, size, reps);
        put(" "); put(VAR_NAME[v]); put("=");
        if(c == -19){ pad("-", 9); continue; }        // ENODEV
        if(c == -5){ pad("ERR", 9); continue; }       // EIO
        if(c < 0){ put("e"); dec(c); continue; }
        // MB/s when the TSC rate is known, else the cycles for the 8 MiB.
        if(cpu_us > 0 && c > 0) pad_dec((i64)((size * reps * (u64)cpu_us) / (u64)c), 9);
        else pad_dec(c, 9);
    }
    put("\n");
    swn(line, len);
}

void _start(void){
    msleep(2000);   // let the services finish starting (their log lines)
    cpu_us = bench(OP_INFO, 0, 0, 0, 0, 0);
    len = 0;
    put("\n[membench] BEGIN kernel memory routines, best of 5 runs of 8 MiB each\n");
    put("[membench] tsc cycles_per_us="); dec(cpu_us);
    put(cpu_us > 0 ? " (cells: MB/s, higher is better)\n" : " (cells: TSC cycles per 8 MiB, lower is better)\n");
    swn(line, len);
    static const u64 sizes[] = {64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, 1048576, 8388608};
    for(int op=0; op<4; op++){
        for(unsigned i=0; i<sizeof(sizes)/sizeof(sizes[0]); i++) row(op, sizes[i], 0, 0);
        // Misaligned: destination +1, source +3 (memset/memset32 use only the destination).
        static const u64 mis[] = {256, 4096, 65536, 1048576};
        for(unsigned i=0; i<sizeof(mis)/sizeof(mis[0]); i++)
            row(op, mis[i], op==OP_MEMSET32 ? 4 : 1, op==OP_MEMSET32 ? 0 : 3);
    }
    sw("[membench] END\n");
    for(;;) SC1(N_SLEEP, 1000);
}
