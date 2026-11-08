/*
 * Copyright (C) FreeUnit Community
 */

#include <nxt_main.h>
#include <nxt_span.h>


/*
 * The PROXY protocol, versions 1 and 2, as HAProxy documents it in
 * doc/proxy-protocol.txt.  Only the parsing lives here.  It is pure: the
 * input is a byte range, the output is a small struct, so the C test suite
 * and a fuzzer can drive every malformed shape without a connection.
 *
 * The parser is strict.  A sender must not expect a receiver to guess:
 * the configured version is the only one accepted, every field must have
 * exactly the documented shape, and a header that the parser cannot
 * account for byte by byte is an error.  The caller drops the connection
 * on an error, so a lenient parser would only hide a misconfigured proxy.
 */

#define NXT_PP2_CMD_LOCAL          0x0
#define NXT_PP2_CMD_PROXY          0x1

#define NXT_PP2_FAMILY_INET        0x1
#define NXT_PP2_FAMILY_INET6       0x2

#define NXT_PP2_TRANSPORT_STREAM   0x1

#define NXT_PP2_TYPE_SSL           0x20
#define NXT_PP2_CLIENT_SSL         0x01

#define NXT_PP2_INET_ADDR_LENGTH   12
#define NXT_PP2_INET6_ADDR_LENGTH  36


static nxt_int_t nxt_proxy_protocol_v1_parse(const u_char *start,
    size_t size, nxt_proxy_protocol_t *pp);
static nxt_int_t nxt_proxy_protocol_v1_field(nxt_span_t *line,
    nxt_str_t *field, nxt_bool_t last);
static nxt_int_t nxt_proxy_protocol_v1_addr(nxt_str_t *field,
    nxt_bool_t inet6, u_char *address);
static nxt_int_t nxt_proxy_protocol_v1_inet(nxt_str_t *field,
    in_addr_t *addr);
static nxt_int_t nxt_proxy_protocol_v1_port(nxt_str_t *field,
    in_port_t *port);
static nxt_int_t nxt_proxy_protocol_v2_parse(const u_char *start,
    size_t size, nxt_proxy_protocol_t *pp);
static nxt_int_t nxt_proxy_protocol_v2_tlvs(nxt_span_t *span,
    nxt_proxy_protocol_t *pp);


static const u_char  nxt_proxy_protocol_v1_prefix[] = "PROXY ";

static const u_char  nxt_proxy_protocol_v2_signature[12] = {
    '\r', '\n', '\r', '\n', '\0', '\r', '\n', 'Q', 'U', 'I', 'T', '\n',
};


nxt_uint_t
nxt_proxy_protocol_version(const nxt_str_t *name)
{
    if (nxt_str_eq(name, "v1", 2)) {
        return NXT_PROXY_PROTOCOL_V1;
    }

    if (nxt_str_eq(name, "v2", 2)) {
        return NXT_PROXY_PROTOCOL_V2;
    }

    return NXT_PROXY_PROTOCOL_OFF;
}


nxt_int_t
nxt_proxy_protocol_parse(nxt_uint_t version, const u_char *start, size_t size,
    nxt_proxy_protocol_t *pp)
{
    nxt_memzero(pp, sizeof(nxt_proxy_protocol_t));

    switch (version) {

    case NXT_PROXY_PROTOCOL_V1:
        return nxt_proxy_protocol_v1_parse(start, size, pp);

    case NXT_PROXY_PROTOCOL_V2:
        return nxt_proxy_protocol_v2_parse(start, size, pp);

    default:
        return NXT_ERROR;
    }
}


/*
 * Version 1 is one text line:
 *
 *     PROXY TCP4 <src addr> <dst addr> <src port> <dst port>\r\n
 *
 * or the same with TCP6, or "PROXY UNKNOWN" followed by anything up to the
 * CRLF.  The line is at most 107 bytes, CRLF included.  The parser first
 * compares the bytes it has with the "PROXY " prefix, so an HTTP request
 * or a TLS ClientHello is refused on its first byte instead of after a
 * wait for a CRLF that never comes.
 */

