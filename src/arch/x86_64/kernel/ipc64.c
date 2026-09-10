#include "ipc64.h"
#include "task.h"
#include "scheduler.h"
#include "vm64.h"
#include "protos.h"
#include "runtime64.h"

static struct message pending[MAX_TASKS];
static u8 pending_valid[MAX_TASKS];
static u8 pending_timed[MAX_TASKS];

static struct task *task_for_pid(u32 pid) {
    u32 slot = PID_SLOT(pid);
    if (slot == 0 || slot >= (u32)task_pool_count) return 0;
    struct task *task = &task_pool[slot];
    if (task->state == TASK_FREE || task->state == TASK_ZOMBIE ||
        (u32)task->id != pid)
        return 0;
    return task;
}

static int deadlock(struct task *sender, struct task *destination) {
    struct task *cursor = destination;
    for (int depth = 0; depth < MAX_TASKS; depth++) {
        if (cursor == sender) return 1;
        if (cursor->state != TASK_BLOCKED_SEND || !cursor->send_to) return 0;
        cursor = task_for_pid(cursor->send_to);
        if (!cursor) return 0;
    }
    return 1;
}

static int deliver(struct task *receiver, vaddr_t address,
                   const struct message *message) {
    return vm64_copy_to(receiver->page_dir, address, message,
                        sizeof(*message)) ? EINVAL : 0;
}

static int send_now(struct task *sender, struct task *receiver,
                    const struct message *message) {
    struct message delivered = *message;
    delivered.from_id = (u32)sender->id;
    int result = deliver(receiver, (vaddr_t)(uptr_t)receiver->recv_buf,
                         &delivered);
    receiver->recv_buf = 0;
    receiver->recv_expect = 0;
    receiver->state = TASK_RUNNING;
    task64_set_result((u32)(receiver - task_pool), result);
    return result;
}

void ipc64_init(void) {
    for (int slot = 0; slot < MAX_TASKS; slot++) {
        pending_valid[slot] = 0;
        pending_timed[slot] = 0;
    }
}

static int send_internal(u32 destination, vaddr_t message_address,
                         u32 deadline, int timed) {
    int current = scheduler_current();
    struct task *sender = &task_pool[current];
    struct task *receiver = task_for_pid(destination);
    if (!receiver || receiver == sender) return receiver ? EDEADLK : ESRCH;
    struct message message;
    if (vm64_copy_from(sender->page_dir, &message, message_address,
                       sizeof(message)))
        return EINVAL;
    if (receiver->state == TASK_BLOCKED_RECV && receiver->recv_buf &&
        (!receiver->recv_expect || receiver->recv_expect == (u32)sender->id))
        return send_now(sender, receiver, &message);
    if (deadlock(sender, receiver)) return EDEADLK;
    pending[current] = message;
    pending_valid[current] = 1;
    sender->send_to = destination;
    sender->send_deadline = deadline;
    sender->state = TASK_BLOCKED_SEND;
    pending_timed[current] = timed != 0;
    if (scheduler_pick_next(current) < 0) {
        pending_valid[current] = 0;
        pending_timed[current] = 0;
        sender->send_to = 0;
        sender->send_deadline = 0;
        sender->state = TASK_RUNNING;
        return EDEADLK;
    }
    return (int)task64_block_switch();
}

int ipc64_send(u32 destination, vaddr_t message_address) {
    return send_internal(destination, message_address, 0, 0);
}

int ipc64_send_timeout(u32 destination, vaddr_t message_address,
                       u32 deadline) {
    return send_internal(destination, message_address, deadline, 1);
}

