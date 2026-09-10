#include <mich/syscall.h>
#include <mich/service.h>
#include <mich/exception.h>
#include <mich/ipc.h>
#include <mich/capability.h>

static int failures;
static volatile unsigned int cow_probe;

static void pass(const char *name) {
    mich_write("[PASS] ");
    mich_write(name);
    mich_write("\n");
}

static void fail(const char *name) {
    failures++;
    mich_write("[FAIL] ");
    mich_write(name);
    mich_write("\n");
}

static void check(int condition, const char *name) {
    if (condition) pass(name);
    else fail(name);
}

static int bytes_equal(const unsigned char *a, const unsigned char *b, int length) {
    for (int i = 0; i < length; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    mich_write("Mich init: userspace test suite starting\n");

    int me = mich_getpid();
    check(me > 0, "getpid");
    check(mich_service_register(MICH_SERVICE_INIT) == 0, "service register");
    check(mich_service_lookup(MICH_SERVICE_INIT) == me, "service lookup");
    unsigned int init_caps = MICH_CAP_SERVICE_REGISTER | MICH_CAP_TASK_ADMIN |
                             MICH_CAP_TASK_ENUM | MICH_CAP_RESOURCE_ADMIN;
    check((mich_cap_get() & init_caps) == init_caps, "boot capabilities received");
    check(mich_memfree() > 0, "memfree");
    check(mich_write((const char *)1) == -1, "bad user pointer rejected");

    cow_probe = 0x11111111u;
    int cow_child = mich_fork();
    if (cow_child == 0) {
        cow_probe = 0x22222222u;
        mich_exit(cow_probe == 0x22222222u ? 0 : 1);
    }
    if (cow_child < 0) {
        fail("copy-on-write fork setup");
    } else {
        int child_status = mich_wait(cow_child);
        check(child_status == 0 && cow_probe == 0x11111111u,
              "copy-on-write address isolation");
        cow_probe = 0x33333333u;
        check(cow_probe == 0x33333333u, "copy-on-write single owner fast path");
    }

    int child = mich_fork();
    if (child == 0) {
        mich_write("Mich init child: ring 3 fork path\n");
        mich_exit(42);
    }

    if (child < 0) {
        fail("fork");
    } else {
        pass("fork");
        check(mich_wait(child) == 42, "wait and exit status");
    }

    int sender = mich_fork();
    if (sender == 0) {
        struct mich_message outgoing;
        outgoing.from_id = 0;
        outgoing.type = 0xCAFE;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) outgoing.data[i] = 0;
        outgoing.data[0] = 'M';
        outgoing.data[1] = 'I';
        outgoing.data[2] = 'C';
        outgoing.data[3] = 'H';
        if (mich_send((unsigned int)me, &outgoing) != 0) mich_exit(2);
        mich_exit(33);
    }
    if (sender < 0) {
        fail("IPC setup");
    } else {
        static const unsigned char expected[4] = { 'M', 'I', 'C', 'H' };
        struct mich_message incoming;
        int received = mich_recv(&incoming);
        check(received == 0 && incoming.from_id == (unsigned int)sender &&
              incoming.type == 0xCAFE &&
              bytes_equal(incoming.data, expected, 4),
              "blocking IPC delivery");
        check(mich_wait(sender) == 33, "IPC sender resumed");
    }

    int cycle_sender = mich_fork();
    if (cycle_sender == 0) {
        struct mich_message message;
        message.from_id = 0;
        message.type = 0xD1;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        mich_exit(mich_send((unsigned int)me, &message) == 0 ? 0 : 1);
    }
    if (cycle_sender < 0) {
        fail("IPC deadlock setup");
    } else {
        struct mich_message outgoing;
        outgoing.from_id = 0;
        outgoing.type = 0xD2;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) outgoing.data[i] = 0;
        mich_yield();
        int deadlock = mich_send((unsigned int)cycle_sender, &outgoing);
        struct mich_message incoming;
        int received = mich_recv_from(0, &incoming);
        check(deadlock == -35 && received == 0 &&
              incoming.from_id == (unsigned int)cycle_sender &&
              mich_wait(cycle_sender) == 0,
              "IPC deadlock cycle rejected");
    }

    int timeout_receiver = mich_fork();
    if (timeout_receiver == 0) {
        for (;;) __asm__ volatile("pause");
    }
    if (timeout_receiver < 0) {
        fail("IPC timeout setup");
    } else {
        struct mich_message message;
        message.from_id = 0;
        message.type = 0xD3;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        int result = mich_send_timeout((unsigned int)timeout_receiver, &message, 3);
        int killed = mich_kill(timeout_receiver);
        int status = mich_wait(timeout_receiver);
        check(result == -110 && killed == 0 && status == 137,
              "IPC send timeout");
    }

    int nb_receiver = mich_fork();
    if (nb_receiver == 0) {
        struct mich_message message;
        if (mich_recv(&message) != 0 || message.type != 0xBEEF)
            mich_exit(1);
        mich_exit(0);
    }
    if (nb_receiver < 0) {
        fail("nonblocking IPC setup");
    } else {
        struct mich_message message;
        message.from_id = 0;
        message.type = 0xBEEF;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        mich_yield();
        check(mich_send_nb((unsigned int)nb_receiver, &message) == 0 &&
              mich_wait(nb_receiver) == 0,
              "nonblocking IPC ready receiver");
    }

    int doomed_receiver = mich_fork();
    if (doomed_receiver == 0) {
        for (;;) __asm__ volatile("pause");
    }
    int blocked_sender = mich_fork();
    if (blocked_sender == 0) {
        struct mich_message message;
        message.from_id = 0;
        message.type = 0xDEAD;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        mich_exit(mich_send((unsigned int)doomed_receiver, &message) == -3 ? 0 : 1);
    }
    if (doomed_receiver < 0 || blocked_sender < 0) {
        fail("dead IPC receiver setup");
    } else {
        mich_yield();
        int killed = mich_kill(doomed_receiver);
        int receiver_status = mich_wait(doomed_receiver);
        int sender_status = mich_wait(blocked_sender);
        check(killed == 0 && receiver_status == 137 && sender_status == 0,
              "blocked sender wakes when receiver dies");
    }

    int doomed_sender = mich_fork();
    if (doomed_sender == 0) {
        for (;;) __asm__ volatile("pause");
    }
    int death_watcher = mich_fork();
    if (death_watcher == 0) {
        struct mich_message message;
        int rc = mich_recv_from((unsigned int)doomed_sender, &message);
        mich_exit(rc == 0 && message.type == MICH_MSG_DIED &&
                  message.from_id == (unsigned int)doomed_sender ? 0 : 1);
    }
    if (doomed_sender < 0 || death_watcher < 0) {
        fail("IPC death notification setup");
    } else {
        mich_yield();
        int killed = mich_kill(doomed_sender);
        int sender_status = mich_wait(doomed_sender);
        int watcher_status = mich_wait(death_watcher);
        check(killed == 0 && sender_status == 137 && watcher_status == 0,
              "receiver notified when sender dies");
    }

    int faulty = mich_fork();
    if (faulty == 0) {
        if (mich_service_register(MICH_SERVICE_TEST) == 0) mich_exit(2);
        if (mich_memfree() >= 0) mich_exit(3);
        volatile unsigned int value = *(volatile unsigned int *)0x70000000u;
        (void)value;
        mich_exit(1);
    }
    if (faulty < 0) {
        fail("user page fault setup");
    } else {
        int expected = MICH_EXIT_EXCEPTION_BASE + MICH_EXCEPTION_PAGE_FAULT;
        check(mich_wait(faulty) == expected, "user page fault contained");
        check(mich_service_lookup(MICH_SERVICE_TEST) < 0,
              "unprivileged service register denied");
    }

    int delegate = mich_fork();
    if (delegate == 0) {
        while (!(mich_cap_get() & MICH_CAP_SERVICE_REGISTER))
            __asm__ volatile("pause");
        if (mich_service_register(MICH_SERVICE_TEST) != 0) mich_exit(2);
        if (mich_cap_drop(MICH_CAP_SERVICE_REGISTER) != 0) mich_exit(3);
        if (mich_cap_get() & MICH_CAP_SERVICE_REGISTER) mich_exit(4);
        if (mich_service_register(MICH_SERVICE_TEST) == 0) mich_exit(5);
        mich_exit(0);
    }
    if (delegate < 0) {
        fail("capability delegation setup");
    } else if (mich_cap_grant(delegate, MICH_CAP_SERVICE_REGISTER) != 0) {
        mich_kill(delegate);
        mich_wait(delegate);
        fail("capability grant");
    } else {
        check(mich_wait(delegate) == 0, "capability grant and drop");
        check(mich_service_lookup(MICH_SERVICE_TEST) < 0,
              "capability-owned service released");
    }

    int no_irq_right = mich_fork();
    if (no_irq_right == 0)
        mich_exit(mich_irq_register(4) < 0 ? 0 : 1);
    check(no_irq_right > 0 && mich_wait(no_irq_right) == 0,
          "ungranted IRQ denied");

    int irq_user = mich_fork();
    if (irq_user == 0) {
        while (mich_irq_register(3) != 0) __asm__ volatile("pause");
        mich_exit(0);
    }
    if (irq_user < 0 || mich_irq_grant(irq_user, 3) != 0) {
        if (irq_user > 0) { mich_kill(irq_user); mich_wait(irq_user); }
        fail("object IRQ grant");
    } else {
        check(mich_wait(irq_user) == 0, "object IRQ grant");
    }

    int io_user = mich_fork();
    if (io_user == 0) {
        struct mich_message start;
        if (mich_recv_from((unsigned int)me, &start) != 0) mich_exit(2);
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
        mich_exit(0);
    }
    if (io_user < 0 || mich_ioport_grant(io_user, 0x80, 1) != 0) {
        if (io_user > 0) { mich_kill(io_user); mich_wait(io_user); }
        fail("object I/O port grant");
    } else {
        struct mich_message start;
        start.from_id = 0;
        start.type = 1;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) start.data[i] = 0;
        int sent = mich_send((unsigned int)io_user, &start);
        check(sent == 0 && mich_wait(io_user) == 0, "object I/O port grant");
    }

    int mmio_user = mich_fork();
    if (mmio_user == 0) {
        struct mich_message start;
        if (mich_recv_from((unsigned int)me, &start) != 0) mich_exit(2);
        unsigned int handle = start.data[0];
        if (mich_mmio_map(handle, 0x02000000u) != 0) mich_exit(3);
        if (mich_mmio_map(handle, 0x02001000u) == 0) mich_exit(4);
        mich_exit(0);
    }
    if (mmio_user < 0) {
        fail("object MMIO setup");
    } else {
        check(mich_mmio_grant(mmio_user, 0x00100000u, 0x1000u) < 0,
              "RAM cannot be granted as MMIO");
        int handle = mich_mmio_grant(mmio_user, 0xF0000000u, 0x1000u);
        struct mich_message start;
        start.from_id = 0;
        start.type = 2;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) start.data[i] = 0;
        start.data[0] = (unsigned char)handle;
        int sent = handle > 0 ? mich_send((unsigned int)mmio_user, &start) : -1;
        check(sent == 0 && mich_wait(mmio_user) == 0, "object MMIO grant and map");
    }

    int dma_free_before = mich_memfree();
    int dma_user = mich_fork();
    if (dma_user == 0) {
        struct mich_message start;
        if (mich_recv_from((unsigned int)me, &start) != 0) mich_exit(2);
        int phys = mich_dma_alloc(2, 0x03000000u);
        if (phys <= 0 || (unsigned int)phys > 0x00FFFFFFu) mich_exit(3);
        volatile unsigned int *dma = (volatile unsigned int *)0x03000000u;
        dma[0] = 0x4D494348u;
        dma[1024] = 0x444D4121u;
        if (dma[0] != 0x4D494348u || dma[1024] != 0x444D4121u) mich_exit(4);
        if (mich_dma_alloc(2, 0x03002000u) >= 0) mich_exit(5);
        mich_exit(0);
    }
    if (dma_user < 0 || mich_dma_grant(dma_user, 3, 0x00FFFFFFu) != 0) {
        if (dma_user > 0) { mich_kill(dma_user); mich_wait(dma_user); }
        fail("DMA buffer grant");
    } else {
        struct mich_message start;
        start.from_id = 0;
        start.type = 3;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) start.data[i] = 0;
        int sent = mich_send((unsigned int)dma_user, &start);
        check(sent == 0 && mich_wait(dma_user) == 0,
              "bounded contiguous DMA buffer");
        check(mich_memfree() == dma_free_before, "DMA pages reclaimed on exit");
    }

    int illegal = mich_fork();
    if (illegal == 0) {
        __asm__ volatile("ud2");
        mich_exit(1);
    }
    if (illegal < 0) {
        fail("invalid opcode setup");
    } else {
        int expected = MICH_EXIT_EXCEPTION_BASE + MICH_EXCEPTION_INVALID_OPCODE;
        check(mich_wait(illegal) == expected, "user invalid opcode contained");
    }

    int attacker = mich_fork();
    if (attacker == 0) {
        int rc = mich_kill(me);
        mich_exit(rc < 0 ? 0 : 1);
    }
    check(attacker > 0 && mich_wait(attacker) == 0,
          "child cannot kill ancestor");

    int manager = mich_fork();
    if (manager == 0) {
        int worker = mich_fork();
        if (worker == 0) {
            for (;;) __asm__ volatile("pause");
        }
        if (worker < 0) mich_exit(2);
        if (mich_kill(worker) != 0) mich_exit(3);
        if (mich_wait(worker) != 137) mich_exit(4);
        mich_exit(0);
    }
    check(manager > 0 && mich_wait(manager) == 0,
          "process can manage descendants");

    int orphan_parent = mich_fork();
    if (orphan_parent == 0) {
        int orphan = mich_fork();
        if (orphan == 0) mich_exit(77);
        if (orphan < 0) mich_exit(2);
        mich_exit(0);
    }
    if (orphan_parent < 0) {
        fail("orphan reparent setup");
    } else {
        check(mich_wait(orphan_parent) == 0, "orphan parent exit");
        check(mich_wait(-1) == 77, "orphan adopted by init");
    }

    if (failures == 0)
        mich_write("Mich init: ALL TESTS PASSED\n");
    else
        mich_write("Mich init: TESTS FAILED\n");

    return failures ? 1 : 0;
}
