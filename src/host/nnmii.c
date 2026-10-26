/* nn::mii.
 *
 * LIFTED (sdklift_gen.c, the SDK's own code): CharInfoAccessor / ParamCharInfoAccessor (constructors with
 * ReplaceCharInfoIfNecessary, IsValid, GetNickname and every getter), all colour getters (GetSkinColor,
 * GetCommonColor, GetFavoriteColor, eye/mouth/mole/wrinkle/noseline colours), GetExpressionCount,
 * detail::GetEyeRotateOffset / GetEyebrowRotateOffset. They only read tables inside the SDK image.
 *
 * SERVICE SIDE, answered here: nn::mii::Database is a session with the system's mii service. The service
 * is built from the same nn::mii::detail library that is linked into the SDK module, so the answers are
 * produced by running that library's own code (lifted too):
 *   BuildDefault(charInfo, index)  = StoreDataRaw::BuildDefault(&utility, index)   [sdk] 0x2f34c0
 *                                    + CharInfoRaw::SetFromStoreDataRaw             [sdk] 0x2ec0d0
 *   BuildRandom(charInfo, desc)    = StoreDataRaw::BuildRandom(&utility, desc)      [sdk] 0x2f3820
 *   Convert(charInfo, ver3)        = Ver3StoreDataRaw::IsValid / BuildToStoreData   [sdk] 0x2f76e0 / 0x2fa470
 * (the same sequence suyu's MiiManager uses: mii_manager.cpp BuildDefault / BuildRandom / ConvertV3ToCharInfo).
 * The default-Mii table is the SDK's own (nn::mii::detail::GetDefaultMii, 0xd8 bytes per entry); the
 * self-test compares it with suyu's public RawData::DefaultMii.
 *
 * Console state modelled: an EMPTY Mii database (no user-made Miis), which is what a console has until the
 * user creates one. [doc] suyu mii_types.h: SourceFlag Database = 1, Default = 2; 6 default Miis.
 * Result codes [doc] suyu mii_result.h (module 126): InvalidArgument 1, InvalidArgumentSize 2,
 * InvalidCharInfo 100; [sdk] 0x301040 AppletOutput::GetResult: cancelled = 0xe7e (module 126, description 7),
 * which [game] 0x71032e88ac compares against.
 * NOT VERIFIED on a console: the Mii author id (a per-console random UUID in system settings; a fixed
 * value is used here, it only feeds the store data's device CRC, which CharInfo does not carry). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

#define MII_MODULE 126
#define MII_RESULT(desc) ((uint32_t)(MII_MODULE | ((desc) << 9)))
#define MII_CHARINFO_SIZE 0x58
#define MII_STOREDATA_SIZE 0x44
#define MII_DEFAULT_COUNT_ 6
#define SOURCE_DATABASE 1
#define SOURCE_DEFAULT 2

static ImportFn L_UtilityCtor, L_BuildDefault, L_BuildRandom, L_SetFromStoreData, L_Ver3IsValid, L_Ver3ToStoreData;

static ImportFn need(const char *name) {
    ImportFn f = sdk_lifted(name);
    if (!f) { printf("nnmii: SDK function %s was not lifted (sdklift_roots.txt) or sdk.elf is missing\n", name); report_and_exit(3); }
    return f;
}
static void mii_bind(void) {
    if (L_UtilityCtor) return;
    L_BuildDefault = need("_ZN2nn3mii6detail12StoreDataRaw12BuildDefaultEPNS1_11UtilityImplEi");
    L_BuildRandom = need("_ZN2nn3mii6detail12StoreDataRaw11BuildRandomEPNS1_11UtilityImplERKNS0_10RandomDescE");
    L_SetFromStoreData = need("_ZN2nn3mii6detail11CharInfoRaw19SetFromStoreDataRawERKNS1_12StoreDataRawE");
    L_Ver3IsValid = need("_ZNK2nn3mii6detail16Ver3StoreDataRaw7IsValidEv");
    L_Ver3ToStoreData = need("_ZNK2nn3mii6detail16Ver3StoreDataRaw16BuildToStoreDataEPNS1_12StoreDataRawEPNS1_11UtilityImplE");
    L_UtilityCtor = need("_ZN2nn3mii6detail11UtilityImplC1Ev");
}

/* [sdk] 0x352a90 nn::settings::system::GetMiiAuthorId(nn::util::Uuid *out): 16 bytes from the settings service. */
static void settings_GetMiiAuthorId(Ctx *c) {
    static const uint8_t id[16] = {0x53, 0x53, 0x42, 0x55, 0x6e, 0x61, 0x40, 0x74, 0x89, 0x69, 0x76, 0x65, 0x4d, 0x69, 0x69, 0x01};
    memcpy((void *)(uintptr_t)A0, id, 16);
    RET(0);
}

