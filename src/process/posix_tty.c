#include "posix_tty.h"
#include "task.h"
#include "vm64.h"

// A whole line fits in the accumulator; the queue behind it holds what a
// program has not read yet. Both are static and bounded like every other
// buffer in the profile, and a full one drops the byte rather than growing,
// which is what the UART overrun path does anyway.
#define POSIX_TTY_LINE_MAX 256u
#define POSIX_TTY_QUEUE_MAX 1024u

struct posix_tty_state {
    u32 active;
    struct posix_termios termios;
    struct posix_winsize winsize;
    posix_tty_output_fn output;
    void *output_context;
    u8 line[POSIX_TTY_LINE_MAX];
    u32 line_length;
    u8 queue[POSIX_TTY_QUEUE_MAX];
    u32 queue_head;
    u32 queue_count;
    u32 eof_pending;
};

static struct posix_tty_state ttys[POSIX_TTY_MAX];

static struct posix_tty_state *tty_for(u32 index) {
    if (index >= POSIX_TTY_MAX) return 0;
    return ttys[index].active ? &ttys[index] : 0;
}

void posix_tty_init(void) {
    for (u32 index = 0; index < POSIX_TTY_MAX; index++) {
        for (u32 byte = 0; byte < sizeof(ttys[index]); byte++)
            ((u8 *)&ttys[index])[byte] = 0;
    }
}

int posix_tty_create(void) {
    for (u32 index = 0; index < POSIX_TTY_MAX; index++) {
        if (ttys[index].active) continue;
        struct posix_tty_state *tty = &ttys[index];
        for (u32 byte = 0; byte < sizeof(*tty); byte++)
            ((u8 *)tty)[byte] = 0;
        // The defaults an interactive program expects: CR maps to NL, NL
        // maps back to CR-NL on the way out, lines are assembled, echoed
        // and interruptible.
        tty->termios.iflag = POSIX_ICRNL;
        tty->termios.oflag = POSIX_OPOST | POSIX_ONLCR;
        tty->termios.lflag = POSIX_ISIG | POSIX_ICANON | POSIX_ECHO;
        tty->termios.cc[POSIX_VINTR] = 0x03;
        tty->termios.cc[POSIX_VQUIT] = 0x1C;
        tty->termios.cc[POSIX_VERASE] = 0x7F;
        tty->termios.cc[POSIX_VKILL] = 0x15;
        tty->termios.cc[POSIX_VEOF] = 0x04;
        tty->termios.cc[POSIX_VSUSP] = 0x1A;
        // The serial port reports no geometry, so the console carries the
        // classic 80x25 until TIOCSWINSZ says otherwise.
        tty->winsize.rows = 25;
        tty->winsize.cols = 80;
        tty->active = 1;
        return (int)index;
    }
    return POSIX_TTY_ENFILE;
}

void posix_tty_destroy(u32 index) {
    if (index >= POSIX_TTY_MAX) return;
    for (u32 byte = 0; byte < sizeof(ttys[index]); byte++)
        ((u8 *)&ttys[index])[byte] = 0;
}

void posix_tty_set_output(u32 index, posix_tty_output_fn output,
                          void *context) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return;
    tty->output = output;
    tty->output_context = context;
}

posix_tty_output_fn posix_tty_output_get(u32 index, void **context) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return 0;
    if (context) *context = tty->output_context;
    return tty->output;
}

static void tty_put(struct posix_tty_state *tty, u8 byte) {
    if (tty->output) tty->output(tty->output_context, byte);
}

// Output mapping: with ONLCR a newline leaves as CR-NL, and the echo of
// what was typed takes the same path a write does. The echo of an erase is
// the backspace-space-backspace trio, because the terminal does the
// erasing, not the tty.
static void tty_emit(struct posix_tty_state *tty, u8 byte) {
    if (byte == '\n' && (tty->termios.oflag & POSIX_OPOST) &&
        (tty->termios.oflag & POSIX_ONLCR)) {
        tty_put(tty, '\r');
    }
    tty_put(tty, byte);
}

static void tty_echo(struct posix_tty_state *tty, u8 byte) {
    if (tty->termios.lflag & POSIX_ECHO) tty_emit(tty, byte);
}

static u32 queue_push(struct posix_tty_state *tty, const u8 *data, u32 count) {
    u32 accepted = 0;
    for (; accepted < count && tty->queue_count < POSIX_TTY_QUEUE_MAX;
         accepted++) {
        u32 slot = (tty->queue_head + tty->queue_count) % POSIX_TTY_QUEUE_MAX;
        tty->queue[slot] = data[accepted];
        tty->queue_count++;
    }
    return accepted;
}

