// registry.h - the fighter registry of the native port (plan\phases\N5_own_module.md, docs\NEW_FIGHTER.md).
//
// With SSBU_CHARACTERS=<folder> the runtime reads <folder>\<name>\fighter.json of every sub-folder at start; each
// manifest is one NEW fighter. The runtime itself answers the game's request for the script module lua2cpp_<name>
// (fighter_sdk\newmodule_host.c); the components that own the engine's per-kind tables (still built as DLLs with
// hook.h: the kind component and the roster component, in the folder registry_bin) ask the runtime for the list:
//
//     const SsbuRegistry *reg = ssbu_registry_get();      // NULL in an exe without the registry
//
// Contains nothing of the game.
#pragma once
#include <stdint.h>

typedef struct SsbuRegFighter {
    char name[32];                 // lower-case name: kind hash "fighter_kind_<name>", module "lua2cpp_<name>", roster id "ui_chara_<name>"
    char donor[32];                // the existing fighter that supplies what the folder does not bring
    char dll[64];                  // file name of the script DLL in the fighter's folder ("" = none)
    char folder[260];              // the fighter's folder
    int kind;                      // the fighter kind number given to it (0x5E, 0x5F, ...)
    int own_name;                  // 1: the engine's name row is the fighter's own name (own module, own file paths)
} SsbuRegFighter;

typedef struct SsbuRegistry {
    uint32_t size, version;        // sizeof(SsbuRegistry), 1
    int count;
    const SsbuRegFighter *fighters;
    // install the runtime's own hooks (module request, script registration); call once from mod_init
    void (*start)(void);
    // A factory of the DONOR's script module is about to create an agent for the new fighter `name` on this thread ...
    void (*agent_begin)(const char *name);
    // ... and has returned `agent`. From here on the agent belongs to the new fighter's module: its class table and
    // every script it registers are addresses of lua2cpp_<name>. name == NULL: `agent` is an ordinary fighter's
    // (an address that belonged to a new fighter's agent earlier is forgotten).
    void (*agent_end)(const char *name, uint64_t agent);
    // version 2 (N8; check size before use): the new fighter `name` is being set up on this thread (its fighter init
    // runs), NULL = no longer. While it is set, a file path the engine builds with the DONOR's name (the paths of the
    // article models come from the donor's article list) is answered with the same path under the fighter's own name
    // when the fighter's folder has a row for it.
    void (*owner_set)(const char *name);
    // version 3 (N12; check size before use): as agent_begin, for an agent of an ARTICLE (weapon) of the new fighter
    // `name`. The fighter's own script names (registered by name for its fighter agents) are not given to these.
    void (*agent_begin_weapon)(const char *name);
    // version 4 (N13; check size before use): THE ARTICLE'S OWN AGENT. The engine wants agent `category` (0 status
    // script, 1 game, 2 effect, 3 sound: the four weapon agent dispatchers in that order) for a weapon of the new
    // fighter `name`'s own article `article`. ctx is the game thread's register context with the factory arguments
    // (x1 battle object, x2 module accessor, x3 lua state). When the fighter's DLL has the function
    // `<article>__install` the runtime makes the agent itself from the engine's base class (no other fighter's module
    // is involved): the status agent's install is that function, the animation-command agents get the DLL's scripts
    // `<article>__<game_|effect_|sound_|expression_><name>` under the hash of `<prefix><name>`. Returns 1 with the
    // agent in x0, or 0 (the DLL has nothing for this article: the caller keeps the template's agent).
    int (*article_agent)(void *ctx, const char *name, const char *article, int category);
} SsbuRegistry;

#ifdef _WIN32
#include <windows.h>
static inline const SsbuRegistry *ssbu_registry_get(void) {
    typedef const SsbuRegistry *(*Fn)(void);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleA(NULL), "ssbu_registry");
    return f ? f() : NULL;
}
#endif