/* close5: nn::settings::system::GetRegionCode(RegionCode *out) [sdk 0x353420]: one settings-service call, the 32-bit code is
 * stored at *out (void function; aborts on a service failure). The code is a CONSOLE SETTING (suyu set/settings_types.h:230
 * SystemRegionCode: Japan 0, Usa 1, Europe 2, Australia 3, HongKongTaiwanKorea 4, China 5; system_settings_server.cpp returns
 * the configured value). The port has no console setting, so, like the language, it comes from Windows: the user's
 * geographic location (GetUserDefaultGeoName, ISO 3166 code) mapped to the sales region below; SSBU_REGION=<0..5> overrides.
 * A country that is not in the table stops with a message instead of a guessed region. The mapping is the port's decision. */
static int host_region_code(char *iso_out) {
    static const char *const map[] = {
        "0JP",
        "1US", "1CA", "1MX", "1BR", "1AR", "1CL", "1CO", "1PE",
        "3AU", "3NZ",
        "4HK", "4TW", "4KR",
        "5CN",
        "2IT", "2FR", "2DE", "2ES", "2GB", "2IE", "2PT", "2NL", "2BE", "2LU", "2AT", "2CH", "2SE", "2NO", "2DK", "2FI", "2PL", "2CZ", "2SK",
        "2HU", "2GR", "2RO", "2BG", "2HR", "2SI", "2EE", "2LV", "2LT", "2MT", "2CY", "2IS", "2RU", "2ZA", NULL };
    const char *ov = getenv("SSBU_REGION");
    WCHAR w[8] = {0};
    char iso[4] = "??";
    if (GetUserDefaultGeoName(w, 8) >= 3) { iso[0] = (char)w[0]; iso[1] = (char)w[1]; iso[2] = 0; }
    if (iso_out) memcpy(iso_out, iso, 3);
    if (ov && ov[0] >= '0' && ov[0] <= '5' && !ov[1]) return ov[0] - '0';
    for (int i = 0; map[i]; i++) if (map[i][1] == iso[0] && map[i][2] == iso[1]) return map[i][0] - '0';
    return -1;
}
static void settings_GetRegionCode(Ctx *c) {
    char iso[4];
    int r = host_region_code(iso);
    if (r < 0) {
        printf("nn::settings::system::GetRegionCode: Windows location \"%s\" has no region in the port's table: set SSBU_REGION=0..5 (Japan, Americas, Europe, Australia, HK/TW/KR, China)\n", iso);
        guest_trap("nn::settings::system::GetRegionCode: no region for this Windows location (set SSBU_REGION)", c->x[30] - 4);
    }
    trace_printf("nn::settings::system::GetRegionCode -> %d (Windows location %s)\n", r, iso);
    wr32(A0, (uint32_t)r);
}

/* The work objects live on the host stack: guest pointers are host pointers. */
typedef struct { uint8_t utility[0x20]; uint8_t store[0x60]; } MiiWork;   /* [sdk] UtilityImpl 0x10 bytes, StoreDataRaw 0x44 */

