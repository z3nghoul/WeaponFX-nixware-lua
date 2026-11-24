#include "d3d_hooks.h"
#include "log.h"
#include "patch.h"
#include "../core/weapon_fx.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <intrin.h>
#include <atomic>
#include <cstring>

namespace Wfx {
namespace D3d {
namespace {

// IDXGISwapChain::Present in the vtable (IUnknown 3, IDXGIObject 4,
// IDXGIDeviceSubObject 1: Present is the 9th entry).
constexpr int kPresentSlot = 8;

// ID3D11DeviceContext vtable slots, CINTERFACE order of d3d11.h. Stable: the
// interface is frozen.
enum Kind { kDii, kDi, kD, kDInst, kDiiInd, kDiInd, kClearDsv, kKinds };
constexpr int kSlot[kKinds] = { 20, 12, 13, 21, 39, 40, 53 };
const char* const kName[kKinds] = { "DrawIndexedInstanced", "DrawIndexed", "Draw", "DrawInstanced",
                                    "DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "ClearDepthStencilView" };

// The prologues of the d3d11.dll thunks these slots point at (Windows 11
// 10.0.26100/26200, d3d11.dll dated 2026-10-14; the third and fourth
// sets per kind are from 10.0.26300, d3d11.dll 0x94DE539A, added 2026-11-20: the table
// switches to the fourth a few seconds after the device comes up - same checks). d3d11 switches the immediate
// context's whole table between two sets of implementations at run time (a
// few seconds after a device comes up, and again later), so each kind has one
// shape per set. A thunk of any other shape is refused and logged with its
// bytes; a Windows update that changes one costs that one hook, not a bad
// patch. callAt: a rel32 call inside the patched bytes (the shared validation
// call), replayed as an absolute call. Every other byte has no RIP-relative
// operand and replays as is (checked with a disassembler, 2026-10-16).
struct Shape { short bytes[24]; size_t len; int callAt; };
constexpr Shape kShapes[kKinds][4] = {
    // DrawIndexedInstanced
    { { { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x41, 0x8B, 0xD8, 0x44, 0x8B, 0xDA, 0x4C, 0x8D, 0x91, 0x28, 0xFF, 0xFF,
          0xFF }, 19, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
          0xEC, 0x70 }, 20, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41,
          0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40 }, 24, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x70 }, 20, -1 } },
    // DrawIndexed
    { { { 0x48, 0x83, 0xEC, 0x38, 0x48, 0x81, 0xC1, 0x28, 0xFF, 0xFF, 0xFF, 0xE8, -1, -1, -1, -1 }, 16, 11 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60 }, 20, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41,
          0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 }, 24, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x60 }, 20, -1 } },
    // Draw
    { { { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x81, 0xC1, 0x28, 0xFF, 0xFF, 0xFF, 0xE8, -1, -1, -1, -1 }, 16, 11 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60 }, 15, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC,
          0x20 }, 18, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x41, 0x8B,
          0xF8, 0x8B, 0xF2 }, 20, -1 } },
    // DrawInstanced
    { { { 0x48, 0x83, 0xEC, 0x38, 0x44, 0x8B, 0xDA, 0x4C, 0x8D, 0x91, 0x28, 0xFF, 0xFF, 0xFF, 0xE8, -1, -1, -1, -1 },
        19, 14 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60 }, 20, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC,
          0x30 }, 18, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x60 }, 20, -1 } },
    // DrawIndexedInstancedIndirect
    { { { 0x48, 0x83, 0xEC, 0x28, 0x45, 0x8B, 0xD0, 0x4C, 0x8B, 0xC9, 0xE8, -1, -1, -1, -1 }, 15, 10 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60 }, 15, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x20 }, 20, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x41, 0x8B,
          0xF8,
          0x48, 0x8B, 0xF2 }, 21, -1 } },
    // DrawInstancedIndirect
    { { { 0x48, 0x83, 0xEC, 0x28, 0x45, 0x8B, 0xD0, 0x4C, 0x8B, 0xC9, 0xE8, -1, -1, -1, -1 }, 15, 10 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60 }, 15, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x20 }, 20, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x41, 0x8B,
          0xF8,
          0x48, 0x8B, 0xF2 }, 21, -1 } },
    // ClearDepthStencilView
    { { { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x8B, 0xD8, 0x4C, 0x8B, 0xD9, 0x45, 0x33, 0xD2 }, 15, -1 },
      { { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70 }, 15, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48,
          0x83, 0xEC, 0x50, 0x0F, 0x29, 0x70, 0xE8 }, 24, -1 },
      { { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x0F,
          0x29, 0x70, 0xE8 }, 20, -1 } },
};

