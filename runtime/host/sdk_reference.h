/* MM2: the SDK self-test's reference data (Mii defaults from the suyu sources, zlib/image vectors) is
 * not available; empty sets keep nnmii.c / nnsdk2.c compiling. The self-tests are not part of the boot flow. */
#define MII_DEFAULT_COUNT 6
static const uint32_t mii_default_raw[6][49] = {{0}};
static const uint16_t mii_default_nick[6][10] = {{0}};
#define MII_ACCESSOR_COUNT 1
static const char *const mii_accessor_names[1] = {""};
static const uint8_t mii_accessor_expect[6][1] = {{0}};
static const struct { const char *name; const uint8_t *plain; unsigned plain_size; const uint8_t *comp; unsigned comp_size; } zvecs[] = {
  { 0, 0, 0, 0, 0 },
};
static const struct { const char *name; const uint8_t *data; unsigned size; const uint8_t *rgb; int width, height; } jvecs[] = {
  { 0, 0, 0, 0, 0, 0 },
};
static const uint8_t zvec_corrupt[1] = { 0 };
#define ZVEC_CORRUPT_PLAIN_SIZE 1
