// critbob.cpp - "Crit bob animation" DLL (injected into Minecraft Bedrock by critbob_inject.exe).
// Polls C:\Users\Public\CheesyProxxy\critbob.cfg 4x/second. cfg = 2 dwords: 0x424F4243 ("CBOB"), enable (0/1).
// cfg OFF  ->  hooks are removed, vtable slots restored, and the DLL UNLOADS ITSELF (eject). Log: critbob.log
//
// What it does: when you attack while jumping (space held, or within jumpWindowMs after pressing it) the
// held item plays a crit "bob": quick forward plunge + drop + tip toward the camera, then a damped-spring
// return with a small overshoot (timings taken from the reference clip). The pose is multiplied onto the
// first-person held-item matrix, so it is independent of DX11 / DX12 (it is game code, not the GPU API).
//
// REQUIRED BEFORE IT SHOWS ANYTHING: critbob_sig.txt (same folder as the cfg) with the signature of the
// first-person held-item transform function of YOUR Bedrock build (see critbob_sig.txt template).
// Without it the log says "NO SIGNATURE" and the DLL only counts triggers - nothing moves on screen.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

#define DIR_PATH  L"C:\\Users\\Public\\CheesyProxxy\\"
#define CFG_PATH  DIR_PATH L"critbob.cfg"
#define LOG_PATH  DIR_PATH L"critbob.log"
#define SIG_PATH  DIR_PATH L"critbob_sig.txt"
#define INI_PATH  DIR_PATH L"critbob.ini"
#define CFG_MAGIC 0x424F4243u

static HMODULE g_mod;
static volatile LONG g_stop = 0;

static void logf_(const char* fmt, ...) {
    HANDLE h = CreateFileW(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[512]; SYSTEMTIME t; GetLocalTime(&t);
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); n += _vsnprintf_s(buf + n, sizeof buf - n, _TRUNCATE, fmt, a); va_end(a);
    if (n < (int)sizeof buf - 2) { buf[n++] = '\r'; buf[n++] = '\n'; }
    DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h);
}

// ------------------------------------------------------------------ settings (critbob.ini, optional)
struct Cfg { float dipY = -0.13f, pushZ = 0.09f, pitchDeg = 38.f, rollDeg = -11.f, attackMs = 62.f, holdMs = 38.f,
                   omega = 24.f, zeta = 0.55f; int jumpWindowMs = 650; int spaceMode = 1; } cfg;

static void loadIni() {
    auto F = [](const wchar_t* k, float d) { wchar_t b[64]; GetPrivateProfileStringW(L"bob", k, L"", b, 64, INI_PATH); return *b ? (float)_wtof(b) : d; };
    cfg.dipY = F(L"dipY", cfg.dipY); cfg.pushZ = F(L"pushZ", cfg.pushZ); cfg.pitchDeg = F(L"pitchDeg", cfg.pitchDeg);
    cfg.rollDeg = F(L"rollDeg", cfg.rollDeg); cfg.attackMs = F(L"attackMs", cfg.attackMs); cfg.holdMs = F(L"holdMs", cfg.holdMs);
    cfg.omega = F(L"settleOmega", cfg.omega); cfg.zeta = F(L"settleZeta", cfg.zeta);
    cfg.jumpWindowMs = (int)F(L"jumpWindowMs", (float)cfg.jumpWindowMs); cfg.spaceMode = (int)F(L"spaceMode", 1.f);
}

// ------------------------------------------------------------------ animation
struct Pose { float y, z, pitch, roll; };
static LARGE_INTEGER g_freq;
static volatile LONG64 g_fireQpc = 0;
static volatile LONG g_fires = 0;

