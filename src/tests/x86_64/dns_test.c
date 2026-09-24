#include "tests64.h"
#include "dns_message.h"

// Exercises the DNS message layer against both well-formed replies and the
// hostile ones an off-path attacker would send. The parser runs on bytes that
// arrive from the network before anything has been authenticated, so every
// rejection below is a memory-safety property, not a style preference.
//
// This file prints nothing. The runner owns pass and fail reporting.

static unsigned int append(unsigned char *out, unsigned int cursor,
                           const unsigned char *bytes, unsigned int count) {
    for (unsigned int index = 0; index < count; index++)
        out[cursor + index] = bytes[index];
    return cursor + count;
}

static unsigned int header(unsigned char *out, unsigned int transaction,
                           unsigned int flags, unsigned int questions,
                           unsigned int answers) {
    out[0] = (unsigned char)(transaction >> 8);
    out[1] = (unsigned char)(transaction & 0xFF);
    out[2] = (unsigned char)(flags >> 8);
    out[3] = (unsigned char)(flags & 0xFF);
    out[4] = (unsigned char)(questions >> 8);
    out[5] = (unsigned char)(questions & 0xFF);
    out[6] = (unsigned char)(answers >> 8);
    out[7] = (unsigned char)(answers & 0xFF);
    out[8] = 0; out[9] = 0; out[10] = 0; out[11] = 0;
    return 12;
}

static const unsigned char example_question[] = {
    7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0, 0, 1, 0, 1
};

