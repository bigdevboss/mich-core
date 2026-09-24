#include "dns_message.h"

static unsigned int name_length(const char *name) {
    unsigned int length = 0;
    while (name && name[length] && length <= DNS_NAME_MAX) length++;
    return length;
}

static int names_equal(const char *left, const char *right) {
    unsigned int index = 0;
    for (; index <= DNS_NAME_MAX; index++) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

static unsigned int read_be16(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8) | bytes[1];
}

static unsigned int read_be32(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24) | ((unsigned int)bytes[1] << 16) |
           ((unsigned int)bytes[2] << 8) | bytes[3];
}

static void write_be16(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
}

// Encodes a dotted name as length-prefixed labels. Rejects empty labels, an
// oversized label, and an oversized name so a malformed caller string can
// never produce a query that a server would read past.
static unsigned int encode_name(unsigned char *out, unsigned int limit,
                                const char *name) {
    unsigned int written = 0;
    unsigned int index = 0;
    while (name[index]) {
        unsigned int label = 0;
        while (name[index + label] && name[index + label] != '.') label++;
        if (!label || label > DNS_LABEL_MAX) return 0;
        if (written + label + 1 >= limit) return 0;
        out[written++] = (unsigned char)label;
        for (unsigned int byte = 0; byte < label; byte++)
            out[written++] = (unsigned char)name[index + byte];
        index += label;
        if (name[index] == '.') index++;
    }
    if (!written || written + 1 > limit) return 0;
    out[written++] = 0;
    return written;
}

// Walks a possibly compressed name. Two rules make a pointer loop impossible:
// a pointer may only target a strictly lower offset, and the jump count is
// capped. Both are required; either one alone still allows a cycle.
static int skip_name(const unsigned char *message, unsigned int length,
                     unsigned int offset, unsigned int *next,
                     char *text, unsigned int text_limit) {
    unsigned int jumps = 0;
    unsigned int cursor = offset;
    unsigned int written = 0;
    int followed = 0;
    while (cursor < length) {
        unsigned char marker = message[cursor];
        if ((marker & 0xC0) == 0xC0) {
            if (cursor + 1 >= length) return -1;
            unsigned int target = ((unsigned int)(marker & 0x3F) << 8) |
                                  message[cursor + 1];
            if (target >= cursor) return -1;
            if (++jumps > DNS_JUMP_MAX) return -1;
            if (!followed) {
                if (next) *next = cursor + 2;
                followed = 1;
            }
            cursor = target;
            continue;
        }
        if (marker & 0xC0) return -1;
        if (!marker) {
            if (!followed && next) *next = cursor + 1;
            if (text && written < text_limit) text[written] = 0;
            return 0;
        }
        if (marker > DNS_LABEL_MAX) return -1;
        if (cursor + 1 + marker > length) return -1;
        if (text) {
            if (written && written + 1 < text_limit) text[written++] = '.';
            if (written + marker >= text_limit) return -1;
            for (unsigned int byte = 0; byte < marker; byte++)
                text[written++] = (char)message[cursor + 1 + byte];
        } else if (written + marker + 1 > DNS_NAME_MAX) {
            return -1;
        }
        written += text ? 0 : marker + 1;
        cursor += 1 + marker;
    }
    return -1;
}

unsigned int dns_build_query(unsigned char *message, unsigned int limit,
                                  const char *name, unsigned int type,
                                  unsigned int transaction) {
    if (!message || !name || limit < 12 + 5) return 0;
    unsigned int length = name_length(name);
    if (!length || length > DNS_NAME_MAX) return 0;
    for (unsigned int index = 0; index < 12; index++) message[index] = 0;
    write_be16(message, transaction & 0xFFFF);
    write_be16(message + 2, 0x0100);
    write_be16(message + 4, 1);
    unsigned int written = encode_name(message + 12, limit - 12 - 4, name);
    if (!written) return 0;
    unsigned int cursor = 12 + written;
    write_be16(message + cursor, type);
    write_be16(message + cursor + 2, 1);
    return cursor + 4;
}

