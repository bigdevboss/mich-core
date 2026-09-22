#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mich/syscall.h>

/* The serial console syscall drains one nul-terminated string of at most
   256 bytes per call, so streaming output flushes in bounded chunks. */
#define MICH_STDIO_STREAM 128
#define MICH_STDIO_NUMBER 24
#define MICH_STDIO_WIDTH 512

/* One buffer equals one POSIX_IO_MAX descriptor chunk, so a full flush or
   refill is a single request on the kernel facade. */
#define STDIO_BUFFER 512

struct stdio_file {
    int descriptor;
    int error;
    int eof;
    int reading;
    unsigned char *buffer;
    size_t begin;
    size_t end;
    size_t pending;
};

struct stdio_format {
    char *buffer;        /* zero when formatting streams to the console */
    FILE *file;          /* set when formatting writes into a FILE */
    size_t capacity;
    size_t length;
    char stream[MICH_STDIO_STREAM];
    size_t stream_length;
};

static void stdio_flush(struct stdio_format *state) {
    if (state->stream_length) {
        state->stream[state->stream_length] = 0;
        mich_syscall1(MICH_SYS_WRITE, (unsigned long)state->stream);
        state->stream_length = 0;
    }
}

static void stdio_emit(struct stdio_format *state, char value) {
    if (state->buffer) {
        /* Keep the terminator slot reserved; the return value still
           reports the full untruncated length like POSIX snprintf. */
        if (state->length + 1 < state->capacity)
            state->buffer[state->length] = value;
        state->length++;
        return;
    }
    if (state->file) {
        fputc(value, state->file);
        state->length++;
        return;
    }
    if (state->stream_length == MICH_STDIO_STREAM - 1) stdio_flush(state);
    state->stream[state->stream_length++] = value;
    state->length++;
}

static void stdio_pad(struct stdio_format *state, char fill, int count) {
    for (int index = 0; index < count; index++) stdio_emit(state, fill);
}

static void stdio_emit_number(struct stdio_format *state,
                              unsigned long value, unsigned base,
                              int uppercase, int negative, int zero_pad,
                              int width, int left_align) {
    char digits[MICH_STDIO_NUMBER];
    int count = 0;
    for (;;) {
        unsigned digit = (unsigned)(value % base);
        digits[count++] = (char)(digit < 10 ? '0' + digit
                                            : (uppercase ? 'A' : 'a') + digit - 10);
        value /= base;
        if (!value) break;
    }
    int total = count + (negative ? 1 : 0);
    int padding = width > total ? width - total : 0;
    char fill = (zero_pad && !left_align) ? '0' : ' ';
    /* Space padding goes before the sign, zero padding after it. */
    if (fill == ' ' && !left_align) stdio_pad(state, ' ', padding);
    if (negative) stdio_emit(state, '-');
    if (fill == '0') stdio_pad(state, '0', padding);
    for (int index = count - 1; index >= 0; index--)
        stdio_emit(state, digits[index]);
    if (left_align) stdio_pad(state, ' ', padding);
}

static void stdio_emit_string(struct stdio_format *state, const char *text,
                              int width, int left_align) {
    if (!text) text = "(null)";
    size_t length = strlen(text);
    int padding = width > 0 && (size_t)width > length
        ? (int)((size_t)width - length) : 0;
    if (!left_align) stdio_pad(state, ' ', padding);
    for (size_t index = 0; index < length; index++) stdio_emit(state, text[index]);
    if (left_align) stdio_pad(state, ' ', padding);
}

static void stdio_emit_pointer(struct stdio_format *state, void *pointer) {
    stdio_emit(state, '0');
    stdio_emit(state, 'x');
    stdio_emit_number(state, (unsigned long)pointer, 16, 0, 0, 0, 0, 0);
}

static void stdio_format_run(struct stdio_format *state,
                             const char *format, va_list arguments) {
    while (*format) {
        if (*format != '%') {
            stdio_emit(state, *format++);
            continue;
        }
        format++;
        int left_align = 0;
        int zero_pad = 0;
        for (;;) {
            if (*format == '-') {
                left_align = 1;
                format++;
            } else if (*format == '0') {
                zero_pad = 1;
                format++;
            } else break;
        }
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < MICH_STDIO_WIDTH) width = width * 10 + (*format - '0');
            format++;
        }
        int long_argument = 0;
        for (;;) {
            if (*format == 'l' || *format == 'z') {
                long_argument = 1;
                format++;
            } else if (*format == 'h') {
                format++;
            } else break;
        }
        int negative = 0;
        unsigned long value = 0;
        const char *text = 0;
        switch (*format) {
        case 'd':
        case 'i':
            value = long_argument ? (unsigned long)va_arg(arguments, long)
                                  : (unsigned long)va_arg(arguments, int);
            if ((long)value < 0) {
                negative = 1;
                value = 0UL - value;
            }
            stdio_emit_number(state, value, 10, 0, negative, zero_pad, width,
                              left_align);
            break;
        case 'u':
            value = long_argument ? va_arg(arguments, unsigned long)
                                  : (unsigned long)(unsigned)va_arg(arguments, unsigned);
            stdio_emit_number(state, value, 10, 0, 0, zero_pad, width,
                              left_align);
            break;
        case 'x':
        case 'X':
            value = long_argument ? va_arg(arguments, unsigned long)
                                  : (unsigned long)(unsigned)va_arg(arguments, unsigned);
            stdio_emit_number(state, value, 16, *format == 'X', 0, zero_pad,
                              width, left_align);
            break;
        case 'p':
            stdio_emit_pointer(state, va_arg(arguments, void *));
            break;
        case 'c':
            stdio_emit(state, (char)va_arg(arguments, int));
            break;
        case 's':
            text = va_arg(arguments, const char *);
            stdio_emit_string(state, text, width, left_align);
            break;
        case '%':
            stdio_emit(state, '%');
            break;
        default:
            /* Unknown conversions are forwarded verbatim instead of
               being silently dropped. */
            stdio_emit(state, '%');
            if (*format) stdio_emit(state, *format);
            break;
        }
        if (*format) format++;
    }
}