static double msSince(LONG64 q0) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)(n.QuadPart - q0) * 1000.0 / (double)g_freq.QuadPart; }
static float easeOut(float t) { t = 1.f - t; return 1.f - t * t * t; }
static float envelope(float ms) {                       // 0 -> 1 (ease-out), hold, then underdamped spring 1 -> 0
    if (ms < 0) return 0;
    if (ms < cfg.attackMs) return easeOut(ms / cfg.attackMs);
    ms -= cfg.attackMs;
    if (ms < cfg.holdMs) return 1.f;
    float t = (ms - cfg.holdMs) / 1000.f, w = cfg.omega, z = cfg.zeta, wd = w * sqrtf(1.f - z * z);
    float x = expf(-z * w * t) * (cosf(wd * t) + (z * w / wd) * sinf(wd * t));
    return (t > 0.35f && fabsf(x) < 0.002f) ? 0.f : x;
}
static bool currentPose(Pose& p) {
    LONG64 q0 = g_fireQpc; if (!q0) return false;
    float ms = (float)msSince(q0);
    if (ms > 900.f) { g_fireQpc = 0; return false; }
    float a = envelope(ms), aP = envelope(ms - 15.f), aR = envelope(ms - 28.f);   // pitch/roll lag a little = weight
    p.y = cfg.dipY * a; p.z = cfg.pushZ * a; p.pitch = cfg.pitchDeg * aP * 0.0174532925f; p.roll = cfg.rollDeg * aR * 0.0174532925f;
    return true;
}
static void fire() { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_fireQpc = q.QuadPart; InterlockedIncrement(&g_fires); }

// ------------------------------------------------------------------ held-item hook (signature from critbob_sig.txt)
struct Spec { std::string pattern; int stolen = 0; int matrixArg = 2; bool preCall = false; } g_spec;
typedef void* (__fastcall* Held_t)(void*, void*, void*, void*);
static Held_t o_Held = nullptr;
static uint8_t* g_target = nullptr; static uint8_t g_orig[32]; static int g_n = 0; static void* g_tramp = nullptr;

