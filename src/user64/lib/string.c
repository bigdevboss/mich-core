#include <string.h>

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t index = 0; index < length; index++) out[index] = in[index];
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    /* Copy forward unless the ranges overlap backwards, where a forward
       copy would clobber the not yet read source bytes. */
    if (out < in || out >= in + length) {
        for (size_t index = 0; index < length; index++) out[index] = in[index];
    } else {
        for (size_t index = length; index > 0; index--) out[index - 1] = in[index - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    for (size_t index = 0; index < length; index++) out[index] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *first = (const unsigned char *)left;
    const unsigned char *second = (const unsigned char *)right;
    for (size_t index = 0; index < length; index++) {
        if (first[index] != second[index]) {
            return first[index] < second[index] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (left[index] != right[index] || !left[index]) {
            return (int)(unsigned char)left[index] -
                   (int)(unsigned char)right[index];
        }
    }
    return 0;
}

char *strchr(const char *text, int character) {
    for (;;) {
        if (*text == (char)character) return (char *)text;
        if (!*text) return 0;
        text++;
    }
}

char *strrchr(const char *text, int character) {
    const char *last = 0;
    for (;;) {
        if (*text == (char)character) last = text;
        if (!*text) return (char *)last;
        text++;
    }
}
