/* newmodule_host.c - N5 (plan\phases\N5_own_module.md): the fighter registry and SCRIPT MODULES THAT EXIST ONLY AS OUR DLL.
 * Included at the end of nnro.c (it uses that file's module table). OPT-IN: without SSBU_CHARACTERS=<folder> nothing
 * of this file runs and no hook is installed. Contains nothing of the game.
 *
 * 1. The registry. <folder>\<name>\fighter.json of every sub-folder = one new fighter (name, donor, dll, kind).
 *    Kinds are given in name order from 0x5E. The fighters' DLLs are staged into registry_bin\fighters\ (unless
 *    SSBU_FIGHTER_BIN is set: then that folder is used as it is) and SSBU_MODS defaults to registry_bin, the folder of
 *    the registry's components (kind tables, roster rows: still DLLs built with hook.h; they get the list through
 *    ssbu_registry(), fighter_sdk\include\registry.h).
 *
 * 2. A module without an NRO. The game's script module worker (main 0x71022b6620) formats
 *    "prebuilt:/nro/<build>/lua2cpp_<name>.nro" (call at 0x71022b67d0, return address 0x71022b67d4; x4 = the name,
 *    x25 = the request object, callee-saved), hashes the string, looks it up in the archive and, when it is there,
 *    loads the file and calls nn::ro::LoadModule; then request + 0x132 = 1 ("loaded"). A name the archive does not
 *    have falls through silently and the requester waits for ever (N4, run r4).
 *    The hook on the formatter recognises that call site by its return address (N4's "callee hook + return address")
 *    and answers a registered name itself: the module object is a NroModule of OURS with an address range of its own
 *    behind the registry's modules (reserved, never committed: the addresses are names of functions, not memory):
 *      - function at (range + offset) = the function of the fighter's DLL bound at that offset (fighter_host.c's
 *        dispatch, S10's synthetic addresses), else the DONOR module's function at the same offset;
 *      - the donor's module image is a dependency (its class tables and constants are data of that image): when it
 *        is not loaded the request is turned into a request for the donor's module, which the game then loads by
 *        its own path; a request for a module that is already loaded is answered "loaded".
 *    An agent the donor's factory makes for a new fighter is moved into the new module (agent_end): its class table
 *    is replaced by a copy whose donor-module entries are the new module's addresses, and every script it registers
 *    (lib::L2CAgent::sv_set_function_hash, lua2cpp::L2CAgentBase::sv_set_status_func: hooked by name) is registered
 *    under the new module's address. From then on the engine calls lua2cpp_<name> addresses for that agent.
 *    What still is the donor's: the factories themselves, the data of the image, and every function the DLL does
 *    not have as source (docs\NEW_FIGHTER.md).
 *
 * 3. Paths under the new name. The same formatter builds the engine's file paths. A formatted string that contains
 *    a new fighter's name as a path token is logged once ("[registry] path asked") and, as long as stage 2 of the
 *    loose-file layer has no row for it, rewritten to the donor's name, so the archive's file answers. */
#include <stdarg.h>
#include "include/registry.h"

#define NM_MAX 8
#define NM_VT_SLOTS 96
#define NM_WORKER_RET 0x71022b67d4ULL
#define NM_FORMATTER 0x7101ea04d0ULL

typedef struct NewMod {
    SsbuRegFighter f;
    char donor_module[48], module[48];
    NroInfo info;
    NroModule mod;                 /* what module_at() returns for the new range */
    NroModule *dm;                 /* the donor's module */
    FnEntry *fns; uint64_t *bind_addr;
    int active;
    uint64_t vt_src[64], vt_dst[64]; int vt_count;
    long requests, answered, substituted, agents, remapped, vt_clones, paths, paths_missing, paths_own;
    char display[64];              /* N9: the manifest's "display_name": the fighter's name text on the screens ("" = the donor's) */
    uint16_t display16[64];
    long nameid_calls, text_calls;
    uint64_t *cat_hash;            /* N12: hash of every DLL function name (script category of a registration) */
    long own_scripts, own_script_agents, frame_calls;
    int frame_at;                  /* N12: 0 = not looked up, -1 = none, else ovr index + 1 of <name>_frame */
} NewMod;
static NewMod g_nm[NM_MAX];
static int g_nm_count, g_nm_started;
static SsbuRegFighter g_nm_list[NM_MAX];
static SRWLOCK g_nm_lock = SRWLOCK_INIT;
static _Thread_local NewMod *t_nm_current;
static _Thread_local int t_nm_weapon;              /* N12: the agent being made is an article's */
static _Thread_local uint64_t t_nm_cat_agent;      /* N12: the agent whose own scripts were registered */
static struct { uint64_t agent; NewMod *nm; } g_nm_agent[2048];
static ImportFn g_nm_o_fmt, g_nm_o_fnhash, g_nm_o_status;

static void nm_log(const char *fmt, ...) {
    char line[700]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("%s\n", line);
    if (g_nm_started) trace_printf("%s\n", line);      /* the manifests are read from a constructor: before that only the log */
}

/* ---------------------------------------------------------------- manifests */
static int nm_json_str(const char *js, const char *key, char *out, size_t n) {
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') p++;
    size_t k = 0;
    if (*p == '"') { for (p++; *p && *p != '"' && k + 1 < n; p++) out[k++] = *p; }
    else for (; *p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && *p != ' ' && k + 1 < n; p++) out[k++] = *p;
    out[k] = 0;
    return 1;
}
static int nm_cmp_fighter(const void *a, const void *b) { return strcmp(((const NewMod *)a)->f.name, ((const NewMod *)b)->f.name); }

static void nm_scan(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *root = getenv("SSBU_CHARACTERS");
    if (!root || !root[0]) return;
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { nm_log("[registry] SSBU_CHARACTERS=%s: no such folder", root); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || g_nm_count >= NM_MAX) continue;
        char path[MAX_PATH * 2], js[4096];
        snprintf(path, sizeof path, "%s\\%s\\fighter.json", root, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;                              /* a folder without a manifest is a plain file mod (loose_files.c) */
        size_t n = fread(js, 1, sizeof js - 1, f); fclose(f); js[n] = 0;
        NewMod *nm = &g_nm[g_nm_count];
        char v[64];
        memset(nm, 0, sizeof *nm);
        if (!nm_json_str(js, "name", nm->f.name, sizeof nm->f.name) || !nm->f.name[0]) snprintf(nm->f.name, sizeof nm->f.name, "%.31s", fd.cFileName);
        if (!nm_json_str(js, "donor", nm->f.donor, sizeof nm->f.donor) || !nm->f.donor[0]) { nm_log("[registry] %s: no \"donor\": skipped", path); continue; }
        nm_json_str(js, "dll", nm->f.dll, sizeof nm->f.dll);
        nm_json_str(js, "display_name", nm->display, sizeof nm->display);
        for (int q = 0; q < 63 && nm->display[q]; q++) nm->display16[q] = (uint8_t)nm->display[q];
        nm->f.own_name = !(nm_json_str(js, "own_name", v, sizeof v) && (v[0] == 'f' || v[0] == '0'));
        nm->f.kind = nm_json_str(js, "kind", v, sizeof v) && v[0] >= '0' && v[0] <= '9' ? (int)strtol(v, NULL, 0) : -1;
        snprintf(nm->f.folder, sizeof nm->f.folder, "%s\\%s", root, fd.cFileName);
        snprintf(nm->module, sizeof nm->module, "lua2cpp_%s", nm->f.name);
        snprintf(nm->donor_module, sizeof nm->donor_module, "lua2cpp_%s", nm->f.donor);
        g_nm_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_nm, (size_t)g_nm_count, sizeof g_nm[0], nm_cmp_fighter);
    const char *bin = getenv("SSBU_FIGHTER_BIN");
    int staged = 0;
    if (g_nm_count && !(bin && bin[0])) { CreateDirectoryA("registry_bin", NULL); CreateDirectoryA("registry_bin\\fighters", NULL); }
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        if (nm->f.kind < 0) nm->f.kind = 0x5e + i;     /* "auto": the first free kind numbers, in name order */
        if (nm->f.dll[0] && !(bin && bin[0])) {
            char src[MAX_PATH * 2], dst[MAX_PATH * 2];
            snprintf(src, sizeof src, "%s\\%s", nm->f.folder, nm->f.dll);
            snprintf(dst, sizeof dst, "registry_bin\\fighters\\%s.dll", nm->f.name);
            if (CopyFileA(src, dst, FALSE)) staged++; else nm_log("[registry] %s: cannot stage %s (Windows error %lu)", nm->f.name, src, (unsigned long)GetLastError());
        }
        g_nm_list[i] = nm->f;
        nm_log("[registry] fighter %s: kind 0x%x, donor %s, script DLL %s, folder %s, own name %d", nm->f.name, nm->f.kind, nm->f.donor,
               nm->f.dll[0] ? nm->f.dll : "(none)", nm->f.folder, nm->f.own_name);
    }
    if (g_nm_count) {
        if (!(bin && bin[0])) _putenv_s("SSBU_FIGHTER_BIN", "registry_bin\\fighters");
        const char *mods = getenv("SSBU_MODS");
        if (!(mods && mods[0])) _putenv_s("SSBU_MODS", "registry_bin");
        nm_log("[registry] %d fighter(s) from %s; %d script DLL(s) staged; fighter DLL folder %s; components from %s", g_nm_count, root, staged,
               getenv("SSBU_FIGHTER_BIN"), getenv("SSBU_MODS"));
    }
}
__attribute__((constructor)) static void nm_constructor(void) { nm_scan(); }