int vsnprintf(char *buffer, size_t size, const char *format,
              va_list arguments) {
    struct stdio_format state;
    state.buffer = buffer;
    state.file = 0;
    state.capacity = size;
    state.length = 0;
    state.stream_length = 0;
    stdio_format_run(&state, format, arguments);
    if (size) {
        size_t terminator = state.length < size - 1 ? state.length : size - 1;
        buffer[terminator] = 0;
    }
    return (int)state.length;
}

int snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

int vprintf(const char *format, va_list arguments) {
    struct stdio_format state;
    state.buffer = 0;
    state.file = 0;
    state.capacity = 0;
    state.length = 0;
    state.stream_length = 0;
    stdio_format_run(&state, format, arguments);
    stdio_flush(&state);
    return (int)state.length;
}

int printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

int vfprintf(FILE *file, const char *format, va_list arguments) {
    if (!file) {
        errno = EINVAL;
        return -1;
    }
    struct stdio_format state;
    state.buffer = 0;
    state.file = file;
    state.capacity = 0;
    state.length = 0;
    state.stream_length = 0;
    stdio_format_run(&state, format, arguments);
    return file->error ? -1 : (int)state.length;
}

int fprintf(FILE *file, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(file, format, arguments);
    va_end(arguments);
    return result;
}

int puts(const char *text) {
    if (mich_write(text)) return -1;
    if (mich_write("\n")) return -1;
    return 0;
}

int putchar(int character) {
    char pair[2] = { (char)character, 0 };
    if (mich_write(pair)) return -1;
    return (unsigned char)character;
}

static int file_flush(FILE *file) {
    if (file->reading) {
        /* ISO C leaves input fflush undefined; rewinding the descriptor to
           the logical position keeps a later ftell or fseek honest. */
        if (file->begin < file->end &&
            lseek(file->descriptor, -(off_t)(file->end - file->begin),
                  SEEK_CUR) == (off_t)-1) {
            file->error = 1;
            return EOF;
        }
        file->begin = 0;
        file->end = 0;
        return 0;
    }
    if (!file->pending) return 0;
    ssize_t written = write(file->descriptor, file->buffer, file->pending);
    if (written < 0 || (size_t)written < file->pending) {
        /* The facade reports a full VFS file as a short or failed write;
           keep the unwritten tail staged so the error is observable. */
        if (written > 0) {
            memmove(file->buffer, file->buffer + written,
                    file->pending - (size_t)written);
            file->pending -= (size_t)written;
        }
        file->error = 1;
        return EOF;
    }
    file->pending = 0;
    return 0;
}

static int file_fill(FILE *file) {
    ssize_t got = read(file->descriptor, file->buffer, STDIO_BUFFER);
    if (got < 0) {
        file->error = 1;
        return EOF;
    }
    file->begin = 0;
    file->end = (size_t)got;
    if (!got) file->eof = 1;
    return got ? 0 : EOF;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return 0;
    }
    int flags;
    int reading;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
        reading = 1;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        reading = 0;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        reading = 0;
    } else {
        errno = EINVAL;
        return 0;
    }
    for (int index = 1; mode[index]; index++) {
        if (mode[index] == 'b') continue;
        /* Update modes need a read/write descriptor surface the kernel
           facade does not offer yet. */
        if (mode[index] != '+') errno = EINVAL;
        else errno = EOPNOTSUPP;
        return 0;
    }
    int descriptor = open(path, flags, 0666);
    if (descriptor < 0) return 0;
    FILE *file = malloc(sizeof(*file));
    if (!file) {
        close(descriptor);
        errno = ENOMEM;
        return 0;
    }
    file->buffer = malloc(STDIO_BUFFER);
    if (!file->buffer) {
        free(file);
        close(descriptor);
        errno = ENOMEM;
        return 0;
    }
    file->descriptor = descriptor;
    file->error = 0;
    file->eof = 0;
    file->reading = reading;
    file->begin = 0;
    file->end = 0;
    file->pending = 0;
    return file;
}

