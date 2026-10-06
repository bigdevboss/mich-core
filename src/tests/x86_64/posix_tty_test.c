#include "types.h"
#include "object.h"
#include "posix_abi.h"
#include "posix_tty.h"
#include "posix_pledge.h"
#include "posix_profile.h"
#include "task.h"
#include "vfs.h"
#include "tests64.h"

// The line discipline, the termios boundary and the two new devfs nodes.
// A real serial port is not part of this scope: the console's output hook
// is swapped for a sink in the test, so echo and the output mapping are
// read back from bytes instead of inferred from the console log.

struct tty_sink {
    u8 bytes[512];
    u32 count;
};

static void sink_output(void *context, u8 byte) {
    struct tty_sink *sink = context;
    if (sink->count < sizeof(sink->bytes)) sink->bytes[sink->count++] = byte;
}

static int sink_holds(const struct tty_sink *sink, u32 offset,
                      const char *expected, u32 length) {
    if (sink->count < offset + length) return 0;
    for (u32 index = 0; index < length; index++)
        if (sink->bytes[offset + index] != (u8)expected[index]) return 0;
    return 1;
}

static int reads(const u8 *expected, u32 length) {
    u8 buffer[64];
    u32 received = 0;
    if (length > sizeof(buffer)) return 0;
    if (posix_tty_read(0, buffer, sizeof(buffer), &received)) return 0;
    if (received != length) return 0;
    for (u32 index = 0; index < length; index++)
        if (buffer[index] != expected[index]) return 0;
    return 1;
}

