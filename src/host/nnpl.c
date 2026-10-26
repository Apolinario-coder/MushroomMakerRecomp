/* nn::pl: the system's shared fonts, read from plain TrueType files on the host.
 *
 * User fonts are optional. Open Sans, Source Sans Pro and the Yuzu button font are the bundled fallback.
 *
 * SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExtension, 3 ChineseTraditional,
 *                 4 Korean, 5 NintendoExtension. SharedFontLoadState: 0 Loading, 1 Loaded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define FONT_TYPES 7

static const char *const font_files[FONT_TYPES] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static struct { void *data; size_t size; int tried; } g_fonts[FONT_TYPES];
static CRITICAL_SECTION g_lock;

static int read_font(unsigned type,const char *path){
    FILE *file=fopen(path,"rb");if(!file)return 0;
    if(fseek(file,0,SEEK_END)){fclose(file);return 0;}long size=ftell(file);
    if(size<12 || size>32*1024*1024 || fseek(file,0,SEEK_SET)){fclose(file);return 0;}
    unsigned char *data=_aligned_malloc((size_t)size+16,4096);
    int ok=data && fread(data,1,(size_t)size,file)==(size_t)size;
    fclose(file);
    ok=ok && (!memcmp(data,"\0\1\0\0",4) || !memcmp(data,"OTTO",4));
    if(!ok){if(data)_aligned_free(data);return 0;}
    memset(data+size,0,16);g_fonts[type].data=data;g_fonts[type].size=(size_t)size;return 1;
}

static void load_font(unsigned type) {
    if (type >= FONT_TYPES) return;
    EnterCriticalSection(&g_lock);
    if (!g_fonts[type].tried) {
        g_fonts[type].tried = 1;
        char path[MAX_PATH];
        const char *dir = getenv("SSBU_FONTS");
        snprintf(path, sizeof path, "%s\\%s", dir ? dir : "..\\sysdata\\fonts", font_files[type]);
        read_font(type,path);
        if(!g_fonts[type].data){
            const char *open=getenv("SSBU_OPEN_FONTS");
            if(open){snprintf(path,sizeof path,"%s\\%s",open,font_files[type]);read_font(type,path);}
        }
        if(!g_fonts[type].data){
            GetModuleFileNameA(NULL,path,sizeof path);char *slash=strrchr(path,'\\');
            if(slash){snprintf(slash+1,sizeof path-(size_t)(slash+1-path),"fonts_open\\%s",font_files[type]);read_font(type,path);}
        }
        printf("nn::pl: shared font %u %s (%s, %zu bytes)\n", type, g_fonts[type].data ? "loaded" : "NOT FOUND", path, g_fonts[type].size);
        if (!g_fonts[type].data) {
            printf("nn::pl: bundled replacement font missing or invalid; restore fonts_open from the release package\n");
            fflush(stdout);
            report_and_exit(7);
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void pl_RequestSharedFontLoad(Ctx *c) { load_font((unsigned)A0); }
static void pl_GetSharedFontLoadState(Ctx *c) { load_font((unsigned)A0); RET(1); }
static void pl_GetSharedFontAddress(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? (uint64_t)(uintptr_t)g_fonts[t].data : 0); }
static void pl_GetSharedFontSize(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? g_fonts[t].size : 0); }
static void pl_IsVrMode(Ctx *c) { RET(0); }
static void nothing(Ctx *c) { (void)c; }

static const ImportDef pl_imports[] = {
    {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", pl_RequestSharedFontLoad},
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetSharedFontLoadState},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetSharedFontAddress},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSharedFontSize},
    {"_ZN2nn2pl8IsVrModeEv", pl_IsVrMode}, {"_ZN2nn2pl11BeginVrModeEv", nothing}, {"_ZN2nn2pl9EndVrModeEv", nothing},
};

void register_pl_imports(void) {
    InitializeCriticalSection(&g_lock);
    register_imports(pl_imports, (int)(sizeof pl_imports / sizeof pl_imports[0]));
}