static void applyPose(float* m, const Pose& p) {        // column-major 4x4, m = m * T * R
    float cp = cosf(p.pitch), sp = sinf(p.pitch), cr = cosf(p.roll), sr = sinf(p.roll);
    float R[16] = { cr, sr, 0, 0,  -sr * cp, cr * cp, sp, 0,  sr * sp, -cr * sp, cp, 0,  0, p.y, p.z, 1 }, o[16];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) { float s = 0; for (int k = 0; k < 4; ++k) s += m[k * 4 + r] * R[c * 4 + k]; o[c * 4 + r] = s; }
    memcpy(m, o, sizeof o);
}
static void* __fastcall d_Held(void* a, void* b, void* c, void* d) {
    void* args[4] = { a, b, c, d }; Pose p; bool on = currentPose(p) && !g_stop;
    if (on && g_spec.preCall) { __try { applyPose((float*)args[g_spec.matrixArg], p); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    void* r = o_Held(a, b, c, d);
    if (on && !g_spec.preCall) { __try { applyPose((float*)args[g_spec.matrixArg], p); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return r;
}

static bool parsePattern(const char* p, std::vector<int>& out) {
    out.clear();
    while (*p) {
        while (*p == ' ') ++p; if (!*p) break;
        if (*p == '?') { out.push_back(-1); while (*p == '?') ++p; continue; }
        char h[3] = { p[0], p[1] ? p[1] : 0, 0 }; char* e = NULL; long v = strtol(h, &e, 16); if (e == h) return false;
        out.push_back((int)v); p += 2;
    }
    return !out.empty();
}
static uint8_t* findUnique(const std::vector<int>& pat) {
    uint8_t* base = (uint8_t*)GetModuleHandleW(NULL); auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt); uint8_t* hit = NULL; int hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = base + sec[s].VirtualAddress; size_t sz = sec[s].Misc.VirtualSize; if (sz < pat.size()) continue;
        for (size_t i = 0; i + pat.size() <= sz; ++i) {
            size_t k = 0; while (k < pat.size() && (pat[k] < 0 || b[i + k] == (uint8_t)pat[k])) ++k;
            if (k == pat.size()) { hit = b + i; if (++hits > 1) return NULL; }
        }
    }
    return hits == 1 ? hit : NULL;
}
static void absJmp(uint8_t* at, const void* dest) { at[0] = 0xFF; at[1] = 0x25; at[2] = at[3] = at[4] = at[5] = 0; uint64_t d = (uint64_t)dest; memcpy(at + 6, &d, 8); }

// critbob_sig.txt:  line1 = byte pattern, line2 = stolen bytes (14..32, whole instructions, no rip-relative),
//                   line3 = index (0-3) of the argument that is the 4x4 matrix, line4 = "pre" or "post" (default post)
static bool readSig() {
    HANDLE h = CreateFileW(SIG_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[2048] = { 0 }; DWORD got = 0; ReadFile(h, buf, sizeof buf - 1, &got, NULL); CloseHandle(h);
    std::vector<std::string> lines; std::string cur;
    for (DWORD i = 0; i < got; ++i) { char c = buf[i]; if (c == '\n' || c == '\r') { if (!cur.empty()) lines.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) lines.push_back(cur);
    std::vector<std::string> v; for (auto& l : lines) if (l[0] != '#' && l[0] != ';') v.push_back(l);
    if (v.size() < 3) return false;
    g_spec.pattern = v[0]; g_spec.stolen = atoi(v[1].c_str()); g_spec.matrixArg = atoi(v[2].c_str());
    g_spec.preCall = v.size() > 3 && v[3].compare(0, 3, "pre") == 0;
    return !g_spec.pattern.empty();
}
static bool g_warned = false;
#define WARN1(...) do { if (!g_warned) { logf_(__VA_ARGS__); g_warned = true; } } while (0)
static bool hookOn() {
    if (g_target) return true;
    if (!readSig()) { WARN1("NO SIGNATURE: critbob_sig.txt missing/empty - the bob cannot be drawn (triggers are still counted)"); return false; }
    std::vector<int> pat;
    if (g_spec.stolen < 14 || g_spec.stolen > 32 || g_spec.matrixArg < 0 || g_spec.matrixArg > 3 || !parsePattern(g_spec.pattern.c_str(), pat)) { WARN1("critbob_sig.txt: bad values"); return false; }
    uint8_t* t = findUnique(pat);
    if (!t) { WARN1("signature not found or not unique in this Bedrock build"); return false; }
    g_tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!g_tramp) return false;
    memcpy(g_orig, t, g_spec.stolen); g_n = g_spec.stolen;
    memcpy(g_tramp, t, g_n); absJmp((uint8_t*)g_tramp + g_n, t + g_n); o_Held = (Held_t)g_tramp;
    DWORD old; if (!VirtualProtect(t, g_n, PAGE_EXECUTE_READWRITE, &old)) { logf_("VirtualProtect failed (%lu)", GetLastError()); return false; }
    absJmp(t, (void*)d_Held); for (int i = 14; i < g_n; ++i) t[i] = 0x90;
    VirtualProtect(t, g_n, old, &old); FlushInstructionCache(GetCurrentProcess(), t, g_n);
    g_target = t; logf_("held-item hook installed at exe+0x%llx (matrix arg %d, %s-call)", (unsigned long long)(t - (uint8_t*)GetModuleHandleW(NULL)), g_spec.matrixArg, g_spec.preCall ? "pre" : "post");
    return true;
}
static void hookOff() {
    if (!g_target) return;
    DWORD old; VirtualProtect(g_target, g_n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_target, g_orig, g_n); VirtualProtect(g_target, g_n, old, &old); FlushInstructionCache(GetCurrentProcess(), g_target, g_n);
    g_target = nullptr; logf_("held-item hook removed");       // trampoline stays allocated: a thread may still be inside it
}

// ------------------------------------------------------------------ swap chain vtable (DX11 + DX12 share IDXGISwapChain)
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static Present_t o_Present; static Present1_t o_Present1; static void** g_slot0, **g_slot1;
static volatile LONG g_api = 0;                                   // 11 / 12 once the first frame is seen
static void onFrame(IUnknown* sc) {
    if (g_api) return;
    void* dev = nullptr;
    static const GUID iid12 = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };   // ID3D12Device
    IDXGISwapChain* s = nullptr; if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain), (void**)&s)) || !s) return;
    if (SUCCEEDED(s->GetDevice(iid12, &dev)) && dev) { ((IUnknown*)dev)->Release(); g_api = 12; }
    else if (SUCCEEDED(s->GetDevice(__uuidof(ID3D11Device), &dev)) && dev) { ((IUnknown*)dev)->Release(); g_api = 11; }
    s->Release(); if (g_api) logf_("renderer: DirectX %ld (the held-item hook is renderer independent)", (long)g_api);
}
static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* s, UINT a, UINT b) { onFrame(s); return o_Present(s, a, b); }
static HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1* s, UINT a, UINT b, const DXGI_PRESENT_PARAMETERS* p) { onFrame(s); return o_Present1(s, a, b, p); }
static bool patchSlot(void** slot, void* fn, void** orig) { DWORD old; if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false; *orig = *slot; *slot = fn; VirtualProtect(slot, sizeof(void*), old, &old); return true; }
static void unpatchSlots() {
    if (g_slot0 && o_Present)  { DWORD o; if (VirtualProtect(g_slot0, sizeof(void*), PAGE_READWRITE, &o)) { *g_slot0 = (void*)o_Present;  VirtualProtect(g_slot0, sizeof(void*), o, &o); } g_slot0 = nullptr; }
    if (g_slot1 && o_Present1) { DWORD o; if (VirtualProtect(g_slot1, sizeof(void*), PAGE_READWRITE, &o)) { *g_slot1 = (void*)o_Present1; VirtualProtect(g_slot1, sizeof(void*), o, &o); } g_slot1 = nullptr; }
}
static bool installPresent() {
    HINSTANCE hi = GetModuleHandleW(NULL); WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = hi; wc.lpszClassName = L"critbob_dummy"; RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, hi, NULL); if (!w) return false;
    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof sd); sd.BufferCount = 2; sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = w; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc; if (patchSlot(&vt[8], (void*)h_Present, (void**)&o_Present)) { g_slot0 = &vt[8]; ok = true; }
        IDXGISwapChain1* s1 = NULL;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&s1)) && s1) { void** v1 = *(void***)s1; if (patchSlot(&v1[22], (void*)h_Present1, (void**)&o_Present1)) { g_slot1 = &v1[22]; ok = true; } s1->Release(); }
    }
    if (sc) sc->Release(); if (ctx) ctx->Release(); if (dev) dev->Release();
    DestroyWindow(w); UnregisterClassW(wc.lpszClassName, hi); return ok;
}