/* ---------------------------------------------------------------- the module object */
static NewMod *nm_find(const char *name) {
    for (int i = 0; i < g_nm_count; i++) if (strcmp(g_nm[i].f.name, name) == 0) return &g_nm[i];
    return NULL;
}
static NroModule *nm_registry_module(const char *module) {
    ro_init_tables();
    for (int i = 0; i < nro_registry_count; i++) if (strcmp(g_mods[i].info->name, module) == 0) return &g_mods[i];
    return NULL;
}
/* address range of new module number i: behind the last module of the registry, 2 MB slots */
static uint64_t nm_base(int index, uint32_t image_size) {
    uint64_t end = NRO_BASE;
    for (int i = 0; i < nro_registry_count; i++) {
        uint64_t e = nro_registry[i].base + (((uint64_t)nro_registry[i].image_size + 0xffff) & ~(uint64_t)0xffff);
        if (e > end) end = e;
    }
    end = (end + (1u << NRO_SLOT_SHIFT) - 1) & ~(uint64_t)((1u << NRO_SLOT_SHIFT) - 1);
    (void)image_size;
    return end + (uint64_t)(index + 1) * 0x1000000ULL;         /* 16 MB apart: no fighter module is larger than 4 MB */
}
static void nm_factory_hooks(NewMod *nm);
static int nm_activate(NewMod *nm) {
    if (nm->active) return 1;
    if (!nm->dm) nm->dm = nm_registry_module(nm->donor_module);
    NroModule *dm = nm->dm, *m = &nm->mod;
    if (!dm || !dm->loaded || !dm->fns) return 0;
    AcquireSRWLockExclusive(&g_ro_lock);
    if (!nm->info.base) {
        nm->info = *dm->info;
        nm->info.name = nm->module;
        nm->info.base = nm_base((int)(nm - g_nm), dm->info->image_size);
        /* the range is reserved so that no allocation of the process can get these addresses; it is never committed */
        VirtualAlloc((void *)(uintptr_t)nm->info.base, dm->info->image_size, MEM_RESERVE, PAGE_NOACCESS);
        nm->fns = malloc((size_t)dm->fn_count * sizeof *nm->fns);
        for (int i = 0; i < dm->fn_count; i++) { nm->fns[i].addr = dm->fns[i].addr - dm->info->base + nm->info.base; nm->fns[i].fn = dm->fns[i].fn; }
        nm->bind_addr = malloc((size_t)dm->sym_count * sizeof *nm->bind_addr);
    }
    for (int i = 0; i < dm->sym_count; i++) {
        uint64_t a = dm->bind_addr ? dm->bind_addr[i] : 0;
        nm->bind_addr[i] = a - dm->info->base < dm->info->image_size ? a - dm->info->base + nm->info.base : a;
    }
    memset(m, 0, sizeof *m);
    m->info = &nm->info; m->dll = dm->dll;
    m->fns = nm->fns; m->fn_count = dm->fn_count; m->syms = dm->syms; m->sym_count = dm->sym_count;
    m->bind_fn = dm->bind_fn; m->bind_addr = nm->bind_addr;
    m->loaded = 1;
    for (uint64_t s = 0; s < ((uint64_t)nm->info.image_size + (1u << NRO_SLOT_SHIFT) - 1) >> NRO_SLOT_SHIFT; s++)
        g_slot[((nm->info.base - NRO_BASE) >> NRO_SLOT_SHIFT) + s] = m;
    ReleaseSRWLockExclusive(&g_ro_lock);
    nm->vt_count = 0;
    free(nm->cat_hash); nm->cat_hash = NULL; nm->frame_at = 0;        /* N12: looked up again in the DLL attached now */
    nm->active = 1;
    fighter_attach(m);                                  /* <fighter DLL folder>\<name>.dll: the fighter's own script functions */
    nm_factory_hooks(nm);                               /* N7: the fighter's own agent factories (below) */
    int own = m->fighter ? ((FsMod *)m->fighter)->ovr_count : 0;
    nm_log("[registry] module %s is OURS: no NRO; address range %llx (+0x%x, reserved, not committed); %d of its %d functions come from %s.dll, "
           "the others from the donor module %s (at %llx) at the same offset",
           nm->module, (unsigned long long)nm->info.base, nm->info.image_size, own, dm->fn_count, nm->f.name, nm->donor_module, (unsigned long long)dm->info->base);
    return 1;
}

