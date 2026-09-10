#include "exception.h"
#include "paging.h"
#include "proc.h"
#include "protos.h"
#include "scheduler.h"
#include "serial.h"
#include "task.h"

static int exception_is_process_local(unsigned int vec) {
    return vec < 32 && vec != 2 && vec != 8 && vec != 18;
}

void exception_dispatch(unsigned int vec, unsigned int err,
                        unsigned int cr2, unsigned int eip,
                        unsigned int cs) {
    int current = scheduler_current();
    int from_user = (cs & 3u) == 3u;

    if (from_user && vec == 14 && (err & 3u) == 3u && current > 0 &&
        current < task_pool_count &&
        paging_handle_cow(task_pool[current].page_dir, cr2))
        return;

    if (from_user && current > 0 && current < task_pool_count &&
        exception_is_process_local(vec)) {
        struct task *t = &task_pool[current];
        serial_write("Mich: user exception pid=0x");
        serial_write_hex((unsigned int)t->id);
        serial_write(" vec=0x");
        serial_write_hex(vec);
        serial_write(" err=0x");
        serial_write_hex(err);
        serial_write(" eip=0x");
        serial_write_hex(eip);
        if (vec == 14) {
            serial_write(" cr2=0x");
            serial_write_hex(cr2);
        }
        serial_write("; terminating task\n");
        proc_exit(128 + (int)vec);
    }

    panic(vec, err, cr2, eip);
}
