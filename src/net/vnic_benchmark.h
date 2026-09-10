#ifndef VNIC_BENCHMARK_H
#define VNIC_BENCHMARK_H

#include "object.h"
#include "net_abi.h"

int vnic_benchmark_run(struct kernel_object *vnic,
                       struct vnic_benchmark_request *request);

enum {
    VNIC_BENCH64_RX = 0,
    VNIC_BENCH64_TX = 1,
    VNIC_BENCH64_ECHO = 2
};

#define VNIC_BENCH64_SAMPLE_COUNT 4096

struct vnic_bench64_result {
    u64 min_cycles;
    u64 avg_cycles;
    u64 p50_cycles;
    u64 p99_cycles;
};

int vnic_bench64_run(struct kernel_object *vnic, u32 direction,
                     u32 frame_size, struct vnic_bench64_result *result);

#endif