/* ---------------------------------------------------------------- agents */
static NewMod *nm_agent_lookup(uint64_t agent) {
    NewMod *nm = NULL;
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockShared(&g_nm_lock);
    for (int k = 0; k < 2048 && g_nm_agent[i].agent; k++, i = (i + 1) & 2047) if (g_nm_agent[i].agent == agent) { nm = g_nm_agent[i].nm; break; }
    ReleaseSRWLockShared(&g_nm_lock);
    return nm;
}
static void nm_agent_set(uint64_t agent, NewMod *nm) {      /* nm == NULL: forget (the slot stays in the probe chain) */
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockExclusive(&g_nm_lock);
    for (int k = 0; k < 2048; k++, i = (i + 1) & 2047) {
        if (g_nm_agent[i].agent == agent) { g_nm_agent[i].nm = nm; break; }
        if (!g_nm_agent[i].agent) { if (nm) { g_nm_agent[i].agent = agent; g_nm_agent[i].nm = nm; } break; }
    }
    ReleaseSRWLockExclusive(&g_nm_lock);
}
static uint64_t nm_remap(NewMod *nm, uint64_t a) {
    NroModule *dm = nm->dm;
    if (a - dm->info->base < dm->info->image_size && fn_in_table(dm->fns, dm->fn_count, a)) return a - dm->info->base + nm->info.base;
    return a;
}
static uint64_t nm_clone_vtable(NewMod *nm, uint64_t vt) {
    NroModule *dm = nm->dm;
    if (vt - dm->info->base >= dm->info->image_size) return vt;        /* a class of another module: nothing of the donor's in it */
    for (int i = 0; i < nm->vt_count; i++) if (nm->vt_src[i] == vt) return nm->vt_dst[i];
    uint64_t *copy = calloc(NM_VT_SLOTS + 2, 8), lo = vt - 0x10, end = dm->info->base + dm->info->image_size;
    int moved = 0;
    for (int i = 0; i < NM_VT_SLOTS + 2 && lo + 8ull * i + 8 <= end; i++) {
        uint64_t v = rd64(lo + 8ull * i);
        copy[i] = i >= 2 ? nm_remap(nm, v) : v;
        if (copy[i] != v) moved++;
    }
    uint64_t dst = (uint64_t)(uintptr_t)(copy + 2);
    if (nm->vt_count < 64) { nm->vt_src[nm->vt_count] = vt; nm->vt_dst[nm->vt_count++] = dst; }
    nm->vt_clones++;
    if (nm->vt_clones <= 40) nm_log("[registry] %s: class table %s+0x%llx copied for the new module: %d entries are now %s addresses", nm->module, nm->donor_module,
           (unsigned long long)(vt - dm->info->base), moved, nm->module);
    return dst;
}
static void nm_agent_begin(const char *name) {
    NewMod *nm = name ? nm_find(name) : NULL;
    t_nm_current = nm && nm_activate(nm) ? nm : NULL;
    t_nm_weapon = 0; t_nm_cat_agent = 0;            /* N12 (defined with the script registration below) */
    if (nm && !t_nm_current) { static int said; if (!said++) nm_log("[registry] %s: the donor module %s is not loaded: the agent stays the donor's", nm->module, nm->donor_module); }
}
static void nm_agent_end(const char *name, uint64_t agent) {
    NewMod *nm = t_nm_current;
    t_nm_current = NULL;
    if (!name) { if (agent && g_nm_count) nm_agent_set(agent, NULL); return; }
    if (!nm || agent < 0x10000) return;
    uint64_t vt = rd64(agent), nv = nm_clone_vtable(nm, vt);
    if (nv != vt) wr64(agent, nv);
    nm_agent_set(agent, nm);
    /* the process ends through the game's own exit path, which does not run atexit functions registered this late:
     * the counters are printed while the game runs instead */
    if (nm->agents == 9 || (nm->agents && nm->agents % 64 == 0))
        nm_log("[registry] %s so far: %ld module requests (%ld answered directly, %ld loaded the donor image), %ld agents moved into the module, %ld class tables copied, "
               "%ld script registrations moved to its addresses, %ld paths with its name asked (%ld not files of the archive)",
               nm->module, nm->requests, nm->answered, nm->substituted, nm->agents, nm->vt_clones, nm->remapped, nm->paths, nm->paths_missing);
    if (nm->agents++ < 24) nm_log("[registry] %s: agent %llx (class table %s+0x%llx) belongs to the new module", nm->module, (unsigned long long)agent,
                                  vt - nm->dm->info->base < nm->dm->info->image_size ? nm->donor_module : "(other)", (unsigned long long)(vt - nm->dm->info->base));
}
/* N12 task 3: THE FIGHTER'S OWN SCRIPT NAMES. A factory registers the donor's scripts by hash, one call of
 * sv_set_function_hash per script, so the agent's script table holds the donor's names only and a motion whose script
 * name the donor never had finds nothing. At the FIRST registration made for a new fighter's FIGHTER agent (not an
 * article's: agent_begin_weapon), the script category of that agent is the prefix of the fighter-DLL script whose name
 * has that hash ("game_", "effect_", "sound_", "expression_"); right after it every function that exists ONLY in the
 * fighter's source and carries that prefix is registered too, under the hash of its own name, at its own (synthetic)
 * address of the new module. Names the donor has are registered as before (remapped). SSBU_REG_OWNSCRIPTS=0: off. */