static void build_default(Ctx *c, uint64_t out, int index) {
    MiiWork w;
    memset(&w, 0, sizeof w);
    mii_bind();
    c->x[0] = P(w.utility); L_UtilityCtor(c);
    c->x[0] = P(w.store); c->x[1] = P(w.utility); c->x[2] = (uint64_t)index; L_BuildDefault(c);
    c->x[0] = out; c->x[1] = P(w.store); L_SetFromStoreData(c);
}

/* [sdk] 0x2fc230: Database() stores a null service pointer at +0. */
static void mii_Database_ctor(Ctx *c) { wr64(A0, 0); }
static void mii_Database_dtor(Ctx *c) { (void)c; }
static void mii_Database_Initialize(Ctx *c) { RET(0); }

/* Get(this, int *outCount, CharInfo *out, int max, int sourceFlag) -> Result. [doc] suyu MiiManager::Get:
 * database entries first (none), then the default Miis when the Default flag is set; a buffer that is too
 * small gives InvalidArgumentSize with the entries that fitted. */
static void mii_Database_GetImpl(Ctx *c, int elements) {
    uint64_t out_count = A1, out = A2;
    int max = (int)A3, flag = (int)A4, count = 0;
    uint32_t result = 0;
    if (flag & SOURCE_DEFAULT)
        for (int i = 0; i < MII_DEFAULT_COUNT_; i++) {
            if (count >= max) { result = MII_RESULT(2); break; }
            uint64_t item = out + (uint64_t)count * (MII_CHARINFO_SIZE + (elements ? 4 : 0));
            build_default(c, item, i);
            if (elements) wr32(item + MII_CHARINFO_SIZE, 1); /* Source::Default, distinct from SourceFlag::Default. */
            count++;
        }
    if (out_count) wr32(out_count, (uint32_t)count);
    RET(result);
}
static void mii_Database_Get(Ctx *c) { mii_Database_GetImpl(c, 0); }
static void mii_Database_GetElements(Ctx *c) { mii_Database_GetImpl(c, 1); }
static void mii_Database_GetCount(Ctx *c) { RET(((int)A1 & SOURCE_DEFAULT) ? MII_DEFAULT_COUNT_ : 0); }
static void mii_Database_IsUpdated(Ctx *c) { RET(0); }    /* the database never changes */

/* BuildDefault(this, CharInfo *out, int index). [sdk] 0x2fc5e0: the SDK aborts when the service refuses,
 * which it does for index >= 6 ([doc] suyu IDatabaseService::BuildDefault: InvalidArgument). */
static void mii_Database_BuildDefault(Ctx *c) {
    int index = (int)A2;
    if (index < 0 || index >= MII_DEFAULT_COUNT_) {
        printf("\nnn::mii::Database::BuildDefault: index %d is out of range (the SDK aborts here)\n", index);
        guest_trap("nn::mii::Database::BuildDefault with an invalid index", c->x[30] - 4);
    }
    build_default(c, A1, index);
    RET(0);
}
/* BuildRandom(this, CharInfo *out, const RandomDesc &desc { int age, gender, race }) [sdk] 0x2fc574. */
static void mii_Database_BuildRandom(Ctx *c) {
    uint64_t out = A1, desc = A2;
    MiiWork w;
    memset(&w, 0, sizeof w);
    mii_bind();
    c->x[0] = P(w.utility); L_UtilityCtor(c);
    c->x[0] = P(w.store); c->x[1] = P(w.utility); c->x[2] = desc; L_BuildRandom(c);
    c->x[0] = out; c->x[1] = P(w.store); L_SetFromStoreData(c);
    RET(0);
}
/* Convert(this, CharInfo *out, const Ver3StoreData &) -> Result. [doc] suyu ConvertV3ToCharInfo. */
static void mii_Database_Convert(Ctx *c) {
    uint64_t out = A1, ver3 = A2;
    MiiWork w;
    memset(&w, 0, sizeof w);
    mii_bind();
    c->x[0] = ver3; L_Ver3IsValid(c);
    if (!(c->x[0] & 1)) { RET(MII_RESULT(100)); return; }
    c->x[0] = P(w.utility); L_UtilityCtor(c);
    c->x[0] = ver3; c->x[1] = P(w.store); c->x[2] = P(w.utility); L_Ver3ToStoreData(c);
    c->x[0] = out; c->x[1] = P(w.store); L_SetFromStoreData(c);
    RET(0);
}