// ------------------------------------------------------------------ trigger thread: attack while jumping
static DWORD WINAPI triggerThread(LPVOID) {
    bool pl = false, ps = false; LONG64 jumpQ = 0; LARGE_INTEGER q;
    while (!g_stop) {
        bool sp = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0, lm = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (sp && !ps) { QueryPerformanceCounter(&q); jumpQ = q.QuadPart; }
        bool air = sp || (jumpQ && msSince(jumpQ) < cfg.jumpWindowMs);
        if (lm && !pl && (air || !cfg.spaceMode)) fire();
        pl = lm; ps = sp; Sleep(1);
    }
    return 0;
}

// ------------------------------------------------------------------ cfg polling + self eject
static bool readCfg(DWORD& on) {
    HANDLE h = CreateFileW(CFG_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD buf[2] = { 0, 0 }, got = 0; BOOL ok = ReadFile(h, buf, 8, &got, NULL); CloseHandle(h);
    if (!ok || got < 8 || buf[0] != CFG_MAGIC) return false; on = buf[1]; return true;
}
static DWORD WINAPI worker(LPVOID) {
    QueryPerformanceFrequency(&g_freq); loadIni();
    logf_("critbob.dll loaded");
    if (installPresent()) logf_("swap-chain hook installed"); else logf_("swap-chain hook NOT installed (only used for the renderer log line)");
    hookOn();
    HANDLE tt = CreateThread(NULL, 0, triggerThread, NULL, 0, NULL);
    LONG lastFires = 0; bool hooked = g_target != nullptr;
    while (!g_stop) {
        DWORD on = 1;
        if (readCfg(on) && !on) break;                                   // OFF pressed in HTMI.proxx -> eject
        if (!hooked) hooked = hookOn();                                  // retry (a signature file may be added while the game runs)
        if (g_fires != lastFires && !g_target) { lastFires = g_fires; logf_("trigger #%ld (no hook installed, nothing to animate)", (long)g_fires); }
        for (int i = 0; i < 5 && !g_stop; ++i) Sleep(50);
    }
    // ---- eject: stop the trigger thread, restore bytes + vtable slots, let in-flight calls drain, unload
    InterlockedExchange(&g_stop, 1);
    if (tt) { WaitForSingleObject(tt, 1000); CloseHandle(tt); }
    hookOff(); unpatchSlots();
    logf_("OFF: ejected");
    Sleep(400);
    FreeLibraryAndExitThread(g_mod, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) { g_mod = mod; DisableThreadLibraryCalls(mod); HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (t) CloseHandle(t); }
    else if (reason == DLL_PROCESS_DETACH && !reserved) { InterlockedExchange(&g_stop, 1); hookOff(); unpatchSlots(); }
    return TRUE;
}