static const char *const g_nm_cat[4] = { "game_", "effect_", "sound_", "expression_" };
static uint64_t nm_hash40(const char *s);
static int nm_cat_of_hash(NewMod *nm, uint64_t h) {
    FsMod *fm = nm->mod.fighter;
    if (!fm) return -1;
    if (!nm->cat_hash) {
        nm->cat_hash = calloc((size_t)fm->ovr_count + 1, sizeof *nm->cat_hash);
        for (int i = 0; i < fm->ovr_count; i++) nm->cat_hash[i] = nm_hash40(fm->ovr[i].name);
    }
    for (int i = 0; i < fm->ovr_count; i++)
        if (nm->cat_hash[i] == h)
            for (int k = 0; k < 4; k++) if (strncmp(fm->ovr[i].name, g_nm_cat[k], strlen(g_nm_cat[k])) == 0) return k;
    return -1;
}
static void nm_own_scripts(Ctx *c, NewMod *nm, uint64_t agent, int cat) {
    FsMod *fm = nm->mod.fighter;
    uint64_t keep[31]; memcpy(keep, c->x, sizeof keep);
    int n = 0; char names[400]; int k = 0; names[0] = 0;
    for (int i = 0; i < fm->ovr_count; i++) {
        if (!fm->stat[i].is_new || strncmp(fm->ovr[i].name, g_nm_cat[cat], strlen(g_nm_cat[cat])) != 0) continue;
        c->x[0] = agent; c->x[1] = nm->info.base + fm->ovr[i].off; c->x[2] = nm_hash40(fm->ovr[i].name);
        g_nm_o_fnhash(c);
        memcpy(c->x, keep, sizeof keep);
        n++; nm->own_scripts++;
        if (k < 360) k += snprintf(names + k, sizeof names - k, " %s", fm->ovr[i].name);
    }
    if (nm->own_script_agents++ < 8) nm_log("[n12] %s: %s agent %llx: %d scripts of the fighter's OWN names registered by name (functions only in its source):%s", nm->module, g_nm_cat[cat],
                                            (unsigned long long)agent, n, n ? names : " (none)");
}
static void nm_h_fnhash(Ctx *c, void *user) {          /* lib::L2CAgent::sv_set_function_hash(this, function, hash) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : nm_agent_lookup(c->x[0]);
    if (nm && nm->active) { uint64_t a = nm_remap(nm, c->x[1]); if (a != c->x[1]) { c->x[1] = a; nm->remapped++; } }
    uint64_t agent = c->x[0], h = c->x[2] & 0xffffffffffULL;
    g_nm_o_fnhash(c);
    static int on = -1; if (on < 0) { const char *e = getenv("SSBU_REG_OWNSCRIPTS"); on = !(e && e[0] == '0'); }
    if (on && nm && nm->active && t_nm_current == nm && !t_nm_weapon && t_nm_cat_agent != agent) {
        int cat = nm_cat_of_hash(nm, h);
        if (cat >= 0) { t_nm_cat_agent = agent; nm_own_scripts(c, nm, agent, cat); }
    }
}

/* N12 task 2: A PER-FRAME ROUTINE OF THE FIGHTER'S OWN. The engine calls the frame line
 * lua2cpp::L2CFighterCommon::sys_line_system_control_fighter once per fight frame per fighter (its status agent in x0).
 * When that agent belongs to a new fighter and its DLL has the function `<name>_frame` (written as
 * L2C_VOID_FUNCTION(<name>, <name>_frame) { ... } in its sources; a function only in the source), it runs first, with
 * the agent and its module accessor as any status function. SSBU_REG_FRAME=0: off. */
static ImportFn g_nm_o_frame;
static void nm_h_frame(Ctx *c, void *user) {
    (void)user;
    NewMod *nm = g_nm_count ? nm_agent_lookup(c->x[0]) : NULL;
    FsMod *fm = nm && nm->active ? nm->mod.fighter : NULL;
    if (fm && fm->dll) {
        if (nm->frame_at == 0) {
            char want[64]; snprintf(want, sizeof want, "%s_frame", nm->f.name);
            nm->frame_at = -1;
            for (int i = 0; i < fm->ovr_count; i++) if (strcmp(fm->ovr[i].name, want) == 0) { nm->frame_at = i + 1; break; }
            nm_log("[n12] %s: per-frame routine %s %s", nm->module, want, nm->frame_at > 0 ? "found in the fighter's DLL: called once per fight frame from the frame line" : "not in the fighter's DLL");
        }
        if (nm->frame_at > 0) {
            uint64_t keep[31]; memcpy(keep, c->x, sizeof keep);
            fighter_dispatch(c, &nm->mod, nm->info.base + fm->ovr[nm->frame_at - 1].off, NULL);
            memcpy(c->x, keep, sizeof keep);
            nm->frame_calls++;
        }
    }
    g_nm_o_frame(c);
}
static void nm_h_status(Ctx *c, void *user) {          /* lua2cpp::L2CAgentBase::sv_set_status_func(this, kind, line, function) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : nm_agent_lookup(c->x[0]);
    if (nm && nm->active) { uint64_t a = nm_remap(nm, c->x[3]); if (a != c->x[3]) { c->x[3] = a; nm->remapped++; } }
    g_nm_o_status(c);
}

/* ---------------------------------------------------------------- N7: the new fighter's OWN agent factories
 * The engine's script dispatchers pass the hash of the agent's name ("<donor>" or "<donor>_<article>") in x0 and jump
 * to the donor module's exported create_agent_* function (main's import of the donor's symbol). A new fighter's DLL
 * can carry these factories as source that compares the NEW names (tests\n6_s_factories.py). Every create_agent_*
 * export of a donor module is hooked once; while an agent is being made for a new fighter on this thread
 * (agent_begin .. agent_end) and the fighter's DLL has a function at the same offset of its own module, the name hash
 * becomes the new name's (the checksum is linear: the same text with another beginning needs no text) and the
 * DLL's factory runs. It returns 0 for a name it does not make: then the donor's factory runs with the donor's
 * name, as before (that agent is then moved into the new module by agent_end). SSBU_REG_FACTORY=0: off. */
static struct { uint64_t addr; NroModule *dm; ImportFn orig; char sym[96]; } g_nm_fact[64];
static int g_nm_fact_count;
static uint32_t nm_crc_plain(const char *s) {
    uint32_t crc = 0xffffffffu;
    for (; *s; s++) { crc ^= (uint8_t)*s; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
static uint64_t nm_rename40(uint64_t h, const char *p, const char *p2) {
    unsigned len = (unsigned)(h >> 32) & 0xff, lp = (unsigned)strlen(p), lp2 = (unsigned)strlen(p2);
    uint32_t x = nm_crc_plain(p) ^ nm_crc_plain(p2);
    if (len < lp) return h;
    for (unsigned i = 0; i < (len - lp) * 8; i++) x = (x >> 1) ^ (0xedb88320u & (0u - (x & 1)));
    return ((uint64_t)((len - lp + lp2) & 0xff) << 32) | ((uint32_t)h ^ x);
}
static long g_nm_fact_own, g_nm_fact_donor;
static void nm_h_factory(Ctx *c, void *user) {
    int k = (int)(intptr_t)user;
    NewMod *nm = t_nm_current;
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_REG_FACTORY"); on = !(e && e[0] == '0'); }
    if (on && nm && nm->active && nm->dm == g_nm_fact[k].dm && nm->mod.fighter) {
        uint64_t keep[31]; memcpy(keep, c->x, sizeof keep);
        uint64_t own = g_nm_fact[k].addr - nm->dm->info->base + nm->info.base;
        c->x[0] = nm_rename40(keep[0] & 0xffffffffffULL, nm->f.donor, nm->f.name);
        if (fighter_dispatch(c, &nm->mod, own, NULL) && c->x[0]) {
            if (g_nm_fact_own++ < 14 || g_nm_fact_own % 32 == 0)
                nm_log("[registry] %s: agent made by ITS OWN factory %s (in %s.dll, at %s+0x%llx) under its own name (hash %llx; the donor's name hash was %llx); so far %ld by own factories, %ld by "
                       "the donor's", nm->module, g_nm_fact[k].sym, nm->f.name, nm->module, (unsigned long long)(own - nm->info.base), (unsigned long long)nm_rename40(keep[0] & 0xffffffffffULL, nm->f.donor, nm->f.name),
                       (unsigned long long)(keep[0] & 0xffffffffffULL), g_nm_fact_own, g_nm_fact_donor);
            return;
        }
        memcpy(c->x, keep, sizeof keep);
        if (g_nm_fact_donor++ < 14) nm_log("[registry] %s: factory %s: the fighter's DLL does not make the agent with name hash %llx: the DONOR's factory makes it (%ld so far)", nm->module,
                                           g_nm_fact[k].sym, (unsigned long long)(keep[0] & 0xffffffffffULL), g_nm_fact_donor);
    }
    g_nm_fact[k].orig(c);
}
static void nm_factory_hooks(NewMod *nm) {
    NroModule *dm = nm->dm;
    int fresh = 0, have = 0;
    for (int i = 0; i < dm->sym_count; i++) {
        const char *name = dm->syms[i].name;
        if (!name || !dm->syms[i].defined || !strstr(name, "create_agent_") || !dm->bind_addr) continue;
        uint64_t a = dm->bind_addr[i];
        if (a - dm->info->base >= dm->info->image_size) continue;
        int known = 0;
        for (int k = 0; k < g_nm_fact_count; k++) if (g_nm_fact[k].addr == a) known = 1;
        if (known) { have++; continue; }
        if (g_nm_fact_count >= 64) break;
        int k = g_nm_fact_count;
        g_nm_fact[k].addr = a; g_nm_fact[k].dm = dm; snprintf(g_nm_fact[k].sym, sizeof g_nm_fact[k].sym, "%.95s", name);
        g_nm_fact_count++;                              /* the slot is taken before the hook can fire */
        if (hook_install(a, nm_h_factory, (void *)(intptr_t)k, &g_nm_fact[k].orig)) fresh++;
        else { g_nm_fact[k].addr = 0; nm_log("[registry] %s: the factory %s at %llx could not be hooked", nm->module, name, (unsigned long long)a); }
    }
    if (fresh || !have) nm_log("[registry] %s: %d agent factories of the donor module %s hooked now (%d before): a new fighter's own factory answers for its own names", nm->module, fresh,
                               nm->donor_module, have);
}

/* ---------------------------------------------------------------- the formatter: module requests and paths */
static uint64_t nm_hash40(const char *s) {
    uint32_t crc = 0xffffffffu; size_t n = 0;
    for (; s[n]; n++) {
        uint8_t ch = (uint8_t)s[n]; if (ch >= 'A' && ch <= 'Z') ch += 32;
        crc ^= ch; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ((uint64_t)(n & 0xff) << 32) | (uint32_t)~crc;
}
/* the archive's own file lookup (as the worker does it inline at 0x71022b68cc..): file path index, or -1 */
static int64_t nm_arc_file_index(uint64_t hash) {
    uint64_t fsinfo = rd64(0x7105331f20ULL);
    if (!fsinfo) return -2;
    uint64_t holder = rd64(fsinfo + 0x78); if (!holder) return -2;
    uint64_t tab = rd64(holder); if (!tab) return -2;
    uint64_t buckets = rd64(tab + 0x50), entries = rd64(tab + 0x58);
    if (!buckets || !entries) return -2;
    uint32_t nb = rd32(buckets + 4); if (!nb) return -2;
    uint64_t b = buckets + 8 + (hash % nb) * 8;
    uint32_t start = rd32(b), count = rd32(b + 4);
    for (uint32_t i = 0; i < count; i++) {             /* the game searches the sorted range; a scan is enough for a log line */
        uint64_t e = rd64(entries + 8ull * (start + i));
        if ((e & 0xffffffffffULL) == hash) return (int64_t)(e >> 40);
    }
    return -1;
}
static int nm_token_char(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }
static uint64_t g_nm_seen[4096]; static int g_nm_seen_count;
int (*g_nm_stage2_has)(uint64_t path_hash);            /* set by the loose-file layer's stage 2 when it has a row for the path */
static char *nm_stristr(char *s, const char *name) {    /* the engine also writes the name with a capital (its sound-name row) */
    size_t n = strlen(name);
    for (; *s; s++) if (_strnicmp(s, name, n) == 0) return s;
    return NULL;
}
/* N8: THE DONOR'S NAME IN A PATH WHILE A NEW FIGHTER IS SET UP (registry owner_set). The engine builds the paths of a
 * fighter's ARTICLE models from the donor's article list, with the donor's name; with a load group of its own the new
 * fighter has these files under its own name (stage 2 rows). The path is rewritten in place to the fighter's own
 * name when (a) stage 2 has a row for the renamed path and (b) the text can take it: the same length or shorter, or
 * the engine's fixed string (256 bytes of text + a 16-bit length that equals the text's length). SSBU_REG_OWNPATH=0: off. */
static _Thread_local NewMod *t_nm_owner;
static long g_nm_ownpath, g_nm_ownpath_no;
static void nm_owner_set(const char *name) { t_nm_owner = name ? nm_find(name) : NULL; }
static int nm_owner_path(char *buf, uint64_t lr) {
    NewMod *nm = t_nm_owner;
    static int on = -1; if (on < 0) { const char *e = getenv("SSBU_REG_OWNPATH"); on = !(e && e[0] == '0'); }
    if (!nm || !on || !g_nm_stage2_has || !strchr(buf, '/')) return 0;
    size_t ln = strlen(nm->f.name), ld = strlen(nm->f.donor), n = strlen(buf);
    if (n >= 250 || nm_stristr(buf, nm->f.name)) return 0;
    char out[512]; size_t k = 0; int swapped = 0;
    for (const char *p = buf; *p && k + ln + 2 < sizeof out; ) {
        if (_strnicmp(p, nm->f.donor, ld) == 0 && (p == buf || !nm_token_char(p[-1])) && !nm_token_char(p[ld])) {
            int cap = p[0] >= 'A' && p[0] <= 'Z';
            memcpy(out + k, nm->f.name, ln); if (cap && out[k] >= 'a' && out[k] <= 'z') out[k] -= 32;
            k += ln; p += ld; swapped++;
        } else out[k++] = *p++;
    }
    out[k] = 0;
    if (!swapped) return 0;
    uint64_t h = nm_hash40(out);
    int own = g_nm_stage2_has(h);
    if (!own) return 0;
    uint16_t *len = (uint16_t *)(buf + 256);
    MEMORY_BASIC_INFORMATION mi;
    int fixed = VirtualQuery(len, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY | PAGE_EXECUTE_READ)) && *len == (uint16_t)n;
    int known = 0;
    for (int q = 0; q < g_nm_seen_count; q++) if (g_nm_seen[q] == h) { known = 1; break; }
    if (!known && g_nm_seen_count < 4096) g_nm_seen[g_nm_seen_count++] = h;
    if (k > n && !fixed) {
        g_nm_ownpath_no++;
        if (!known) nm_log("[registry] path asked with the DONOR's name while %s is set up: %s (by %llx): %s's folder has %s, but the text cannot be lengthened here: the donor's file answers",
                           nm->f.name, buf, (unsigned long long)lr, nm->f.name, out);
        return 0;
    }
    g_nm_ownpath++;
    if (!known) nm_log("[registry] path asked with the DONOR's name while %s is set up: %s (by %llx) -> answered with the fighter's OWN %s %s (stage 2 row); %ld such paths so far, %ld left to the donor",
                       nm->f.name, buf, (unsigned long long)lr, own == 3 ? "load group" : own == 2 ? "folder" : "file", out, g_nm_ownpath, g_nm_ownpath_no);
    memcpy(buf, out, k + 1);
    if (fixed) *len = (uint16_t)k;
    return 1;
}
static void nm_path(char *buf, uint64_t lr) {
    if (t_nm_owner && nm_owner_path(buf, lr)) return;
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        size_t ln = strlen(nm->f.name), ld = strlen(nm->f.donor);
        char *p = nm_stristr(buf, nm->f.name);
        if (!p || !strchr(buf, '/')) continue;
        char was[512]; snprintf(was, sizeof was, "%s", buf);
        uint64_t h_new = nm_hash40(buf);
        int own = g_nm_stage2_has ? g_nm_stage2_has(h_new) : 0;
        if (own) {                                                  /* N6: the path has a row of its own (loose_files.c, stage 2) */
            int known = 0;
            for (int k = 0; k < g_nm_seen_count; k++) if (g_nm_seen[k] == h_new) { known = 1; break; }
            if (!known) {
                if (g_nm_seen_count < 4096) g_nm_seen[g_nm_seen_count++] = h_new;
                nm->paths++; nm->paths_own++;
                nm_log("[registry] path asked: %s (hash %llx, by %llx) -> OWN %s of %s's folder (stage 2 row); so far %ld own, %ld donor fallbacks", buf, (unsigned long long)h_new,
                       (unsigned long long)lr, own == 3 ? "LOAD GROUP (directory row)" : own == 2 ? "FOLDER" : "FILE", nm->f.name, nm->paths_own, nm->paths - nm->paths_own);
            }
            return;
        }
        int swapped = 0;
        while (p) {
            int tok = (p == buf || !nm_token_char(p[-1])) && !nm_token_char(p[ln]);
            if (tok && ld <= ln) {
                int cap = p[0] >= 'A' && p[0] <= 'Z';
                memmove(p + ld, p + ln, strlen(p + ln) + 1); memcpy(p, nm->f.donor, ld);
                if (cap && p[0] >= 'a' && p[0] <= 'z') p[0] -= 32;
                swapped++; p = nm_stristr(p + ld, nm->f.name);
            } else p = nm_stristr(p + 1, nm->f.name);
        }
        if (!swapped) continue;
        {
            /* the engine's fixed strings are 256 bytes of text followed by a 16-bit length (0x71017e89c0: `strh wzr,
             * [buffer + 256]`), and it appends at that length: a shortened text needs the shorter length */
            MEMORY_BASIC_INFORMATION mi;
            uint16_t *len = (uint16_t *)(buf + 256);
            if (VirtualQuery(len, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY | PAGE_EXECUTE_READ))
                && *len == (uint16_t)strlen(was)) *len = (uint16_t)strlen(buf);
        }
        int known = 0;
        for (int k = 0; k < g_nm_seen_count; k++) if (g_nm_seen[k] == h_new) { known = 1; break; }
        if (!known) {
            if (g_nm_seen_count < 4096) g_nm_seen[g_nm_seen_count++] = h_new;
            int64_t fi = nm_arc_file_index(nm_hash40(buf));
            nm->paths++; if (fi < 0) nm->paths_missing++;
            nm_log("[registry] path asked: %s (hash %llx, by %llx) -> not in the archive -> DONOR FALLBACK, the donor's: %s: %s", was, (unsigned long long)h_new, (unsigned long long)lr, buf,
                   fi >= 0 ? "found" : fi == -1 ? "NOT a file path of the archive (a folder, a stream, or missing)" : "(archive tables not ready)");
            if (fi >= 0) nm_log("[registry] path file index: %s = 0x%llx", buf, (unsigned long long)fi);
        }
        return;
    }
}
static void nm_h_fmt(Ctx *c, void *user) {
    (void)user;
    uint64_t buf = c->x[0], lr = c->x[30];
    if (lr == NM_WORKER_RET && c->x[4] > 0x10000) {
        const char *name = (const char *)(uintptr_t)c->x[4];
        uint64_t req = c->x[25];
        char module[64]; snprintf(module, sizeof module, "lua2cpp_%.40s", name);
        NewMod *nm = nm_find(name);
        NroModule *rm = nm ? NULL : nm_registry_module(module);
        if (nm) {
            nm->requests++;
            if (!nm->dm) nm->dm = nm_registry_module(nm->donor_module);
            if (!nm->dm) { nm_log("[registry] module request %s: the donor module %s is not a module of the game: NOT answered", module, nm->donor_module); g_nm_o_fmt(c); return; }
            if (nm->active && !nm->dm->loaded) { fighter_detach(&nm->mod); nm->active = 0; }      /* the donor's image went away with the last match */
            if (!nm->dm->loaded) {
                /* the donor's image is a dependency: this request loads it (the game's own path, its own LoadModule) */
                c->x[4] = (uint64_t)(uintptr_t)nm->f.donor;
                nm->substituted++;
                nm_log("[registry] module request %s (request object %llx): answered by OUR module (no NRO of that name); its donor image %s is not loaded: "
                       "this request loads it", module, (unsigned long long)req, nm->donor_module);
                g_nm_o_fmt(c);
                return;
            }
            *(uint8_t *)(uintptr_t)(req + 0x132) = 1;                  /* "loaded", as the worker writes it at 0x71022b6cd0 */
            *(char *)(uintptr_t)buf = 0;                               /* an empty path: the worker goes back to its queue */
            nm->answered++;
            nm_log("[registry] module request %s (request object %llx): answered by OUR module (no NRO of that name); donor image %s already loaded", module,
                   (unsigned long long)req, nm->donor_module);
            return;
        }
        if (rm && rm->loaded) {                        /* loaded earlier as a new fighter's donor: LoadModule would refuse a second load */
            *(uint8_t *)(uintptr_t)(req + 0x132) = 1;
            *(char *)(uintptr_t)buf = 0;
            nm_log("[registry] module request %s (request object %llx): the module is already loaded (as a donor): answered \"loaded\"", module, (unsigned long long)req);
            return;
        }
        g_nm_o_fmt(c);
        return;
    }
    g_nm_o_fmt(c);
    if (buf > 0x10000) nm_path((char *)(uintptr_t)buf, lr);
}

/* The other string builders that make file paths from a kind's name (found by what a run asked for and did not get):
 *   0x710147f330 (buffer of 256 bytes, format, arguments): used 12 times by the per-kind path builder 0x71017e88d0;
 *   0x71017df5f0 (buffer, kind, file type, x3): the per-kind sub-builder of the same function.
 * Both return the path in the buffer given in x0. */
/*   0x71017dfe50 (buffer, kind, file type, x3, x4): a second sub-builder of it;
 *   0x710353e330 (out, path string in x1): the path builder's last step, "path string -> file index" (entry 3:
 *   the string is looked at BEFORE the call). */
static const uint64_t g_nm_path_fn[] = { 0x710147f330ULL, 0x71017df5f0ULL, 0x71017dfe50ULL, 0x710353e330ULL };
#define NM_PATH_PRE 3
static ImportFn g_nm_o_path[sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]];
static void nm_h_pathfmt(Ctx *c, void *user) {
    uint64_t buf = c->x[0], lr = c->x[30];
    int i = (int)(intptr_t)user;
    if (i >= NM_PATH_PRE) { if (c->x[1] > 0x10000) nm_path((char *)(uintptr_t)c->x[1], lr); g_nm_o_path[i](c); return; }
    g_nm_o_path[i](c);
    if (buf > 0x10000) nm_path((char *)(uintptr_t)buf, lr);
}

/* N9: THE PORTRAIT / ICON FILES OF A NEW ROSTER ROW. 0x71032384c0(x0 = ui_chara id (0xc1 << 56 | row << 40 | hash40 of
 * "ui_chara_<name>"), w1 = colour, w2 = picture type) returns the path hash of
 * "ui/replace/chara/chara_<type>/chara_<type>_<name_id>_<colour, 2 digits>.bntx" (N1 2.1; the row of a new fighter is a
 * copy of its donor's, so its name_id is the donor's and the answer is the donor's picture). After the call: when the
 * id is a new fighter's and the answer is one of the donor's picture paths, and the fighter's folder brings the same
 * path under ITS name (a stage 2 row: characters\<name>\ui\replace\chara\chara_<type>\chara_<type>_<name>_<cc>.bntx),
 * the answer is that path's hash. One log line per (fighter, type, colour). SSBU_REG_PORTRAIT=0: off (log only). */
#define NM_PORTRAIT 0x71032384c0ULL
static ImportFn g_nm_o_portrait;
static long g_nm_portrait_calls, g_nm_portrait_own;
static void nm_h_portrait(Ctx *c, void *user) {
    (void)user;
    uint64_t id = c->x[0], lr = c->x[30]; uint32_t colour = (uint32_t)c->x[1], type = (uint32_t)c->x[2];
    g_nm_o_portrait(c);
    static int on = -1; if (on < 0) { const char *e = getenv("SSBU_REG_PORTRAIT"); on = !(e && e[0] == '0'); }
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        char t[320]; snprintf(t, sizeof t, "ui_chara_%s", nm->f.name);
        if ((id & 0xffffffffffULL) != nm_hash40(t)) continue;
        uint64_t got = c->x[0], hd = 0; int found = -1, cc = -1, asked_own = 0; char own[320] = "";
        for (int ty = 0; ty < 16 && found < 0; ty++)
            for (int col = 0; col < 16; col++) {
                snprintf(t, sizeof t, "ui/replace/chara/chara_%d/chara_%d_%s_%02d.bntx", ty, ty, nm->f.donor, col);
                hd = nm_hash40(t);
                snprintf(own, sizeof own, "ui/replace/chara/chara_%d/chara_%d_%s_%02d.bntx", ty, ty, nm->f.name, col);
                /* with a "display_name" the row's name_id is the fighter's own name (nm_h_nameid): the answer then
                 * already names the fighter's own file, and the DONOR's is the fallback when the folder has none */
                if (hd == (got & 0xffffffffffULL)) { found = ty; cc = col; break; }
                if (nm_hash40(own) == (got & 0xffffffffffULL)) { found = ty; cc = col; asked_own = 1; break; }
            }
        int has = 0; uint64_t h = 0;
        if (found >= 0) {
            h = nm_hash40(own);
            has = g_nm_stage2_has ? g_nm_stage2_has(h) : 0;
            if (asked_own && !has) { c->x[0] = (got & ~0xffffffffffULL) | hd; got = c->x[0]; }      /* no own file: the donor's picture */
        }
        g_nm_portrait_calls++;
        static uint64_t seen[256]; static int nseen; int known = 0;
        uint64_t key = ((uint64_t)i << 48) | ((uint64_t)type << 32) | ((uint64_t)colour << 16) | (uint64_t)(has != 0);
        for (int k = 0; k < nseen; k++) if (seen[k] == key) { known = 1; break; }
        if (!known && nseen < 256) {
            seen[nseen++] = key;
            if (found < 0) nm_log("[registry] picture path asked for ui_chara_%s (colour %u, type %u, by %llx): answer %llx is not a picture path of the donor %s: left as it is", nm->f.name, colour, type,
                                  (unsigned long long)lr, (unsigned long long)got, nm->f.donor);
            else nm_log("[registry] picture path asked for ui_chara_%s (colour %u, type %u, by %llx): the row's answer is the donor's chara_%d_%s_%02d.bntx -> %s", nm->f.name, colour, type,
                        (unsigned long long)lr, found, nm->f.donor, cc, has && on ? "answered with the fighter's OWN FILE of its folder (stage 2 row)" : has ? "the folder has its own (SSBU_REG_PORTRAIT=0: not used)" :
                        "the folder brings no such file: the donor's picture");
        }
        if (has && on) { c->x[0] = (got & ~0xffffffffffULL) | h; g_nm_portrait_own++; }
        return;
    }
}