/* AppendMii(int *outIndex): the Mii edit applet in "append" mode. Nothing can be made here: the user
 * closes the applet. [sdk] 0x300bc4..0x300be4 + 0x301040: Result 0xe7e, *outIndex untouched.
 * Read in close4: [sdk] 0x300b90 builds an AppletInput (mode 1) and calls detail::StartImpl (0x3015b0): library applet
 * 0x12, 0x100 bytes in, the applet's exit reason mapped through the table at 0xa79858 (0xe7e for the non-normal ones),
 * 0x20 bytes out = { u32 result (0 done, 1 cancelled -> 0xe7e, 0x301030), s32 index }; only on success is *outIndex
 * written (0x300be4). [game] 0x71032e88a4 treats exactly 0xe7e as "no Mii was added".
 * STILL NOT REAL: the editor itself (the user making a Mii) and a writable Mii database do not exist in this port;
 * "cancelled" is the one outcome of the real applet that needs neither. [doc] suyu am/frontend/applet_mii_edit.cpp
 * instead adds a random Mii and reports success: an emulator stand-in, not the console's behaviour. */
static void mii_AppendMii(Ctx *c) {
    printf("nn::mii::AppendMii: the Mii edit applet is not available; answered as cancelled\n");
    trace_printf("     nn::mii::AppendMii -> cancelled (0xe7e)\n");
    RET(MII_RESULT(7));
}

#define DB(n) "_ZN2nn3mii8Database" n
static const ImportDef mii_imports[] = {
    {DB("C1Ev"), mii_Database_ctor}, {DB("D1Ev"), mii_Database_dtor}, {DB("10InitializeEv"), mii_Database_Initialize},
    {DB("3GetEPiPNS0_15CharInfoElementEii"), mii_Database_GetElements},
    {DB("3GetEPiPNS0_8CharInfoEii"), mii_Database_Get}, {"_ZNK2nn3mii8Database8GetCountEi", mii_Database_GetCount},
    {DB("9IsUpdatedEi"), mii_Database_IsUpdated}, {"_ZNK2nn3mii8Database12BuildDefaultEPNS0_8CharInfoEi", mii_Database_BuildDefault},
    {DB("11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE"), mii_Database_BuildRandom},
    {DB("7ConvertEPNS0_8CharInfoERKNS0_13Ver3StoreDataE"), mii_Database_Convert},
    {"_ZN2nn3mii9AppendMiiEPi", mii_AppendMii},
};

void register_nnmii_imports(void) {
    sdk_host_override("_ZN2nn8settings6system14GetMiiAuthorIdEPNS_4util4UuidE", settings_GetMiiAuthorId);
    sdk_host_override("_ZN2nn8settings6system13GetRegionCodeEPNS1_10RegionCodeE", settings_GetRegionCode);   /* close5 */
    if (!sdk_image_ready()) return;                       /* without sdk.elf the earlier placeholders stay */
    int n = (int)(sizeof mii_imports / sizeof mii_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(mii_imports[i].name)) printf("nnmii: \"%s\" is not an import of the game (typo?)\n", mii_imports[i].name);
    register_imports(mii_imports, n);
}