// Implementations hooked per kind: room for every shape above (a table of the
// same kind can alternate between sets, and each set has its own shape).
constexpr int kPer = 4;
Detour g_Det[kKinds][kPer];
// Thunks already refused (unknown shape), so a refusal is logged once, not
// every frame.
constexpr int kRefusedMax = 16;
uintptr_t g_Refused[kRefusedMax] = {};
int g_RefusedCount = 0;

std::atomic<int> g_InDetour{0};
// CPU cost watch: the time spent in our draw hooks minus the time inside the
// game's original calls is what WeaponFX adds on the CPU per draw - at a few
// hundred draws a frame and a CPU-bound game that is FPS. Logged every 5 s
// from the present hook, with the per-frame driver's own time. The draws are
// issued on the render thread only (no deferred contexts in the frame), so
// plain counters.
int64_t g_HookTicks = 0, g_OrigTicks = 0, g_FrameFnTicks = 0;
uint32_t g_CostFrames = 0, g_CostDraws = 0;
ULONGLONG g_CostNext = 0;

inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// Every kCostSample-th draw is timed and counted kCostSample times: two
// QueryPerformanceCounter calls on each of ~650 draws a frame (four with the
// game's call) were themselves about 0.06 ms a frame (2026-10-17).
constexpr uint32_t kCostSample = 16;
bool g_CostSampling = false;   // the draw in flight is timed (render thread, hooks do not nest)

struct Guard {
    int64_t t0 = 0;
    bool timed;
    Guard() : timed(++g_CostDraws % kCostSample == 0) {
        g_InDetour.fetch_add(1, std::memory_order_acquire);
        g_CostSampling = timed;
        if (timed) t0 = Qpc();
    }
    ~Guard() {
        if (timed) g_HookTicks += (Qpc() - t0) * kCostSample;
        g_CostSampling = false;
        g_InDetour.fetch_sub(1, std::memory_order_release);
    }
};

// Around the game's own call inside a hook.
struct OrigTime {
    int64_t t0;
    OrigTime() : t0(g_CostSampling ? Qpc() : 0) {}
    ~OrigTime() { if (t0) g_OrigTicks += (Qpc() - t0) * kCostSample; }
};

using WeaponFx::kTargets;

// Per target, the marker texture's two views (linear, sRGB).
std::atomic<ID3D11ShaderResourceView*> g_MarkerA[kTargets] = {}, g_MarkerB[kTargets] = {};
// Second texture's views for kAttach (g_tColor2), paired with g_MarkerA/B[kAttach] (g_tColor).
std::atomic<ID3D11ShaderResourceView*> g_Attach2A{nullptr}, g_Attach2B{nullptr};
std::atomic<unsigned> g_Draws{0}, g_Marked{0}, g_MarkedGloves{0};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
void** g_PresentSlotAt = nullptr;
PresentFn g_OrigPresent = nullptr;
FrameFn g_OnFrame = nullptr;
std::atomic<int> g_InPresent{0};

// Is a marker view bound to the pixel shader? 16 slots: Source 2 puts a
// material's textures low; the marker material samples it as its colour.
// Diagnostic: every distinct (pixel shader, slots holding the marker) pair of
// a marked draw, logged once - tells the weapon's own draws from a draw that
// only inherited a stale marker binding.
constexpr int kSeenPairs = 96;
constexpr UINT kMinMarkedIndices = 36;   // a quad is 6, a triangle strip of a few quads 24
void* g_SeenPs[kSeenPairs] = {};
unsigned g_SeenMask[kSeenPairs] = {};
bool g_SeenRejected[kSeenPairs] = {};
void* g_SeenVb[kSeenPairs] = {};
UINT g_SeenIdx[kSeenPairs] = {};
int g_SeenPairs = 0;

