// musl compatibility shim. musl (Alpine) does not define htonll/ntohll (they are
// nonstandard BSD/glibc extensions), but Ableton Link's byte-stream serializer
// uses them. Provide them here; force-included before Link on the musl build.
#ifndef ALOOP_MUSL_COMPAT_H
#define ALOOP_MUSL_COMPAT_H
#include <stdint.h>
#include <arpa/inet.h>
#if !defined(htonll)
static inline uint64_t htonll(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(x & 0xFFFFFFFF)) << 32) | htonl((uint32_t)(x >> 32));
#else
    return x;
#endif
}
#endif
#if !defined(ntohll)
static inline uint64_t ntohll(uint64_t x) { return htonll(x); }
#endif
#endif