static nxt_int_t
nxt_proxy_protocol_v1_parse(const u_char *start, size_t size,
    nxt_proxy_protocol_t *pp)
{
    size_t        n, max;
    nxt_str_t     field;
    nxt_span_t    line;
    nxt_bool_t    inet6;
    const u_char  *p, *end;

    n = nxt_min(size, nxt_length(nxt_proxy_protocol_v1_prefix));

    if (memcmp(start, nxt_proxy_protocol_v1_prefix, n) != 0) {
        return NXT_ERROR;
    }

    max = nxt_min(size, NXT_PROXY_PROTOCOL_V1_MAX_LENGTH);
    end = start + max;

    for (p = start; p + 1 < end; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            break;
        }
    }

    if (p + 1 >= end) {
        /* No CRLF yet.  A longer line cannot be a v1 header at all. */
        return (size < NXT_PROXY_PROTOCOL_V1_MAX_LENGTH) ? NXT_AGAIN
                                                         : NXT_ERROR;
    }

    pp->length = (p + 2) - start;

    /* The fields after "PROXY " and before the CRLF. */

    nxt_span_init(&line, start + nxt_length(nxt_proxy_protocol_v1_prefix), p);

    /*
     * "PROXY UNKNOWN" may end the line or be followed by anything the
     * sender wants to say, so it is checked before the fields are split.
     */

    n = nxt_length("UNKNOWN");

    if (nxt_span_len(&line) >= n
        && memcmp(line.pos, "UNKNOWN", n) == 0
        && (nxt_span_len(&line) == n || line.pos[n] == ' '))
    {
        pp->local = 1;
        return NXT_OK;
    }

    if (nxt_proxy_protocol_v1_field(&line, &field, 0) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_str_eq(&field, "TCP4", 4)) {
        inet6 = 0;
        pp->family = AF_INET;

    } else if (nxt_str_eq(&field, "TCP6", 4)) {
#if (NXT_INET6)
        inet6 = 1;
        pp->family = AF_INET6;
#else
        return NXT_ERROR;
#endif

    } else {
        return NXT_ERROR;
    }

    /* The source address. */

    if (nxt_proxy_protocol_v1_field(&line, &field, 0) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_proxy_protocol_v1_addr(&field, inet6, pp->address) != NXT_OK) {
        return NXT_ERROR;
    }

    /* The destination address is checked for shape and then dropped. */

    if (nxt_proxy_protocol_v1_field(&line, &field, 0) != NXT_OK
        || nxt_proxy_protocol_v1_addr(&field, inet6, NULL) != NXT_OK)
    {
        return NXT_ERROR;
    }

    /* The source port, then the destination port, which ends the line. */

    if (nxt_proxy_protocol_v1_field(&line, &field, 0) != NXT_OK
        || nxt_proxy_protocol_v1_port(&field, &pp->port) != NXT_OK)
    {
        return NXT_ERROR;
    }

    if (nxt_proxy_protocol_v1_field(&line, &field, 1) != NXT_OK
        || nxt_proxy_protocol_v1_port(&field, NULL) != NXT_OK)
    {
        return NXT_ERROR;
    }

    return NXT_OK;
}


/*
 * Takes the next field of the line.  A field is followed by exactly one
 * space, or by the end of the line when it is the last one.  An empty
 * field is an error: two spaces in a row are not a separator.
 */

static nxt_int_t
nxt_proxy_protocol_v1_field(nxt_span_t *line, nxt_str_t *field,
    nxt_bool_t last)
{
    const u_char  *p;

    for (p = line->pos; p < line->end; p++) {
        if (*p == ' ') {
            break;
        }
    }

    if (p == line->pos) {
        return NXT_ERROR;
    }

    if (last != (p == line->end)) {
        return NXT_ERROR;
    }

    field->start = (u_char *) line->pos;
    field->length = p - line->pos;

    line->pos = last ? p : p + 1;

    return NXT_OK;
}


/*
 * Parses a v1 address into "address" in network byte order, when it is
 * not NULL.  The text parsers of nxt_sockaddr.c check the shape.
 */

static nxt_int_t
nxt_proxy_protocol_v1_addr(nxt_str_t *field, nxt_bool_t inet6,
    u_char *address)
{
    in_addr_t  addr;

#if (NXT_INET6)
    if (inet6) {
        struct in6_addr  addr6;

        if (nxt_inet6_addr(&addr6, field->start, field->length) != NXT_OK) {
            return NXT_ERROR;
        }

        if (address != NULL) {
            nxt_memcpy(address, &addr6, sizeof(struct in6_addr));
        }

        return NXT_OK;
    }
#endif

    if (nxt_proxy_protocol_v1_inet(field, &addr) != NXT_OK) {
        return NXT_ERROR;
    }

    if (address != NULL) {
        nxt_memcpy(address, &addr, sizeof(in_addr_t));
    }

    return NXT_OK;
}


