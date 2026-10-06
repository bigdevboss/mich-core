#include "posix_io.h"
#include "task.h"
#include "vm64.h"

int posix_io_patch_result(struct task *task, uptr_t request, u32 transferred) {
    return vm64_copy_to(task->page_dir, request + POSIX_IO_TRANSFERRED_OFFSET,
                        &transferred, sizeof(transferred));
}