/* ---------------------------------------------------------------- self-test (SSBU_SDK_SELFTEST) */
#include "sdk_reference.h"
int nnmii_selftest(Ctx *c) {
    int bad = 0;
    if (!sdk_image_ready()) { printf("  mii: sdk.elf not loaded: SKIPPED\n"); return 1; }
    /* 1. the SDK's default-Mii table against suyu's public one */
    ImportFn get_default = need("_ZN2nn3mii6detail13GetDefaultMiiEi");
    int raw_bad = 0;
    for (int i = 0; i < 6; i++) {
        c->x[0] = (uint64_t)i; get_default(c);
        const uint8_t *p = (const uint8_t *)(uintptr_t)c->x[0];
        if (memcmp(p, mii_default_raw[i], 49 * 4) || memcmp(p + 49 * 4, mii_default_nick[i], 20)) raw_bad++;
    }
    printf("  mii: SDK default-Mii table vs suyu RawData::DefaultMii (6 x 0xd8 bytes): %s\n", raw_bad ? "DIFFERENT" : "identical");
    bad += raw_bad;
    /* 2. Database::Get / BuildDefault and every accessor against values computed from the public table */
    uint8_t *db = calloc(1, 0x40), *infos = calloc(1, 8 * MII_CHARINFO_SIZE + 64), *accessor = calloc(1, 0x100);
    int count = -1;
    memset(infos, 0xa5, 8 * MII_CHARINFO_SIZE + 64);
    c->x[0] = P(db); mii_Database_ctor(c);
    c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(infos); c->x[3] = 8; c->x[4] = SOURCE_DATABASE; mii_Database_Get(c);
    int ok = c->x[0] == 0 && count == 0;
    c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(infos); c->x[3] = 8; c->x[4] = SOURCE_DATABASE | SOURCE_DEFAULT; mii_Database_Get(c);
    ok = ok && c->x[0] == 0 && count == 6;
    for (int i = 0; i < 64; i++) if (infos[6 * MII_CHARINFO_SIZE + i] != 0xa5) ok = 0;      /* nothing past 6 x 0x58 */
    c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(infos + 7 * MII_CHARINFO_SIZE - 0x58 * 7); c->x[3] = 4; c->x[4] = SOURCE_DEFAULT; mii_Database_Get(c);
    ok = ok && c->x[0] == MII_RESULT(2) && count == 4;
    c->x[0] = P(db); c->x[1] = SOURCE_DEFAULT; mii_Database_GetCount(c);
    ok = ok && c->x[0] == 6;
    printf("  mii: Database::Get / GetCount (empty database, 6 defaults, short buffer): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    ImportFn ctor = need("_ZN2nn3mii21ParamCharInfoAccessorC1ERKNS0_8CharInfoE"), ctor2 = need("_ZN2nn3mii16CharInfoAccessorC1ERKNS0_8CharInfoE");
    ImportFn valid = need("_ZNK2nn3mii21ParamCharInfoAccessor7IsValidEv"), valid2 = need("_ZNK2nn3mii16CharInfoAccessor7IsValidEv");
    ImportFn nick = need("_ZNK2nn3mii16CharInfoAccessor11GetNicknameEPNS0_8NicknameEi");
    int mismatches = 0, checked = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t info[MII_CHARINFO_SIZE + 16], copy[MII_CHARINFO_SIZE];
        memset(info, 0x5a, sizeof info);
        c->x[0] = P(db); c->x[1] = P(info); c->x[2] = (uint64_t)i; mii_Database_BuildDefault(c);
        for (int k = 0; k < 16; k++) if (info[MII_CHARINFO_SIZE + k] != 0x5a) { printf("  mii: BuildDefault wrote past 0x58 bytes\n"); bad++; break; }
        memcpy(copy, info, sizeof copy);
        for (int a = 0; a < MII_ACCESSOR_COUNT; a++) {
            int is_param = strstr(mii_accessor_names[a], "ParamCharInfoAccessor") != NULL;
            memset(accessor, 0, 0x100);
            c->x[0] = P(accessor); c->x[1] = P(info); (is_param ? ctor : ctor2)(c);
            c->x[0] = P(accessor); (is_param ? valid : valid2)(c);
            if (!(c->x[0] & 1)) { mismatches++; printf("  mii: default %d: accessor says invalid\n", i); break; }
            c->x[0] = P(accessor); need(mii_accessor_names[a])(c);
            checked++;
            if ((uint32_t)c->x[0] != mii_accessor_expect[i][a]) {
                mismatches++;
                printf("  mii: default %d %s = %u, expected %u\n", i, mii_accessor_names[a], (unsigned)c->x[0], mii_accessor_expect[i][a]);
            }
        }
        if (memcmp(copy, info, sizeof copy)) { printf("  mii: accessor constructor modified the caller's CharInfo\n"); bad++; }
        uint16_t name[16];
        memset(name, 0xff, sizeof name);
        memset(accessor, 0, 0x100);
        c->x[0] = P(accessor); c->x[1] = P(info); ctor2(c);
        c->x[0] = P(accessor); c->x[1] = P(name); c->x[2] = 0; nick(c);
        if (memcmp(name, mii_default_nick[i], 20)) { mismatches++; printf("  mii: default %d: nickname differs\n", i); }
    }
    printf("  mii: BuildDefault + %d accessor values of the 6 default Miis vs the public tables: %d mismatches\n", checked, mismatches);
    bad += mismatches;
    /* 3. BuildRandom gives a CharInfo the SDK's own validator accepts; colour getters return finite colours */
    int rnd_bad = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t info[MII_CHARINFO_SIZE];
        uint32_t desc[4] = {(uint32_t)(i % 4), (uint32_t)(i % 3), (uint32_t)(i % 4), 0};      /* 3 = "all" for each */
        c->x[0] = P(db); c->x[1] = P(info); c->x[2] = P(desc); mii_Database_BuildRandom(c);
        memset(accessor, 0, 0x100);
        c->x[0] = P(accessor); c->x[1] = P(info); ctor(c);
        c->x[0] = P(accessor); valid(c);
        if (!(c->x[0] & 1)) rnd_bad++;
    }
    printf("  mii: 20 x BuildRandom validated by the SDK's CharInfo verifier: %d invalid\n", rnd_bad);
    bad += rnd_bad;
    free(db); free(infos); free(accessor);
    {   /* close5: region code boundary: writes one 32-bit code 0..5 at *out and nothing beyond it */
        char iso[4]; uint32_t out[2] = {0xaaaaaaaau, 0xbbbbbbbbu};
        int want = host_region_code(iso), rbad = 0;
        if (want >= 0) {
            c->x[0] = (uint64_t)(uintptr_t)out; settings_GetRegionCode(c);
            rbad = !(out[0] == (uint32_t)want && out[0] <= 5 && out[1] == 0xbbbbbbbbu);
        }
        printf("  mii / settings: GetRegionCode for Windows location %s -> %d%s: %s\n", iso, want, want < 0 ? " (not in the table: the call would stop)" : "", rbad ? "FAILED" : "ok");
        bad += rbad;
    }
    return bad;
}

