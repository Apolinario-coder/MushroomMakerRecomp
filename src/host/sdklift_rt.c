/* Loader and glue for the code lifted out of the real SDK module (sdklift_gen.c, made by sdklift.py).
 *
 * The SDK image (binaries\13.0.4\sdk.elf, the user's own dump converted with nso2elf) is mapped at
 * SDK_BASE with its three PT_LOAD segments and relocated like the game image, so the lifted functions
 * find the SDK's own tables and globals at the addresses their code computes. The SDK's static
 * constructors are run only where explicitly needed (audio); no SDK service session exists. Functions that are self-contained
 * (accessors, table getters, codecs, struct setters) are lifted; whatever needs a system service is
 * answered by hand in the nn*.c files.
 *
 * Relocations ([sdk] .rela.dyn 0x541010, .rela.plt 0x5ce118): R_AARCH64_RELATIVE (0x403),
 * GLOB_DAT (0x401), JUMP_SLOT (0x402), ABS64 (0x101). The 13 undefined symbols (malloc, free, calloc,
 * realloc, aligned_alloc, nnMain, ...) are left 0: lifted code never calls through the GOT, sdklift.py
 * binds PLT stubs statically (to lifted code or to the host handler of the same name). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

static int g_ready;
static uint8_t *g_file;                   /* sdk.elf as read from disk */
static size_t g_file_size;
static uint64_t g_symtab, g_strtab, g_nsym;
static ImportFn *g_host_fn;               /* per sdk_host_names entry */

int sdk_image_ready(void) { return g_ready; }

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("\nsdklift: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    report_and_exit(3);
}