/*
 * An IPv4 address is four decimal octets separated by dots.  An octet is
 * one to three digits without a leading zero and is at most 255.  The
 * shared nxt_inet_addr() is not used: it cannot return 255.255.255.255
 * and it accepts leading zeroes.
 */

static nxt_int_t
nxt_proxy_protocol_v1_inet(nxt_str_t *field, in_addr_t *addr)
{
    size_t      i, length, digits;
    u_char      *start;
    uint32_t    value;
    nxt_uint_t  octet, octets;

    start = field->start;
    length = field->length;

    value = 0;
    octet = 0;
    octets = 0;
    digits = 0;

    for (i = 0; i <= length; i++) {

        if (i == length || start[i] == '.') {
            if (digits == 0) {
                return NXT_ERROR;
            }

            value = (value << 8) | octet;
            octets++;

            if (octets == 4) {
                break;
            }

            octet = 0;
            digits = 0;
            continue;
        }

        if (start[i] < '0' || start[i] > '9') {
            return NXT_ERROR;
        }

        if (digits == 1 && octet == 0) {
            return NXT_ERROR;
        }

        octet = octet * 10 + (start[i] - '0');
        digits++;

        if (octet > 255) {
            return NXT_ERROR;
        }
    }

    if (octets != 4 || i != length) {
        return NXT_ERROR;
    }

    *addr = htonl(value);

    return NXT_OK;
}


/* A port is one to five decimal digits without a leading zero. */

static nxt_int_t
nxt_proxy_protocol_v1_port(nxt_str_t *field, in_port_t *port)
{
    size_t      i, length;
    u_char      *start;
    nxt_uint_t  digit, value;

    start = field->start;
    length = field->length;

    if (length == 0 || length > nxt_length("65535")) {
        return NXT_ERROR;
    }

    if (length > 1 && start[0] == '0') {
        return NXT_ERROR;
    }

    value = 0;

    for (i = 0; i < length; i++) {
        /* Values below '0' become large unsigned integers. */
        digit = start[i] - '0';

        if (digit > 9) {
            return NXT_ERROR;
        }

        value = value * 10 + digit;
    }

    if (value > 65535) {
        return NXT_ERROR;
    }

    if (port != NULL) {
        *port = htons((in_port_t) value);
    }

    return NXT_OK;
}


/*
 * Version 2 is binary: a 12-byte signature, one byte of version and
 * command, one byte of address family and transport, a 16-bit big-endian
 * length, then "length" bytes of addresses and TLVs.  The length is the
 * peer's claim, so it is checked against the cap before any wait for the
 * rest of the header, and every address block and TLV is taken through a
 * span that cannot read past it.
 */