void NotePair(ID3D11DeviceContext* c, unsigned mask, UINT indexCount, bool rejected = false) {
    ID3D11PixelShader* ps = nullptr;
    c->PSGetShader(&ps, nullptr, nullptr);
    if (ps) ps->Release();   // compared only
    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOff = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOff);
    if (vb) vb->Release();   // compared only
    for (int k = 0; k < g_SeenPairs; ++k)
        if (g_SeenPs[k] == ps && g_SeenMask[k] == mask && g_SeenRejected[k] == rejected && g_SeenIdx[k] == indexCount) return;
    if (g_SeenPairs >= kSeenPairs) return;
    g_SeenPs[g_SeenPairs] = ps;
    g_SeenMask[g_SeenPairs] = mask;
    g_SeenRejected[g_SeenPairs] = rejected;
    g_SeenVb[g_SeenPairs] = vb;
    g_SeenIdx[g_SeenPairs] = indexCount;
    ++g_SeenPairs;
    Log("d3d: %s - PS %p, VB0 %p, marker in slots %#x, %u indices", rejected ? "stale marker, not a viewmodel mesh (left alone)" : "marked draw",
        (void*)ps, (void*)vb, mask, indexCount);
}

// The slots the marker has been seen in (union of every marked draw's mask),
// for the per-draw check: reading all 16 views and releasing them is 32
// reference counts on every one of ~400 draws a frame - most of what the
// hooks cost on the CPU (2026-10-16). Most draws read only the learnt span;
// every kFullScanEvery-th draw reads all 16, so a game update that moves the
// marker to other slots is relearnt within a frame or two instead of losing
// the weapon. Reset when the marker views change. Render thread only.
constexpr unsigned kFullScanEvery = 256;
unsigned g_MarkerSpan = 0;
uintptr_t g_SpanFor = 0;   // the markers the span was learnt for (their views mixed), compared only
unsigned g_ScanTick = 0;

