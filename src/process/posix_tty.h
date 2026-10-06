#ifndef POSIX_TTY_H
#define POSIX_TTY_H

#include "types.h"
#include "posix_abi.h"

// The tty pool is static and bounded like the pipe and task pools. Index 0
// is the serial console the kernel brings up; the pool exists rather than a
// single instance so a second line can hang off another port without a new
// type, and so the unit test can drive an instance of its own.
#define POSIX_TTY_MAX 4u

// What a control character under ISIG means. The line discipline answers
// the event instead of sending the signal, because who receives it depends
// on the foreground group, which lives above this layer.
#define POSIX_TTY_EVENT_NONE 0u
#define POSIX_TTY_EVENT_INTR (1u << 0)
#define POSIX_TTY_EVENT_QUIT (1u << 1)
#define POSIX_TTY_EVENT_SUSP (1u << 2)

// Fewer than a line's worth of bytes from a read, and no line to hand over
// yet. A blocking descriptor parks instead; a non-blocking one reports the
// way every other empty source in the profile does.
#define POSIX_TTY_EAGAIN (-11)
#define POSIX_TTY_ENOTTY (-25)
#define POSIX_TTY_ENFILE (-23)
#define POSIX_TTY_EINVAL (-22)
#define POSIX_TTY_EIO (-5)

// The flag bits this line discipline implements, so a termios carrying
// anything else is refused instead of silently half-applied.
#define POSIX_TTY_IFLAG_KNOWN POSIX_ICRNL
#define POSIX_TTY_OFLAG_KNOWN (POSIX_OPOST | POSIX_ONLCR)
#define POSIX_TTY_LFLAG_KNOWN (POSIX_ISIG | POSIX_ICANON | POSIX_ECHO)

// The only line discipline the profile carries; a termios asking for
// another one is refused rather than quietly running this one.
#define POSIX_N_TTY 0u

// One byte on its way out of the tty: the echo of what was typed, or the
// bytes a write put there. The console points this at the serial port; the
// unit test points it at its own sink and reads the bytes back.
typedef void (*posix_tty_output_fn)(void *context, u8 byte);

struct task;

void posix_tty_init(void);

// Take a line from the pool with the default termios. Returns the index or
// POSIX_TTY_ENFILE.
int posix_tty_create(void);

// Drop a line: the console keeps its index for the life of the boot, so
// this exists for the pool accounting the tests assert.
void posix_tty_destroy(u32 index);

void posix_tty_set_output(u32 index, posix_tty_output_fn output,
                          void *context);
// Read back what the line is wired to, so a caller that borrows the hook
// (the unit test, a future pty) can put it back.
posix_tty_output_fn posix_tty_output_get(u32 index, void **context);

// One byte arriving from the port. Returns POSIX_TTY_EVENT_NONE when the
// byte was taken (echoed, buffered, erased, or kept out of the way as a
// harmless control character), or an event bit when the caller owes a
// signal to the foreground group.
u32 posix_tty_input_byte(u32 index, u8 byte);

// Hand over buffered input. Canonical mode answers whole lines, so a read
// with no complete line parks (P5b) or reports POSIX_TTY_EAGAIN; the EOF
// character on an empty line makes the next read answer zero bytes, which
// is how a program reading a file paste sees the end.
int posix_tty_read(u32 index, u8 *buffer, u32 length, u32 *transferred);

int posix_tty_write(u32 index, const u8 *data, u32 length, u32 *transferred);

int posix_tty_termios_get(u32 index, struct posix_termios *out);
int posix_tty_termios_set(u32 index, const struct posix_termios *in);
int posix_tty_winsize_get(u32 index, struct posix_winsize *out);
int posix_tty_winsize_set(u32 index, const struct posix_winsize *in);

// The ioctl body for a console descriptor: the request numbers above, the
// argument pointing at the structure the request names. Unknown requests
// and unknown descriptors answer ENOTTY, never a silent zero.
i64 posix_tty_ioctl(struct task *task, u32 index, u64 request,
                    uptr_t argument);

u32 posix_tty_active_count(void);

#endif
