/*
 * Copyright (C) FreeUnit Community
 */

#ifndef _NXT_PROXY_PROTOCOL_H_INCLUDED_
#define _NXT_PROXY_PROTOCOL_H_INCLUDED_


/*
 * The PROXY protocol header.  A proxy such as HAProxy or an L4 load
 * balancer sends it at the start of a connection to announce the address
 * of its own client.  The listener option "proxy_protocol" names the
 * version that the listener expects.  The versions are not auto-detected:
 * the configured version is the only one accepted, and a connection
 * without a valid header is dropped.
 */

#define NXT_PROXY_PROTOCOL_OFF         0
#define NXT_PROXY_PROTOCOL_V1          1
#define NXT_PROXY_PROTOCOL_V2          2

/*
 * The longest header that is accepted.  A v1 line is at most 107 bytes.
 * A v2 header declares its own length in a 16-bit field.  The field comes
 * from the peer, so it is not trusted: a header above this cap is rejected
 * as soon as the length is known, before the rest of it is read.
 */
#define NXT_PROXY_PROTOCOL_MAX_LENGTH  1024

/* The worst case of the specification: 107 bytes, CRLF included. */
#define NXT_PROXY_PROTOCOL_V1_MAX_LENGTH                                      \
    nxt_length("PROXY UNKNOWN ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff "       \
               "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff 65535 65535\r\n")

#define NXT_PROXY_PROTOCOL_V2_HEADER_LENGTH  16


typedef struct {
    /* The number of bytes that the header occupies. */
    size_t          length;

    /*
     * A v2 LOCAL command or a v1 UNKNOWN protocol.  The header carries no
     * client address, so the connection keeps its real peer address.
     */
    uint8_t         local;         /* 1 bit */

    /* A v2 PP2_TYPE_SSL TLV with PP2_CLIENT_SSL: the client spoke TLS. */
    uint8_t         tls;           /* 1 bit */

    /* AF_INET or AF_INET6.  Only set when "local" is 0. */
    uint8_t         family;

    /* The client port in network byte order. */
    in_port_t       port;

    /* The client address in network byte order, 4 or 16 bytes. */
    u_char          address[16];
} nxt_proxy_protocol_t;


/* Maps the option value "v1", "v2" or "off" to a NXT_PROXY_PROTOCOL_*. */
NXT_EXPORT nxt_uint_t nxt_proxy_protocol_version(const nxt_str_t *name);

/*
 * Parses the header at the start of a connection.  It returns NXT_OK and
 * fills "pp" when "size" bytes hold a whole valid header, NXT_AGAIN when
 * they are a valid prefix of a header that is below the cap, and NXT_ERROR
 * when they cannot start a valid header of the configured version.  On
 * NXT_ERROR, nothing in "pp" may be used.
 */
NXT_EXPORT nxt_int_t nxt_proxy_protocol_parse(nxt_uint_t version,
    const u_char *start, size_t size, nxt_proxy_protocol_t *pp);

/*
 * Fills the binary part of a sockaddr with the announced client address.
 * The caller allocates "sa" with the socklen and the text length that
 * nxt_proxy_protocol_sockaddr_size() returns; nxt_sockaddr_text() then
 * formats the text.  The size function returns NXT_ERROR for a family that
 * this build cannot hold.
 */
NXT_EXPORT nxt_int_t nxt_proxy_protocol_sockaddr_size(
    const nxt_proxy_protocol_t *pp, socklen_t *socklen,
    size_t *address_length);
NXT_EXPORT void nxt_proxy_protocol_sockaddr_set(const nxt_proxy_protocol_t *pp,
    nxt_sockaddr_t *sa);


#endif /* _NXT_PROXY_PROTOCOL_H_INCLUDED_ */