// Whose marker the bound views carry: a WeaponFx::Target, or kNone.
int MarkerBound(ID3D11DeviceContext* c, UINT indexCount) {
    ID3D11ShaderResourceView* a[kTargets];
    ID3D11ShaderResourceView* b[kTargets];
    uintptr_t mix = 0;
    bool any = false;
    for (int t = 0; t < kTargets; ++t) {
        a[t] = g_MarkerA[t].load(std::memory_order_relaxed);
        b[t] = g_MarkerB[t].load(std::memory_order_relaxed);
        mix = mix * 31 + (uintptr_t)a[t] * 7 + (uintptr_t)b[t];
        any = any || a[t] || b[t];
    }
    ID3D11ShaderResourceView* const a2 = g_Attach2A.load(std::memory_order_relaxed);
    ID3D11ShaderResourceView* const b2 = g_Attach2B.load(std::memory_order_relaxed);
    mix = mix * 31 + (uintptr_t)a2 * 7 + (uintptr_t)b2;
    any = any || a2 || b2;
    if (!any) return WeaponFx::kNone;
    if (g_SpanFor != mix) { g_SpanFor = mix; g_MarkerSpan = 0; }
    UINT lo = 0, cnt = 16;
    if (g_MarkerSpan && ++g_ScanTick % kFullScanEvery) {
        unsigned long first = 0, last = 0;
        _BitScanForward(&first, g_MarkerSpan);
        _BitScanReverse(&last, g_MarkerSpan);
        lo = first;
        cnt = last - first + 1;
    }
    ID3D11ShaderResourceView* srv[16] = {};
    c->PSGetShaderResources(lo, cnt, srv);
    unsigned masks[kTargets] = {};
    unsigned maskAttach2 = 0;
    for (unsigned k = 0; k < cnt; ++k) {
        if (srv[k]) {
            for (int t = 0; t < kTargets; ++t)
                if (srv[k] == a[t] || srv[k] == b[t]) masks[t] |= 1u << (lo + k);
            if (srv[k] == a2 || srv[k] == b2) maskAttach2 |= 1u << (lo + k);
        }
        if (srv[k]) srv[k]->Release();
    }
    // The marker material binds two textures (g_tColor and g_tColor2, host/material.cpp).
    // For viewmodel targets both slots carry that target's own texture (2 bits in masks[t]).
    // For kAttach, g_tColor and g_tColor2 carry two distinct textures (masks[kAttach] and
    // maskAttach2 each have a bit in a different slot), so attachments never collide with
    // kWeapon even during a 1st<->3rd person transition.
    int who = WeaponFx::kNone;
    static ID3D11PixelShader* s_markerPs = nullptr;
    for (int t = 0; t < kTargets; ++t) {
        unsigned mask = masks[t];
        bool matched = false;
        if (t == WeaponFx::kAttach) {
            mask = masks[t] | maskAttach2;
            matched = masks[t] != 0 && maskAttach2 != 0 && (mask & (mask - 1)) != 0;
        } else {
            matched = (mask & (mask - 1)) != 0;
        }
        if (!mask) continue;
        g_MarkerSpan |= mask;
        if (who < 0 && matched) {
            if (indexCount < kMinMarkedIndices) { NotePair(c, mask, indexCount, true); continue; }
            ID3D11PixelShader* ps = nullptr;
            c->PSGetShader(&ps, nullptr, nullptr);
            if (ps) ps->Release();
            if (!ps) { NotePair(c, mask, indexCount, true); continue; }
            if (!s_markerPs) s_markerPs = ps;
            if (ps != s_markerPs) {
                NotePair(c, mask, indexCount, true);
                ID3D11ShaderResourceView* nullSrv = nullptr;
                for (UINT slot = 0; slot < 16; ++slot)
                    if (mask >> slot & 1u) c->PSSetShaderResources(slot, 1, &nullSrv);
                continue;
            }
            who = t;
        }
        NotePair(c, mask, indexCount);
    }
    return who;
}

template <class F>
void CallIssue(void* f) { (*(F*)f)(); }

// When d3d11.dll's Set-3 wrapper thunk (e.g. Dii_3 at 0x156640) is hooked and
// its trampoline internally dispatches to the Set-2 thunk (Dii_2 at 0x154050),
// or when our own code issues a D3D11 Clear/Draw inside a hook, the inner hook
// must pass straight through to its trampoline without re-entering WeaponFx.
thread_local bool g_InThreadHook = false;

struct ThreadHookScope {
    bool entered;
    ThreadHookScope() : entered(!g_InThreadHook) {
        if (entered) g_InThreadHook = true;
    }
    ~ThreadHookScope() {
        if (entered) g_InThreadHook = false;
    }
};

// One instantiation per hooked implementation: a shared detour would not know
// which trampoline to return through.
template <int S>
void STDMETHODCALLTYPE HkDii(ID3D11DeviceContext* c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT))g_Det[kDii][S].orig)(c, n, inst,
                                                                                                         s, b, si);
        return;
    }
    g_Draws.fetch_add(1, std::memory_order_relaxed);
    const int target = MarkerBound(c, n);
    if (target >= 0) (target == WeaponFx::kGloves ? g_MarkedGloves : g_Marked).fetch_add(1, std::memory_order_relaxed);
    const uint64_t key = ((uint64_t)n << 40) ^ ((uint64_t)inst << 32) ^ ((uint64_t)(uint32_t)b << 20) ^ ((uint64_t)s << 8) ^ si;
    if (WeaponFx::BeforeDraw(c, target, key)) return;
    auto issue = [&] {
        OrigTime ot;
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT))g_Det[kDii][S].orig)(c, n, inst,
                                                                                                         s, b, si);
    };
    const WeaponFx::DrawIndexedArgs diArgs = { n, inst, s, b, si };
    WeaponFx::Draw(c, target, &CallIssue<decltype(issue)>, &issue, key, &diArgs);
}

