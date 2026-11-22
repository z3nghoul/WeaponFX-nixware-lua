#include "engine.h"
#include "log.h"
#include "material.h"
#include "patch.h"
#include "../core/weapon_fx.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace Engine {
namespace {

using WeaponFx::kTargets;
using WeaponFx::kWeapon;
using WeaponFx::kGloves;

// scenesystem!RenderObjects(?, ?, items, count, ?, passContext, ?). Its first
// 15 bytes (mov rax,rsp; two stores; push rbx/rsi/r15) have no RIP-relative
// operand, so they replay from the trampoline as they are.
constexpr short kSigRenderObjects[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, -1, -1, -1, -1,
    0x49, 0x63, 0xF1, 0x49, 0x8B, 0xD8, 0x4C, 0x8B, 0xFA, 0x83, 0xFE, 0x01 };
constexpr size_t kPatchLen = 15;   // 14 for the jump, ending mid `push r15`, so one more

// Engine layouts (2026-10 builds). Every read is guarded; a field that moved
// reads as "not ours", and the call goes through untouched.
constexpr size_t kItemSobj = 0x18;      // scene object (one per model)
constexpr size_t kItemMat = 0x20;       // CMaterial2*
constexpr size_t kItemMat2 = 0x28;      // the resolved variant the draw takes its render state from
constexpr size_t kItemColor = 0x50;     // RGBA8 tint
constexpr size_t kMatParamBlock = 0x20;
constexpr size_t kParamShaderName = 0x30;
constexpr size_t kPassLflags = 0x48;    // pass context -> layer flags
constexpr size_t kPassViewBits = 0x480; // pass context -> view flags
constexpr uint64_t kViewIsViewmodel = 0x100;
constexpr uint32_t kLflagDepth = 0x1000000;
constexpr size_t kDefaultStride = 0x70;
constexpr int kMaxItems = 4096;         // a corrupt count is a refusal, not a walk off the array
constexpr char kWeaponShader[] = "csgo_weapon.vfx";
constexpr char kCharacterShader[] = "csgo_character.vfx";
// The gloves' materials and models live under .../arms/glove_<name>/ (base
// game, 2026-10). Arms and sleeves are csgo_character as well, in other
// folders. One that moved out of this folder is a glove no longer treated as
// one - the effect is gone from it, nothing else changes.
constexpr char kGloveFolder[] = "/arms/glove_";
constexpr uint32_t kMarkerColor = 0xFFFFFFFFu;

Detour g_Det;


bool g_Attached = false;
size_t g_Stride = kDefaultStride;
std::atomic<int> g_InHook{0};
std::atomic<int> g_Mode[kTargets] = {};   // Mode, per target (Off = 0)
std::atomic<unsigned> g_Calls{0}, g_WeaponItems{0}, g_GloveItems{0}, g_Stripped{0}, g_NoRoom{0};

// Scratch for the rewritten item lists. RenderObjects runs on several
// scenesystem workers at once; a slot is claimed per call and released when
// the original returns (it consumes the list before returning). An overlay
// call needs two lists at once, so it claims two.
constexpr int kScratchSlots = 8;
constexpr size_t kScratchBytes = 0x40000;
uint8_t* g_Scratch[kScratchSlots] = {};
std::atomic<uint32_t> g_ScratchBusy{0};

int ClaimScratch() {
    for (int i = 0; i < kScratchSlots; ++i) {
        const uint32_t bit = 1u << i;
        uint32_t cur = g_ScratchBusy.load(std::memory_order_relaxed);
        while (!(cur & bit)) {
            if (g_ScratchBusy.compare_exchange_weak(cur, cur | bit, std::memory_order_acquire)) {
                if (!g_Scratch[i])
                    g_Scratch[i] = (uint8_t*)VirtualAlloc(nullptr, kScratchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_Scratch[i]) return i;
                g_ScratchBusy.fetch_and(~bit, std::memory_order_release);
                return -1;
            }
        }
    }
    return -1;
}
void ReleaseScratch(int i) {
    if (i >= 0) g_ScratchBusy.fetch_and(~(1u << i), std::memory_order_release);
}

bool ReadPtr(uintptr_t at, void** out) { return ReadMem(at, out, sizeof(*out)); }

// The material's shader, as a class: 1 csgo_weapon, 2 csgo_character, 0 other.
// Cached by material pointer (direct mapped, the low two bits carry the class;
// materials are heap objects, 16-aligned), so the string is read once per
// material, not per item per pass.
constexpr int kCacheSize = 512;
constexpr uintptr_t kClassBits = 3;
enum ShaderClass : uintptr_t { kOther = 0, kWeaponShaderClass = 1, kCharacterShaderClass = 2 };
std::atomic<uintptr_t> g_ShaderCache[kCacheSize];

// Cache slot of a pointer. Multiplicative, not the low bits: materials come
// 2 KB-aligned, and the arms, sleeve and glove of one model all fell into one
// slot by their low bits - each evicted the next, every item missed, every
// pass (2026-10-17, measured).
unsigned SlotOf(uintptr_t p, unsigned size) {
    return (unsigned)(((uint64_t)p * 0x9E3779B97F4A7C15ull) >> 40) % size;
}

uintptr_t ShaderClassOf(void* mat) {
    if (!mat) return kOther;
    const uintptr_t key = (uintptr_t)mat;
    std::atomic<uintptr_t>& slot = g_ShaderCache[SlotOf(key, kCacheSize)];
    const uintptr_t c = slot.load(std::memory_order_relaxed);
    if ((c & ~kClassBits) == key) return c & kClassBits;
    void* block = nullptr;
    void* name = nullptr;
    char buf[sizeof(kCharacterShader)] = {};
    uintptr_t cls = kOther;
    if (ReadPtr(key + kMatParamBlock, &block) && block && ReadPtr((uintptr_t)block + kParamShaderName, &name) && name &&
        ReadMem((uintptr_t)name, buf, sizeof(kWeaponShader))) {
        if (memcmp(buf, kWeaponShader, sizeof(kWeaponShader)) == 0) cls = kWeaponShaderClass;   // the terminator too
        else if (ReadMem((uintptr_t)name, buf, sizeof(kCharacterShader)) &&
                 memcmp(buf, kCharacterShader, sizeof(kCharacterShader)) == 0)
            cls = kCharacterShaderClass;
    }
    if (!(key & kClassBits)) slot.store(key | cls, std::memory_order_relaxed);
    return cls;
}

// --- the model path ---------------------------------------------------------
// scene object -> +o1 -> (+o2 ->) "....vmdl". Found by scanning the first
// scene object asked about and remembered (2026-10: +0x110 -> +0xB8); a read
// that stops giving a .vmdl rescans, at most once a second.
uint32_t g_ModelO1 = 0;
std::atomic<int> g_ModelO2{-2};   // -2 not found yet, -1 the string is at +o1 itself
ULONGLONG g_ModelScanAt = 0;
SRWLOCK g_ScanLock = SRWLOCK_INIT;   // the offsets: one scanner at a time (several workers)

bool ReadVmdl(uintptr_t at, char* out, size_t cap) {
    void* s = nullptr;
    if (!ReadPtr(at, &s) || (uintptr_t)s < 0x10000) return false;
    if (!ReadMem((uintptr_t)s, out, cap - 1)) {
        // The string may end before the page does; read it byte by byte.
        size_t i = 0;
        for (; i + 1 < cap; ++i)
            if (!ReadMem((uintptr_t)s + i, out + i, 1) || !out[i]) break;
        out[i] = 0;
    }
    out[cap - 1] = 0;
    const size_t n = strnlen(out, cap);
    for (size_t i = 0; i < n; ++i)
        if (out[i] < 0x20 || out[i] > 0x7E) return false;
    return n > 5 && _stricmp(out + n - 5, ".vmdl") == 0;
}

bool ModelAt(void* sobj, uint32_t o1, int o2, char* out, size_t cap) {
    if (o2 < 0) return ReadVmdl((uintptr_t)sobj + o1, out, cap);
    void* p = nullptr;
    return ReadPtr((uintptr_t)sobj + o1, &p) && (uintptr_t)p >= 0x10000 && ReadVmdl((uintptr_t)p + (uint32_t)o2, out, cap);
}

bool FindModel(void* sobj, char* out, size_t cap) {
    const int o2 = g_ModelO2.load(std::memory_order_acquire);
    if (o2 != -2 && ModelAt(sobj, g_ModelO1, o2, out, cap)) return true;
    AcquireSRWLockExclusive(&g_ScanLock);
    const ULONGLONG now = GetTickCount64();
    bool found = false;
    if (now >= g_ModelScanAt) {
        g_ModelScanAt = now + 1000;
        for (uint32_t a = 0; a < 0x400 && !found; a += 8) {
            if (ModelAt(sobj, a, -1, out, cap)) {
                g_ModelO1 = a;
                g_ModelO2.store(-1, std::memory_order_release);
                found = true;
                break;
            }
            for (uint32_t b = 0; b < 0x100; b += 8)
                if (ModelAt(sobj, a, (int)b, out, cap)) {
                    g_ModelO1 = a;
                    g_ModelO2.store((int)b, std::memory_order_release);
                    Log("engine: model path at scene object +%#x -> +%#x (\"%s\")", a, b, out);
                    found = true;
                    break;
                }
        }
    }
    ReleaseSRWLockExclusive(&g_ScanLock);
    return found;
}

bool ItemModel(uintptr_t item, char* out, size_t cap) {
    void* sobj = nullptr;
    out[0] = 0;
    return ReadPtr(item + kItemSobj, &sobj) && sobj && FindModel(sobj, out, cap);
}

bool IsGloveModel(const char* path) {
    char low[sizeof(kGloveFolder)];
    const size_t n = strlen(path), k = sizeof(kGloveFolder) - 1;
    for (size_t i = 0; i + k <= n; ++i) {
        for (size_t j = 0; j < k; ++j) low[j] = (char)(path[i + j] | (path[i + j] >= 'A' && path[i + j] <= 'Z' ? 0x20 : 0));
        if (memcmp(low, kGloveFolder, k) == 0) return true;
    }
    return false;
}

// Is this csgo_character item a glove? Cached by material, with the verdict in
// the low bit (materials are 2 KB-aligned).
constexpr int kGloveCache = 256;
std::atomic<uintptr_t> g_GloveCache[kGloveCache];
// Distinct csgo_character materials logged so far, so the log says once what
// each arm, sleeve and glove material is called - and does not grow per pass.
constexpr unsigned kMaxLoggedMaterials = 64;
std::atomic<unsigned> g_LoggedMaterials{0};

// The material's logical name through its vtable slot 0 (what the editor's
// world paint reads names with). A call into the engine: guarded, and made
// once per material on a cache miss.
bool MaterialName(void* mat, char* out, size_t cap) {
    out[0] = 0;
    void* vt = nullptr;
    void* fn = nullptr;
    if (!mat || !ReadPtr((uintptr_t)mat, &vt) || !vt || !ReadPtr((uintptr_t)vt, &fn) || !fn) return false;
    const char* name = nullptr;
    __try {
        name = ((const char*(__fastcall*)(void*))fn)(mat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!name || !ReadMem((uintptr_t)name, out, 1)) return false;
    size_t i = 0;
    for (; i + 1 < cap; ++i)
        if (!ReadMem((uintptr_t)name + i, out + i, 1) || !out[i]) break;
    out[i] = 0;
    return i > 0;
}

// The first-person arms are one scene object of the agent's own model
// (agents/models/ctm_sas/ctm_sas.vmdl) with a material per part: the bare
// arm, the sleeve, the glove (2026-10-17). So a glove is told by its material,
// not its model: characters/models/shared/arms/glove_<name>/materials/...
// Gloves with a skin on are the other way round: a composite material
// (compmat_1.vmat) on a scene object of their own,
// agents/models/shared/arms/glove_sporty/glove_sporty.vmdl. Either names it.
// Which part of the viewmodel a csgo_character item is, told by its material: the
// bare arm (hands, "bare_arm_*"), the sleeve ("sleeve_*"), the glove (the glove
// folder, or a composite whose scene object's model is one). The cache keeps the
// verdict by material in the low bits of the key: 1 none, 2 gloves, 3 hands,
// 4 sleeves.
int CharacterTarget(uintptr_t item) {
    void* mat = nullptr;
    if (!ReadPtr(item + kItemMat, &mat) || !mat) return WeaponFx::kNone;
    const uintptr_t key = (uintptr_t)mat;
    std::atomic<uintptr_t>& slot = g_GloveCache[SlotOf(key, kGloveCache)];
    const uintptr_t c = slot.load(std::memory_order_relaxed);
    if ((c & ~(uintptr_t)7) == key && (c & 7) != 0) return (c & 7) == 1 ? WeaponFx::kNone : (int)(c & 7) - 1;
    char name[260];
    if (!MaterialName(mat, name, sizeof(name))) return WeaponFx::kNone;   // not cached: a material mid-teardown
    char model[260];
    const bool haveModel = ItemModel(item, model, sizeof(model));
    int target = WeaponFx::kNone;
    if (strstr(name, "bare_arm")) target = WeaponFx::kHands;
    else if (strstr(name, "sleeve")) target = WeaponFx::kSleeves;
    else if (IsGloveModel(name) || (haveModel && IsGloveModel(model))) target = kGloves;
    // A "not a glove" verdict is only kept when it is certain. A composite material
    // (compmat_N, the glove with a skin) is told by its scene object's model, and
    // that read fails for some items: the miss was cached for the material and
    // the glove was left out for good - or for as long as nobody re-evaluated it
    // (2026-11-20: one glove without the effect, a bit of it blinking).
    const bool composite = strstr(name, "compmat") != nullptr;
    if (target != WeaponFx::kNone || haveModel || !composite)
        slot.store((key & ~(uintptr_t)7) | (target == WeaponFx::kNone ? 1u : (uintptr_t)(target + 1)), std::memory_order_relaxed);
    if (g_LoggedMaterials.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedMaterials) {
        static const char* const kPart[] = { "", " - gloves", " - hands", " - sleeves" };
        Log("engine: character material %s (%p, model %s)%s", name, mat, haveModel ? model : "?",
            target > 0 ? kPart[target] : "");
    }
    return target;
}

// Whose the item is: kWeapon, one of the character parts, or kNone. The model of
// a glove is read only when the gloves are wanted (`gloves`).
int ItemTarget(uintptr_t item, bool gloves) {
    void* mat = nullptr;
    if (!ReadPtr(item + kItemMat, &mat)) return WeaponFx::kNone;
    const uintptr_t cls = ShaderClassOf(mat);
    if (cls == kWeaponShaderClass) return kWeapon;
    if (cls == kCharacterShaderClass && gloves) return CharacterTarget(item);
    return WeaponFx::kNone;
}

// --- the model in hands, per target -----------------------------------------
char g_Model[kTargets][260] = {};
std::atomic<unsigned> g_ModelSeq[kTargets] = {};      // odd while g_Model is written
std::atomic<unsigned> g_ModelSerial[kTargets] = {};
SRWLOCK g_ModelWrite = SRWLOCK_INIT;                  // one writer at a time (several workers)

// `path` cut after the glove's own folder: ".../arms/glove_hardknuckle".
bool CutGloveFolder(char* path) {
    for (char* p = path; *p; ++p) *p = *p == '\\' ? '/' : *p;
    char* at = nullptr;
    for (char* p = path; *p && !at; ++p)
        if (_strnicmp(p, kGloveFolder, sizeof(kGloveFolder) - 1) == 0) at = p;
    if (!at) return false;
    char* end = strchr(at + sizeof(kGloveFolder) - 1, '/');   // the end of glove_<name>
    if (end) *end = 0;
    return true;
}

// The gloves' "model" for the settings: the glove's folder,
// "characters/models/shared/arms/glove_hardknuckle". From the material for
// stock gloves (their scene object is the agent's, the same for every glove),
// from the model for skinned ones (their material is a composite).
bool GloveFolder(uintptr_t item, char* out, size_t cap) {
    void* mat = nullptr;
    if (ReadPtr(item + kItemMat, &mat) && MaterialName(mat, out, cap) && CutGloveFolder(out)) return true;
    return ItemModel(item, out, cap) && CutGloveFolder(out);
}

void NoteModel(int target, uintptr_t item) {
    char path[260];
    if (target == WeaponFx::kHands || target == WeaponFx::kSleeves) {
        // No model of their own: one section ("hands", "sleeves"), seen once in hands.
        strcpy_s(path, target == WeaponFx::kHands ? "hands" : "sleeves");
    } else if (!(target == kGloves ? GloveFolder(item, path, sizeof(path)) : ItemModel(item, path, sizeof(path)))) {
        return;
    }
    AcquireSRWLockExclusive(&g_ModelWrite);
    if (strcmp(path, g_Model[target]) != 0) {
        g_ModelSeq[target].fetch_add(1, std::memory_order_acq_rel);
        strcpy_s(g_Model[target], path);
        g_ModelSeq[target].fetch_add(1, std::memory_order_acq_rel);
        g_ModelSerial[target].fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// Every distinct model among the marked items, logged once: the weapon itself,
// its attachments, the gloves, and anything else that draws with those shaders
// in the viewmodel pass. Scene objects are remembered by pointer, so a model is
// read once per object, not per frame.
constexpr int kSeenObjects = 64;
void* g_SeenObj[kSeenObjects] = {};
int g_SeenCount = 0;

void NoteItemModels(uintptr_t base, int count, const int8_t* targets) {
    if (g_ModelO2.load(std::memory_order_relaxed) == -2 || g_SeenCount >= kSeenObjects) return;
    AcquireSRWLockExclusive(&g_ModelWrite);
    for (int i = 0; i < count && g_SeenCount < kSeenObjects; ++i) {
        if (targets[i] < 0) continue;
        const uintptr_t e = base + (size_t)i * g_Stride;
        void* sobj = nullptr;
        if (!ReadPtr(e + kItemSobj, &sobj) || !sobj) continue;
        bool seen = false;
        for (int k = 0; k < g_SeenCount && !seen; ++k) seen = g_SeenObj[k] == sobj;
        if (seen) continue;
        g_SeenObj[g_SeenCount++] = sobj;
        if (targets[i] != kWeapon) continue;   // one scene object for arms and gloves; their materials are logged
        char path[260];
        void* mat = nullptr;
        ReadPtr(e + kItemMat, &mat);
        Log("engine: weapon item %d/%d model %s (scene object %p, material %p)", i, count,
            ItemModel(e, path, sizeof(path)) ? path : "?", sobj, mat);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// --- the passes ---------------------------------------------------------------

// Per call: whose each item is. The list is walked several times; reading the
// verdict once keeps the glove model reads to one per item.
struct Classified {
    int8_t target[kMaxItems];
    int count[kTargets];
};

// The colour pass delivers the viewmodel as a run of count=1 calls, one thread
// per run, in an order that is not fixed (seen: Weapon, Hands, Sleeves, Gloves,
// Gloves; the code was once written for Sleeves, Gloves, Gloves, Weapon, Hands).
// The queue below holds a run back and draws it inside out at the run's last
// call. The last call is found from the length of the previous runs, never from
// a target: a run that is cut short or late is dropped, not drawn - its
// pointers belong to a pass that is over.
std::atomic<int> g_RunFirst{-1};     // target that opened the last run
std::atomic<int> g_RunLength{0};     // calls in the last complete run (0: not learnt yet)
thread_local int g_RunCount = 0;     // calls of the run in progress on this thread
thread_local LONGLONG g_LastCallTick = 0;
constexpr LONGLONG kRunGapMs = 4;    // a pause longer than this opens a new run
std::atomic<unsigned> g_RunsFlushed{0}, g_RunsDropped{0}, g_RunsGap{0}, g_RunsRepeat{0}, g_RunLenChanges{0};
std::atomic<ULONGLONG> g_RunLogTick{0};

struct QueuedTarget {
    uint8_t raw[16 * 0x80];
    uint8_t marked[16 * 0x80];
    int count = 0;
    bool active = false;
    bool overlay = false;
    void* p1 = nullptr;
    void* p2 = nullptr;
    void* p5 = nullptr;
    void* L = nullptr;
    void* p7 = nullptr;
};
thread_local QueuedTarget g_Queued[kTargets];
thread_local int g_QueuedTotal = 0;

void FlushQueued(RenderObjectsFn orig, void** result) {
    constexpr int kOrder[kTargets] = { WeaponFx::kHands, kGloves, WeaponFx::kSleeves, kWeapon };
    void* lastRes = nullptr;
    for (int t : kOrder) {
        QueuedTarget& q = g_Queued[t];
        if (q.count <= 0) continue;
        const int n = q.count;
        q.count = 0;
        if (!q.active) {
            lastRes = orig(q.p1, q.p2, q.raw, n, q.p5, q.L, q.p7);
        } else if (q.overlay) {
            orig(q.p1, q.p2, q.raw, n, q.p5, q.L, q.p7);
            lastRes = orig(q.p1, q.p2, q.marked, n, q.p5, q.L, q.p7);
        } else {
            lastRes = orig(q.p1, q.p2, q.marked, n, q.p5, q.L, q.p7);
        }
    }
    g_QueuedTotal = 0;
    if (result) *result = lastRes;
}

// Forget a held run without drawing it.
void DropQueued() {
    for (QueuedTarget& q : g_Queued) q.count = 0;
    g_QueuedTotal = 0;
}

bool StripMarked(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
                 const int8_t* strip, void** result) {
    const uintptr_t base = (uintptr_t)items;
    int n0 = 0;
    for (int i = 0; i < count; ++i) n0 += strip[i] ? 1 : 0;
    if (!n0) return false;
    g_Stripped.fetch_add((unsigned)n0, std::memory_order_relaxed);
    if (n0 == count) { *result = nullptr; return true; }
    if ((size_t)count * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    const int slot = ClaimScratch();
    if (slot < 0) { g_NoRoom.fetch_add(1); return false; }
    uint8_t* out = g_Scratch[slot];
    size_t n = 0;
    for (int i = 0; i < count; ++i) {
        if (strip[i]) continue;
        if (!ReadMem(base + (size_t)i * g_Stride, out + n * g_Stride, g_Stride)) { ReleaseScratch(slot); return false; }
        ++n;
    }
    *result = orig(p1, p2, out, (int)n, p5, L, p7);
    ReleaseScratch(slot);
    return true;
}

// Copies `count` items into `out`, putting `marker[target]` on the ones whose
// target is in `which` and leaving the rest (unless `onlyWhich`). Returns the
// item count written, -1 on a failed read.
int BuildList(uintptr_t base, int count, const int8_t* targets, void* const* marker, unsigned which, bool onlyWhich,
              uint8_t* out) {
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const int t = targets[i];
        const bool mark = t >= 0 && (which >> t & 1u);
        if (onlyWhich && !mark) continue;
        uint8_t* dst = out + (size_t)n * g_Stride;
        if (!ReadMem(base + (size_t)i * g_Stride, dst, g_Stride)) return -1;
        if (mark) {
            // Both material slots: +0x28 is the variant the draw takes its
            // render state from; the item's own would draw the marker with its
            // depth and blend.
            *(void**)(dst + kItemMat) = marker[t];
            *(void**)(dst + kItemMat2) = marker[t];
            *(uint32_t*)(dst + kItemColor) = kMarkerColor;
        }
        ++n;
    }
    return n;
}

// The viewmodel's colour pass with the markers on: the replace targets' items
// swapped for their marker in the engine's list in a single atomic orig() call
// so another worker thread's RenderObjects call (e.g. the weapon) never
// interleaves between the character pass's items.
bool MarkItems(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
               const int8_t* targets, void* const* marker, unsigned replace, unsigned overlay, void** result) {
    const uintptr_t base = (uintptr_t)items;
    if (!replace && !overlay) return false;
    if ((size_t)count * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    int slotA = -1, slotB = -1;
    int nA = count, nB = 0;
    void* listA = items;
    if (replace) {
        slotA = ClaimScratch();
        if (slotA < 0) { g_NoRoom.fetch_add(1); return false; }
        nA = BuildList(base, count, targets, marker, replace, false, g_Scratch[slotA]);
        listA = g_Scratch[slotA];
    }
    if (overlay && nA >= 0) {
        slotB = ClaimScratch();
        if (slotB < 0) { g_NoRoom.fetch_add(1); ReleaseScratch(slotA); return false; }
        nB = BuildList(base, count, targets, marker, overlay, true, g_Scratch[slotB]);
    }
    if (nA < 0 || nB < 0) { ReleaseScratch(slotA); ReleaseScratch(slotB); return false; }
    *result = orig(p1, p2, listA, nA, p5, L, p7);
    if (nB > 0) orig(p1, p2, g_Scratch[slotB], nB, p5, L, p7);
    ReleaseScratch(slotA);
    ReleaseScratch(slotB);
    return true;
}

bool HandleImpl(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
                void** result) {
    uint64_t view = 0;
    uint32_t lflags = 0;
    if (!items || !L || count <= 0 || count > kMaxItems || !ReadMem((uintptr_t)L + kPassViewBits, &view, 8) ||
        !(view & kViewIsViewmodel) || !ReadMem((uintptr_t)L + kPassLflags, &lflags, 4))
        return false;
    WeaponFx::SetViewmodelPass(true);   // cleared by the hook's guard when the call returns
    void* marker[kTargets] = {};
    Mode mode[kTargets] = {};
    unsigned on = 0;
    for (int t = 0; t < kTargets; ++t) {
        mode[t] = (Mode)g_Mode[t].load(std::memory_order_relaxed);
        marker[t] = mode[t] == Mode::Off ? nullptr : Material::MarkerIfReady(t);
        if (marker[t]) on |= 1u << t;
    }
    // The gloves' model is followed whatever their mode: the settings are found
    // by it (a [glove_sporty] section can turn them on while [gloves] is off),
    // and the first glove seen is what lets the host create their marker. The
    // model read is cached per scene object, so this is one lookup per item.
    const bool wantGloves = true;
    g_Calls.fetch_add(1, std::memory_order_relaxed);
    // On the stack: 4 KB per list, far inside a worker's stack, and
    // nothing here recurses.
    Classified cl;
    for (int& n : cl.count) n = 0;
    const uintptr_t base = (uintptr_t)items;
    int first[kTargets];
    for (int& f : first) f = -1;
    for (int i = 0; i < count; ++i) {
        const int t = ItemTarget(base + (size_t)i * g_Stride, wantGloves);
        cl.target[i] = (int8_t)t;
        if (t >= 0) {
            ++cl.count[t];
            if (first[t] < 0) first[t] = i;
        }
    }
    {
        int any = 0;
        for (int n : cl.count) any += n;
        if (!any) return false;
    }
    if (lflags & kLflagDepth) {
        if (!on) return false;
        int8_t strip[kMaxItems];
        for (int i = 0; i < count; ++i) strip[i] = (int8_t)(cl.target[i] >= 0 && (on >> cl.target[i] & 1u));
        return StripMarked(orig, p1, p2, items, count, p5, L, p7, strip, result);
    }
    for (int t = 0; t < kTargets; ++t)
        if (first[t] >= 0) NoteModel(t, base + (size_t)first[t] * g_Stride);
    NoteItemModels(base, count, cl.target);
    if (!on) {
        if (g_QueuedTotal > 0) DropQueued();
        return false;
    }
    unsigned replace = 0, overlay = 0;
    for (int t = 0; t < kTargets; ++t) {
        if (!(on >> t & 1u) || !cl.count[t]) continue;
        const bool asReplace = mode[t] == Mode::Replace || WeaponFx::ReadsScene(t);
        (asReplace ? replace : overlay) |= 1u << t;
    }
    // The viewmodel items arrive as separate count=1 calls (see g_RunLength).
    // Queue them into the 4 target buckets and flush at the run's last call in
    // the physical inside-out order: Hands -> Gloves -> Sleeves -> Weapon.
    if (count == 1 && cl.target[0] >= 0 && g_Stride <= 0x80) {
        const int t = cl.target[0];
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const bool gap = g_LastCallTick == 0 || (now.QuadPart - g_LastCallTick) * 1000 > freq.QuadPart * kRunGapMs;
        g_LastCallTick = now.QuadPart;
        const int runFirst = g_RunFirst.load(std::memory_order_relaxed);
        if (gap || (t == runFirst && t != kGloves && g_RunCount > 0)) {   // a new run begins
            (gap ? g_RunsGap : g_RunsRepeat).fetch_add(1, std::memory_order_relaxed);
            if (g_RunCount > 0) {
                if (g_RunLength.exchange(g_RunCount, std::memory_order_relaxed) != g_RunCount)
                    g_RunLenChanges.fetch_add(1, std::memory_order_relaxed);
            }
            g_RunCount = 0;
            g_RunFirst.store(t, std::memory_order_relaxed);
            if (g_QueuedTotal > 0) {   // the old run was cut short
                DropQueued();
                g_RunsDropped.fetch_add(1, std::memory_order_relaxed);
            }
            const ULONGLONG tick = GetTickCount64();
            if (tick - g_RunLogTick.load(std::memory_order_relaxed) >= 2000) {
                g_RunLogTick.store(tick, std::memory_order_relaxed);
                Log("runs: flushed %u dropped %u | starts by gap %u by repeat %u | length %d (changed %u times) | thread %lu",
                    g_RunsFlushed.load(), g_RunsDropped.load(), g_RunsGap.load(), g_RunsRepeat.load(),
                    g_RunLength.load(), g_RunLenChanges.load(), GetCurrentThreadId());
            }
        }
        ++g_RunCount;
        QueuedTarget& q = g_Queued[t];
        if (q.count < 16 && ReadMem(base, q.raw + (size_t)q.count * g_Stride, g_Stride)) {
            uint8_t* m = q.marked + (size_t)q.count * g_Stride;
            memcpy(m, q.raw + (size_t)q.count * g_Stride, g_Stride);
            const bool isAct = (on >> t & 1u) != 0;
            if (isAct) {
                *(void**)(m + kItemMat) = marker[t];
                *(void**)(m + kItemMat2) = marker[t];
                *(uint32_t*)(m + kItemColor) = kMarkerColor;
                if (t == kWeapon) g_WeaponItems.fetch_add(1, std::memory_order_relaxed);
                if (t == kGloves) g_GloveItems.fetch_add(1, std::memory_order_relaxed);
            }
            q.active = isAct;
            q.overlay = isAct && mode[t] == Mode::Overlay && !WeaponFx::ReadsScene(t);
            q.p1 = p1; q.p2 = p2; q.p5 = p5; q.L = L; q.p7 = p7;
            ++q.count;
            ++g_QueuedTotal;
            const int runLength = g_RunLength.load(std::memory_order_relaxed);
            if (runLength > 0 && g_RunCount >= runLength) {
                g_RunsFlushed.fetch_add(1, std::memory_order_relaxed);
                FlushQueued(orig, result);
            } else {
                *result = nullptr;
            }
            return true;
        }
    }
    if (!replace && !overlay) return false;
    if ((replace | overlay) & (1u << kWeapon)) g_WeaponItems.fetch_add((unsigned)cl.count[kWeapon], std::memory_order_relaxed);
    if ((replace | overlay) & (1u << kGloves)) g_GloveItems.fetch_add((unsigned)cl.count[kGloves], std::memory_order_relaxed);
    return MarkItems(orig, p1, p2, items, count, p5, L, p7, cl.target, marker, replace, overlay, result);
}

void* __fastcall HkRenderObjects(void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7) {
    struct Guard {
        Guard() { g_InHook.fetch_add(1, std::memory_order_acquire); }
        ~Guard() {
            WeaponFx::SetViewmodelPass(false);
            g_InHook.fetch_sub(1, std::memory_order_release);
        }
    } guard;
    const RenderObjectsFn orig = (RenderObjectsFn)g_Det.orig;
    void* r = nullptr;
    if (HandleImpl(orig, p1, p2, items, count, p5, L, p7, &r)) return r;
    return orig(p1, p2, items, count, p5, L, p7);
}

// The item size, read back out of the function: the walk advances its pointer
// with `add r64, imm8`. Majority vote over the first 0x600 bytes; the default
// stands when nothing votes.
size_t DeriveStride(uintptr_t fn) {
    unsigned tally[0x100] = {};
    uint8_t code[0x604] = {};
    if (!ReadMem(fn, code, sizeof(code))) return 0;
    for (size_t o = 0; o < 0x600; ++o) {
        if ((code[o] == 0x48 || code[o] == 0x49) && code[o + 1] == 0x83 && code[o + 2] >= 0xC0 && code[o + 2] <= 0xC7) {
            const uint8_t imm = code[o + 3];
            if (imm >= 0x40 && imm <= 0xC0 && imm % 8 == 0) ++tally[imm];
        }
    }
    unsigned best = 0, votes = 0;
    for (unsigned i = 0; i < 0x100; ++i)
        if (tally[i] > votes) { votes = tally[i]; best = i; }
    return votes ? best : 0;
}

} // namespace

bool Install() {
    if (g_Det.on) return true;
    constexpr size_t kSigLen = sizeof(kSigRenderObjects) / sizeof(kSigRenderObjects[0]);
    int hits = 0;
    uintptr_t fn = FindUnique("scenesystem.dll", kSigRenderObjects, kSigLen, &hits);
    bool overJmp = false;
    if (!fn) {
        // Zero hits is a game update - or another tool's detour already over
        // the prologue. The common one is a plain `E9 rel32` (the cheat's own
        // hook for its chams): the same signature with its first 5 bytes
        // replaced by E9 and a wildcard rel32 finds it, and we go on top of it.
        short alt[kSigLen];
        for (size_t i = 0; i < kSigLen; ++i) alt[i] = kSigRenderObjects[i];
        alt[0] = 0xE9;
        for (size_t i = 1; i < 5; ++i) alt[i] = -1;
        int altHits = 0;
        fn = FindUnique("scenesystem.dll", alt, kSigLen, &altHits);
        if (!fn) {
            Log("engine: RenderObjects signature matched %d times (need 1), with a jmp over it %d times - not hooked",
                hits, altHits);
            return false;
        }
        overJmp = true;
        Log("engine: RenderObjects already starts with a jmp (another tool's detour); going on top of it");
    }
    if (const size_t s = DeriveStride(fn)) g_Stride = s;
    Log("engine: RenderObjects %p, item stride %#zx", (void*)fn, g_Stride);
    if (!Material::Resolve()) return false;
    if (overJmp) return Wfx::InstallOverJmp(g_Det, "RenderObjects", fn, (void*)&HkRenderObjects);
    return Wfx::Install(g_Det, "RenderObjects", fn, kSigRenderObjects, kPatchLen, -1, (void*)&HkRenderObjects);
}

void Remove() {
    Wfx::Remove(g_Det);
    for (int i = 0; i < 500 && g_InHook.load(std::memory_order_acquire) > 0; ++i) Sleep(1);
}

bool Attach(size_t itemStride) {
    if (itemStride >= 0x40 && itemStride <= 0x100 && itemStride % 8 == 0) g_Stride = itemStride;
    if (!Material::Resolve()) return false;
    g_Attached = true;
    Log("engine: attached to the host's RenderObjects, item stride %#zx", g_Stride);
    return true;
}

bool Handle(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
            void** result) {
    if (!g_Attached || !orig) return false;
    return HandleImpl(orig, p1, p2, items, count, p5, L, p7, result);
}


bool Wanted() {
    for (int t = 0; t < kTargets; ++t)
        if (g_Mode[t].load(std::memory_order_relaxed) != (int)Mode::Off) return true;
    return false;
}

void SetMode(int target, Mode m) {
    if (target >= 0 && target < kTargets) g_Mode[target].store((int)m, std::memory_order_relaxed);
}
Mode GetMode(int target) {
    return target >= 0 && target < kTargets ? (Mode)g_Mode[target].load(std::memory_order_relaxed) : Mode::Off;
}

void Model(int target, char* out, unsigned cap, unsigned* serial) {
    if (!out || !cap) return;
    out[0] = 0;
    if (target < 0 || target >= kTargets) return;
    for (int tries = 0; tries < 100; ++tries) {
        const unsigned s0 = g_ModelSeq[target].load(std::memory_order_acquire);
        if (s0 & 1) { YieldProcessor(); continue; }
        strncpy_s(out, cap, g_Model[target], _TRUNCATE);
        if (g_ModelSeq[target].load(std::memory_order_acquire) == s0) {
            if (serial) *serial = g_ModelSerial[target].load(std::memory_order_relaxed);
            return;
        }
    }
    out[0] = 0;
}

void Stats(unsigned* calls, unsigned* weaponItems, unsigned* stripped, unsigned* noRoom) {
    if (calls) *calls = g_Calls.load();
    if (weaponItems) *weaponItems = g_WeaponItems.load();
    if (stripped) *stripped = g_Stripped.load();
    if (noRoom) *noRoom = g_NoRoom.load();
}

unsigned GloveItems() { return g_GloveItems.load(); }

} // namespace Engine
} // namespace Wfx
