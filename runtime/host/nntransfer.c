/* Local transfer-memory objects for the recompiled SDK's in-process audio renderer. */
#include <windows.h>
#include <stdlib.h>
#include "sdklift.h"

#define TM_PREFIX 0x7d000000u
#define TM_ERROR(n) ((uint32_t)((n) * 512u + 1u))
typedef struct Transfer {
    struct Transfer *next;
    uint64_t address, size;
    uint32_t handle, mappings, permission;
    int open;
} Transfer;
typedef struct TransferMap {
    struct TransferMap *next;
    Transfer *memory;
    uint64_t object;
} TransferMap;
static SRWLOCK tm_lock = SRWLOCK_INIT;
static Transfer *tm_objects;
static TransferMap *tm_maps;
static uint32_t tm_serial;

static int tm_writable(uint64_t address, uint64_t size) {
    if (!address || !size || address > UINT64_MAX - size) return 0;
    uint64_t end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)address, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        uint64_t next = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (next <= address) return 0;
        address = next;
    }
    return 1;
}
static Transfer *tm_find(uint32_t handle) {
    for (Transfer *p = tm_objects; p; p = p->next) if (p->handle == handle) return p;
    return NULL;
}
static void tm_collect(Transfer *p) {
    if (p->open || p->mappings) return;
    Transfer **link = &tm_objects;
    while (*link != p) link = &(*link)->next;
    *link = p->next;
    free(p);
}
static uint32_t tm_close(uint32_t handle) {
    Transfer *p = tm_find(handle);
    if (!p || !p->open) return TM_ERROR(114);
    p->open = 0;
    tm_collect(p);
    return 0;
}

int transfer_memory_svc(Ctx *c, uint32_t number) {
    if (number == 0x16 && ((uint32_t)c->x[0] & 0xff000000u) == TM_PREFIX) {
        AcquireSRWLockExclusive(&tm_lock);
        c->x[0] = tm_close((uint32_t)c->x[0]);
        ReleaseSRWLockExclusive(&tm_lock);
        return 1;
    }
    if (number != 0x15) return 0;
    uint64_t address = c->x[1], size = c->x[2], permission = c->x[3];
    uint32_t result = 0, handle = 0;
    if (!address || (address & 0xfff)) result = TM_ERROR(102);
    else if (!size || (size & 0xfff) || address > UINT64_MAX - size) result = TM_ERROR(101);
    else if (permission != 0 && permission != 1 && permission != 3) result = TM_ERROR(108);
    else if (!tm_writable(address, size)) result = TM_ERROR(106);
    AcquireSRWLockExclusive(&tm_lock);
    for (Transfer *p = tm_objects; !result && p; p = p->next)
        if (address < p->address + p->size && p->address < address + size) result = TM_ERROR(106);
    if (!result) {
        Transfer *p = tm_serial < 0xffffffu ? calloc(1, sizeof *p) : NULL;
        if (!p) result = TM_ERROR(103);
        else {
            p->address = address; p->size = size; p->permission = (uint32_t)permission; p->open = 1;
            handle = p->handle = TM_PREFIX | ++tm_serial;
            p->next = tm_objects; tm_objects = p;
        }
    }
    ReleaseSRWLockExclusive(&tm_lock);
    trace_printf("svcCreateTransferMemory(%llx, %llx, %llu) -> %x, result %x\n",
                 (unsigned long long)address, (unsigned long long)size, (unsigned long long)permission, handle, result);
    c->x[0] = result; c->x[1] = handle; /* The SDK SVC wrapper writes w1 to its caller's output. */
    return 1;
}

static uint32_t tm_unmap(uint64_t object) {
    TransferMap **link = &tm_maps;
    while (*link && (*link)->object != object) link = &(*link)->next;
    if (!*link) return rd8(object) == 2 ? TM_ERROR(125) : 0;
    TransferMap *map = *link;
    *link = map->next;
    map->memory->mappings--;
    tm_collect(map->memory);
    free(map);
    wr64(object + 8, 0); wr8(object, 1); wr8(object + 2, 0);
    return 0;
}
static void transfer_Map(Ctx *c) {
    uint64_t out = c->x[0], object = c->x[1], permission = c->x[2];
    trace_printf("MapTransferMemory out=%llx object=%llx permission=%llu\n", (unsigned long long)out, (unsigned long long)object, (unsigned long long)permission);
    if (!tm_writable(out, 8) || !tm_writable(object, 0x40)) { c->x[0] = TM_ERROR(115); return; }
    if (permission != 0 && permission != 1 && permission != 3) { c->x[0] = TM_ERROR(108); return; }
    AcquireSRWLockExclusive(&tm_lock);
    Transfer *p = tm_find(rd32(object + 0x18));
    uint32_t result = !p || !p->open ? TM_ERROR(114) : rd8(object) != 1 ? TM_ERROR(125) :
                      rd64(object + 0x10) != p->size ? TM_ERROR(101) :
                      permission != p->permission || p->mappings ? TM_ERROR(125) : 0;
    TransferMap *map = result ? NULL : malloc(sizeof *map);
    if (!result && !map) result = TM_ERROR(104);
    if (!result) {
        /* shortcut: same-process maps alias the original buffer; cross-process protection needs separate views. */
        map->object = object; map->memory = p; map->next = tm_maps; tm_maps = map; p->mappings++;
        wr64(object + 8, p->address); wr8(object, 2); wr8(object + 2, 0); wr64(out, p->address);
    }
    ReleaseSRWLockExclusive(&tm_lock);
    c->x[0] = result;
}
static void transfer_Unmap(Ctx *c) {
    uint64_t object = c->x[0];
    if (!tm_writable(object, 0x40)) { guest_trap("UnmapTransferMemory: invalid object", object); return; }
    AcquireSRWLockExclusive(&tm_lock);
    uint32_t result = tm_unmap(object);
    ReleaseSRWLockExclusive(&tm_lock);
    if (result) guest_trap("UnmapTransferMemory: no local mapping", object);
}
static void transfer_Destroy(Ctx *c) {
    uint64_t object = c->x[0];
    if (!tm_writable(object, 0x40)) { guest_trap("DestroyTransferMemory: invalid object", object); return; }
    AcquireSRWLockExclusive(&tm_lock);
    uint32_t result = tm_unmap(object);
    if (!result && rd8(object + 1)) result = tm_close(rd32(object + 0x18));
    if (!result) { wr8(object, 0); wr8(object + 1, 0); wr64(object + 8, 0); wr64(object + 0x10, 0); wr32(object + 0x18, 0); }
    ReleaseSRWLockExclusive(&tm_lock);
    if (result) guest_trap("DestroyTransferMemory: invalid local handle", object);
}
void register_transfer_imports(void) {
    sdk_host_override("_ZN2nn2os17MapTransferMemoryEPPvPNS0_18TransferMemoryTypeENS0_16MemoryPermissionE", transfer_Map);
    sdk_host_override("_ZN2nn2os19UnmapTransferMemoryEPNS0_18TransferMemoryTypeE", transfer_Unmap);
    sdk_host_override("_ZN2nn2os21DestroyTransferMemoryEPNS0_18TransferMemoryTypeE", transfer_Destroy);
}
