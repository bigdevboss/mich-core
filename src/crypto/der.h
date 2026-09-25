#ifndef MICH_CRYPTO_DER_H
#define MICH_CRYPTO_DER_H

#include "types.h"

#define DER_TAG_BOOLEAN 0x01u
#define DER_TAG_INTEGER 0x02u
#define DER_TAG_BIT_STRING 0x03u
#define DER_TAG_OCTET_STRING 0x04u
#define DER_TAG_NULL 0x05u
#define DER_TAG_OID 0x06u
#define DER_TAG_UTF8_STRING 0x0Cu
#define DER_TAG_PRINTABLE_STRING 0x13u
#define DER_TAG_IA5_STRING 0x16u
#define DER_TAG_UTC_TIME 0x17u
#define DER_TAG_GENERALIZED_TIME 0x18u
#define DER_TAG_SEQUENCE 0x30u
#define DER_TAG_SET 0x31u

#define DER_TAG_CONTEXT 0x80u
#define DER_TAG_CONSTRUCTED 0x20u

// Elements are read through a cursor that carries its own end, and a nested
// element produces a new cursor clamped to that element. Reading past the
// buffer is then not something the parser has to remember to check.
struct der_reader {
    const u8 *data;
    u32 offset;
    u32 end;
};

int der_init(struct der_reader *reader, const u8 *data, u32 length);

// 1 when the cursor has consumed everything it was given.
int der_at_end(const struct der_reader *reader);

// 0 and the next tag, or -1 when nothing is left.
int der_peek(const struct der_reader *reader, u8 *tag);

// Reads one element of the expected tag and hands back its contents. The
// pointer aims into the caller's buffer, which has to outlive every value
// taken from it.
int der_read(struct der_reader *reader, u8 tag, const u8 **value,
             u32 *length);

// Same, but the contents become a cursor of their own.
int der_read_nested(struct der_reader *reader, u8 tag,
                    struct der_reader *inner);

// Reads one element of any tag, reporting what it was. Used for the optional
// and context-tagged members of a certificate.
int der_read_any(struct der_reader *reader, u8 *tag, const u8 **value,
                 u32 *length);

// Reads an element together with its header, which is what has to be hashed
// when a signature covers a whole structure rather than its contents.
int der_read_element(struct der_reader *reader, u8 tag, const u8 **element,
                     u32 *element_length, const u8 **value, u32 *length);

// Unsigned INTEGER as a big-endian span with the DER sign byte removed. Fails
// on a negative value, which no field this parser reads is allowed to be.
int der_read_unsigned(struct der_reader *reader, const u8 **value,
                      u32 *length);

// Small unsigned INTEGER that has to fit in 32 bits.
int der_read_u32(struct der_reader *reader, u32 *value);

#endif
