/* MM2: no exception test vectors generated (gen_eh_tests.py uses SSBU offsets). The declarations keep
 * cxxrt_selftest.h compiling; the C++ runtime self-test is not part of the boot flow. */
typedef struct { uint64_t pc, pad; signed char kind_a, sel_a, kind_b, sel_b; } EhVec;   /* thrown A = std::out_of_range, B = std::ios_base::failure */
static const EhVec eh_vecs[] = {
  { 0, 0, 0, 0, 0, 0 },
};
typedef struct { uint64_t start, end, lsda; } EhFnVec;
static const EhFnVec eh_fn_vecs[] = {
  { 0, 0, 0 },
};