/* N9: THE NAME TEXT OF A NEW ROSTER ROW (manifest "display_name"). [lifted]
 *   0x7103261d60(out, database, ui_chara id): the row's name_id as a fixed string (out + 4 = length, out + 8 = text);
 *     the picture path function and the label builders ("nam_chr<k>_<nn>_<name_id>") take the name from it;
 *   0x71037a3fc0(messages, label) -> 1 when the message files have the label; 0x71037a3ea0(messages, label) -> the
 *     text (16-bit characters), as the select screen's grid function 0x71019eb840 uses them (0x71019ee4d4, 0x71019ee4f4).
 * For a fighter WITH a display name: the name_id of its row is answered with the fighter's own name, so every label
 * and picture path is built with it; a label "nam_chr..._<name>" is answered with the display name, and a picture
 * the folder does not bring falls back to the donor's (nm_h_portrait). Without "display_name": nothing changes. */
#define NM_NAMEID 0x7103261d60ULL
#define NM_MSG_HAS 0x71037a3fc0ULL
#define NM_MSG_GET 0x71037a3ea0ULL
static ImportFn g_nm_o_nameid, g_nm_o_msg[2];
static int nm_mem_ok(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static void nm_h_nameid(Ctx *c, void *user) {
    (void)user;
    uint64_t out = c->x[0], id = c->x[2], lr = c->x[30];
    g_nm_o_nameid(c);
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        char t[96];
        if (!nm->display[0]) continue;
        snprintf(t, sizeof t, "ui_chara_%s", nm->f.name);
        if ((id & 0xffffffffffULL) != nm_hash40(t) || !nm_mem_ok(out, 0x48)) continue;
        char *text = (char *)(uintptr_t)(out + 8); int32_t *len = (int32_t *)(uintptr_t)(out + 4);
        size_t was = strnlen(text, 0x40);
        if (was >= 0x40 || (int32_t)was != *len) {
            if (nm->nameid_calls++ < 3) nm_log("[registry] name_id of ui_chara_%s (by %llx): the answer does not have the expected shape (length %d, text length %u): left as it is", nm->f.name,
                                               (unsigned long long)lr, *len, (unsigned)was);
            return;
        }
        if (nm->nameid_calls++ < 4) nm_log("[registry] name_id asked for ui_chara_%s (by %llx): the row's \"%s\" -> answered with the fighter's own name \"%s\" (manifest display_name \"%s\")", nm->f.name,
                                           (unsigned long long)lr, text, nm->f.name, nm->display);
        memcpy(text, nm->f.name, strlen(nm->f.name) + 1); *len = (int32_t)strlen(nm->f.name);
        {   /* out + 0: the string's own checksum, as the engine's append 0x710149ebc0 recomputes it (start 0x811c9dc5, per character: * 0x89, then xor) */
            uint32_t hs = 0x811c9dc5u;
            for (const char *q = nm->f.name; *q; q++) hs = (hs * 0x89u) ^ (uint8_t)*q;
            *(uint32_t *)(uintptr_t)out = hs;
        }
        return;
    }
}
static void nm_h_msg(Ctx *c, void *user) {
    int get = (int)(intptr_t)user;
    uint64_t label = c->x[1], lr = c->x[30];
    if (nm_mem_ok(label, 12) && !memcmp((const char *)(uintptr_t)label, "nam_chr", 7)) {
        const char *s = (const char *)(uintptr_t)label; size_t n = strnlen(s, 96);
        for (int i = 0; i < g_nm_count && n < 96; i++) {
            NewMod *nm = &g_nm[i];
            size_t ln = strlen(nm->f.name);
            if (!nm->display[0] || n <= ln + 1 || s[n - ln - 1] != '_' || strcmp(s + n - ln, nm->f.name)) continue;
            if (nm->text_calls++ < 12) nm_log("[registry] message label %s %s (by %llx): answered with the fighter's display name \"%s\"", s, get ? "text asked" : "existence asked",
                                              (unsigned long long)lr, nm->display);
            c->x[0] = get ? (uint64_t)(uintptr_t)nm->display16 : 1;
            return;
        }
    }
    g_nm_o_msg[get](c);
}

