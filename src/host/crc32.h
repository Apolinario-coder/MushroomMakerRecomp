/* ARM64 CRC32 instructions (crc32b/h/w/x, crc32cb/ch/cw/cx) as C.
 *
 * The checksum is the reflected (LSB-first) CRC-32 with polynomial 0xEDB88320 for the CRC32
 * family and 0x82F63B78 for the CRC32C family, and the operand's bytes are taken in little-endian
 * order (low byte first). The value in the register is the same value a table driven crc32_le
 * keeps while it runs, so the usual CRC-32 is the accumulator seeded with 0xFFFFFFFF and
 * complemented once at the end: that is exactly how the game's own CRC loop (main 0x14e5490) uses
 * it. The bit-by-bit form is used because the instructions are rare in this game (one unrolled
 * loop) and it needs no table; each byte costs eight shifts. */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

static inline uint32_t crc32_step1(uint32_t acc, uint32_t byte, uint32_t poly) {
    acc ^= byte & 0xffu;
    for (int i = 0; i < 8; i++) acc = (acc >> 1) ^ (poly & (uint32_t)(0u - (acc & 1u)));
    return acc;
}

static inline uint32_t crc32bhwx(uint32_t acc, uint64_t v, int width, uint32_t poly) {
    for (int i = 0; i < width; i++) acc = crc32_step1(acc, (uint32_t)(v >> (8 * i)), poly);
    return acc;
}

static inline uint32_t crc32b(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 1, 0xedb88320u); }
static inline uint32_t crc32h(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 2, 0xedb88320u); }
static inline uint32_t crc32w(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 4, 0xedb88320u); }
static inline uint32_t crc32x(uint32_t acc, uint64_t v) { return crc32bhwx(acc, v, 8, 0xedb88320u); }

static inline uint32_t crc32cb(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 1, 0x82f63b78u); }
static inline uint32_t crc32ch(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 2, 0x82f63b78u); }
static inline uint32_t crc32cw(uint32_t acc, uint32_t v) { return crc32bhwx(acc, v, 4, 0x82f63b78u); }
static inline uint32_t crc32cx(uint32_t acc, uint64_t v) { return crc32bhwx(acc, v, 8, 0x82f63b78u); }

#endif