template <int S>
void STDMETHODCALLTYPE HkDi(ID3D11DeviceContext* c, UINT n, UINT s, INT b) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT))g_Det[kDi][S].orig)(c, n, s, b);
        return;
    }
    g_Draws.fetch_add(1, std::memory_order_relaxed);
    if (WeaponFx::BeforeDraw(c, WeaponFx::kNone)) return;
    auto issue = [&] { OrigTime ot; ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT))g_Det[kDi][S].orig)(c, n, s, b); };
    WeaponFx::Draw(c, WeaponFx::kNone, &CallIssue<decltype(issue)>, &issue);
}

template <int S>
void STDMETHODCALLTYPE HkD(ID3D11DeviceContext* c, UINT n, UINT s) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT))g_Det[kD][S].orig)(c, n, s);
        return;
    }
    g_Draws.fetch_add(1, std::memory_order_relaxed);
    if (WeaponFx::BeforeDraw(c, WeaponFx::kNone)) return;
    auto issue = [&] { OrigTime ot; ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT))g_Det[kD][S].orig)(c, n, s); };
    WeaponFx::Draw(c, WeaponFx::kNone, &CallIssue<decltype(issue)>, &issue);
}

template <int S>
void STDMETHODCALLTYPE HkDInst(ID3D11DeviceContext* c, UINT n, UINT inst, UINT s, UINT si) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT))g_Det[kDInst][S].orig)(c, n, inst, s, si);
        return;
    }
    g_Draws.fetch_add(1, std::memory_order_relaxed);
    if (WeaponFx::BeforeDraw(c, WeaponFx::kNone)) return;
    auto issue = [&] {
        OrigTime ot;
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT))g_Det[kDInst][S].orig)(c, n, inst, s, si);
    };
    WeaponFx::Draw(c, WeaponFx::kNone, &CallIssue<decltype(issue)>, &issue);
}

template <int K, int S>
void STDMETHODCALLTYPE HkInd(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT off) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT))g_Det[K][S].orig)(c, args, off);
        return;
    }
    g_Draws.fetch_add(1, std::memory_order_relaxed);
    if (WeaponFx::BeforeDraw(c, WeaponFx::kNone)) return;
    auto issue = [&] { OrigTime ot; ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT))g_Det[K][S].orig)(c, args, off); };
    WeaponFx::Draw(c, WeaponFx::kNone, &CallIssue<decltype(issue)>, &issue);
}

template <int S>
void STDMETHODCALLTYPE HkClear(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
                               UINT8 stencil) {
    Guard g;
    ThreadHookScope ths;
    if (!ths.entered) {
        ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8))g_Det[kClearDsv][S]
             .orig)(c, dsv, flags, depth, stencil);
        return;
    }
    WeaponFx::BeforeClearDepth(c, dsv, flags, stencil);
    ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8))g_Det[kClearDsv][S]
         .orig)(c, dsv, flags, depth, stencil);
}

void* DetourFor(int kind, int s) {
#define WFX_PICK(T) (s == 0 ? (void*)&T<0> : s == 1 ? (void*)&T<1> : s == 2 ? (void*)&T<2> : (void*)&T<3>)
    switch (kind) {
    case kDii:      return WFX_PICK(HkDii);
    case kDi:       return WFX_PICK(HkDi);
    case kD:        return WFX_PICK(HkD);
    case kDInst:    return WFX_PICK(HkDInst);
    case kClearDsv: return WFX_PICK(HkClear);
    case kDiiInd:   return s == 0 ? (void*)&HkInd<kDiiInd, 0> : s == 1 ? (void*)&HkInd<kDiiInd, 1> : s == 2 ? (void*)&HkInd<kDiiInd, 2> : (void*)&HkInd<kDiiInd, 3>;
    case kDiInd:    return s == 0 ? (void*)&HkInd<kDiInd, 0> : s == 1 ? (void*)&HkInd<kDiInd, 1> : s == 2 ? (void*)&HkInd<kDiInd, 2> : (void*)&HkInd<kDiInd, 3>;
    default:        return nullptr;
    }
#undef WFX_PICK
}