static int question_matches(const unsigned char *message, unsigned int length,
                            const char *name, unsigned int type,
                            unsigned int *answer_offset) {
    char parsed[DNS_NAME_MAX + 1];
    unsigned int next = 0;
    if (skip_name(message, length, 12, &next, parsed, sizeof(parsed)))
        return 0;
    if (next + 4 > length) return 0;
    if (read_be16(message + next) != type) return 0;
    if (read_be16(message + next + 2) != 1) return 0;
    if (!names_equal(parsed, name)) return 0;
    if (answer_offset) *answer_offset = next + 4;
    return 1;
}

int dns_parse_reply(const unsigned char *message, unsigned int length,
                         const char *name, unsigned int type,
                         unsigned int transaction, unsigned int *truncated,
                         struct dns_result *result) {
    if (!message || !name || !result || length < 12) return -1;
    if (read_be16(message) != (transaction & 0xFFFF)) return -1;
    unsigned int flags = read_be16(message + 2);
    if (!(flags & 0x8000)) return -1;
    if (truncated) *truncated = (flags & 0x0200) ? 1 : 0;
    if (flags & 0x0200) return 0;
    if ((flags & 0x000F) != 0) return -1;
    if (read_be16(message + 4) != 1) return -1;
    unsigned int answers = read_be16(message + 6);
    unsigned int cursor = 0;
    if (!question_matches(message, length, name, type, &cursor)) return -1;

    char wanted[DNS_NAME_MAX + 1];
    for (unsigned int index = 0; index <= DNS_NAME_MAX; index++) {
        wanted[index] = name[index];
        if (!name[index]) break;
    }
    wanted[DNS_NAME_MAX] = 0;

    result->ipv4_count = 0;
    result->ipv6_count = 0;
    result->ttl = 0;
    unsigned int aliases = 0;
    for (unsigned int index = 0; index < answers; index++) {
        char owner[DNS_NAME_MAX + 1];
        unsigned int next = 0;
        if (skip_name(message, length, cursor, &next, owner, sizeof(owner)))
            return -1;
        if (next + 10 > length) return -1;
        unsigned int record = read_be16(message + next);
        unsigned int klass = read_be16(message + next + 2);
        unsigned int ttl = read_be32(message + next + 4);
        unsigned int size = read_be16(message + next + 8);
        unsigned int data = next + 10;
        if (data + size > length) return -1;
        cursor = data + size;
        if (klass != 1 || !names_equal(owner, wanted)) continue;
        if (record == DNS_TYPE_CNAME) {
            if (++aliases > DNS_CNAME_MAX) return -1;
            if (skip_name(message, length, data, 0, wanted, sizeof(wanted)))
                return -1;
            // An alias expiring before its target would leave the cached
            // address reachable under a name that no longer points at it, so
            // the whole chain is held to its shortest TTL.
            if (!result->ttl || ttl < result->ttl) result->ttl = ttl;
            continue;
        }
        if (record == DNS_TYPE_A && type == DNS_TYPE_A) {
            if (size != 4) return -1;
            if (result->ipv4_count < DNS_ADDRESS_MAX)
                result->ipv4[result->ipv4_count++] = read_be32(message + data);
        } else if (record == DNS_TYPE_AAAA &&
                   type == DNS_TYPE_AAAA) {
            if (size != 16) return -1;
            if (result->ipv6_count < DNS_ADDRESS_MAX) {
                for (unsigned int byte = 0; byte < 16; byte++)
                    result->ipv6[result->ipv6_count][byte] =
                        message[data + byte];
                result->ipv6_count++;
            }
        } else {
            continue;
        }
        if (!result->ttl || ttl < result->ttl) result->ttl = ttl;
    }
    return result->ipv4_count || result->ipv6_count ? 0 : -1;
}