int test_dns_message(void) {
    unsigned char message[DNS_TCP_MESSAGE_MAX];
    struct dns_result result;
    unsigned int truncated = 0;
    unsigned int cursor = 0;

    // 1. A query encodes as length-prefixed labels with the class and type
    //    trailer, and reports the exact byte count it wrote.
    unsigned int length =
        dns_build_query(message, sizeof(message), "example.com", DNS_TYPE_A,
                        0x1234);
    if (length != 12 + 13 + 4) return -1;
    if (message[12] != 7 || message[20] != 3 || message[24] != 0) return -1;
    if (message[25] != 0 || message[26] != DNS_TYPE_A) return -1;
    if (message[0] != 0x12 || message[1] != 0x34) return -1;

    // 2. A label above 63 bytes has no valid encoding and must be refused
    //    rather than silently truncated into a different name.
    char oversized[80];
    for (unsigned int index = 0; index < 70; index++) oversized[index] = 'a';
    oversized[70] = 0;
    if (dns_build_query(message, sizeof(message), oversized, DNS_TYPE_A, 1))
        return -1;

    // 3. A name above 255 bytes likewise has no valid encoding.
    char toolong[300];
    unsigned int filled = 0;
    while (filled < 260) {
        toolong[filled++] = 'a';
        toolong[filled++] = 'b';
        toolong[filled++] = '.';
    }
    toolong[filled] = 0;
    if (dns_build_query(message, sizeof(message), toolong, DNS_TYPE_A, 1))
        return -1;

    // 4. A straightforward A reply yields the address and its TTL.
    static const unsigned char a_answer[] = {
        0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34
    };
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    cursor = append(message, cursor, a_answer, sizeof(a_answer));
    if (dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                        &truncated, &result))
        return -1;
    if (result.ipv4_count != 1 || result.ipv4[0] != 0x5DB8D822u) return -1;
    if (result.ttl != 60 || truncated) return -1;

    // 5. Name comparison is case insensitive, as the protocol requires.
    if (dns_parse_reply(message, cursor, "EXAMPLE.CoM", DNS_TYPE_A, 0x1234,
                        &truncated, &result))
        return -1;

    // 6. A reply carrying somebody else's transaction id is a forgery
    //    attempt and must not be accepted for this question.
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x4321,
                         &truncated, &result))
        return -1;

    // 7. A reply whose question section names a different host must not
    //    satisfy the pending query.
    if (!dns_parse_reply(message, cursor, "other.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 8. An A record must not answer a AAAA question. The cache is keyed on
    //    name and type precisely so this cannot leak across.
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_AAAA,
                         0x1234, &truncated, &result))
        return -1;

    // 9. A record whose payload length disagrees with its type is malformed.
    static const unsigned char short_a[] = {
        0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 3, 93, 184, 216
    };
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    cursor = append(message, cursor, short_a, sizeof(short_a));
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 10. A compression pointer aimed at itself is the classic decompression
    //     loop. It must be refused, and the parser must return at all.
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    message[cursor++] = 0xC0;
    message[cursor++] = 0x0C;
    message[cursor++] = 0; message[cursor++] = 1;
    message[cursor++] = 0; message[cursor++] = 1;
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 11. A pointer that jumps forward is refused even though it terminates.
    //     Allowing it would let a crafted message walk unbounded, so the rule
    //     is strictly backward targets only.
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    message[cursor++] = 0xC0;
    message[cursor++] = 0x14;
    message[cursor++] = 0; message[cursor++] = 1;
    message[cursor++] = 0; message[cursor++] = 1;
    cursor = append(message, cursor, example_question, 13);
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 12. A record that claims more payload than the message holds must not
    //     be read past the end of the buffer.
    static const unsigned char overrun[] = {
        0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0xFF, 0xF0, 1, 2, 3, 4
    };
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    cursor = append(message, cursor, overrun, sizeof(overrun));
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 13. A non-zero rcode carries no usable answer.
    cursor = header(message, 0x1234, 0x8183, 1, 0);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 14. The truncation bit is not an error: the caller is told to retry
    //     over TCP, so parsing succeeds while reporting no addresses.
    truncated = 0;
    cursor = header(message, 0x1234, 0x8380, 1, 0);
    if (dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                        &truncated, &result))
        return -1;
    if (!truncated) return -1;

    // 15. A CNAME chain is followed to the address, and the cached lifetime
    //     collapses to the shortest hop so an alias cannot outlive its target.
    static const unsigned char www_question[] = {
        3, 'w','w','w', 7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
        0, 1, 0, 1
    };
    static const unsigned char cname_chain[] = {
        0xC0, 0x0C, 0, 5, 0, 1, 0, 0, 0, 30, 0, 2, 0xC0, 0x10,
        0xC0, 0x10, 0, 1, 0, 1, 0, 0, 0, 90, 0, 4, 10, 0, 0, 7
    };
    cursor = header(message, 0x1234, 0x8180, 1, 2);
    cursor = append(message, cursor, www_question, sizeof(www_question));
    cursor = append(message, cursor, cname_chain, sizeof(cname_chain));
    if (dns_parse_reply(message, cursor, "www.example.com", DNS_TYPE_A,
                        0x1234, &truncated, &result))
        return -1;
    if (result.ipv4_count != 1 || result.ipv4[0] != 0x0A000007u) return -1;
    if (result.ttl != 30) return -1;

    // 16. A AAAA reply yields the full sixteen byte address.
    static const unsigned char aaaa_question[] = {
        7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0, 0, 28, 0, 1
    };
    static const unsigned char aaaa_answer[] = {
        0xC0, 0x0C, 0, 28, 0, 1, 0, 0, 0, 50, 0, 16,
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    cursor = header(message, 0x1234, 0x8180, 1, 1);
    cursor = append(message, cursor, aaaa_question, sizeof(aaaa_question));
    cursor = append(message, cursor, aaaa_answer, sizeof(aaaa_answer));
    if (dns_parse_reply(message, cursor, "example.com", DNS_TYPE_AAAA, 0x1234,
                        &truncated, &result))
        return -1;
    if (result.ipv6_count != 1) return -1;
    if (result.ipv6[0][0] != 0x20 || result.ipv6[0][1] != 0x01) return -1;
    if (result.ipv6[0][15] != 1 || result.ttl != 50) return -1;

    // 17. A reply that is only a header, with the answer section promised but
    //     absent, must not be mined for records that are not there.
    cursor = header(message, 0x1234, 0x8180, 1, 4);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 18. A query flag where the response bit is clear is not a response.
    cursor = header(message, 0x1234, 0x0100, 1, 1);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    cursor = append(message, cursor, a_answer, sizeof(a_answer));
    if (!dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    // 19. Records for a name the question never asked about are ignored
    //     rather than cached, which is the cache-poisoning case.
    static const unsigned char bystander[] = {
        7, 'e','v','i','l','x','y','z', 3, 'c','o','m', 0,
        0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 6, 6, 6, 6
    };
    cursor = header(message, 0x1234, 0x8180, 1, 2);
    cursor = append(message, cursor, example_question,
                    sizeof(example_question));
    cursor = append(message, cursor, bystander, sizeof(bystander));
    cursor = append(message, cursor, a_answer, sizeof(a_answer));
    if (dns_parse_reply(message, cursor, "example.com", DNS_TYPE_A, 0x1234,
                        &truncated, &result))
        return -1;
    if (result.ipv4_count != 1 || result.ipv4[0] != 0x5DB8D822u) return -1;

    // 20. A truncated header is too short to interpret at all.
    if (!dns_parse_reply(message, 8, "example.com", DNS_TYPE_A, 0x1234,
                         &truncated, &result))
        return -1;

    return 0;
}