static nxt_int_t
nxt_proxy_protocol_v2_parse(const u_char *start, size_t size,
    nxt_proxy_protocol_t *pp)
{
    size_t        n, length, addr_length;
    nxt_span_t    span;
    nxt_uint_t    ver_cmd, family;
    const u_char  *p, *skip;

    n = nxt_min(size, sizeof(nxt_proxy_protocol_v2_signature));

    if (memcmp(start, nxt_proxy_protocol_v2_signature, n) != 0) {
        return NXT_ERROR;
    }

    if (size < NXT_PROXY_PROTOCOL_V2_HEADER_LENGTH) {
        return NXT_AGAIN;
    }

    p = start + sizeof(nxt_proxy_protocol_v2_signature);

    ver_cmd = p[0];
    family = p[1];
    length = ((size_t) p[2] << 8) | p[3];

    if ((ver_cmd & 0xF0) != 0x20) {
        return NXT_ERROR;
    }

    /* The 16-bit field cannot overflow a size_t, so a plain sum is safe. */

    if (NXT_PROXY_PROTOCOL_V2_HEADER_LENGTH + length
        > NXT_PROXY_PROTOCOL_MAX_LENGTH)
    {
        return NXT_ERROR;
    }

    pp->length = NXT_PROXY_PROTOCOL_V2_HEADER_LENGTH + length;

    switch (ver_cmd & 0x0F) {

    case NXT_PP2_CMD_LOCAL:
        /*
         * A health check or another connection of the proxy itself.  The
         * address block, whatever its family says, must be skipped, and
         * the connection keeps its real peer address.
         */
        if (size < pp->length) {
            return NXT_AGAIN;
        }

        pp->local = 1;

        return NXT_OK;

    case NXT_PP2_CMD_PROXY:
        break;

    default:
        return NXT_ERROR;
    }

    /*
     * Only TCP over IPv4 or IPv6 announces an address that a client can
     * have.  AF_UNSPEC, AF_UNIX and the datagram transports are refused:
     * an unknown address is not an address, and a lenient receiver would
     * silently fall back to the proxy's own.
     */

    switch (family) {

    case (NXT_PP2_FAMILY_INET << 4) | NXT_PP2_TRANSPORT_STREAM:
        pp->family = AF_INET;
        addr_length = NXT_PP2_INET_ADDR_LENGTH;
        break;

    case (NXT_PP2_FAMILY_INET6 << 4) | NXT_PP2_TRANSPORT_STREAM:
#if (NXT_INET6)
        pp->family = AF_INET6;
        addr_length = NXT_PP2_INET6_ADDR_LENGTH;
        break;
#else
        return NXT_ERROR;
#endif

    default:
        return NXT_ERROR;
    }

    if (length < addr_length) {
        return NXT_ERROR;
    }

    if (size < pp->length) {
        return NXT_AGAIN;
    }

    nxt_span_init(&span, start + NXT_PROXY_PROTOCOL_V2_HEADER_LENGTH,
                  start + pp->length);

    /* src_addr, dst_addr, src_port, dst_port. */

    n = (pp->family == AF_INET) ? 4 : 16;

    if (nxt_span_copy(&span, pp->address, n) != 0
        || nxt_span_take(&span, n, &skip) != 0
        || nxt_span_copy(&span, &pp->port, sizeof(in_port_t)) != 0
        || nxt_span_take(&span, sizeof(in_port_t), &skip) != 0)
    {
        return NXT_ERROR;
    }

    return nxt_proxy_protocol_v2_tlvs(&span, pp);
}


/*
 * The TLVs after the addresses: a type byte, a 16-bit big-endian length,
 * and the value.  They must fill the rest of the header exactly.  Only
 * PP2_TYPE_SSL is read, for its client bit; every other type is skipped
 * by its length.
 */

static nxt_int_t
nxt_proxy_protocol_v2_tlvs(nxt_span_t *span, nxt_proxy_protocol_t *pp)
{
    size_t        length;
    const u_char  *tlv, *value;

    while (nxt_span_len(span) != 0) {

        if (nxt_span_take(span, 3, &tlv) != 0) {
            return NXT_ERROR;
        }

        length = ((size_t) tlv[1] << 8) | tlv[2];

        if (nxt_span_take(span, length, &value) != 0) {
            return NXT_ERROR;
        }

        if (tlv[0] == NXT_PP2_TYPE_SSL) {
            /* pp2_tlv_ssl: client (1 byte), verify (4 bytes), sub-TLVs. */

            if (length < 5) {
                return NXT_ERROR;
            }

            if (value[0] & NXT_PP2_CLIENT_SSL) {
                pp->tls = 1;
            }
        }
    }

    return NXT_OK;
}


nxt_int_t
nxt_proxy_protocol_sockaddr_size(const nxt_proxy_protocol_t *pp,
    socklen_t *socklen, size_t *address_length)
{
    switch (pp->family) {

    case AF_INET:
        *socklen = sizeof(struct sockaddr_in);
        *address_length = NXT_INET_ADDR_STR_LEN;
        return NXT_OK;

#if (NXT_INET6)
    case AF_INET6:
        *socklen = sizeof(struct sockaddr_in6);
        *address_length = NXT_INET6_ADDR_STR_LEN;
        return NXT_OK;
#endif

    default:
        return NXT_ERROR;
    }
}


void
nxt_proxy_protocol_sockaddr_set(const nxt_proxy_protocol_t *pp,
    nxt_sockaddr_t *sa)
{
    sa->type = SOCK_STREAM;
    sa->u.sockaddr.sa_family = pp->family;

#if (NXT_INET6)
    if (pp->family == AF_INET6) {
        nxt_memcpy(&sa->u.sockaddr_in6.sin6_addr, pp->address,
                   sizeof(struct in6_addr));
        sa->u.sockaddr_in6.sin6_port = pp->port;
        return;
    }
#endif

    nxt_memcpy(&sa->u.sockaddr_in.sin_addr, pp->address, sizeof(in_addr_t));
    sa->u.sockaddr_in.sin_port = pp->port;
}