static void nm_report(void) {
    for (int i = 0; i < g_nm_count; i++) if (g_nm[i].display[0])
        nm_log("[registry] %s at exit: display name \"%s\": name_id answered %ld times with the fighter's own name, %ld message labels answered with the display name", g_nm[i].f.name, g_nm[i].display,
               g_nm[i].nameid_calls, g_nm[i].text_calls);
    if (g_nm_portrait_calls) nm_log("[registry] picture paths at exit: %ld asked for new roster rows, %ld answered with a file of the fighter's own folder", g_nm_portrait_calls, g_nm_portrait_own);
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        FsMod *fm = nm->mod.fighter;
        long long calls = 0; int called = 0;
        for (int k = 0; fm && k < fm->ovr_count; k++) { calls += fm->stat[k].calls; called += fm->stat[k].calls != 0; }
        nm_log("[registry] %s at exit: %ld module requests (%ld answered directly, %ld loaded the donor image), %ld agents moved into the module, %ld class tables copied, "
               "%ld script registrations moved to its addresses; %d functions of its DLL were called, %lld calls; %ld paths with its name asked (%ld not files of the archive)",
               nm->module, nm->requests, nm->answered, nm->substituted, nm->agents, nm->vt_clones, nm->remapped, called, calls, nm->paths, nm->paths_missing);
        nm_log("[n12] %s at exit: %ld own-name script registrations on %ld fighter agents; per-frame routine called %ld times", nm->module, nm->own_scripts, nm->own_script_agents, nm->frame_calls);
    }
}
static void nm_start(void) {
    if (g_nm_started || !g_nm_count) return;
    g_nm_started = 1;
    int a = hook_install(NM_FORMATTER, nm_h_fmt, NULL, &g_nm_o_fmt);
    int b = hook_install_name("_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E", nm_h_fnhash, NULL, &g_nm_o_fnhash);
    int d = hook_install_name("_ZN7lua2cpp12L2CAgentBase18sv_set_status_funcERKN3lib8L2CValueES4_Pv", nm_h_status, NULL, &g_nm_o_status);
    int e = 0;
    for (int i = 0; i < (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]); i++) e += !!hook_install(g_nm_path_fn[i], nm_h_pathfmt, (void *)(intptr_t)i, &g_nm_o_path[i]);
    int pt = hook_install(NM_PORTRAIT, nm_h_portrait, NULL, &g_nm_o_portrait);
    int any_display = 0;
    for (int i = 0; i < g_nm_count; i++) any_display |= g_nm[i].display[0] != 0;
    if (any_display) {          /* only when a manifest asks for a name text of its own */
        int n1 = hook_install(NM_NAMEID, nm_h_nameid, NULL, &g_nm_o_nameid);
        int n2 = hook_install(NM_MSG_HAS, nm_h_msg, (void *)(intptr_t)0, &g_nm_o_msg[0]);
        int n3 = hook_install(NM_MSG_GET, nm_h_msg, (void *)(intptr_t)1, &g_nm_o_msg[1]);
        nm_log("[registry] name text hooks (a manifest has \"display_name\"): name_id getter %s, message label exists %s, message text %s", n1 ? "installed" : "FAILED", n2 ? "installed" : "FAILED",
               n3 ? "installed" : "FAILED");
    }
    nm_log("[registry] runtime hooks: path formatter 0x%llx %s, sv_set_function_hash %s, sv_set_status_func %s, %d of %d other path builders, picture path 0x%llx %s", (unsigned long long)NM_FORMATTER,
           a ? "installed" : "FAILED", b ? "installed" : "FAILED", d ? "installed" : "FAILED", e, (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]), (unsigned long long)NM_PORTRAIT, pt ? "installed" : "FAILED");
    {   /* N12: the fighter's own per-frame routine (nm_h_frame) */
        const char *e = getenv("SSBU_REG_FRAME");
        if (!(e && e[0] == '0'))
            nm_log("[n12] per-frame routine hook on the fighters' frame line: %s",
                   hook_install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", nm_h_frame, NULL, &g_nm_o_frame) ? "installed or waiting for lua2cpp_common" : hook_error());
    }
    atexit(nm_report);
}

