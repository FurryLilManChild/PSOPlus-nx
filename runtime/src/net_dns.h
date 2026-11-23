/* A minimal DNS client codec (RFC 1035): one A query, one answer. Pure functions, no sockets, so the
 * host test (tools/tests/net_native_test.c) checks them against canned packets. Used by net_native.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* builds a standard query (recursion desired, one question: name, type A, class IN) into out.
 * Returns its length, or 0 when the name is empty, has an empty label or a label over 63 bytes, or out is
 * too small. A trailing dot is allowed. */
size_t dns_build_query(uint8_t* out, size_t cap, uint16_t id, const char* name);

enum {
    DNS_OK = 0,          /* ip[] holds the first A record of the answer section */
    DNS_NOT_ANSWER = 1,  /* not a response to this query (other id, not a response, other question count) */
    DNS_NXDOMAIN = 2,    /* RCODE 3: the name does not exist */
    DNS_SERVFAIL = 3,    /* any other nonzero RCODE (code in *rcode) */
    DNS_NO_A = 4,        /* a valid answer without an A record (CNAME only, or empty) */
    DNS_MALFORMED = 5,   /* truncated or bad name compression */
};
/* parses a response to the query with this id; *rcode gets the RCODE when the header was read */
int dns_parse_answer(const uint8_t* p, size_t n, uint16_t id, uint8_t ip[4], int* rcode);

/* "a.b.c.d" or "a.b.c.d:port" (port 1-65535): 1 and the address in network order, else 0 */
int dns_parse_ipv4(const char* s, uint8_t ip[4], uint16_t* port);

#ifdef __cplusplus
}
#endif