int ipc64_send_nb(u32 destination, vaddr_t message_address) {
    int current = scheduler_current();
    struct task *sender = &task_pool[current];
    struct task *receiver = task_for_pid(destination);
    if (!receiver || receiver == sender) return receiver ? EDEADLK : ESRCH;
    if (receiver->state != TASK_BLOCKED_RECV || !receiver->recv_buf ||
        (receiver->recv_expect && receiver->recv_expect != (u32)sender->id))
        return -1;
    struct message message;
    if (vm64_copy_from(sender->page_dir, &message, message_address,
                       sizeof(message)))
        return EINVAL;
    return send_now(sender, receiver, &message);
}

int ipc64_recv(u32 expected, vaddr_t message_address) {
    int current = scheduler_current();
    struct task *receiver = &task_pool[current];
    if (vm64_user_access(receiver->page_dir, message_address,
                         sizeof(struct message), 1))
        return EINVAL;
    if (expected == (u32)receiver->id) return EDEADLK;
    if (expected && !task_for_pid(expected)) {
        struct message probe;
        probe.from_id = expected;
        probe.type = MSG_DIED;
        for (u32 byte = 0; byte < sizeof(probe.data); byte++) probe.data[byte] = 0;
        deliver(receiver, message_address, &probe);
        return 0;
    }
    for (int slot = 1; slot < task_pool_count; slot++) {
        struct task *sender = &task_pool[slot];
        if (sender->state != TASK_BLOCKED_SEND ||
            sender->send_to != (u32)receiver->id || !pending_valid[slot] ||
            (expected && (u32)sender->id != expected))
            continue;
        struct message message = pending[slot];
        message.from_id = (u32)sender->id;
        int result = deliver(receiver, message_address, &message);
        pending_valid[slot] = 0;
        pending_timed[slot] = 0;
        sender->send_to = 0;
        sender->send_deadline = 0;
        sender->state = TASK_RUNNING;
        task64_set_result((u32)slot, result);
        return result;
    }
    receiver->recv_buf = (struct message *)(uptr_t)message_address;
    receiver->recv_expect = expected;
    receiver->state = TASK_BLOCKED_RECV;
    if (scheduler_pick_next(current) < 0) {
        receiver->recv_buf = 0;
        receiver->recv_expect = 0;
        receiver->state = TASK_RUNNING;
        return EDEADLK;
    }
    return (int)task64_block_switch();
}

void ipc64_task_died(u32 pid) {
    u32 dead_slot = PID_SLOT(pid);
    if (dead_slot < MAX_TASKS) {
        pending_valid[dead_slot] = 0;
        pending_timed[dead_slot] = 0;
    }
    for (int slot = 1; slot < task_pool_count; slot++) {
        struct task *task = &task_pool[slot];
        if (task->state == TASK_BLOCKED_SEND && task->send_to == pid) {
            pending_valid[slot] = 0;
            pending_timed[slot] = 0;
            task->send_to = 0;
            task->send_deadline = 0;
            task->state = TASK_RUNNING;
            task64_set_result((u32)slot, ESRCH);
        }
        if (task->state == TASK_BLOCKED_RECV && task->recv_buf &&
            task->recv_expect == pid) {
            struct message message;
            message.from_id = pid;
            message.type = MSG_DIED;
            for (u32 byte = 0; byte < sizeof(message.data); byte++)
                message.data[byte] = 0;
            int result = deliver(task, (vaddr_t)(uptr_t)task->recv_buf, &message);
            task->recv_buf = 0;
            task->recv_expect = 0;
            task->state = TASK_RUNNING;
            task64_set_result((u32)slot, result);
        }
    }
}

void ipc64_tick(u32 now) {
    for (int slot = 1; slot < task_pool_count; slot++) {
        struct task *sender = &task_pool[slot];
        if (sender->state != TASK_BLOCKED_SEND || !pending_timed[slot] ||
            (i32)(now - sender->send_deadline) < 0)
            continue;
        pending_valid[slot] = 0;
        pending_timed[slot] = 0;
        sender->send_to = 0;
        sender->send_deadline = 0;
        sender->state = TASK_RUNNING;
        task64_set_result((u32)slot, ETIMEDOUT);
    }
}