int fclose(FILE *file) {
    if (!file) {
        errno = EINVAL;
        return EOF;
    }
    int result = 0;
    if (!file->reading && file_flush(file)) result = EOF;
    unsigned char *buffer = file->buffer;
    if (close(file->descriptor)) result = EOF;
    free(file);
    free(buffer);
    return result;
}

int fflush(FILE *file) {
    if (!file) return 0;
    return file_flush(file);
}

size_t fread(void *pointer, size_t size, size_t count, FILE *file) {
    if (!file || !size || (!pointer && count)) {
        errno = EINVAL;
        return 0;
    }
    if (!file->reading) {
        file->error = 1;
        errno = EBADF;
        return 0;
    }
    if (count && size > (size_t)-1 / count) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t total = size * count;
    size_t done = 0;
    while (done < total) {
        if (file->begin == file->end && file_fill(file)) break;
        size_t available = file->end - file->begin;
        size_t chunk = total - done < available ? total - done : available;
        memcpy((unsigned char *)pointer + done, file->buffer + file->begin,
               chunk);
        file->begin += chunk;
        done += chunk;
    }
    return size ? done / size : 0;
}

size_t fwrite(const void *pointer, size_t size, size_t count, FILE *file) {
    if (!file || !size || (!pointer && count)) {
        errno = EINVAL;
        return 0;
    }
    if (file->reading) {
        file->error = 1;
        errno = EBADF;
        return 0;
    }
    if (count && size > (size_t)-1 / count) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t total = size * count;
    size_t done = 0;
    while (done < total) {
        if (file->pending == STDIO_BUFFER && file_flush(file)) break;
        size_t room = STDIO_BUFFER - file->pending;
        size_t chunk = total - done < room ? total - done : room;
        memcpy(file->buffer + file->pending,
               (const unsigned char *)pointer + done, chunk);
        file->pending += chunk;
        done += chunk;
    }
    return done / size;
}

int fgetc(FILE *file) {
    if (!file) {
        errno = EINVAL;
        return EOF;
    }
    if (!file->reading) {
        file->error = 1;
        errno = EBADF;
        return EOF;
    }
    if (file->begin == file->end && file_fill(file)) return EOF;
    return file->buffer[file->begin++];
}

int fputc(int character, FILE *file) {
    if (!file) {
        errno = EINVAL;
        return EOF;
    }
    if (file->reading) {
        file->error = 1;
        errno = EBADF;
        return EOF;
    }
    if (file->pending == STDIO_BUFFER && file_flush(file)) return EOF;
    file->buffer[file->pending++] = (unsigned char)character;
    return (unsigned char)character;
}

char *fgets(char *destination, int size, FILE *file) {
    if (!destination || size <= 0 || !file || !file->reading) {
        errno = EINVAL;
        return 0;
    }
    int length = 0;
    while (length + 1 < size) {
        int character = fgetc(file);
        if (character == EOF) break;
        destination[length++] = (char)character;
        if (character == '\n') break;
    }
    if (!length) return 0;
    destination[length] = 0;
    return destination;
}

int fputs(const char *text, FILE *file) {
    if (!text || !file) {
        errno = EINVAL;
        return EOF;
    }
    size_t length = strlen(text);
    if (!length) return 0;
    return fwrite(text, 1, length, file) == length ? 0 : EOF;
}

int fseek(FILE *file, long offset, int whence) {
    if (!file || whence < SEEK_SET || whence > SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    off_t adjusted = (off_t)offset;
    if (file->reading) {
        /* SEEK_CUR is relative to the consumed position, not the end of
           the read-ahead buffer. */
        if (whence == SEEK_CUR) adjusted -= (off_t)(file->end - file->begin);
        file->begin = 0;
        file->end = 0;
    } else if (file_flush(file)) {
        return -1;
    }
    file->eof = 0;
    if (lseek(file->descriptor, adjusted, whence) == (off_t)-1) {
        file->error = 1;
        return -1;
    }
    return 0;
}

long ftell(FILE *file) {
    if (!file) {
        errno = EINVAL;
        return -1;
    }
    off_t position = lseek(file->descriptor, 0, SEEK_CUR);
    if (position == (off_t)-1) {
        file->error = 1;
        return -1;
    }
    if (file->reading) position -= (off_t)(file->end - file->begin);
    else position += (off_t)file->pending;
    return (long)position;
}

void rewind(FILE *file) {
    if (!file) return;
    fseek(file, 0, SEEK_SET);
    file->error = 0;
}

int feof(FILE *file) {
    return file && file->eof;
}

int ferror(FILE *file) {
    return file && file->error;
}

void clearerr(FILE *file) {
    if (!file) return;
    file->eof = 0;
    file->error = 0;
}
