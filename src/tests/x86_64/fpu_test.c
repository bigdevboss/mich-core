// Lazy-FPU benefit probe: how much does a full FPU/XMM context save (fxsave,
// 512B) + restore (fxrstor, 512B) cost per context switch, and how many
// context switches the test-suite run performs?
//
// The scheduler saves/restores the FPU on EVERY switch (kernel.c
// scheduler64_switch -> fpu64_save/fpu64_load, no fpu_dirty gate). If this
// pair is cheap in TCG and most switches already use FPU/SSE, lazy FPU
// (CR0.TS + #NM, save only on demand) would save little -> not worth it.
// If it is expensive and many switches are FPU-clean, lazy FPU pays off.
//
// Measurement: time N (fxsave+fxrstor) pairs with rdtsc, report cycles/pair.
// The pair is the exact cost the scheduler pays per switch.

#include "net_test.h"
#include "kernel64_internal.h"

#define FPU_BENCH_ROUNDS 512

// fxsave/fxrstor require a 16-byte aligned area; a stack local is not
// guaranteed to be, so use a static aligned buffer. fxsave overwrites it,
// so no need to pre-zero.
static u8 fpu_bench_state[512] __attribute__((aligned(16)));

static u64 fpu64_bench(u64 rounds)
{
    u64 i;
    u64 t0;
    u64 cycles = 0;

    // Warm up the FPU (so the measured loop is representative).
    for (i = 0; i < 64; i++) {
        asm volatile("fxsave %0" :: "m"(*fpu_bench_state) : "memory");
        asm volatile("fxrstor %0" :: "m"(*fpu_bench_state) : "memory");
    }

    t0 = test_cycles();
    for (i = 0; i < rounds; i++) {
        asm volatile("fxsave %0" :: "m"(*fpu_bench_state) : "memory");
        asm volatile("fxrstor %0" :: "m"(*fpu_bench_state) : "memory");
    }
    cycles = test_cycles() - t0;

    return cycles;
}

int test_fpu64(void)
{
    u64 cycles = fpu64_bench(FPU_BENCH_ROUNDS);
    u64 per_pair = cycles / FPU_BENCH_ROUNDS;

    // per_pair (hex): cost of one fxsave+fxrstor pair (what the scheduler
    // pays per context switch). ctxswitch (hex): total switches so far.
    serial64_write("Mich x86_64: FPU fxsave+fxrstor=0x");
    serial64_hex(per_pair);
    serial64_write(" cyc/pair, ctxswitch=0x");
    serial64_hex(scheduler64_switch_count());
    serial64_write("\n");

    return cycles ? 0 : -1;
}
