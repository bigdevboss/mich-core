#include "der.h"

// Four length bytes already describe more than any certificate this parser
// accepts, and refusing longer encodings keeps the arithmetic below inside
// 32 bits.
#define DER_MAX_LENGTH_BYTES 4u

int der_init(struct der_reader *reader, const u8 *data, u32 length) {
    if (!reader || (!data && length)) return -1;
    reader->data = data;
    reader->offset = 0;
    reader->end = length;
    return 0;
}

int der_at_end(const struct der_reader *reader) {
    return reader && reader->offset >= reader->end;
}

int der_peek(const struct der_reader *reader, u8 *tag) {
    if (!reader || !tag || reader->offset >= reader->end) return -1;
    *tag = reader->data[reader->offset];
    return 0;
}

// Decodes tag and length, leaving the cursor on the contents. Every rule of
// DER that BER relaxes is enforced here, because a certificate that can be
// encoded two ways hashes two ways, and the signature covers the bytes.
static int der_header(struct der_reader *reader, u8 *tag, u32 *length,
                      u32 *header_length) {
    if (!reader || reader->offset >= reader->end) return -1;
    u32 start = reader->offset;
    u8 first = reader->data[start];
    // High tag numbers need multiple bytes and appear in nothing a
    // certificate contains.
    if ((first & 0x1Fu) == 0x1Fu) return -1;
    u32 cursor = start + 1u;
    if (cursor >= reader->end) return -1;

    u8 marker = reader->data[cursor];
    cursor++;
    u32 value_length;
    if (marker < 0x80u) {
        value_length = marker;
    } else if (marker == 0x80u) {
        // Indefinite length: legal in BER, and the reason one structure could
        // otherwise arrive in several encodings.
        return -1;
    } else if (marker == 0xFFu) {
        return -1;
    } else {
        u32 count = marker & 0x7Fu;
        if (count > DER_MAX_LENGTH_BYTES) return -1;
        if (count > reader->end - cursor) return -1;
        if (!reader->data[cursor]) return -1;
        value_length = 0;
        for (u32 index = 0; index < count; index++)
            value_length = (value_length << 8) | reader->data[cursor + index];
        // A value that fits the short form has to use it.
        if (value_length < 0x80u) return -1;
        // And the long form has to be no longer than needed.
        if (count > 1u && value_length < (1u << ((count - 1u) * 8u)))
            return -1;
        cursor += count;
    }
    // Subtraction instead of addition: cursor + value_length would wrap.
    if (value_length > reader->end - cursor) return -1;

    *tag = first;
    *length = value_length;
    *header_length = cursor - start;
    return 0;
}

int der_read_any(struct der_reader *reader, u8 *tag, const u8 **value,
                 u32 *length) {
    if (!reader || !tag || !value || !length) return -1;
    u32 header;
    if (der_header(reader, tag, length, &header)) return -1;
    *value = reader->data + reader->offset + header;
    reader->offset += header + *length;
    return 0;
}

int der_read(struct der_reader *reader, u8 tag, const u8 **value,
             u32 *length) {
    if (!reader || !value || !length) return -1;
    u8 found;
    u32 header;
    u32 found_length;
    if (der_header(reader, &found, &found_length, &header)) return -1;
    if (found != tag) return -1;
    *value = reader->data + reader->offset + header;
    *length = found_length;
    reader->offset += header + found_length;
    return 0;
}

int der_read_nested(struct der_reader *reader, u8 tag,
                    struct der_reader *inner) {
    const u8 *value;
    u32 length;
    if (!inner || der_read(reader, tag, &value, &length)) return -1;
    return der_init(inner, value, length);
}

int der_read_element(struct der_reader *reader, u8 tag, const u8 **element,
                     u32 *element_length, const u8 **value, u32 *length) {
    if (!reader || !element || !element_length || !value || !length) return -1;
    u8 found;
    u32 header;
    u32 found_length;
    u32 start = reader->offset;
    if (der_header(reader, &found, &found_length, &header)) return -1;
    if (found != tag) return -1;
    *element = reader->data + start;
    *element_length = header + found_length;
    *value = reader->data + start + header;
    *length = found_length;
    reader->offset = start + header + found_length;
    return 0;
}

int der_read_unsigned(struct der_reader *reader, const u8 **value,
                      u32 *length) {
    const u8 *raw;
    u32 raw_length;
    if (der_read(reader, DER_TAG_INTEGER, &raw, &raw_length)) return -1;
    if (!raw_length) return -1;
    // A leading 0x80 bit means negative, which none of these fields may be.
    if (raw[0] & 0x80u) return -1;
    if (raw_length > 1u && !raw[0]) {
        // The pad byte is only allowed when the next byte would look negative.
        if (!(raw[1] & 0x80u)) return -1;
        raw++;
        raw_length--;
    }
    *value = raw;
    *length = raw_length;
    return 0;
}

int der_read_u32(struct der_reader *reader, u32 *value) {
    const u8 *raw;
    u32 length;
    if (!value || der_read_unsigned(reader, &raw, &length)) return -1;
    if (length > 4u) return -1;
    u32 result = 0;
    for (u32 index = 0; index < length; index++)
        result = (result << 8) | raw[index];
    *value = result;
    return 0;
}