int nnmii_elements_selftest(Ctx *c) {
    {
        const char *names[]={
            "_ZN2nn3mii6detail25CalculateSizeAndAlignmentC1Ev",
            "_ZN2nn3mii6detail25CalculateSizeAndAlignment7AddSizeEm",
            "_ZN2nn3mii6detail25CalculateSizeAndAlignment20AddSizeWithAlignmentEmm",
            "_ZN2nn3mii6detail25CalculateSizeAndAlignment20AddSizeWithAlignmentEmmi"};
        uint64_t actual[3]={0x1122334455667788ULL,0,0x8877665544332211ULL},expected=0;
        for(unsigned i=0;i<4;i++){
            ImportFn host=import_lookup(names[i]),sdk=sdk_lifted(names[i]);
            if(!host || !sdk)return 1;
            c->x[0]=P(actual+1);c->x[1]=23;c->x[2]=16;c->x[3]=3;host(c);
            c->x[0]=P(&expected);c->x[1]=23;c->x[2]=16;c->x[3]=3;sdk(c);
            if(actual[1]!=expected || actual[0]!=0x1122334455667788ULL || actual[2]!=0x8877665544332211ULL)return 1;
        }
        puts("Mii size calculator: eight-byte ABI and original SDK results passed");
    }
    {
        const char *names[]={
            "_ZN2nn3mii6detail15BufferAllocatorC1EPvm",
            "_ZN2nn3mii6detail15BufferAllocator7AlignUpEm",
            "_ZN2nn3mii6detail15BufferAllocator7AdvanceEm"};
        uint64_t actual[5]={0x1122334455667788ULL,0,0,0,0x8877665544332211ULL},expected[3]={0};
        for(unsigned i=0;i<3;i++){
            ImportFn host=import_lookup(names[i]),sdk=sdk_lifted(names[i]);
            if(!host || !sdk)return 1;
            c->x[0]=P(actual+1);c->x[1]=i==0?0x1231:(i==1?16:37);c->x[2]=4096;host(c);
            c->x[0]=P(expected);c->x[1]=i==0?0x1231:(i==1?16:37);c->x[2]=4096;sdk(c);
            if(memcmp(actual+1,expected,sizeof expected) || actual[0]!=0x1122334455667788ULL || actual[4]!=0x8877665544332211ULL)return 1;
        }
        puts("Mii buffer allocator: 24-byte ABI and original SDK results passed");
    }
    {
        uint8_t elements[6 * 0x5c + 16], infos[6 * MII_CHARINFO_SIZE], accessor[0x100];
        uint8_t db[0x40] = {0}; int count = -1;
        c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(infos); c->x[3] = 6; c->x[4] = SOURCE_DEFAULT;
        mii_Database_Get(c);
        ImportFn get = import_lookup("_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii");
        int good = get == mii_Database_GetElements;
        memset(elements, 0xa5, sizeof elements); count = -1;
        c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(elements); c->x[3] = 6; c->x[4] = SOURCE_DATABASE;
        mii_Database_GetElements(c);
        good = good && !c->x[0] && count == 0 && elements[0] == 0xa5;
        c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(elements); c->x[3] = 6; c->x[4] = SOURCE_DEFAULT;
        mii_Database_GetElements(c);
        good = good && !c->x[0] && count == 6;
        /* Each original BuildDefault call generates a fresh UUID in the first 16 bytes. */
        for (int i = 0; i < 6; i++) good = good && !memcmp(elements + i * 0x5c + 16, infos + i * MII_CHARINFO_SIZE + 16, MII_CHARINFO_SIZE - 16) && rd32(P(elements) + i * 0x5c + MII_CHARINFO_SIZE) == 1;
        for (int i = 6 * 0x5c; i < sizeof elements; i++) good = good && elements[i] == 0xa5;
        memset(elements, 0xa5, sizeof elements);
        c->x[0] = P(db); c->x[1] = P(&count); c->x[2] = P(elements); c->x[3] = 2; c->x[4] = SOURCE_DEFAULT;
        mii_Database_GetElements(c);
        good = good && c->x[0] == MII_RESULT(2) && count == 2;
        for (int i = 2 * 0x5c; i < sizeof elements; i++) good = good && elements[i] == 0xa5;
        ImportFn ctor = need("_ZN2nn3mii16CharInfoAccessorC1ERKNS0_8CharInfoE");
        ImportFn valid = need("_ZNK2nn3mii16CharInfoAccessor7IsValidEv");
        for (int i = 0; i < 6; i++) {
            memset(accessor, 0, sizeof accessor);
            c->x[0] = P(accessor); c->x[1] = P(infos + i * MII_CHARINFO_SIZE); ctor(c);
            c->x[0] = P(accessor); valid(c);
            if (!(c->x[0] & 1)) good = 0;
        }
        printf("  mii: CharInfoElement stride, source, empty database and bounds: %s\n", good ? "ok" : "FAILED");
        return good ? 0 : 1;
    }
}