static int load_image(void) {
    char path[1024];
    const char *env = getenv("SSBU_SDK_ELF");
    const char *g_elf = __argc > 1 ? __argv[1] : NULL;       /* same argument boot.c uses for main.elf */
    if (env && env[0]) snprintf(path, sizeof path, "%s", env);
    else {
        snprintf(path, sizeof path, "%s", g_elf ? g_elf : "..\\binaries\\mm2\\main.elf");
        char *slash = strrchr(path, '\\'), *slash2 = strrchr(path, '/');
        if (slash2 > slash) slash = slash2;
        snprintf(slash ? slash + 1 : path, 16, "sdk.elf");
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("sdklift: cannot open %s (set SSBU_SDK_ELF): lifted SDK functions are unavailable\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    g_file_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = malloc(g_file_size);
    if (fread(g_file, 1, g_file_size, f) != g_file_size || memcmp(g_file, "\x7f" "ELF", 4)) { fclose(f); printf("sdklift: %s is not readable as an ELF\n", path); return 0; }
    fclose(f);
    uint8_t *img = VirtualAlloc((void *)(uintptr_t)SDK_BASE, SDK_IMAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (img != (uint8_t *)(uintptr_t)SDK_BASE) { printf("sdklift: cannot map the SDK image at %llx\n", (unsigned long long)SDK_BASE); return 0; }
    uint64_t phoff = *(uint64_t *)(g_file + 0x20);
    int phnum = *(uint16_t *)(g_file + 0x38);
    uint64_t dynoff = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = g_file + phoff + (uint64_t)i * 56;
        uint32_t type = *(uint32_t *)ph;
        uint64_t off = *(uint64_t *)(ph + 8), va = *(uint64_t *)(ph + 16), filesz = *(uint64_t *)(ph + 32), memsz = *(uint64_t *)(ph + 40);
        if (type == 1) {
            if (va + memsz > SDK_IMAGE_SIZE || off + filesz > g_file_size) die("segment %d of sdk.elf does not fit", i);
            memcpy(img + va, g_file + off, filesz);
        } else if (type == 2) dynoff = va;
    }
    if (!dynoff) die("sdk.elf has no dynamic section");
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, hash = 0;
    for (uint64_t *dp = (uint64_t *)(img + dynoff); dp[0]; dp += 2) {
        switch (dp[0]) {
        case 7: rela = dp[1]; break;
        case 8: relasz = dp[1]; break;
        case 23: jmprel = dp[1]; break;
        case 2: pltrelsz = dp[1]; break;
        case 6: g_symtab = dp[1]; break;
        case 5: g_strtab = dp[1]; break;
        case 4: hash = dp[1]; break;
        }
    }
    g_nsym = hash ? *(uint32_t *)(img + hash + 4) : 0;       /* DT_HASH nchain = number of symbols */
    if (!g_nsym) die("sdk.elf: no DT_HASH");
    uint64_t tables[2][2] = {{rela, relasz}, {jmprel, pltrelsz}};
    long counts[4] = {0, 0, 0, 0};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < tables[t][1] / 24; i++) {
            uint64_t *r = (uint64_t *)(img + tables[t][0] + i * 24);
            uint64_t off = r[0], info = r[1], addend = r[2];
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (off + 8 > SDK_IMAGE_SIZE) die("relocation outside the image");
            if (type == 0x403) { wr64(SDK_BASE + off, SDK_BASE + addend); counts[0]++; continue; }
            if (type != 0x401 && type != 0x402 && type != 0x101) die("unknown relocation type %x", type);
            uint8_t *s = img + g_symtab + (uint64_t)sym * 24;
            uint16_t shndx = *(uint16_t *)(s + 6);
            uint64_t value = *(uint64_t *)(s + 8);
            if (shndx) { wr64(SDK_BASE + off, SDK_BASE + value + (type == 0x101 ? addend : 0)); counts[1]++; }
            else { wr64(SDK_BASE + off, 0); counts[2]++; }
        }
    trace_printf("sdklift: SDK image mapped at %llx (%lu relative, %lu symbolic, %lu undefined relocations)\n",
                 (unsigned long long)SDK_BASE, counts[0], counts[1], counts[2]);
    return 1;
}

uint64_t sdk_symbol(const char *name) {
    if (!g_ready) return 0;
    uint8_t *img = (uint8_t *)(uintptr_t)SDK_BASE;
    for (uint64_t i = 0; i < g_nsym; i++) {
        uint8_t *s = img + g_symtab + i * 24;
        if (*(uint16_t *)(s + 6) && strcmp((const char *)img + g_strtab + *(uint32_t *)s, name) == 0) return SDK_BASE + *(uint64_t *)(s + 8);
    }
    return 0;
}

static const SdkFn *find_fn(uint64_t addr) {
    int lo = 0, hi = sdk_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sdk_fns[mid].addr == addr) return &sdk_fns[mid];
        if (sdk_fns[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

ImportFn sdk_lifted(const char *name) {
    if (!g_ready) return NULL;
    uint64_t a = sdk_symbol(name);
    const SdkFn *f = a ? find_fn(a) : NULL;
    return f ? f->fn : NULL;
}

/* Name of the SDK symbol that contains `addr`, for diagnostics. */
static const char *sdk_name_of(uint64_t addr, uint64_t *offset) {
    uint8_t *img = (uint8_t *)(uintptr_t)SDK_BASE;
    const char *best = "?";
    uint64_t best_off = ~0ull;
    for (uint64_t i = 0; i < g_nsym; i++) {
        uint8_t *s = img + g_symtab + i * 24;
        uint64_t v = SDK_BASE + *(uint64_t *)(s + 8);
        if (*(uint16_t *)(s + 6) && v <= addr && addr - v < best_off) { best_off = addr - v; best = (const char *)img + g_strtab + *(uint32_t *)s; }
    }
    *offset = best_off;
    return best;
}

void sdk_dispatch(Ctx *c, uint64_t addr) {
    if (addr >= SDK_BASE && addr < SDK_BASE + SDK_IMAGE_SIZE) {
        const SdkFn *f = find_fn(addr);
        if (!f) {
            uint64_t off;
            const char *n = sdk_name_of(addr, &off);
            printf("\nsdklift: lifted SDK code (f_%llx) calls SDK address %llx (%s+0x%llx) through a pointer, and that function was not lifted.\n"
                   "Add `addr %llx` to sdklift_roots.txt and run `python sdklift.py gen`.\n",
                   (unsigned long long)c->fn, (unsigned long long)(addr - SDK_BASE), n, (unsigned long long)off, (unsigned long long)(addr - SDK_BASE));
            guest_trap("indirect call to an SDK function that was not lifted", addr);
        }
        f->fn(c);
        return;
    }
    call_indirect(c, addr);
}

/* Host handlers for SDK-internal callees that are cut (`cut <name>` in sdklift_roots.txt) and are not
 * imports of the game: a service call inside otherwise pure SDK code. */
static struct { const char *name; ImportFn fn; } g_override[32];
static int g_override_count;
void sdk_host_override(const char *name, ImportFn fn) {
    if (g_override_count < 32) { g_override[g_override_count].name = name; g_override[g_override_count].fn = fn; g_override_count++; }
    for (int i = 0; g_host_fn && i < sdk_host_name_count; i++) if (strcmp(sdk_host_names[i], name) == 0) g_host_fn[i] = fn;
}

/* Address of a function the game itself defines (the SDK module imports malloc, free, calloc, realloc and
 * aligned_alloc from the application). */
static uint64_t game_symbol(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return GUEST_BASE + dyn_syms[i].value;
    return 0;
}

void sdk_host_call(Ctx *c, int index) {
    ImportFn f = g_host_fn[index];
    if (!f) {
        const char *name = sdk_host_names[index];
        for (int i = 0; i < g_override_count && !f; i++) if (strcmp(g_override[i].name, name) == 0) f = g_override[i].fn;
        if (!f) f = import_lookup(name);
        if (!f) {                                           /* malloc & co: functions the GAME defines and the SDK imports */
            uint64_t g = game_symbol(name);
            if (g) { call_indirect(c, g); return; }
        }
        if (!f) {
            printf("\nsdklift: lifted SDK code (f_%llx) calls %s, which has no host handler.\n", (unsigned long long)c->fn, name);
            guest_trap("lifted SDK code called a function without a host handler", c->x[30] - 4);
        }
        g_host_fn[index] = f;
    }
    f(c);
}

void sdk_trap_call(Ctx *c, const char *name) {
    printf("\nsdklift: lifted SDK code (f_%llx, called from guest %llx) reached %s: an abort path or a cut service boundary.\n",
           (unsigned long long)c->fn, (unsigned long long)c->x[30] - 4, name);
    trace_printf("sdklift: lifted SDK code f_%llx reached %s\n", (unsigned long long)c->fn, name);
    guest_trap("lifted SDK code reached an abort path", c->x[30] - 4);
}

/* ---------------------------------------------------------------- setjmp / longjmp (see sdklift.h) */
#define SDK_JMP_SLOTS 8
static _Thread_local SdkJmpRec *t_jmp;
static _Thread_local unsigned long long t_jmp_stamp;

SdkJmpRec *sdk_jmp_record(Ctx *c) {
    if (!t_jmp) t_jmp = _aligned_malloc(sizeof(SdkJmpRec) * SDK_JMP_SLOTS, 16), memset(t_jmp, 0, sizeof(SdkJmpRec) * SDK_JMP_SLOTS);
    uint64_t key = c->x[0];
    SdkJmpRec *r = NULL;
    for (int i = 0; i < SDK_JMP_SLOTS; i++) if (t_jmp[i].key == key) r = &t_jmp[i];
    if (!r) {                                               /* the least recently set record is the one to reuse */
        r = &t_jmp[0];
        for (int i = 1; i < SDK_JMP_SLOTS; i++) if (t_jmp[i].stamp < r->stamp) r = &t_jmp[i];
    }
    r->key = key;
    r->stamp = ++t_jmp_stamp;
    for (int i = 0; i < 12; i++) r->x[i] = c->x[19 + i];
    r->x[12] = c->x[31];
    for (int i = 0; i < 8; i++) r->d[i] = c->v[8 + i].d[0];
    return r;
}
void sdk_jmp_restore(Ctx *c, SdkJmpRec *r) {
    for (int i = 0; i < 12; i++) c->x[19 + i] = r->x[i];
    c->x[31] = r->x[12];
    for (int i = 0; i < 8; i++) { c->v[8 + i].d[0] = r->d[i]; c->v[8 + i].d[1] = 0; }
    c->x[0] = (uint64_t)(r->value ? r->value : 1);
}
void sdk_longjmp(Ctx *c) {
    SdkJmpRec *r = NULL;
    for (int i = 0; t_jmp && i < SDK_JMP_SLOTS; i++) if (t_jmp[i].key == c->x[0] && t_jmp[i].stamp) r = &t_jmp[i];
    if (!r) {
        printf("\nsdklift: longjmp to a jmp_buf (%llx) no lifted setjmp filled on this thread\n", (unsigned long long)c->x[0]);
        guest_trap("lifted SDK longjmp without setjmp", c->x[30] - 4);
    }
    r->value = (int)c->x[1];
    longjmp(r->host, 1);
}

/* ---------------------------------------------------------------- registration
 * Every import of the game that still has no handler and whose SDK implementation was lifted gets the
 * lifted code as its handler (looked up by symbol, so aliases such as C1/C2 work). Handlers that
 * already exist are only replaced where an nn*.c file asks for it with sdk_install(). */
int sdk_install(const char *name) {
    ImportFn f = sdk_lifted(name);
    if (!f || !import_data(name)) return 0;
    ImportDef d = {name, f};
    register_imports(&d, 1);
    return 1;
}

int sdk_missing_host_callees(FILE *out) {
    int missing = 0;
    for (int i = 0; i < sdk_host_name_count; i++) {
        int found = import_lookup(sdk_host_names[i]) != NULL || game_symbol(sdk_host_names[i]) != 0;
        for (int k = 0; k < g_override_count; k++) if (strcmp(g_override[k].name, sdk_host_names[i]) == 0) found = 1;
        if (!found) { if (out) fprintf(out, "  lifted SDK code needs a host handler for %s\n", sdk_host_names[i]); missing++; }
    }
    return missing;
}

void register_sdklift_imports(void) {
    g_host_fn = calloc((size_t)sdk_host_name_count + 1, sizeof *g_host_fn);
    g_ready = load_image();
    if (!g_ready) return;
    {   /* close5 (c5b): the SDK's .eh_frame_hdr (through its MOD0 header at +8) is given to the exception runtime, so a
         * throw that starts in lifted SDK code (libc++'s __throw_length_error ...) is walked through the SDK's frames with
         * their own CFI up to the game's frames. Clean-up pads of SDK frames have no resume entry: nnunwind.c skips them. */
        int eh_register_image(uint64_t base, uint64_t size, uint64_t eh_frame_hdr);
        if (!eh_register_image(SDK_BASE, SDK_IMAGE_SIZE, 0)) trace_printf("sdklift: the exception tables of the SDK image were not found\n");
    }
    int installed = 0;
    for (int i = 0; i < dyn_sym_count; i++) {
        const char *name = dyn_syms[i].name;
        if (dyn_syms[i].defined || !name[0] || import_lookup(name)) continue;
        if (strncmp(name, "_ZN2nn", 6) && strncmp(name, "_ZNK2nn", 7) && strncmp(name, "nn", 2)) continue;
        installed += sdk_install(name);
    }
    trace_printf("sdklift: %d SDK functions lifted, %d installed as import handlers, %d host callees\n", sdk_fn_count, installed, sdk_host_name_count);
}

/* ------------------------------------------- state the SDK's own constructors would have built
 * nn::os::detail::g_OsResourceManagerStorage is a bss object of the SDK module, and the SDK's own
 * constructor (OsResourceManager::OsResourceManager, reached from nnosInitialize) is what makes its
 * members usable. This port does not run the SDK's OS constructor: every nn::os call the game makes is answered by a
 * C handler that keeps its own state, so that object is normally never read. It is read by the one
 * lifted nn::os path the C side does not replace, nn::os::SetMemoryHeapSize -> MemoryHeapManager: the
 * lifted SetHeapSize walks the manager's free list (an IntrusiveList whose sentinel node sits at +0x18,
 * next at +0x20) before it writes anything, so an all-zero manager sends it to a null node. The
 * constructor links an empty list to itself; do the same, and leave the rest of the object zero, which
 * is what the handlers that use a real heap (AllocateMemoryBlock/FreeMemoryBlock) expect to never see. */
void sdk_os_state_init(void) {
    uint64_t storage = sdk_symbol("_ZN2nn2os6detail26g_OsResourceManagerStorageE");
    if (!storage) { fprintf(stderr, "sdk_os_state_init: g_OsResourceManagerStorage is not in the SDK image\n"); return; }
    uint64_t heap = storage + 0x370;                  /* nn::os::detail::MemoryHeapManager inside it */
    if (rd64(heap + 0x20) == 0) {                     /* sentinel node's next: 0 = never linked */
        wr64(heap + 0x18, heap + 0x18);               /* sentinel.prev = sentinel: empty list */
        wr64(heap + 0x20, heap + 0x18);
        trace_printf("sdk_os_state_init: heap manager at %llx, empty free list linked to %llx\n",
                     (unsigned long long)heap, (unsigned long long)(heap + 0x18));
    }
}

/* MM2 SDK .init_array at 0xc13768 constructs the local software renderer and its internal heap. */
#define SDK_AUDIO_MANAGER_SLOT (SDK_BASE + 0xc114c8ULL)
#define SDK_AUDIO_LIST (SDK_BASE + 0xc370d0ULL)
void sdk_audio_state_init(Ctx *c) {
    if (!sdk_image_ready()) die("audio initialization needs the SDK image");
    /* .init_array entry 0xc13758 initializes the audio lists, mutex and events before the CPU manager. */
    if (!rd64(SDK_AUDIO_LIST)) {
        sdk_dispatch(c, SDK_BASE + 0x9de80ULL);
        if (rd64(SDK_AUDIO_LIST) != SDK_AUDIO_LIST + 0x50 || rd64(SDK_AUDIO_LIST + 8) != SDK_AUDIO_LIST + 0x10)
            die("audio list constructor failed");
        trace_printf("sdklift: original audio list constructor completed\n");
    }
    uint64_t manager = rd64(SDK_AUDIO_MANAGER_SLOT);
    if (!manager) die("software audio manager is missing from the SDK image");
    if (rd64(manager)) return;
    sdk_dispatch(c, SDK_BASE + 0xaef50ULL);
    if (!rd64(manager) || !rd64(manager + 0x1840)) die("software audio manager constructor failed");
    trace_printf("sdklift: original software audio constructor completed\n");
}

int sdk_audio_heap_selftest(Ctx *c) {
    uint64_t manager = rd64(SDK_AUDIO_MANAGER_SLOT), heap = rd64(manager + 0x1840), blocks[2];
    sdk_audio_state_init(c);
    if (!heap || rd64(manager + 0x1840) != heap) return 1;
    if (rd64(SDK_AUDIO_LIST) != SDK_AUDIO_LIST + 0x50 || rd64(SDK_AUDIO_LIST + 8) != SDK_AUDIO_LIST + 0x10) return 1;
    puts("SDK audio lists: original constructor and idempotent initialization OK");
    ImportFn alloc = sdk_lifted("_ZN2nn4lmem19AllocateFromExpHeapEPNS0_6detail8HeapHeadEm");
    ImportFn free_block = sdk_lifted("_ZN2nn4lmem13FreeToExpHeapEPNS0_6detail8HeapHeadEPv");
    if (!alloc || !free_block) return 1;
    for (int i = 0; i < 2; i++) {
        c->x[0] = heap; c->x[1] = 32; alloc(c); blocks[i] = c->x[0];
        if (blocks[i] < manager + 0x40 || blocks[i] + 32 > manager + 0x1840 || (blocks[i] & 7)) return 1;
        memset((void *)(uintptr_t)blocks[i], 0xa0 + i, 32);
    }
    if (!(blocks[0] + 32 <= blocks[1] || blocks[1] + 32 <= blocks[0])) return 1;
    c->x[0] = heap; c->x[1] = 0x8000; alloc(c);
    if (c->x[0] || rd8(blocks[0]) != 0xa0 || rd8(blocks[1] + 31) != 0xa1) return 1;
    for (int i = 0; i < 2; i++) { c->x[0] = heap; c->x[1] = blocks[i]; free_block(c); }
    c->x[0] = heap; c->x[1] = 0x1000; alloc(c);
    if (!c->x[0]) return 1;
    c->x[1] = c->x[0]; c->x[0] = heap; free_block(c);
    for (int i = 0; i < 9; i++) if (!find_fn(rd64(rd64(manager) + i * 8))) return 1;
    for (int i = 0; i < 4; i++) if (!find_fn(rd64(rd64(manager + 0x10) + i * 8))) return 1;
    for (int i = 0; i < 16; i++) if (!find_fn(rd64(SDK_BASE + 0xbccbe8ULL + i * 8))) return 1;
    {
        uint8_t names[0x302], active[0x100];
        ImportFn list = import_lookup("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi");
        ImportFn get_active = import_lookup("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE");
        ImportFn channels = import_lookup("_ZN2nn5audio21GetActiveChannelCountEv");
        if (!list || !get_active || !channels) return 1;
        channels(c); if (c->x[0] != 2) return 1;
        memset(names, 0xa5, sizeof names);
        c->x[0] = (uint64_t)(uintptr_t)(names + 1); c->x[1] = 0; list(c);
        if (c->x[0] || names[1] != 0xa5) return 1;
        c->x[0] = 0; c->x[1] = (uint32_t)-1; list(c);
        if (c->x[0]) return 1;
        c->x[0] = (uint64_t)(uintptr_t)(names + 1); c->x[1] = 3; list(c);
        if (c->x[0] != 1 || names[0] != 0xa5 || names[0x100] != 0) return 1;
        for (int i = 0x101; i < sizeof names; i++) if (names[i] != 0xa5) return 1;
        c->x[0] = (uint64_t)(uintptr_t)active; get_active(c);
        if (!active[0] || memcmp(active, names + 1, sizeof active)) return 1;
        puts("Native audio device: bounded enumeration and active route agree");
    }
    puts("SDK audio heap: original constructor, allocation, bounds, exhaustion and free OK");
    puts("SDK audio manager: all recompiled virtual methods present");
    {
        uint64_t param[8] = {0};
        ImportFn init = import_lookup("_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE");
        ImportFn size = sdk_lifted("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
        if (!init || !size) return 1;
        c->x[0] = (uint64_t)(uintptr_t)param; init(c);
        if (rd32((uint64_t)(uintptr_t)param + 0x2c) != 1 || rd32((uint64_t)(uintptr_t)param + 0x30) != 1) return 1;
        ImportFn valid = sdk_lifted("_ZN2nn5audio29IsValidAudioRendererParameterERKNS0_22AudioRendererParameterE");
        if (!valid) return 1;
        c->x[0] = (uint64_t)(uintptr_t)param; valid(c);
        if (c->x[0] != 1) return 1;
        wr32((uint64_t)(uintptr_t)param + 0x30, 0);
        c->x[0] = (uint64_t)(uintptr_t)param; valid(c);
        if (c->x[0]) return 1;
        wr32((uint64_t)(uintptr_t)param + 0x30, 1);
        puts("SDK CPU audio parameters: original validator accepts the backend and execution mode");
        c->x[0] = (uint64_t)(uintptr_t)param; size(c);
        printf("SDK software audio work buffer: %llu bytes\n", (unsigned long long)c->x[0]);
        if (!c->x[0] || c->x[0] > (1ULL << 30)) return 1;
    }
    return 0;
}

/* NIST SP 800-38A F.2: fixed public test vectors, no project keys involved. */
int sdk_aes_selftest(Ctx *c) {
    const char *hex[] = {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "000102030405060708090a0b0c0d0e0f",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"
    };
    uint8_t vectors[4][64] = {{0}};
    for (int v = 0; v < 4; v++)
        for (int i = 0; i < (v < 2 ? 16 : 64); i++)
            if (sscanf(hex[v] + i * 2, "%2hhx", &vectors[v][i]) != 1) return 1;
    ImportFn functions[] = {
        sdk_lifted("_ZN2nn6crypto16EncryptAes128CbcEPvmPKvmS3_mS3_m"),
        sdk_lifted("_ZN2nn6crypto16DecryptAes128CbcEPvmPKvmS3_mS3_m")
    };
    if (!functions[0] || !functions[1]) return 1;
    for (int decrypt = 0; decrypt < 2; decrypt++)
        for (int blocks = 1; blocks <= 4; blocks += 3)
            for (int inplace = 0; inplace < 2; inplace++) {
                uint8_t output[66], input[66], key[16], iv[16];
                int n = blocks * 16;
                memset(output, 0xa5, sizeof output); memset(input, 0x5a, sizeof input);
                memcpy(input + 1, vectors[decrypt ? 3 : 2], n);
                memcpy(key, vectors[0], 16); memcpy(iv, vectors[1], 16);
                uint8_t *dest = inplace ? input + 1 : output + 1;
                c->x[0] = (uint64_t)(uintptr_t)dest; c->x[1] = n;
                c->x[2] = (uint64_t)(uintptr_t)key; c->x[3] = 16;
                c->x[4] = (uint64_t)(uintptr_t)iv; c->x[5] = 16;
                c->x[6] = (uint64_t)(uintptr_t)(input + 1); c->x[7] = n;
                functions[decrypt](c);
                if (c->x[0] != (uint64_t)n || memcmp(dest, vectors[decrypt ? 2 : 3], n)) return 1;
                if (memcmp(key, vectors[0], 16) || memcmp(iv, vectors[1], 16)) return 1;
                for (int i = 0; i < sizeof input; i++) {
                    if ((i == 0 || i > n) && input[i] != 0x5a) return 1;
                    if ((i == 0 || i > n || inplace) && output[i] != 0xa5) return 1;
                }
                if (!inplace && memcmp(input + 1, vectors[decrypt ? 3 : 2], n)) return 1;
            }
    puts("SDK AES-CBC: NIST encryption/decryption, one/four blocks, unaligned buffers and in-place bounds OK");
    return 0;
}
