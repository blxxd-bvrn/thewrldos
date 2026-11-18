/* Decompression for software packages (.deb, .rpm): gzip, xz and Zstandard.
 * src/pkg/unpack.c, built against the C library shim; plain C types only. */
#ifndef UNPACK_H
#define UNPACK_H
#include <stdint.h>

/* the compression a buffer starts with */
enum { UNPACK_NONE, UNPACK_GZIP, UNPACK_XZ, UNPACK_ZSTD, UNPACK_BZIP2, UNPACK_LZMA };
int unpack_kind(const uint8_t *src, uint32_t n);
const char *unpack_kind_name(int kind);
/* Decompress the whole of src (an uncompressed buffer is copied). Returns a buffer
 * from the kernel heap (kfree) with *outlen bytes, or NULL with a reason in err.
 * max_out bounds the result (memory). */
uint8_t *unpack_all(const uint8_t *src, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap);
/* give other tasks a turn if the current one has run a while (called during long decodes) */
void unpack_yield(void);

#endif