u32 posix_tty_input_byte(u32 index, u8 byte) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_EVENT_NONE;
    struct posix_termios *termios = &tty->termios;
    if (termios->lflag & POSIX_ISIG) {
        // A zero control character is the "disabled" spelling, so it never
        // matches the byte being tested.
        if (termios->cc[POSIX_VINTR] && byte == termios->cc[POSIX_VINTR])
            return POSIX_TTY_EVENT_INTR;
        if (termios->cc[POSIX_VQUIT] && byte == termios->cc[POSIX_VQUIT])
            return POSIX_TTY_EVENT_QUIT;
        if (termios->cc[POSIX_VSUSP] && byte == termios->cc[POSIX_VSUSP])
            return POSIX_TTY_EVENT_SUSP;
    }
    if ((termios->iflag & POSIX_ICRNL) && byte == '\r') byte = '\n';
    if (!(termios->lflag & POSIX_ICANON)) {
        queue_push(tty, &byte, 1);
        tty_echo(tty, byte);
        return POSIX_TTY_EVENT_NONE;
    }
    if (byte == '\n') {
        queue_push(tty, tty->line, tty->line_length);
        tty->line_length = 0;
        queue_push(tty, &byte, 1);
        tty_echo(tty, byte);
        return POSIX_TTY_EVENT_NONE;
    }
    if (termios->cc[POSIX_VERASE] && byte == termios->cc[POSIX_VERASE]) {
        if (tty->line_length) {
            tty->line_length--;
            if (termios->lflag & POSIX_ECHO) {
                tty_emit(tty, '\b');
                tty_emit(tty, ' ');
                tty_emit(tty, '\b');
            }
        }
        return POSIX_TTY_EVENT_NONE;
    }
    if (termios->cc[POSIX_VKILL] && byte == termios->cc[POSIX_VKILL]) {
        tty->line_length = 0;
        return POSIX_TTY_EVENT_NONE;
    }
    if (termios->cc[POSIX_VEOF] && byte == termios->cc[POSIX_VEOF]) {
        // EOF on a partial line hands that line over; on an empty one it
        // marks the end the next read reports as zero bytes.
        if (tty->line_length) {
            queue_push(tty, tty->line, tty->line_length);
            tty->line_length = 0;
        } else {
            tty->eof_pending = 1;
        }
        return POSIX_TTY_EVENT_NONE;
    }
    if (tty->line_length < POSIX_TTY_LINE_MAX) {
        tty->line[tty->line_length++] = byte;
        tty_echo(tty, byte);
    }
    return POSIX_TTY_EVENT_NONE;
}

int posix_tty_read(u32 index, u8 *buffer, u32 length, u32 *transferred) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!buffer || !transferred) return POSIX_TTY_EINVAL;
    if (!tty->queue_count) {
        if (tty->eof_pending) {
            tty->eof_pending = 0;
            *transferred = 0;
            return 0;
        }
        *transferred = 0;
        return POSIX_TTY_EAGAIN;
    }
    u32 count = length < tty->queue_count ? length : tty->queue_count;
    for (u32 offset = 0; offset < count; offset++) {
        buffer[offset] = tty->queue[tty->queue_head];
        tty->queue_head = (tty->queue_head + 1u) % POSIX_TTY_QUEUE_MAX;
        tty->queue_count--;
    }
    *transferred = count;
    return 0;
}

int posix_tty_write(u32 index, const u8 *data, u32 length, u32 *transferred) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!data || !transferred) return POSIX_TTY_EINVAL;
    for (u32 offset = 0; offset < length; offset++)
        tty_emit(tty, data[offset]);
    *transferred = length;
    return 0;
}

int posix_tty_termios_get(u32 index, struct posix_termios *out) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!out) return POSIX_TTY_EINVAL;
    *out = tty->termios;
    return 0;
}

int posix_tty_termios_set(u32 index, const struct posix_termios *in) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!in) return POSIX_TTY_EINVAL;
    // N_TTY is the only line discipline here, so a selector naming another
    // one is refused rather than quietly running the same code.
    if (in->line != POSIX_N_TTY || (in->iflag & ~(u32)POSIX_TTY_IFLAG_KNOWN) ||
        (in->oflag & ~(u32)POSIX_TTY_OFLAG_KNOWN) ||
        (in->lflag & ~(u32)POSIX_TTY_LFLAG_KNOWN))
        return POSIX_TTY_EINVAL;
    tty->termios = *in;
    return 0;
}

int posix_tty_winsize_get(u32 index, struct posix_winsize *out) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!out) return POSIX_TTY_EINVAL;
    *out = tty->winsize;
    return 0;
}

int posix_tty_winsize_set(u32 index, const struct posix_winsize *in) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty) return POSIX_TTY_ENOTTY;
    if (!in) return POSIX_TTY_EINVAL;
    tty->winsize = *in;
    return 0;
}

i64 posix_tty_ioctl(struct task *task, u32 index, u64 request,
                    uptr_t argument) {
    struct posix_tty_state *tty = tty_for(index);
    if (!tty || !task) return POSIX_TTY_ENOTTY;
    switch (request) {
    case POSIX_TCGETS: {
        struct posix_termios copy;
        if (posix_tty_termios_get(index, &copy)) return POSIX_TTY_ENOTTY;
        if (vm64_copy_to(task->page_dir, argument, &copy, sizeof(copy)))
            return POSIX_TTY_EIO;
        return 0;
    }
    case POSIX_TCSETS: {
        struct posix_termios copy;
        if (vm64_copy_from(task->page_dir, &copy, argument, sizeof(copy)))
            return POSIX_TTY_EIO;
        return posix_tty_termios_set(index, &copy);
    }
    case POSIX_TIOCGWINSZ: {
        struct posix_winsize copy;
        if (posix_tty_winsize_get(index, &copy)) return POSIX_TTY_ENOTTY;
        if (vm64_copy_to(task->page_dir, argument, &copy, sizeof(copy)))
            return POSIX_TTY_EIO;
        return 0;
    }
    case POSIX_TIOCSWINSZ: {
        struct posix_winsize copy;
        if (vm64_copy_from(task->page_dir, &copy, argument, sizeof(copy)))
            return POSIX_TTY_EIO;
        return posix_tty_winsize_set(index, &copy);
    }
    default:
        return POSIX_TTY_ENOTTY;
    }
}

u32 posix_tty_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < POSIX_TTY_MAX; index++)
        if (ttys[index].active) count++;
    return count;
}
