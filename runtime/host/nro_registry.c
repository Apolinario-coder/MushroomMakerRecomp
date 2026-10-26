/* MM2 has no run-time script modules (no nn::ro / nn::lr imports): an empty registry.
 * Same shape build_nro.py writes for the SSBU build (see nnro.c). */
#include <stdint.h>
typedef struct { const char *name; uint64_t base; uint32_t image_size, file_size; uint8_t build_id[20]; } NroInfo;
const NroInfo nro_registry[] = {
  { 0, 0, 0, 0, { 0 } },
};
const int nro_registry_count = 0;