bool Refused(uintptr_t fn) {
    for (int i = 0; i < g_RefusedCount; ++i)
        if (g_Refused[i] == fn) return true;
    return false;
}

void HookOne(int kind, uintptr_t fn) {
    if (!fn || Refused(fn)) return;
    int free = -1;
    for (int s = 0; s < kPer; ++s) {
        if (g_Det[kind][s].on && g_Det[kind][s].fn == fn) return;
        if (!g_Det[kind][s].on && free < 0) free = s;
    }
    if (free < 0) return;
    for (const Shape& sh : kShapes[kind]) {
        if (!sh.len) continue;
        uint8_t cur[24] = {};
        if (!ReadMem(fn, cur, sh.len)) break;
        size_t i = 0;
        for (; i < sh.len; ++i)
            if (sh.bytes[i] >= 0 && cur[i] != (uint8_t)sh.bytes[i]) break;
        if (i != sh.len) continue;
        if (Install(g_Det[kind][free], kName[kind], fn, sh.bytes, sh.len, sh.callAt, DetourFor(kind, free))) return;
        break;
    }
    if (g_RefusedCount < kRefusedMax) g_Refused[g_RefusedCount++] = fn;
    uint8_t b[8] = {};
    ReadMem(fn, b, sizeof(b));
    Log("d3d: %s at %p (d3d11 +%#llx) is not a known shape: %02X %02X %02X %02X %02X %02X %02X %02X - not hooked",
        kName[kind], (void*)fn, (unsigned long long)(fn - (uintptr_t)GetModuleHandleW(L"d3d11.dll")), b[0], b[1], b[2],
        b[3], b[4], b[5], b[6], b[7]);
}

} // namespace

void FrameCost(int64_t frameTicks) {
    g_FrameFnTicks += frameTicks;
    ++g_CostFrames;
    const ULONGLONG now = GetTickCount64();
    if (now < g_CostNext) return;
    if (g_CostNext && g_CostFrames) {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        const double ms = 1000.0 / (double)fq.QuadPart / g_CostFrames;
        Log("d3d: CPU cost - draw hooks %.3f ms/frame over %.0f draws/frame (the game's own calls %.3f ms), "
            "per-frame driver %.3f ms/frame, %u frames",
            (double)(g_HookTicks - g_OrigTicks) * ms, (double)g_CostDraws / g_CostFrames, (double)g_OrigTicks * ms,
            (double)g_FrameFnTicks * ms, g_CostFrames);
    }
    g_CostNext = now + 5000;
    g_HookTicks = g_OrigTicks = g_FrameFnTicks = 0;
    g_CostFrames = g_CostDraws = 0;
}

namespace {

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_InPresent.fetch_add(1, std::memory_order_acquire);
    ID3D11Device* dev = nullptr;
    // The device is asked for every frame: a device reset hands the swap
    // chain a new one, and a cached pointer would be a dead object.
    const int64_t f0 = Qpc();
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
        if (g_OnFrame) g_OnFrame(sc, dev);
        dev->Release();
    }
    FrameCost(Qpc() - f0);
    const HRESULT hr = g_OrigPresent(sc, sync, flags);
    g_InPresent.fetch_sub(1, std::memory_order_release);
    return hr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

} // namespace

bool InstallPresent(FrameFn onFrame) {
    if (g_PresentSlotAt) return true;
    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!wnd) return false;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        DestroyWindow(wnd);
        Log("d3d: throwaway swap chain failed (%08lX)", (unsigned long)hr);
        return false;
    }
    void** slot = &(*(void***)sc)[kPresentSlot];
    sc->Release();
    if (dev) dev->Release();
    if (ctx) ctx->Release();
    DestroyWindow(wnd);
    g_OnFrame = onFrame;
    g_OrigPresent = (PresentFn)*slot;
    if (!WriteSlot(slot, (void*)&HkPresent)) {
        Log("d3d: the Present slot could not be written");
        return false;
    }
    g_PresentSlotAt = slot;
    Log("d3d: Present hooked (slot %p, original %p)", (void*)slot, (void*)g_OrigPresent);
    return true;
}