int test_posix_tty64(struct task *owner) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    // The boot console is the first line; everything the test creates
    // lands behind it.
    int valid = posix_tty_active_count() == 1;

    int second = posix_tty_create();
    int third = posix_tty_create();
    int fourth = posix_tty_create();
    valid = valid && second == 1 && third == 2 && fourth == 3 &&
        posix_tty_create() == POSIX_TTY_ENFILE &&
        posix_tty_active_count() == POSIX_TTY_MAX;
    posix_tty_destroy(1);
    posix_tty_destroy(2);
    posix_tty_destroy(3);
    valid = valid && posix_tty_active_count() == 1;
    // A destroyed line answers every call with ENOTTY rather than reading
    // whatever the next tenant leaves behind.
    valid = valid && posix_tty_termios_get(1, 0) == POSIX_TTY_ENOTTY &&
        posix_tty_winsize_get(1, 0) == POSIX_TTY_ENOTTY;

    struct tty_sink sink;
    sink.count = 0;
    posix_tty_output_fn saved = posix_tty_output_get(0, 0);
    posix_tty_set_output(0, sink_output, &sink);
    valid = valid && saved != 0 && posix_tty_output_get(0, 0) == sink_output;

    struct posix_termios termios;
    valid = valid && posix_tty_termios_get(0, &termios) == 0 &&
        termios.iflag == POSIX_ICRNL &&
        termios.oflag == (POSIX_OPOST | POSIX_ONLCR) &&
        termios.lflag == (POSIX_ISIG | POSIX_ICANON | POSIX_ECHO) &&
        termios.line == POSIX_N_TTY && termios.cc[POSIX_VINTR] == 0x03 &&
        termios.cc[POSIX_VERASE] == 0x7F;

    // Canonical input: bytes accumulate until the newline, then one read
    // hands back the whole line. The echo goes through the output map, so
    // the newline echoes as the CR-NL pair.
    valid = valid && posix_tty_input_byte(0, 'a') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 'b') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 'c') == POSIX_TTY_EVENT_NONE;
    u8 readback[8];
    u32 got = 7;
    posix_tty_read(0, readback, sizeof(readback), &got);
    valid = valid && got == 0 && sink_holds(&sink, 0, "abc", 3);
    valid = valid && posix_tty_input_byte(0, '\n') == POSIX_TTY_EVENT_NONE &&
        reads((const u8 *)"abc\n", 4) && sink_holds(&sink, 3, "\r\n", 2);

    // No complete line parks a blocking read (that lands with the group
    // work); the non-blocking answer is EAGAIN and nothing is consumed.
    valid = valid && posix_tty_read(0, readback, sizeof(readback), &got) ==
        POSIX_TTY_EAGAIN && got == 0;

    // Erase drops the last byte and paints the erasing on the terminal;
    // kill drops the whole line.
    const u8 typed_erase[] = { 'a', 'b', 0x7F, 'c', '\n' };
    for (u32 index = 0; index < sizeof(typed_erase); index++)
        posix_tty_input_byte(0, typed_erase[index]);
    valid = valid && reads((const u8 *)"ac\n", 3);
    const u8 typed_kill[] = { 'x', 'y', 0x15, 'z', '\n' };
    for (u32 index = 0; index < sizeof(typed_kill); index++)
        posix_tty_input_byte(0, typed_kill[index]);
    valid = valid && reads((const u8 *)"z\n", 2);

    // ICRNL: a carriage return ends the line like a newline does.
    valid = valid && posix_tty_input_byte(0, 'o') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 'k') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, '\r') == POSIX_TTY_EVENT_NONE &&
        reads((const u8 *)"ok\n", 3);

    // EOF on a partial line hands that line over; on an empty one it ends
    // the next read with zero bytes, once.
    valid = valid && posix_tty_input_byte(0, 'h') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 'i') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 0x04) == POSIX_TTY_EVENT_NONE &&
        reads((const u8 *)"hi", 2);
    valid = valid && posix_tty_input_byte(0, 0x04) == POSIX_TTY_EVENT_NONE &&
        posix_tty_read(0, readback, sizeof(readback), &got) == 0 && got == 0 &&
        posix_tty_read(0, readback, sizeof(readback), &got) == POSIX_TTY_EAGAIN;

    // A line longer than the accumulator keeps what fits and drops the
    // rest, the way the UART overrun path does, and the newline still ends
    // the line.
    for (u32 index = 0; index < 300; index++)
        posix_tty_input_byte(0, 'q');
    posix_tty_input_byte(0, '\n');
    u32 long_got = 0;
    u8 long_buffer[256 + 2];
    valid = valid && posix_tty_read(0, long_buffer, sizeof(long_buffer),
                                    &long_got) == 0 && long_got == 257 &&
        long_buffer[256] == '\n';

    // Under ISIG the three control characters never reach the buffer, and
    // the event is what the caller owes the foreground group.
    valid = valid && posix_tty_input_byte(0, 0x03) == POSIX_TTY_EVENT_INTR &&
        posix_tty_input_byte(0, 0x1C) == POSIX_TTY_EVENT_QUIT &&
        posix_tty_input_byte(0, 0x1A) == POSIX_TTY_EVENT_SUSP &&
        posix_tty_read(0, readback, sizeof(readback), &got) == POSIX_TTY_EAGAIN;
    struct posix_termios plain = termios;
    plain.lflag = POSIX_ICANON | POSIX_ECHO;
    // With ISIG cleared the same bytes are ordinary input, so they reach
    // the line; the EOF that closes it hands the byte over.
    valid = valid && posix_tty_termios_set(0, &plain) == 0 &&
        posix_tty_input_byte(0, 0x03) == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, 0x04) == POSIX_TTY_EVENT_NONE &&
        reads((const u8 *)"\x03", 1) && posix_tty_termios_set(0, &termios) == 0;

    // Non-canonical input is readable one byte at a time.
    struct posix_termios raw = termios;
    raw.lflag = POSIX_ECHO;
    valid = valid && posix_tty_termios_set(0, &raw) == 0 &&
        posix_tty_input_byte(0, 'r') == POSIX_TTY_EVENT_NONE &&
        reads((const u8 *)"r", 1) && posix_tty_termios_set(0, &termios) == 0;

    // A termios carrying a line discipline this build does not implement,
    // or a flag outside the implemented set, is refused.
    struct posix_termios rejected = termios;
    rejected.line = 1;
    valid = valid && posix_tty_termios_set(0, &rejected) == POSIX_TTY_EINVAL;
    rejected = termios;
    rejected.lflag |= 0x4000;
    valid = valid && posix_tty_termios_set(0, &rejected) == POSIX_TTY_EINVAL;
    rejected = termios;
    rejected.iflag |= 0x2;
    valid = valid && posix_tty_termios_set(0, &rejected) == POSIX_TTY_EINVAL;

    // Writes take the same output map the echo does.
    sink.count = 0;
    u32 written = 0;
    valid = valid &&
        posix_tty_write(0, (const u8 *)"one\ntwo", 7, &written) == 0 &&
        written == 7 && sink_holds(&sink, 0, "one\r\ntwo", 8);

    // Winsize round trip, and the ioctl boundary answers ENOTTY for a
    // request the tty does not carry.
    struct posix_winsize size;
    valid = valid && posix_tty_winsize_get(0, &size) == 0 &&
        size.rows == 25 && size.cols == 80;
    size.rows = 50;
    size.cols = 132;
    valid = valid && posix_tty_winsize_set(0, &size) == 0 &&
        posix_tty_winsize_get(0, &size) == 0 &&
        size.rows == 50 && size.cols == 132;
    size.rows = 25;
    size.cols = 80;
    // The ioctl boundary: a request the line does not carry and a slot
    // that holds no line both answer ENOTTY. The copy directions need a
    // user address, which only the demo has, so the unit scope is the
    // rejection side.
    valid = valid && posix_tty_winsize_set(0, &size) == 0 &&
        posix_tty_ioctl(owner, 0, 0x9999u, 0) == POSIX_TTY_ENOTTY &&
        posix_tty_ioctl(owner, POSIX_TTY_MAX, POSIX_TCGETS, 0) ==
            POSIX_TTY_ENOTTY;

    // The promise gate row for ioctl, the way the poll test carries its
    // own: a narrowed set denies the request loudly or quietly and an
    // empty set leaves it ungated.
    struct task *gate_task = task_alloc_slot();
    if (gate_task && !posix_profile_admit(gate_task)) {
        valid = valid &&
            posix_pledge_promise(gate_task, "stdio error", 0) == 0 &&
            posix_pledge_gate(gate_task, POSIX_SYSCALL_IOCTL) == 0 &&
            posix_pledge_promise(gate_task, "error", 0) == 0 &&
            posix_pledge_gate(gate_task, POSIX_SYSCALL_IOCTL) ==
                POSIX_PLEDGE_ENOSYS &&
            posix_pledge_promise(gate_task, "", 0) == 0 &&
            posix_pledge_gate(gate_task, POSIX_SYSCALL_IOCTL) == 1;
        posix_pledge_reset(gate_task);
        posix_profile_release(gate_task);
        task_free_slot(gate_task);
    } else {
        if (gate_task) task_free_slot(gate_task);
        valid = 0;
    }

    // The devfs pair: null reads as end of file and swallows writes,
    // console routes both through the line discipline above.
    struct kernel_object *root = vfs_root();
    struct kernel_object *null_node = root ? vfs_create_null(root) : 0;
    struct kernel_object *console_node = root ? vfs_create_console(root) : 0;
    struct kernel_object *null_file = null_node ? vfs_open(null_node) : 0;
    struct kernel_object *console_file = console_node ?
        vfs_open(console_node) : 0;
    u8 scratch[16];
    u32 done = 77;
    valid = valid && null_file && console_file &&
        vfs_read(null_file, 0, scratch, sizeof(scratch), &done) == 0 &&
        done == 0 &&
        vfs_write(null_file, 0, "gone", 4, &done) == 0 && done == 4;
    sink.count = 0;
    valid = valid && vfs_write(console_file, 0, "hi\n", 3, &done) == 0 &&
        done == 3 && sink_holds(&sink, 0, "hi\r\n", 4);
    valid = valid && vfs_read(console_file, 0, scratch, sizeof(scratch),
                              &done) == 0 && done == 0 &&
        posix_tty_input_byte(0, 'v') == POSIX_TTY_EVENT_NONE &&
        posix_tty_input_byte(0, '\n') == POSIX_TTY_EVENT_NONE &&
        vfs_read(console_file, 0, scratch, sizeof(scratch), &done) == 0 &&
        done == 2 && scratch[0] == 'v' && scratch[1] == '\n';

    if (console_file) object_release(console_file);
    if (null_file) object_release(null_file);
    if (console_node) object_release(console_node);
    if (null_node) object_release(null_node);
    if (console_node && vfs_unlink(root, "console")) valid = 0;
    if (null_node && vfs_unlink(root, "null")) valid = 0;
    if (root) object_release(root);

    posix_tty_set_output(0, saved, 0);
    valid = valid && posix_tty_output_get(0, 0) == saved;
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        posix_tty_active_count() == 1;
    return valid ? 0 : -1;
}