static void nm_agent_begin_weapon(const char *name) { nm_agent_begin(name); t_nm_weapon = 1; }      /* N12 */

/* ---------------------------------------------------------------- N13: THE ARTICLE'S OWN AGENT (registry version 4)
 * plan\phases\N13_own_articles.md. A game fighter's module makes its weapon agents in its create_agent_* factories:
 * operator new, the BASE class constructor of the shared module (lua2cpp::L2CWeaponCommon for the status agent,
 * L2CFighterAnimcmdBase for the game / effect / sound agents), a class table of the module whose own entries are the
 * article's install (status agent, slot 9: the sv_set_status_func calls) and the destructors, and one
 * sv_set_function_hash per script (Mario's fireball: tests\n6_s_factories.py list mario). Here the same is done for a
 * NEW fighter's article from its DLL: the class table is a copy of the base class's own table (its destructors are the
 * base's: right for an object the base constructor made), the install slot is the DLL's `<article>__install`, the
 * scripts are the DLL's `<article>__<category prefix><name>` under the hash of `<category prefix><name>`. No other
 * fighter's module is involved. Names: the fighter source's convention, docs in the phase file. */
static uint64_t nm_sym(NewMod *nm, const char *name) {          /* the donor module's import of the name, else main / any loaded module */
    NroModule *dm = nm->dm;
    for (int i = 1; dm && i < dm->sym_count; i++) if (dm->syms[i].name && strcmp(dm->syms[i].name, name) == 0 && dm->bind_addr && dm->bind_addr[i]) return dm->bind_addr[i];
    return hk_address_of(name);
}
static int nm_ovr_index(FsMod *fm, const char *name) {
    for (int i = 0; fm && i < fm->ovr_count; i++) if (strcmp(fm->ovr[i].name, name) == 0) return i;
    return -1;
}
static long g_nm_art_made, g_nm_art_said;
static int nm_article_agent(void *ctx, const char *name, const char *article, int cat) {
    Ctx *c = (Ctx *)ctx;
    NewMod *nm = name ? nm_find(name) : NULL;
    if (!nm || !article || cat < 0 || cat > 3 || !nm_activate(nm)) return 0;
    FsMod *fm = nm->mod.fighter;
    char want[96]; snprintf(want, sizeof want, "%s__install", article);
    int inst = nm_ovr_index(fm, want);
    if (inst < 0) {
        if (g_nm_art_said++ < 4) nm_log("[n13] %s: article %s: the DLL has no %s: the template's agents are kept", nm->module, article, want);
        return 0;
    }
    static const char *const ctor_name[2] = { "_ZN7lua2cpp15L2CWeaponCommonC2EPN3app12BattleObjectEPNS1_26BattleObjectModuleAccessorEP9lua_State",
                                              "_ZN7lua2cpp21L2CFighterAnimcmdBaseC2EPN3app12BattleObjectEPNS1_26BattleObjectModuleAccessorEP9lua_State" };
    static const char *const vt_name[2] = { "_ZTVN7lua2cpp15L2CWeaponCommonE", "_ZTVN7lua2cpp21L2CFighterAnimcmdBaseE" };
    static const uint32_t obj_size[2] = { 0x118, 0xc8 }, nslots[2] = { 15, 9 };     /* Mario's fireball agents: the same sizes and slot counts */
    static uint64_t *table[2][4];                                                     /* [status / animcmd][fighter index]: built once */
    int k = cat ? 1 : 0, fi = (int)(nm - g_nm);
    uint64_t a_new = nm_sym(nm, "_Znwm"), a_ctor = nm_sym(nm, ctor_name[k]), a_vt = nm_sym(nm, vt_name[k]);
    uint64_t a_hash = nm_sym(nm, "_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E");
    if (!a_new || !a_ctor || !a_vt || !a_hash || fi >= 4) {
        if (g_nm_art_said++ < 4) nm_log("[n13] %s: article %s: a game symbol is missing (new %llx, constructor %llx, class table %llx, script registration %llx): the template's agents are kept",
                                         nm->module, article, (unsigned long long)a_new, (unsigned long long)a_ctor, (unsigned long long)a_vt, (unsigned long long)a_hash);
        return 0;
    }
    uint64_t obj = c->x[1], acc = c->x[2], lua = c->x[3], keep[31];
    memcpy(keep, c->x, sizeof keep);
    if (!table[k][fi]) {
        uint64_t *t = calloc(nslots[k] + 2, 8);
        for (uint32_t s = 0; s < nslots[k] + 2; s++) t[s] = rd64(a_vt + 8ull * s);        /* offset to top, type info, the slots */
        if (k == 0) t[2 + 9] = nm->info.base + fm->ovr[inst].off;                           /* slot 9: the article's install (the DLL's) */
        table[k][fi] = t;
        nm_log("[n13] %s: article %s: %s agent class table made from %s (%u slots%s)", nm->module, article, k ? "animation-command" : "status", vt_name[k], nslots[k],
               k ? "" : "; slot 9 = the DLL's install");
    }
    c->x[0] = obj_size[k]; call_indirect(c, a_new);
    uint64_t agent = c->x[0];
    if (!agent) { memcpy(c->x, keep, sizeof keep); return 0; }
    c->x[0] = agent; c->x[1] = obj; c->x[2] = acc; c->x[3] = lua; call_indirect(c, a_ctor);
    wr64(agent, (uint64_t)(uintptr_t)(table[k][fi] + 2));
    nm_agent_set(agent, nm);                                                                /* the agent belongs to the new module */
    int scripts = 0;
    if (k) {
        static const char *const pre[4] = { "", "game_", "effect_", "sound_" };
        size_t la = strlen(article);
        for (int i = 0; i < fm->ovr_count; i++) {
            const char *n = fm->ovr[i].name;
            if (strncmp(n, article, la) != 0 || n[la] != '_' || n[la + 1] != '_' || strncmp(n + la + 2, pre[cat], strlen(pre[cat])) != 0) continue;
            c->x[0] = agent; c->x[1] = nm->info.base + fm->ovr[i].off; c->x[2] = nm_hash40(n + la + 2);
            g_nm_o_fnhash ? g_nm_o_fnhash(c) : call_indirect(c, a_hash);
            scripts++;
        }
    }
    memcpy(c->x, keep, sizeof keep);
    c->x[0] = agent;
    if (g_nm_art_made++ < 12) nm_log("[n13] %s: article %s: OWN %s agent %llx made (base class of the shared module, %s; %d scripts of the DLL registered)", nm->module, article,
                                     cat == 0 ? "status" : cat == 1 ? "game" : cat == 2 ? "effect" : "sound", (unsigned long long)agent, k ? "no install" : "install = the DLL's", scripts);
    return 1;
}
static const SsbuRegistry g_nm_registry = { sizeof(SsbuRegistry), 4, 0, g_nm_list, nm_start, nm_agent_begin, nm_agent_end, nm_owner_set, nm_agent_begin_weapon, nm_article_agent };
__declspec(dllexport) const SsbuRegistry *ssbu_registry(void) {
    static SsbuRegistry r;
    nm_scan();
    r = g_nm_registry;
    r.count = g_nm_count;
    return &r;
}