void RemovePresent() {
    if (!g_PresentSlotAt) return;
    WriteSlot(g_PresentSlotAt, (void*)g_OrigPresent);
    g_PresentSlotAt = nullptr;
    // A frame already inside our Present finishes through the original.
    for (int i = 0; i < 500 && g_InPresent.load(std::memory_order_acquire) > 0; ++i) Sleep(1);
    Log("d3d: Present restored");
}

void HookDraws(ID3D11Device* dev) {
    if (!dev) return;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    if (!imm) return;
    void** vt = *(void***)imm;
    imm->Release();   // only its table is read, here and now
    for (int k = 0; k < kKinds; ++k) HookOne(k, (uintptr_t)vt[kSlot[k]]);
    // d3d11.dll switches the immediate context's vtable at run time between two
    // sets of thunks (set 2 and set 3 in kShapes). Until the second set is
    // first seen during a Present call, every frame that uses it goes unhooked:
    // ClearDepthStencilView and DrawIndexedInstanced miss, causing 0.8 draws/frame
    // and heavy flickering across all shaders. Find and hook the known shapes
    // inside d3d11.dll upfront on the first call.
    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        // Hook both sets of d3d11.dll context thunks upfront (verified by HookOne
        // against kShapes[k] before patching) before FindUnique scans or patches
        // any prologue.
        if (const uintptr_t base = (uintptr_t)GetModuleHandleW(L"d3d11.dll")) {
            constexpr uint32_t kRva2[kKinds] = { 0x154050, 0x154260, 0x1543E0, 0x153810, 0x1B7490, 0x1B7E60, 0x152EF0 };
            constexpr uint32_t kRva3[kKinds] = { 0x156640, 0x155D30, 0x153990, 0x1B5130, 0x1423C0, 0x1424A0, 0x1A7C70 };
            for (int k = 0; k < kKinds; ++k) {
                HookOne(k, base + kRva2[k]);
                HookOne(k, base + kRva3[k]);
            }
        }
        for (int k = 0; k < kKinds; ++k) {
            for (int s = 2; s < 4; ++s) {
                const Shape& sh = kShapes[k][s];
                if (!sh.len) continue;
                int hits = 0;
                if (const uintptr_t fn = FindUnique("d3d11.dll", sh.bytes, sh.len, &hits))
                    HookOne(k, fn);
            }
        }
    }
}

void RemoveDraws() {
    for (int k = kKinds - 1; k >= 0; --k)
        for (Detour& d : g_Det[k]) Remove(d);
    // A draw that entered before its prologue went back is still in our code.
    // Bounded: a context call is microseconds.
    for (int i = 0; i < 500 && g_InDetour.load(std::memory_order_acquire) > 0; ++i) Sleep(1);
    Log("d3d: draw hooks removed%s", g_InDetour.load() > 0 ? " - a draw is still inside" : "");
}

void SetMarkerViews(int target, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b) {
    if (target < 0 || target >= kTargets) return;
    g_MarkerA[target].store(a, std::memory_order_relaxed);
    g_MarkerB[target].store(b, std::memory_order_relaxed);
}

void SetAttachMarkerViews2(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b) {
    g_Attach2A.store(a, std::memory_order_relaxed);
    g_Attach2B.store(b, std::memory_order_relaxed);
}

void ClearMarkerViews() {
    for (int t = 0; t < kTargets; ++t) SetMarkerViews(t, nullptr, nullptr);
    SetAttachMarkerViews2(nullptr, nullptr);
}

bool Hooked() {
    for (auto& per : g_Det)
        for (Detour& d : per)
            if (d.on) return true;
    return false;
}

unsigned MarkedGloves() { return g_MarkedGloves.load(); }

void Stats(unsigned* draws, unsigned* marked, int* hooks) {
    if (draws) *draws = g_Draws.load();
    if (marked) *marked = g_Marked.load();
    int n = 0;
    for (auto& per : g_Det)
        for (Detour& d : per) n += d.on ? 1 : 0;
    if (hooks) *hooks = n;
}

} // namespace D3d
} // namespace Wfx
