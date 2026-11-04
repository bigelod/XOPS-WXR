// XOPS-WXR - d3d8 diagnostic probe
//
// A drop-in d3d8.dll proxy that logs everything we need to know about how XOPS
// drives Direct3D, then forwards every call to the real d3d8.dll unchanged.
// It changes nothing about the game. Desktop use, for now.
//
// Build:   build\build-probe.bat     (VS 2022 Developer Command Prompt, x86)
// Install: copy the built d3d8.dll next to XOPS.exe
// Output:  xops_wxr_probe.log, next to the DLL
//
// Deliberately has no dependencies: no MinHook, no DirectX SDK, no D3DX. We
// own Direct3DCreate8, so the device's vtable slots can be patched directly -
// no detour library, and nothing that could behave differently under Box86.
// Every COM declaration needed is reproduced below, so d3d8.h is not required
// (the June 2010 SDK does not ship it; August 2007 was the last that did).
//
// What it answers, in the order we need the answers:
//   1. Which call sites reach SetTransform, and with which transform type.
//      Reported as IDA addresses, so it resolves the 0x403AA4 / 4013AB9
//      disagreement and locates the D3DTS_VIEW site without more IDA work.
//   2. What the view matrix actually contains - decomposed into camera
//      position and basis vectors, which is where the engine's handedness and
//      its metres-to-units scale come from.
//   3. Whether EndScene is called once per presented frame, which decides
//      where the sync square goes.
//   4. The real CreateDevice parameters and window style, for the resolution
//      and borderless work.
//   5. Whether the HUD is drawn with pre-transformed vertices (D3DFVF_XYZRHW),
//      via SetVertexShader.
//   6. **How XOPS reads input** - which modules and functions it imports, and
//      how often it calls each one per frame. That decides whether we can feed
//      the game input directly, in-process, instead of synthesising keyboard
//      and mouse events. See "INPUT" in the log and feature F-10 in the plan.

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static void ResolveForwards(void);
static void ProbeKeys(PBYTE p);
static BOOL SafeRead(UINT_PTR a, float* out);
static void IdleTick(void);
static void BallisticTick(void);

static void WatchDump(const char* why);
static void BallisticArm(void);
static void ProbeTick(void);
#define PHASE_BEGINSCENE 0
#define PHASE_INPUT      1
#define PHASE_ENDSCENE   2

static void ProbeCyclePhase(void);
static int  g_probePhase = 0;
static void BuildWatchList(void);
static void StructDump(UINT_PTR base, int count, const char* name);
static void SteerTick(void);
static void StickKeys(PBYTE p);
static void VrExclusiveKeys(PBYTE p);
static BOOL InGameplay(void);
static BOOL g_steer = FALSE;   /* defined here; used by the input hook above */

static void SteerRecentre(void);
static void SteerSave(void);
static void SteerLoad(void);
static const char* ClusterName(int c);
static const char* DofName(int m);

static void SteerLog(void);
static BOOL SafeWrite(UINT_PTR a, float v);
static UINT_PTR WatchAddr(int i);
static const char* PhaseName(int p);
static void DumpImports(void);
static void HookInput(void);

#include "xrapi.h"

// ---------------------------------------------------------------------------
// Minimal D3D8 declarations - enough to forward and to read what we log.
// ---------------------------------------------------------------------------

typedef struct { float m[4][4]; } D3DMATRIX8;

typedef struct {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    DWORD BackBufferFormat;            // D3DFORMAT
    UINT  BackBufferCount;
    DWORD MultiSampleType;             // D3DMULTISAMPLE_TYPE
    DWORD SwapEffect;                  // D3DSWAPEFFECT
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;      // D3DFORMAT
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS8;

// Vtable indices. Derived from d3d8.h's method order, and corroborated on the
// binary: SetTransform 0x94 and SetRenderState 0xC8 were both read out of
// XOPS.exe and both match this table.
enum {
    VT_D3D8_CREATEDEVICE      = 15,   // 0x3C

    VT_DEV_RESET              = 14,   // 0x38
    VT_DEV_PRESENT            = 15,   // 0x3C
    VT_DEV_BEGINSCENE         = 34,   // 0x88
    VT_DEV_ENDSCENE           = 35,   // 0x8C
    VT_DEV_SETTRANSFORM       = 37,   // 0x94  <- confirmed in XOPS.exe
    VT_DEV_DRAWPRIMITIVE      = 70,   // 0x118
    VT_DEV_DRAWINDEXEDPRIM    = 71,   // 0x11C
    VT_DEV_DRAWPRIMITIVEUP    = 72,   // 0x120 <- the loc_401396 draw call
    VT_DEV_SETVERTEXSHADER    = 76    // 0x130
};

#define D3DTS_VIEW        2
#define D3DTS_PROJECTION  3
#define D3DFVF_XYZRHW     0x004

typedef void* (__stdcall *PFN_Direct3DCreate8)(UINT);

typedef HRESULT (__stdcall *PFN_CreateDevice)(void*, UINT, DWORD, HWND, DWORD,
                                              D3DPRESENT_PARAMETERS8*, void**);
typedef HRESULT (__stdcall *PFN_Reset)(void*, D3DPRESENT_PARAMETERS8*);
typedef HRESULT (__stdcall *PFN_Present)(void*, const RECT*, const RECT*, HWND, void*);
typedef HRESULT (__stdcall *PFN_BeginScene)(void*);
typedef HRESULT (__stdcall *PFN_EndScene)(void*);
typedef HRESULT (__stdcall *PFN_SetTransform)(void*, DWORD, const D3DMATRIX8*);
typedef HRESULT (__stdcall *PFN_DrawPrimitiveUP)(void*, DWORD, UINT, const void*, UINT);
typedef HRESULT (__stdcall *PFN_DrawPrimitive)(void*, DWORD, UINT, UINT);
typedef HRESULT (__stdcall *PFN_DrawIndexedPrim)(void*, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (__stdcall *PFN_SetVertexShader)(void*, DWORD);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const char* kBuildStamp = "built " __DATE__ " " __TIME__;

static HMODULE  g_hSelf        = NULL;
static HMODULE  g_hRealD3D8    = NULL;
static FILE*    g_log          = NULL;
static CRITICAL_SECTION g_lock;
static BOOL     g_initDone     = FALSE;
static UINT_PTR g_exeBase      = 0;

static PFN_Direct3DCreate8 g_realCreate8 = NULL;

static PFN_CreateDevice     o_CreateDevice     = NULL;
static PFN_Reset            o_Reset            = NULL;
static PFN_Present          o_Present          = NULL;
static PFN_BeginScene       o_BeginScene       = NULL;
static PFN_EndScene         o_EndScene         = NULL;
static PFN_SetTransform     o_SetTransform     = NULL;
static PFN_DrawPrimitiveUP  o_DrawPrimitiveUP  = NULL;
static PFN_DrawPrimitive    o_DrawPrimitive    = NULL;
static PFN_DrawIndexedPrim  o_DrawIndexedPrim  = NULL;
static long g_drawsUP = 0, g_drawsVB = 0, g_drawsIDX = 0;
static PFN_SetVertexShader  o_SetVertexShader  = NULL;

static long g_frame         = 0;
static long g_beginThisFrame = 0;
static long g_endThisFrame   = 0;
static long g_drawsThisFrame = 0;
static long g_xformThisFrame = 0;

// SetTransform calls this frame, split by type. The first run showed 25 per
// frame at startup and then ZERO once in a level, which is the single most
// important open question in the project: if the in-game camera never passes
// through SetTransform, hooking it cannot drive the view. These counters, plus
// the FVF/stride histogram below, settle it in one run.
static long g_xfView = 0, g_xfProj = 0, g_xfWorld = 0, g_xfOther = 0;

// The last VIEW matrix seen, and when. If the eye position stops changing once
// a level loads, the view matrix is not what moves the camera.
static D3DMATRIX8 g_lastView;
static BOOL       g_haveView   = FALSE;
static long       g_lastViewAt = -1;

// Which vertex format each draw used. XYZRHW means the game transformed the
// vertices itself on the CPU; XYZ means it relied on the device - and
// therefore on the matrices we can hook.
static DWORD g_curFvf = 0;
#define MAX_DRAWKINDS 16
static struct { DWORD fvf; UINT stride; long count; } g_draws[MAX_DRAWKINDS];
static int g_drawKinds = 0;

static void NoteDraw(DWORD fvf, UINT stride)
{
    for (int i = 0; i < g_drawKinds; ++i)
        if (g_draws[i].fvf == fvf && g_draws[i].stride == stride) { ++g_draws[i].count; return; }
    if (g_drawKinds < MAX_DRAWKINDS) {
        g_draws[g_drawKinds].fvf = fvf;
        g_draws[g_drawKinds].stride = stride;
        g_draws[g_drawKinds].count = 1;
        ++g_drawKinds;
    }
}

static const char* FvfKind(DWORD f)
{
    switch (f & 0x00E) {
        case 0x002: return "XYZ    (device transforms -> our matrices apply)";
        case 0x004: return "XYZRHW (game pre-transformed -> matrices DO NOT apply)";
        case 0x006: return "XYZB1";
        default:    return "?";
    }
}

static HWND  g_wnd          = NULL;
static LONG  g_lastStyle    = 0;
static RECT  g_lastRect     = {0,0,0,0};

// Call sites we have already reported, so each is logged once and the log stays
// readable. (type, IDA address) pairs.
#define MAX_SITES 64
static struct { DWORD type; UINT_PTR ida; } g_sites[MAX_SITES];
static int g_siteCount = 0;

// Distinct vertex-shader / FVF values seen, same reasoning.
static DWORD g_fvfSeen[32];
static int   g_fvfCount = 0;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void LogV(const char* fmt, va_list ap)
{
    if (!g_log) return;
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);                      // a crash must not cost us the log
}

static void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    LogV(fmt, ap);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
}

// IDA shows addresses against the preferred base (0x400000 for this EXE), so
// report them that way - a number you can paste straight into IDA's Jump To.
static UINT_PTR ToIda(void* ret)
{
    if (!g_exeBase) return (UINT_PTR)ret;
    return (UINT_PTR)ret - g_exeBase + 0x400000;
}

static const char* XformName(DWORD t)
{
    switch (t) {
        case 2:  return "VIEW";
        case 3:  return "PROJECTION";
        case 256: return "WORLD";       // D3DTS_WORLD is 256 in D3D8
        default: return "other";
    }
}

static void LogMatrix(const char* tag, const D3DMATRIX8* m)
{
    if (!m) { Log("    %s: (null)", tag); return; }
    for (int r = 0; r < 4; ++r)
        Log("    %s[%d]  % .5f  % .5f  % .5f  % .5f",
            tag, r, m->m[r][0], m->m[r][1], m->m[r][2], m->m[r][3]);
}

// A view matrix is the inverse of the camera's world matrix. Under D3D's
// row-vector convention the upper 3x3 is the rotation world->view and the
// bottom row is -eye rotated into view space, so:
//
//     eye[j]     = -(t . row j of R)
//     right      = (_11, _21, _31)
//     up         = (_12, _22, _32)
//     forward    = (_13, _23, _33)
//
// Those three vectors are what tell us the engine's handedness, and the eye
// position across a known walk is what gives us metres-to-units.
static void LogViewDecomposed(const D3DMATRIX8* m)
{
    if (!m) return;
    const float t0 = m->m[3][0], t1 = m->m[3][1], t2 = m->m[3][2];
    float eye[3];
    for (int j = 0; j < 3; ++j)
        eye[j] = -(t0 * m->m[j][0] + t1 * m->m[j][1] + t2 * m->m[j][2]);

    Log("    eye      % .3f  % .3f  % .3f   (engine units)", eye[0], eye[1], eye[2]);
    Log("    right    % .4f  % .4f  % .4f", m->m[0][0], m->m[1][0], m->m[2][0]);
    Log("    up       % .4f  % .4f  % .4f", m->m[0][1], m->m[1][1], m->m[2][1]);
    Log("    forward  % .4f  % .4f  % .4f", m->m[0][2], m->m[1][2], m->m[2][2]);

    // right x up should equal forward in a left-handed basis, and -forward in a
    // right-handed one. This single line settles the handedness question.
    const float cx = m->m[1][0] * m->m[2][1] - m->m[2][0] * m->m[1][1];
    const float cy = m->m[2][0] * m->m[0][1] - m->m[0][0] * m->m[2][1];
    const float cz = m->m[0][0] * m->m[1][1] - m->m[1][0] * m->m[0][1];
    const float dot = cx * m->m[0][2] + cy * m->m[1][2] + cz * m->m[2][2];
    Log("    (right x up) . forward = % .4f  -> %s-handed",
        dot, dot > 0 ? "LEFT" : "RIGHT");
}

// A projection matrix built by a perspective-FOV call has a recoverable FOV and
// near/far plane, which is exactly what we need for the FOV and near-clip work.
static void LogProjDecomposed(const D3DMATRIX8* m)
{
    if (!m) return;
    const float m00 = m->m[0][0], m11 = m->m[1][1];
    const float m22 = m->m[2][2], m32 = m->m[3][2];
    if (m11 != 0.0f) {
        const double fovY = 2.0 * atan(1.0 / (double)m11) * 180.0 / 3.14159265358979;
        Log("    fovY ~ %.2f deg   aspect ~ %.4f", fovY, (m00 != 0.0f) ? (m11 / m00) : 0.0f);
    }
    if (m22 != 1.0f && m22 != 0.0f) {
        // LH perspective: m22 = zf/(zf-zn), m32 = -zn*zf/(zf-zn).
        // So zn = -m32/m22 and zf = -m32/(m22-1). Both negations matter -
        // an earlier version dropped them and reported a negative near plane.
        const float zn = -m32 / m22;
        const float zf = -m32 / (m22 - 1.0f);
        Log("    near ~ %.4f   far ~ %.2f", zn, zf);
    }
}

static void LogWindowIfChanged(HWND h)
{
    if (!h) return;
    LONG style = GetWindowLong(h, GWL_STYLE);
    RECT rc; GetClientRect(h, &rc);
    if (h == g_wnd && style == g_lastStyle &&
        rc.right == g_lastRect.right && rc.bottom == g_lastRect.bottom)
        return;

    g_wnd = h; g_lastStyle = style; g_lastRect = rc;
    RECT wr; GetWindowRect(h, &wr);
    Log("WINDOW  hwnd=%p  style=0x%08lX%s%s%s  client=%ldx%ld  window=%ldx%ld  screen=%dx%d",
        h, style,
        (style & WS_CAPTION)    == WS_CAPTION ? " CAPTION"    : "",
        (style & WS_THICKFRAME) ? " THICKFRAME" : "",
        (style & WS_BORDER)     ? " BORDER"     : "",
        rc.right, rc.bottom,
        wr.right - wr.left, wr.bottom - wr.top,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

// ---------------------------------------------------------------------------
// Camera hunt
//
// Run 2 settled it: XOPS transforms its world on the CPU and submits
// pre-transformed XYZRHW vertices, so the device's view matrix is vestigial
// after startup and cannot drive the camera. The camera therefore lives in the
// game's own memory, and that is what we have to find.
//
// The lever: the game DOES set a real view matrix when a level loads (frame 52
// in run 2, eye -18.402 3.481 4.180). That eye position is a known-good value
// to search for. Scan the EXE's data sections for three consecutive floats
// matching it, then watch the candidates every sampled frame - the one that
// tracks the player is the camera position, and the struct around it holds the
// angles.
// ---------------------------------------------------------------------------

static BOOL  g_movedThisInterval = FALSE;
static DWORD g_keysSeen[12];
static int   g_keysSeenN = 0;

// CAMERA HUNT, v5 - differential scan driven by the movement keys.
//
// Why the earlier approach failed twice, and what replaces it:
//
//   * Runs 3 and 4 searched for the eye position taken from the view matrix.
//     Run 4 "found" three addresses that tracked the camera perfectly - and
//     all three were the PROBE's own memory: two stack copies of our eye[3]
//     local, and our g_lastEye global. A scanner searching its own process
//     must exclude itself. We now take MEM_PRIVATE regions only, which leaves
//     out every loaded image including ours, and skip our own stack.
//
//   * More importantly the view matrix is STALE during gameplay - it stops
//     updating around frame 59 - so there is no in-game eye position to search
//     for. The premise was wrong, not just the filter.
//
// So find it the way a memory scanner does, with no reference value at all.
// We already own GetKeyboardState, so we know when a movement key is held.
// Per interval:
//
//     moving interval -> the camera's floats MUST have changed
//     still interval  -> the camera's floats MUST NOT have changed
//
// Intersect over a few intervals and the player position falls out, with no
// choreography needed beyond walking and stopping.

#define SCAN_INTERVAL   30          /* frames per sample interval */
#define MAX_SNAP_BYTES  (64u << 20)
#define MAX_CANDS       200000

typedef struct { BYTE* base; SIZE_T size; SIZE_T snapOff; } Region;
static Region   g_rgn[1024];
static int      g_rgnCount = 0;
static BYTE*    g_snap     = NULL;
static SIZE_T   g_snapUsed = 0;

static UINT_PTR* g_cands     = NULL;
static float*    g_candPrev  = NULL;
static int       g_candN     = 0;
static short*    g_score     = NULL;
static float*    g_candMin   = NULL;   /* lowest value ever observed  */
static float*    g_candMax   = NULL;   /* highest value ever observed */
static short*    g_candMoves = NULL;   /* how many samples differed   */
static float*    g_candIdle  = NULL;   /* value at the start of an idle run */
static short*    g_candVeto  = NULL;   /* set if it moved while we were idle */
static short*    g_picked    = NULL;   /* already on the probe list this build */
/* The owner plays single-player on flat ground and never jumps, so the
   player's height is a CONSTANT. A stored position is three consecutive
   floats; on flat ground two of them move together as he walks and the third
   never changes at all. That shape is far rarer than any single moving float,
   so we track each candidate's immediate neighbours and whether they have
   stayed perfectly constant. */
static float*    g_nbrP4     = NULL;   /* value at address + 4 */
static float*    g_nbrM4     = NULL;   /* value at address - 4 */
static short*    g_nbrP4c    = NULL;   /* 1 while +4 has never changed */
static short*    g_nbrM4c    = NULL;   /* 1 while -4 has never changed */
static long      g_ballFrames = 0;    /* >0 while a ballistic window is open */
static float*    g_ballStart = NULL;   /* ballistic test: value at window open */
static float*    g_ballPeak  = NULL;   /* ballistic test: highest in the window */
static short*    g_ballHit   = NULL;   /* ballistic test: windows passed */
static BOOL      g_haveCands = FALSE;

static UINT_PTR  g_exeLo = 0, g_exeHi = 0;   /* host image, for IDA addresses */
static unsigned g_exeRgnKB = 0;      /* KB of host .data/.bss in the snapshot */
static int  g_intervals = 0;
static BOOL g_huntDone  = FALSE;

/* Run 4 found the camera three times over and every hit was the probe's own
   memory, so v5 excluded all images and our stack. That was the wrong cut: an
   EXE's .data is MEM_IMAGE, and a 2003 game keeps the player position in a
   static global, so excluding images excluded the answer. Run 6 proved it -
   not one of 28892 candidates was inside xops096.exe.

   So exclude US specifically - our stack, our image, and the blocks we
   allocated - and let the host's own writable data back in. */
static BOOL IsOurs(void* addr)
{
    int onStack;
    MEMORY_BASIC_INFORMATION a, b;
    void* mine[16];
    int i, n = 0;

    if (!VirtualQuery(addr, &b, sizeof(b))) return FALSE;
    if (VirtualQuery(&onStack, &a, sizeof(a)) && a.AllocationBase == b.AllocationBase) return TRUE;
    if (VirtualQuery((void*)&IsOurs, &a, sizeof(a)) && a.AllocationBase == b.AllocationBase) return TRUE;

    mine[n++] = g_snap;     mine[n++] = g_cands;    mine[n++] = g_candPrev;
    mine[n++] = g_score;    mine[n++] = g_candMin;  mine[n++] = g_candMax;
    mine[n++] = g_candMoves;mine[n++] = g_candIdle; mine[n++] = g_candVeto;
    mine[n++] = g_nbrP4;    mine[n++] = g_nbrM4;    mine[n++] = g_nbrP4c;
    mine[n++] = g_nbrM4c;   mine[n++] = g_ballStart;mine[n++] = g_ballPeak;
    mine[n++] = g_ballHit;   mine[n++] = g_picked;
    for (i = 0; i < n; ++i)
        if (mine[i] && VirtualQuery(mine[i], &a, sizeof(a))
            && a.AllocationBase == b.AllocationBase) return TRUE;
    return FALSE;
}

static void BuildSnapshot(void)
{
    g_rgnCount = 0; g_snapUsed = 0;
    if (!g_snap) {
        g_snap = (BYTE*)VirtualAlloc(NULL, MAX_SNAP_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_snap) { Log("CAMHUNT  could not reserve snapshot memory"); g_huntDone = TRUE; return; }
    }

    MEMORY_BASIC_INFORMATION mbi;
    BYTE* p = NULL;
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);
    g_exeRgnKB = 0;
    g_exeLo = exe; g_exeHi = exe + 0x100000;
    while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        /* The host EXE's writable sections - .data and .bss - are where a
           static global lives. Its .text is PAGE_EXECUTE_READ and so is cut
           by the protect test below without needing a special case. */
        const BOOL hostData = mbi.Type == MEM_IMAGE
                              && (UINT_PTR)mbi.AllocationBase == exe;
        const BOOL want =
            mbi.State == MEM_COMMIT &&
            (mbi.Type == MEM_PRIVATE || hostData) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0 &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            mbi.RegionSize >= 64;

        if (want && hostData) g_exeRgnKB += (unsigned)(mbi.RegionSize / 1024);
        if (want && !IsOurs(mbi.BaseAddress) &&
            g_rgnCount < 1024 && g_snapUsed + mbi.RegionSize <= MAX_SNAP_BYTES)
        {
            g_rgn[g_rgnCount].base    = (BYTE*)mbi.BaseAddress;
            g_rgn[g_rgnCount].size    = mbi.RegionSize;
            g_rgn[g_rgnCount].snapOff = g_snapUsed;
            memcpy(g_snap + g_snapUsed, mbi.BaseAddress, mbi.RegionSize);
            g_snapUsed += mbi.RegionSize;
            ++g_rgnCount;
        }
        if (next <= p) break;
        p = next;
    }
    Log("CAMHUNT  snapshot: %d regions, %u KB  (of which %u KB is xops096.exe's"
        " own writable data - the probe itself is excluded)",
        g_rgnCount, (unsigned)(g_snapUsed / 1024), g_exeRgnKB);
    if (g_exeRgnKB == 0)
        Log("CAMHUNT  WARNING: no EXE data section in the snapshot - a static"
            " global cannot be found from here.");
}

static BOOL Plausible(float v)
{
    float a;
    if (!(v == v)) return FALSE;              /* NaN */
    a = (float)fabs(v);
    return a > 0.0001f && a < 20000.0f;
}

static void CollectChanged(void)
{
    int r;
    if (!g_cands) {
        g_cands    = (UINT_PTR*)VirtualAlloc(NULL, MAX_CANDS * sizeof(UINT_PTR), MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candPrev = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_score    = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candMin  = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candMax  = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candMoves= (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candIdle = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_candVeto = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_nbrP4    = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_nbrM4    = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_nbrP4c   = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_nbrM4c   = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_ballStart= (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_ballPeak = (float*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(float),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_ballHit  = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        g_picked   = (short*)   VirtualAlloc(NULL, MAX_CANDS * sizeof(short),    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        if (!g_cands || !g_candPrev || !g_score || !g_candMin || !g_candMax || !g_candMoves
            || !g_candIdle || !g_candVeto) {
            Log("CAMHUNT  out of memory"); g_huntDone = TRUE; return;
        }
    }
    g_candN = 0;
    for (r = 0; r < g_rgnCount && g_candN < MAX_CANDS; ++r) {
        const float* live = (const float*)g_rgn[r].base;
        const float* old  = (const float*)(g_snap + g_rgn[r].snapOff);
        const SIZE_T n = g_rgn[r].size / 4;
        SIZE_T i;
        for (i = 0; i < n && g_candN < MAX_CANDS; ++i) {
            float d;
            if (live[i] == old[i]) continue;
            if (!Plausible(live[i]) || !Plausible(old[i])) continue;
            d = (float)fabs(live[i] - old[i]);
            if (d > 50.0f) continue;          /* a camera does not teleport */
            if (d < 0.0005f) continue;        /* nor does it merely jitter */
            g_cands[g_candN]    = (UINT_PTR)&live[i];
            g_candPrev[g_candN] = live[i];
            g_candMin[g_candN]  = (live[i] < old[i]) ? live[i] : old[i];
            g_candMax[g_candN]  = (live[i] > old[i]) ? live[i] : old[i];
            g_candMoves[g_candN]= 1;
            /* Seed the neighbours. A neighbour that is still identical later
               is a constant - a candidate for the player's height. */
            g_nbrP4c[g_candN] = SafeRead((UINT_PTR)&live[i] + 4, &g_nbrP4[g_candN]) ? 1 : 0;
            g_nbrM4c[g_candN] = SafeRead((UINT_PTR)&live[i] - 4, &g_nbrM4[g_candN]) ? 1 : 0;
            ++g_candN;
        }
    }
    g_haveCands = TRUE;
    Log("CAMHUNT  interval %d (MOVING): %d floats changed plausibly", g_intervals, g_candN);
}

// v6: SCORE candidates, do not eliminate them.
//
// v5 eliminated on the first mismatch, and that deleted the right answer. Two
// ways it happens, both seen in run 5:
//
//   * the STILL test demanded exact equality, but a standing player still has
//     gravity resolved every frame, so the position jitters in the low bits;
//   * one bad interval is fatal - if the player stops partway through a
//     "moving" interval, or walks a loop back to the same spot, the position
//     looks unchanged and is dropped with no way back.
//
// Run 5 narrowed 4552 -> 1, and the survivor held a constant 0.012 with zero
// neighbours: a frame-time delta (1/83), not a coordinate. Confidently wrong
// is the worst kind of wrong, so scoring replaces elimination: every candidate
// keeps a running tally, nothing is ever removed, and the best few are
// reported. A single odd interval costs a point instead of the answer.

#define STILL_EPS   0.0008f     /* below this counts as "did not move" */
#define MOVE_EPS    0.0050f     /* above this counts as "moved"        */



/* Is this address still committed and writable? Heap blocks get freed, and a
   candidate list outlives the memory it points at. Checking is far cheaper
   than recovering from a fault. */
static BOOL AddrWritable(UINT_PTR a)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) != sizeof(mbi)) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return FALSE;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static BOOL SafeRead(UINT_PTR a, float* out)
{
    __try { *out = *(const float*)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    return (*out == *out);               /* reject NaN while we are here */
}

static BOOL SafeWrite(UINT_PTR a, float v)
{
    __try { *(float*)a = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    return TRUE;
}

/* Watch every candidate's running range. Called a few times a second; the
   whole point is to know what each variable's natural span looks like, so the
   probe can swing it across its OWN range rather than by some arbitrary
   amount. That is both more informative and much safer. */
static void SampleRanges(void)
{
    int i;
    if (!g_haveCands || !g_candMin) return;
    for (i = 0; i < g_candN; ++i) {
        float v;
        if (!SafeRead(g_cands[i], &v)) { g_score[i] = -999; continue; }
        /* Run 6 listed candidates whose range topped out at 3.7e19. A float
           that leaves the plausible world range is reinterpreted memory - a
           pointer, a packed colour, freed heap - not a coordinate. The initial
           pool test only checked the delta, so these survived; kill them on
           sight instead of letting them clutter the list. */
        if (!(v == v) || (float)fabs(v) > 20000.0f) { g_candVeto[i] = 1; continue; }
        if (v < g_candMin[i]) g_candMin[i] = v;
        if (v > g_candMax[i]) g_candMax[i] = v;
        if (v != g_candPrev[i] && g_candMoves[i] < 30000) ++g_candMoves[i];

        if (g_nbrP4c[i]) {
            float n;
            if (!SafeRead(g_cands[i] + 4, &n) || n != g_nbrP4[i]) g_nbrP4c[i] = 0;
        }
        if (g_nbrM4c[i]) {
            float n;
            if (!SafeRead(g_cands[i] - 4, &n) || n != g_nbrM4[i]) g_nbrM4c[i] = 0;
        }
    }
}


// ---------------------------------------------------------------------------
// IDLE VETO
//
// The owner's observation, and it is the sharpest filter we have: the player
// does not move unless a key is held, and does not turn unless the mouse
// moves. So **any variable that changes while there is no input at all cannot
// be the camera.** It is a timer, an animation phase, an enemy, a particle, or
// physics on something that is not us.
//
// This is a negative test, which makes it much stronger than the positive one.
// "It changed while I walked" has thousands of coincidental matches; "it stayed
// perfectly still while I did nothing, every time" has very few.
//
// Idle means, for a whole run of frames: no key and no mouse button down (both
// live in the same GetKeyboardState array), and the cursor has not moved. We
// can see the cursor exactly, because the game recentres it with SetCursorPos
// and reads it back with GetCursorPos - both of which we own, so the delta we
// compute is the same one the game computes.
// ---------------------------------------------------------------------------

static BOOL  g_anyKeyDown   = FALSE;
static BOOL  g_mouseMoved   = FALSE;
static POINT g_lastCursor   = {0, 0};
static BOOL  g_cursorKnown  = FALSE;
static long  g_idleRun      = 0;        /* consecutive idle frames */
static BOOL  g_idleBaseline = FALSE;    /* have we recorded the run's start? */
static long  g_idleRuns     = 0;
static long  g_vetoTotal    = 0;

#define IDLE_SETTLE  12                 /* frames of idle before we trust it */
#define IDLE_EPS     0.0008f


/* Called once per frame from Present, after input has been read. */
static void IdleTick(void)
{
    int i;
    const BOOL idle = !g_anyKeyDown && !g_mouseMoved;

    /* A ballistic window is deliberately a period of idle motion: the player
       stands still while gravity moves him. Vetoing during it would throw away
       exactly the candidate we are trying to find. */
    if (g_ballFrames > 0) { g_anyKeyDown = FALSE; g_mouseMoved = FALSE; return; }

    if (!idle) {
        g_idleRun = 0;
        g_idleBaseline = FALSE;
        g_anyKeyDown = FALSE; g_mouseMoved = FALSE;
        return;
    }

    ++g_idleRun;
    g_anyKeyDown = FALSE; g_mouseMoved = FALSE;

    if (!g_haveCands || !g_candIdle) return;

    /* Wait a few frames before trusting the run: a player who has just let go
       of a key may still be decelerating, and one who has just landed may
       still be settling. */
    if (g_idleRun < IDLE_SETTLE) return;

    if (!g_idleBaseline) {
        for (i = 0; i < g_candN; ++i)
            if (!SafeRead(g_cands[i], &g_candIdle[i])) g_candIdle[i] = 0.0f;
        g_idleBaseline = TRUE;
        ++g_idleRuns;
        return;
    }

    if ((g_frame % 5) != 0) return;

    for (i = 0; i < g_candN; ++i) {
        float v;
        if (g_candVeto[i]) continue;                  /* already ruled out */
        if (!SafeRead(g_cands[i], &v)) continue;
        if ((float)fabs(v - g_candIdle[i]) > IDLE_EPS) {
            g_candVeto[i] = 1;
            ++g_vetoTotal;
        }
    }
}

static void ScoreCandidates(BOOL wasMoving)
{
    int i, best = -32768, bestN = 0;
    for (i = 0; i < g_candN; ++i) {
        float now, d;
        now = *(const float*)g_cands[i];
        if (!(now == now)) { g_score[i] -= 2; continue; }      /* NaN */
        d = (float)fabs(now - g_candPrev[i]);
        if (wasMoving) {
            if (d > MOVE_EPS && d < 50.0f) ++g_score[i];
            else                           --g_score[i];
        } else {
            if (d < STILL_EPS) ++g_score[i];
            else               --g_score[i];
        }
        g_candPrev[i] = now;
        if (g_score[i] > best) best = g_score[i];
    }
    for (i = 0; i < g_candN; ++i) if (g_score[i] == best) ++bestN;
    Log("CAMHUNT  interval %d (%s): %d tracked, best score %d held by %d, "
        "%d vetoed by idle",
        g_intervals, wasMoving ? "MOVING" : "STILL", g_candN, best, bestN, g_vetoTotal);
}

/* Is this address part of a run of three well-scoring floats? A position is
   almost always stored as x,y,z together, so a triple is far stronger
   evidence than any single float. */
static int TripleScore(UINT_PTR addr, int minScore)
{
    int i, hits = 0;
    for (i = 0; i < g_candN; ++i) {
        if (g_score[i] < minScore) continue;
        if (g_cands[i] == addr + 4 || g_cands[i] == addr + 8) ++hits;
    }
    return hits;
}

static void ReportCandidates(void)
{
    int i, best = -32768, shown = 0, threshold;
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);

    if (!g_haveCands || g_huntDone || !g_score) return;
    if (g_intervals % 4) return;                 /* report every 4th interval */

    for (i = 0; i < g_candN; ++i) if (g_score[i] > best) best = g_score[i];
    threshold = best - 1;
    if (best < 3) { Log("    CAMHUNT  best score only %d - keep walking and stopping", best); return; }

    Log("    CAMHUNT  top candidates (best score %d of %d intervals):", best, g_intervals);
    for (i = 0; i < g_candN && shown < 12; ++i) {
        const float* f;
        int trip;
        char ida[48];
        if (g_score[i] < threshold) continue;
        if (g_candVeto[i]) continue;
        f = (const float*)g_cands[i];
        trip = TripleScore(g_cands[i], threshold);
        ida[0] = 0;
        if (g_cands[i] >= exe && g_cands[i] < exe + 0x100000)
            _snprintf(ida, sizeof(ida), " IDA 0x%08IX", g_cands[i] - exe + 0x400000);
        Log("      0x%08IX%s score=%d triple=%d  [% .3f % .3f % .3f % .3f]",
            g_cands[i], ida, g_score[i], trip, f[0], f[1], f[2], f[3]);
        ++shown;
    }
    Log("      triple=2 means the next two floats score as well -> an x,y,z");
    Log("      triple, which is what a stored position looks like.");
}

static void HuntTick(void)
{
    BOOL moving, still;
    if (g_huntDone) return;
    if (g_frame == 0 || (g_frame % SCAN_INTERVAL) != 0) return;

    moving = g_movedThisInterval;
    still  = !g_movedThisInterval;
    ++g_intervals;

    if (!g_rgnCount) {
        if (still) BuildSnapshot();
    } else if (!g_haveCands) {
        if (moving) CollectChanged();
        else        BuildSnapshot();
    } else {
        ScoreCandidates(moving);
        ReportCandidates();
    }

    g_movedThisInterval = FALSE;
}

// ---------------------------------------------------------------------------
// Input discovery
//
// The question this answers: can we hand XOPS its input directly, in-process,
// rather than synthesising OS-level keyboard and mouse events? The answer
// depends entirely on which path it reads input through, and there are only
// three realistic candidates. So: dump what the EXE imports, then count how
// often each candidate is actually called per frame. The one with a per-frame
// call count is the path the game really uses.
//
//   DirectInput  (dinput.dll / dinput8.dll)
//       -> proxy dinput and return our own state from GetDeviceState.
//          Cleanest of the three: no spoofing, no OS involvement at all.
//   GetAsyncKeyState / GetKeyboardState / GetKeyState  (user32)
//       -> IAT-patch them and answer from our own key table. Also clean.
//   Window messages (WM_KEYDOWN et al, read in the wndproc)
//       -> subclass the window and post our own messages in-process.
//
// Note what this does NOT solve: feeding the game mouse deltas still turns the
// game's camera, because its mouse look IS its camera. So this makes every
// button, key and movement input clean, but it does not by itself decouple aim
// from view - that still needs F-7 test 1. Worth being clear about, because
// the two problems look similar and only one is solved here.
// ---------------------------------------------------------------------------

#define IN_GETASYNCKEYSTATE  0
#define IN_GETKEYBOARDSTATE  1
#define IN_GETKEYSTATE       2
#define IN_GETCURSORPOS      3
#define IN_SETCURSORPOS      4
#define IN_GETFOREGROUNDWINDOW 5
#define IN_COUNT             6

static const char* kInputNames[IN_COUNT] = {
    "GetAsyncKeyState", "GetKeyboardState", "GetKeyState",
    "GetCursorPos", "SetCursorPos", "GetForegroundWindow"
};
static long  g_inCalls[IN_COUNT]  = {0,0,0,0,0,0};
static long  g_inFrame[IN_COUNT]  = {0,0,0,0,0,0};
static FARPROC g_inReal[IN_COUNT] = {0,0,0,0,0,0};

/* Keep running when the window is not focused. On by default: the whole point
   of the UDP tester is that something else has focus. */
static BOOL g_vrExclusive = FALSE;  /* defined here; the gameplay-input block
                                       below is where it is explained */
static BOOL g_noPause  = TRUE;
static HWND g_gameWnd  = NULL;    /* learned from CreateDevice's hFocusWindow */

typedef HWND (WINAPI *PFN_GetForegroundWindow)(void);

// Replace a function pointer in the host EXE's import address table. This is
// how we will later *supply* input rather than merely count it, so proving it
// works now is worth more than the counts themselves.
static void* PatchIat(HMODULE hMod, const char* fnName, void* replacement)
{
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;

    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
         imp->Name; ++imp)
    {
        IMAGE_THUNK_DATA* orig = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk) orig = iat;

        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* nm = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
            if (_stricmp((const char*)nm->Name, fnName) != 0) continue;

            void* old = (void*)iat->u1.Function;
            DWORD prot = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &prot)) {
                iat->u1.Function = (ULONG_PTR)replacement;
                VirtualProtect(&iat->u1.Function, sizeof(void*), prot, &prot);
                return old;
            }
            return NULL;
        }
    }
    return NULL;
}

typedef SHORT (WINAPI *PFN_GetAsyncKeyState)(int);
typedef BOOL  (WINAPI *PFN_GetKeyboardState)(PBYTE);
typedef SHORT (WINAPI *PFN_GetKeyState)(int);
typedef BOOL  (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL  (WINAPI *PFN_SetCursorPos)(int, int);

static SHORT WINAPI hkGetAsyncKeyState(int vk)
{ ++g_inCalls[IN_GETASYNCKEYSTATE]; ++g_inFrame[IN_GETASYNCKEYSTATE];
  return ((PFN_GetAsyncKeyState)g_inReal[IN_GETASYNCKEYSTATE])(vk); }

static BOOL WINAPI hkGetKeyboardState(PBYTE p)
{
    BOOL r;
    ++g_inCalls[IN_GETKEYBOARDSTATE]; ++g_inFrame[IN_GETKEYBOARDSTATE];
    r = ((PFN_GetKeyboardState)g_inReal[IN_GETKEYBOARDSTATE])(p);
    if (r && p) {
        /* Which keys are down decides whether this interval counts as moving
           or still for the camera hunt. */
        static const int kMove[8] = { 'W', 'A', 'S', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
        int i, vk, j;
        for (i = 0; i < 8; ++i)
            if (p[kMove[i]] & 0x80) g_movedThisInterval = TRUE;
        /* Read before StickKeys runs, so the camera hunt still sees only the
           owner's real keypresses. A stick-supplied W is our own doing and
           must not be counted as evidence. */

        ProbeKeys(p);          /* our own control keys, read then removed */
        VrExclusiveKeys(p);    /* then the game loses the desktop keyboard */
        StickKeys(p);          /* then VR input is added back */

        /* Anything still held after that is real input - including the mouse
           buttons, which live in this same array. */
        for (vk = 1; vk < 256; ++vk)
            if (p[vk] & 0x80) { g_anyKeyDown = TRUE; break; }

        /* Record the distinct keys actually used, so a wrong guess about the
           movement bindings shows up in the log instead of silently skewing
           every interval. */
        for (vk = 8; vk < 256; ++vk) {
            BOOL seen = FALSE;
            if (!(p[vk] & 0x80)) continue;
            for (j = 0; j < g_keysSeenN; ++j) if (g_keysSeen[j] == (DWORD)vk) seen = TRUE;
            if (!seen && g_keysSeenN < 12) {
                g_keysSeen[g_keysSeenN++] = (DWORD)vk;
                Log("KEYS      vk 0x%02X ('%c') pressed at frame %ld", vk,
                    (vk >= 32 && vk < 127) ? (char)vk : '?', g_frame);
            }
        }
    }
    return r;
}

static SHORT WINAPI hkGetKeyState(int vk)
{ ++g_inCalls[IN_GETKEYSTATE]; ++g_inFrame[IN_GETKEYSTATE];
  return ((PFN_GetKeyState)g_inReal[IN_GETKEYSTATE])(vk); }

static BOOL WINAPI hkGetCursorPos(LPPOINT p)
{
    BOOL r;
    ++g_inCalls[IN_GETCURSORPOS]; ++g_inFrame[IN_GETCURSORPOS];
    r = ((PFN_GetCursorPos)g_inReal[IN_GETCURSORPOS])(p);
    if (r && p) {
        /* The game recentres the cursor and reads it back to get its look
           delta. We see both halves, so this is exactly the delta the game
           itself computes - a non-zero one means the player turned. */
        if (g_cursorKnown && (p->x != g_lastCursor.x || p->y != g_lastCursor.y))
            g_mouseMoved = TRUE;
        g_lastCursor = *p;
        g_cursorKnown = TRUE;
    }
    /* While steering, the game must not also turn the view from the mouse, or
       the two fight every frame. The game recentres the cursor with
       SetCursorPos and reads it back with GetCursorPos to get its look delta -
       and we own both. Returning exactly what was last set makes that delta
       zero, so the game's own look is suppressed at the source without
       synthesising anything or blocking the real mouse. */
    if (r && p && g_cursorKnown) {
        /* Two reasons to hand back the cursor exactly where the game last put
           it, which makes its look delta zero:

           1. We are steering, and the game must not also turn the view from
              the mouse or the two fight every frame.
           2. The window is not really focused. The cursor is then wherever
              the player left it over another window - the UDP tester, say -
              and the game would read that distance as one enormous look
              delta and spin. Suppressing it is what makes "keep running
              while unfocused" actually usable rather than just not-paused. */
        const BOOL vrOwnsInput = g_vrExclusive && InGameplay();
        const BOOL reallyFocused =
            !g_gameWnd || !g_inReal[IN_GETFOREGROUNDWINDOW] ||
            ((PFN_GetForegroundWindow)g_inReal[IN_GETFOREGROUNDWINDOW])() == g_gameWnd;
        if (g_steer || vrOwnsInput || !reallyFocused) *p = g_lastCursor;
    }

    /* Mid-update: the game has just taken the mouse delta and has not yet
       turned it into a direction. A yaw written here is seen by that step. */
    if (g_probePhase == PHASE_INPUT) ProbeTick();
    return r;
}

static HWND WINAPI hkGetForegroundWindow(void)
{
    HWND real;
    ++g_inCalls[IN_GETFOREGROUNDWINDOW]; ++g_inFrame[IN_GETFOREGROUNDWINDOW];
    real = ((PFN_GetForegroundWindow)g_inReal[IN_GETFOREGROUNDWINDOW])();
    /* Lie only when we know what to lie with, and only to this one caller -
       the real answer still goes to everyone else in the process. */
    if (g_noPause && g_gameWnd) return g_gameWnd;
    return real;
}

static BOOL WINAPI hkSetCursorPos(int x, int y)
{
    ++g_inCalls[IN_SETCURSORPOS]; ++g_inFrame[IN_SETCURSORPOS];
    g_lastCursor.x = x; g_lastCursor.y = y; g_cursorKnown = TRUE;
    return ((PFN_SetCursorPos)g_inReal[IN_SETCURSORPOS])(x, y);
}

// Walk the EXE's import table and log every module, flagging the ones that
// decide the input strategy.
static void DumpImports(void)
{
    HMODULE hExe = GetModuleHandleA(NULL);
    BYTE* base = (BYTE*)hExe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;

    Log("IMPORTS of the host EXE");
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
         imp->Name; ++imp)
    {
        const char* mod = (const char*)(base + imp->Name);
        const char* note = "";
        if (_stricmp(mod, "dinput.dll")  == 0) note = "   <- DirectInput 7: proxy it and return our own device state";
        if (_stricmp(mod, "dinput8.dll") == 0) note = "   <- DirectInput 8: proxy it and return our own device state";
        if (_stricmp(mod, "d3d8.dll")    == 0) note = "   <- us";
        if (_stricmp(mod, "xinput1_3.dll") == 0) note = "   <- XInput";
        Log("  %-24s%s", mod, note);

        // For the input modules, list the functions too - the specific entry
        // points are what a proxy has to implement.
        if (_strnicmp(mod, "dinput", 6) != 0) continue;
        IMAGE_THUNK_DATA* orig = (IMAGE_THUNK_DATA*)(base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                Log("      ordinal %lu", (DWORD)(orig->u1.Ordinal & 0xFFFF));
            } else {
                IMAGE_IMPORT_BY_NAME* nm = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                Log("      %s", (const char*)nm->Name);
            }
        }
    }
    Log("");
}

static void HookInput(void)
{
    HMODULE hExe = GetModuleHandleA(NULL);
    static const char* names[IN_COUNT] = {
        "GetAsyncKeyState", "GetKeyboardState", "GetKeyState",
        "GetCursorPos", "SetCursorPos", "GetForegroundWindow"
    };
    void* hooks[IN_COUNT] = {
        (void*)hkGetAsyncKeyState, (void*)hkGetKeyboardState, (void*)hkGetKeyState,
        (void*)hkGetCursorPos,     (void*)hkSetCursorPos,
        (void*)hkGetForegroundWindow
    };

    Log("INPUT  IAT patch results (a patched entry proves we can supply this");
    Log("       function's answer later, not merely count it):");
    Log("       GetForegroundWindow is how XOPS detects focus loss and pauses;");
    Log("       patching it is what keeps the game running while the UDP");
    Log("       tester has focus. Toggle with the backslash key.");
    for (int i = 0; i < IN_COUNT; ++i) {
        void* old = PatchIat(hExe, names[i], hooks[i]);
        g_inReal[i] = (FARPROC)old;
        Log("  %-18s %s", names[i], old ? "imported - PATCHED" : "not imported");
    }
    Log("");
}


// ---------------------------------------------------------------------------
// ACTIVE PROBE - wiggle one variable at a time and let a human judge it
//
// The passive scanner narrows by correlation and has now twice produced a
// confident wrong answer. This is the opposite approach and a far better
// instrument: drive a known signal into one candidate at a time and let the
// person looking at the screen say what it did. A sine wave is the right
// signal because it is unmistakable - nothing in a game oscillates smoothly at
// 1.5 seconds a cycle unless we are making it.
//
// Controls (press 9 to arm). Only these keys are swallowed - which we can do
// at all because we own GetKeyboardState - and movement is left untouched:
//
//     9      toggle probe mode (the game uses the F keys)
//     8      next candidate   (restores the previous one first)
//     7      previous candidate
//     0      TAG the current candidate - logged with a tag number
//     - / =  halve / double the swing
//
// Deliberately NOT on W/A/S/D. Swallowing those would leave the player unable
// to move while probing, and walking around is the most useful test there is:
// a camera variable tracks you, a coincidence does not.
//
// Each candidate's original value is saved on selection and restored on
// leaving it, so a wrong guess does not accumulate damage.
//
// The write happens in BeginScene: after the game has finished its own update
// for the frame, before it transforms and draws. That is the window where a
// camera variable still affects this frame's image.

#define PROBE_PERIOD 90.0f        /* frames per sine cycle - 1.5s at 60fps */
#define PROBE_MAX    256

static BOOL     g_probeMode = FALSE;
static int      g_probeIdx  = -1;
static UINT_PTR g_probeAddr = 0;
static float    g_probeOrig = 0.0f;
static float    g_probeAmp  = 1.0f;
static int      g_tagCount  = 0;
static UINT_PTR g_probeList[PROBE_MAX];
static short    g_probeScore[PROBE_MAX];
static float    g_probeMinV[PROBE_MAX];
static float    g_probeMaxV[PROBE_MAX];
static short    g_probeMoves[PROBE_MAX];
static short    g_probeShape[PROBE_MAX];
static short    g_probeBall[PROBE_MAX];
static const char* g_probeWhat[PROBE_MAX];
static char     g_probeClass[PROBE_MAX];   /* 'U' 'L' 'O' 'P', or 0 */
static float    g_probeHeight[PROBE_MAX];
static float    g_probeCentre = 0.0f;
static float    g_probeSwing  = 0.0f;
static int      g_probeN    = 0;

/* Take the best-scoring candidates, highest first. */

/* Is `addr` in the pool, unvetoed, and scoring at least `minScore`? The pool is
   built by scanning memory in ascending order, so a neighbour that is also in
   the pool sits within a few indices of this one - a small window search is
   enough and avoids an O(n^2) sweep. */
static BOOL PoolHasMover(int nearIdx, UINT_PTR addr, int minScore)
{
    int j, lo = nearIdx - 6, hi = nearIdx + 6;
    if (lo < 0) lo = 0;
    if (hi > g_candN) hi = g_candN;
    for (j = lo; j < hi; ++j)
        if (g_cands[j] == addr && !g_candVeto[j] && g_score[j] >= minScore) return TRUE;
    return FALSE;
}

/* Score the "position triple on flat ground" shape:
      2  this float moves, the next is constant, the one after that moves
         -> this is X of (X, Y, Z) with Y the fixed height
      1  this float moves and the PREVIOUS is constant with a mover before it
         -> this is Z of (X, Y, Z)
      0  no recognisable shape
   Two movers astride one constant is a much rarer pattern than any single
   moving float, which is what makes it worth testing for. */
/* A standing player's height is metres above the floor, never zero. Run 6's
   whole top-24 claimed the XYZ shape with a constant neighbour of 0.0000 -
   structurally right, physically impossible, and in fact a stride-16 array in
   d3d8's heap. Requiring the constant to be a plausible eye height costs
   nothing and removes that entire class. The view matrix put the in-game eye
   at Y 2.21-2.48, so this window is generous by comparison. */
static BOOL PlausibleHeight(float h)
{
    return h == h && h > 0.2f && h < 12.0f;
}

static int ShapeScore(int i, int minScore)
{
    if (g_nbrP4c[i] && PlausibleHeight(g_nbrP4[i])
        && PoolHasMover(i, g_cands[i] + 8, minScore)) return 2;
    if (g_nbrM4c[i] && PlausibleHeight(g_nbrM4[i])
        && PoolHasMover(i, g_cands[i] - 8, minScore)) return 1;
    return 0;
}


// ---------------------------------------------------------------------------
// BALLISTIC TEST - press the height cheat, let gravity answer
//
// Press 6, then the height cheat, then stand still. For the length of the
// window the idle veto is suspended (gravity settling WOULD trip it, and that
// is the very motion we want to see) and each candidate is judged on shape:
//
//     rise  : it climbs at least BALL_RISE above where it started
//     return: it comes back to within BALL_SETTLE of where it started
//
// Both halves together are the point. A rising counter passes the first test;
// a jittering float passes the second; only something under gravity passes
// both. Whatever does is the player's height, and the floats at -4 and +4 are
// then X and Z of the same position.
// ---------------------------------------------------------------------------

#define BALL_FRAMES  300        /* ~5s at 60fps - rise and fall with room  */
#define BALL_RISE    0.75f      /* metres of climb required                */
#define BALL_SETTLE  0.05f      /* how near the start it must come back     */

static int    g_ballRuns   = 0;   /* the arrays are declared with the pool */

static void BallisticArm(void)
{
    int i;
    if (!g_haveCands || !g_ballStart) {
        Log("BALLISTIC  no candidate pool yet - walk and stop a few times first");
        return;
    }
    for (i = 0; i < g_candN; ++i) {
        if (!SafeRead(g_cands[i], &g_ballStart[i])) g_ballStart[i] = 0.0f;
        g_ballPeak[i] = g_ballStart[i];
    }
    g_ballFrames = BALL_FRAMES;
    ++g_ballRuns;
    Log("");
    Log("BALLISTIC  window %d open for %d frames (~%ds).", g_ballRuns,
        BALL_FRAMES, BALL_FRAMES / 60);
    Log("BALLISTIC  press the height cheat NOW, then stand still and let");
    Log("BALLISTIC  gravity bring you down. Do not walk during the window.");
}

static void BallisticSample(void)
{
    int i;
    for (i = 0; i < g_candN; ++i) {
        float v;
        if (!SafeRead(g_cands[i], &v)) continue;
        if (v > g_ballPeak[i]) g_ballPeak[i] = v;
    }
}

static void BallisticClose(void)
{
    int i, hits = 0;
    Log("");
    Log("BALLISTIC  window closed - scoring rise-then-return");
    for (i = 0; i < g_candN; ++i) {
        float v, rise;
        if (!SafeRead(g_cands[i], &v)) continue;
        rise = g_ballPeak[i] - g_ballStart[i];
        if (rise < BALL_RISE) continue;
        if ((float)fabs(v - g_ballStart[i]) > BALL_SETTLE) continue;
        ++g_ballHit[i];
        /* Gravity settling looks exactly like "changed while idle", so this
           candidate may already have been vetoed. Passing the ballistic test
           is far better evidence than the veto that killed it, so undo it. */
        g_candVeto[i] = 0;
        ++hits;
    }
    Log("BALLISTIC  %d candidates rose >= %.2f and returned to within %.2f",
        hits, BALL_RISE, BALL_SETTLE);
    if (hits == 0)
        Log("BALLISTIC  nothing passed. Was the cheat actually applied, and was"
            " the whole fall inside the window?");
    g_ballFrames = 0;
}

/* Called once per frame from Present. */
static void BallisticTick(void)
{
    if (g_ballFrames <= 0) return;
    BallisticSample();
    if (--g_ballFrames == 0) BallisticClose();
}

/* Does `i` look like the Y of a position, by the ballistic test? If so its
   neighbours at -4 and +4 are X and Z. */
static int BallScore(int i) { return g_ballHit ? g_ballHit[i] : 0; }

/* Keep only candidates that actually moved, are still addressable, and have a
   usable range. Everything else is noise that would waste a keypress. */
static void BuildProbeList(void)
{
    int i, k, rejRange = 0, rejDead = 0;
    g_probeN = 0;
    if (g_picked) for (i = 0; i < g_candN; ++i) g_picked[i] = 0;
    if (!g_score || !g_candN) return;

    /* Rank by shape first, then by score. A float sitting next to a constant
       with another mover beyond it is worth looking at before a higher-scoring
       float with no structure around it. */
    /* Rank on evidence quality, not on correlation strength. In order:
         a ballistic pass   - gravity answered a keypress; nothing else does
         an address in the EXE's own data - where a 2003 game keeps globals
         the XYZ shape     - two movers astride a plausible constant height
         the walk/stop score - weakest, thousands of coincidental matches
       Run 6 ranked on score alone and handed back a d3d8 vertex array. */
    for (k = 0; k < PROBE_MAX; ++k) {
        int best = -32768, bestI = -1, bestRank = -1;
        for (i = 0; i < g_candN; ++i) {
            int j, used = 0, rank, nearby = 0;
            float span;
            if (g_candVeto[i]) continue;                   /* moved while we were idle */
            if (g_picked[i]) continue;                     /* already on the list */
            span = g_candMax[i] - g_candMin[i];
            if (!(span > 0.0005f)) continue;               /* never really moved */

            /* One array must not eat the list. Run 6's top 24 were 24 entries
               of the same stride-16 block; four samples from any one block is
               plenty to judge it by. */
            for (j = 0; j < g_probeN; ++j) {
                if (g_probeList[j] == g_cands[i]) { used = 1; break; }
                if (g_probeList[j] > g_cands[i] - 0x200
                    && g_probeList[j] < g_cands[i] + 0x200) ++nearby;
            }
            if (used || nearby >= 4) continue;

            rank = BallScore(i) ? 100 + BallScore(i) : 0;
            if (g_cands[i] >= g_exeLo && g_cands[i] < g_exeHi) rank += 10;
            rank += ShapeScore(i, 1);

            if (rank < bestRank) continue;
            if (rank == bestRank && g_score[i] <= best) continue;
            bestRank = rank; best = g_score[i]; bestI = i;
        }
        if (bestI < 0) break;
        if (!AddrWritable(g_cands[bestI])) { g_picked[bestI] = 1; ++rejDead; --k; continue; }
        g_probeList[g_probeN]  = g_cands[bestI];
        g_probeScore[g_probeN] = g_score[bestI];
        g_probeMinV[g_probeN]  = g_candMin[bestI];
        g_probeMaxV[g_probeN]  = g_candMax[bestI];
        g_probeMoves[g_probeN] = g_candMoves[bestI];
        g_probeShape[g_probeN] = (short)ShapeScore(bestI, 1);
        g_probeBall[g_probeN]  = (short)BallScore(bestI);
        g_probeWhat[g_probeN]  = "(auto-discovered)";
        g_probeHeight[g_probeN] = g_probeShape[g_probeN] == 2 ? g_nbrP4[bestI]
                                : g_probeShape[g_probeN] == 1 ? g_nbrM4[bestI] : 0.0f;
        g_picked[bestI] = 1;                               /* do not pick twice */
        ++g_probeN;
    }
    for (i = 0; i < g_candN; ++i)
        if (!(g_candMax[i] - g_candMin[i] > 0.0005f)) ++rejRange;

    Log("PROBE    list built: %d candidates", g_probeN);
    Log("PROBE    rejected %d that never moved, %d no longer addressable,",
        rejRange, rejDead);
    Log("PROBE    and %d that changed while the player was idle (%d idle runs)",
        g_vetoTotal, g_idleRuns);
    if (g_idleRuns == 0)
        Log("PROBE    WARNING: no idle run was ever observed, so the strongest"
            " filter never ran. Stand still - no keys, no mouse - for a few seconds.");
    for (i = 0; i < g_probeN && i < 24; ++i) {
        char where[40];
        where[0] = 0;
        if (g_probeList[i] >= g_exeLo && g_probeList[i] < g_exeHi)
            _snprintf(where, sizeof(where), " IDA 0x%08IX",
                      g_probeList[i] - g_exeLo + 0x400000);
        Log("PROBE      [%2d] 0x%08IX%s score=%d moves=%d range % .3f..% .3f  %s",
            i + 1, g_probeList[i], where, g_probeScore[i], g_probeMoves[i],
            g_probeMinV[i], g_probeMaxV[i],
            g_probeBall[i]  ? "<<<<< PASSED THE BALLISTIC TEST" :
            g_probeShape[i] == 2 ? "<<< X of XYZ, constant Y next to it" :
            g_probeShape[i] == 1 ? "<<< Z of XYZ, constant Y before it"  : "");
    }
    /* A ballistic pass identifies Y, so the position triple is -4, this, +4.
       Print it outright: if those three read as a sane world position, and the
       middle one as a sane eye height, the hunt is over. */
    for (i = 0; i < g_probeN && i < 24; ++i) {
        float x, y, z;
        if (!g_probeBall[i]) continue;
        if (!SafeRead(g_probeList[i] - 4, &x)) continue;
        if (!SafeRead(g_probeList[i],     &y)) continue;
        if (!SafeRead(g_probeList[i] + 4, &z)) continue;
        Log("PROBE      [%2d] if this is Y, the position triple at 0x%08IX reads"
            " X=% .3f  Y=% .3f  Z=% .3f", i + 1, g_probeList[i] - 4, x, y, z);
    }
    for (i = 0; i < g_probeN && i < 24; ++i)
        if (g_probeShape[i] && !g_probeBall[i])
            Log("PROBE      [%2d] its constant neighbour holds % .4f - that is the"
                " player height if this is the position", i + 1, g_probeHeight[i]);
}


// ---------------------------------------------------------------------------
// WATCH LIST - the addresses run 7 actually found
//
// Run 7 was the first scan to see inside xops096.exe, and it turned up two
// clusters of statics that track the player. The owner confirmed on screen
// that sine-driving them moved X, Z, pitch and yaw, so these are no longer
// candidates to be hunted - they are addresses to be CONFIRMED and labelled.
//
// ASLR is off in this binary [device], so a VA is a fixed number and these
// literals are stable across runs. Everything here is still [unverified] as
// to WHICH field it is; that is what this table exists to settle.
//
// Cluster A at 0x0048C3B0 and cluster B at 0x0048E0A8 held the same values to
// within 0.007 all session. Most likely the player object and a camera that
// copies it a frame later - which one we drive matters, so both are listed.
// ---------------------------------------------------------------------------

typedef struct {
    UINT_PTR    va;         /* IDA address; ASLR is off so this is fixed */
    float       swing;      /* how far to sine-drive it when probing      */
    BOOL        angle;      /* show it in degrees as well as radians      */
    const char* what;
} Watch;

/* ALL FIVE of cluster A's fields are CONFIRMED [device, runs 8-10]: the owner
   sine-drove each one and reported what the view did. Writes take effect from
   BeginScene, so the game reads these angles during its own transform step -
   the worry that it might derive a forward vector earlier was unfounded.
   Cluster B is therefore never needed; A is what the mod writes. */
static const Watch g_watch[] = {
    { 0x0048C3B0, 0.50f, FALSE, "A+00  Y vertical        *** CONFIRMED run 8 ***" },
    { 0x0048C3B4, 3.00f, FALSE, "A+04  horizontal axis 1 *** CONFIRMED run 9 ***" },
    { 0x0048C3B8, 3.00f, FALSE, "A+08  horizontal axis 2 *** CONFIRMED run 9 ***" },
    { 0x0048C3BC, 0.60f, TRUE,  "A+0C  YAW               *** CONFIRMED run 10 ***"},
    { 0x0048C3C4, 0.30f, TRUE,  "A+14  PITCH             *** CONFIRMED run 10 ***"},
    { 0x0048E0A8, 3.00f, FALSE, "B+00  -26.89..-20.87               Z? (mirror)"},
    { 0x0048E0B0, 3.00f, FALSE, "B+08  5.82..14.44                  X? (mirror)"},
    { 0x0048E0B4, 0.60f, TRUE,  "B+0C  0.597..3.345 rad             YAW? (mirror)"},
    { 0x0048E0BC, 0.30f, TRUE,  "B+14  -0.524..0.048 rad            PITCH? (mirror)"},
};
#define WATCH_N ((int)(sizeof(g_watch) / sizeof(g_watch[0])))

static UINT_PTR WatchAddr(int i)
{
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);
    return g_watch[i].va - 0x400000 + exe;
}

/* Print the raw struct around an address as floats, with offsets. The watch
   list only names fields we already suspect; this shows everything, including
   the ones we have no theory for. A float that is ALWAYS exactly zero next to
   yaw and pitch is the roll candidate - or proof there is no roll field. */
static void StructDump(UINT_PTR base, int count, const char* name)
{
    int i;
    Log("STRUCT   %s at 0x%08IX, %d floats:", name, base, count);
    for (i = 0; i < count; ++i) {
        float v;
        const UINT_PTR a = base + (UINT_PTR)i * 4;
        char note[64];
        if (!SafeRead(a, &v)) { Log("STRUCT     +%02X  unreadable", i * 4); continue; }
        note[0] = 0;
        if (v == 0.0f)
            _snprintf(note, sizeof(note), "  <- exactly zero (nothing writes it)");
        else if (v > -6.3f && v < 6.3f)
            _snprintf(note, sizeof(note), "  (%.2f deg if an angle)", v * 57.2957795f);
        Log("STRUCT     +%02X  % 12.4f%s", i * 4, v, note);
    }
}

/* Print every watched float with its label. A value that is plausibly an angle
   in radians is also shown in degrees, because "0.597 rad" means nothing at a
   glance and "34.2 deg" means everything. */
static void WatchDump(const char* why)
{
    int i;
    Log("WATCH    %s  [write phase: %s]", why, PhaseName(g_probePhase));
    for (i = 0; i < WATCH_N; ++i) {
        float v;
        char deg[32];
        if (!SafeRead(WatchAddr(i), &v)) { Log("WATCH      0x%08IX  unreadable", g_watch[i].va); continue; }
        deg[0] = 0;
        if (g_watch[i].angle)
            _snprintf(deg, sizeof(deg), "  = %7.2f deg", v * 57.2957795f);
        Log("WATCH      0x%08IX % 11.4f%-20s  %s", g_watch[i].va, v, deg, g_watch[i].what);
    }
    /* Read the two clusters out as positions, so they can be checked against
       something measured independently - the view matrix eye, or simply where
       you know you are standing. */
    {
        float ax, ay, az, bx, bz;
        if (SafeRead(WatchAddr(1), &ax) && SafeRead(WatchAddr(0), &ay)
            && SafeRead(WatchAddr(2), &az))
            Log("WATCH      cluster A as a position:  X=% .3f  Y=% .3f  Z=% .3f", ax, ay, az);
        if (SafeRead(WatchAddr(6), &bx) && SafeRead(WatchAddr(5), &bz))
            Log("WATCH      cluster B as a position:  X=% .3f  Z=% .3f", bx, bz);
    }
}

/* Load the watch list into the probe, in a FIXED order that never reshuffles.
   Run 7's lists were rebuilt on every toggle of probe mode, so the index the
   owner was stepping through meant something different each time and a tag
   could not be pinned to anything. */
static void BuildWatchList(void)
{
    int i;
    g_probeN = 0;
    for (i = 0; i < WATCH_N && i < PROBE_MAX; ++i) {
        float v;
        const UINT_PTR a = WatchAddr(i);
        if (!AddrWritable(a) || !SafeRead(a, &v)) {
            Log("PROBE    watch entry 0x%08IX is not readable - skipped", g_watch[i].va);
            continue;
        }
        g_probeList[g_probeN]  = a;
        g_probeMinV[g_probeN]  = v - g_watch[i].swing;
        g_probeMaxV[g_probeN]  = v + g_watch[i].swing;
        g_probeScore[g_probeN] = 0;
        g_probeMoves[g_probeN] = 0;
        g_probeShape[g_probeN] = 0;
        g_probeBall[g_probeN]  = 0;
        g_probeHeight[g_probeN]= 0.0f;
        g_probeWhat[g_probeN]  = g_watch[i].what;
        ++g_probeN;
    }
    Log("PROBE    watch list loaded: %d confirmed addresses, fixed order.", g_probeN);
    Log("PROBE    8 steps through them. The order does not change, so [3] is");
    Log("PROBE    always the same address - tag it with 0 and tell me the number.");
    WatchDump("values as the list was loaded");
}


// ---------------------------------------------------------------------------
// WHEN we write matters as much as where
//
// Position responded to a write in BeginScene because position is USED
// directly - the game reads it when it transforms the world. Yaw and pitch
// may well not be. The usual shape of a 2003 FPS update is
//
//     read mouse -> update yaw/pitch -> derive a forward vector -> render
//
// and if the forward vector is derived during the update, then a yaw written
// in BeginScene is written after the only read that would have mattered, and
// changes nothing on screen. That looks exactly like "yaw is not at this
// address", which would be the wrong conclusion to draw.
//
// So the write phase is switchable, and the three choices bracket the update:
//
//   BEGINSCENE  after the update, before the draw  (where position worked)
//   INPUT       during the update, from inside the GetCursorPos hook - the
//               game calls it once a frame while reading the mouse, so this
//               lands BEFORE any vector is derived from that delta
//   ENDSCENE    after the draw, so it persists into the next frame's update
//
// If a yaw write does nothing at BEGINSCENE but turns the view at INPUT, the
// derived-vector theory is confirmed and F-7 knows it must write early.
// ---------------------------------------------------------------------------

static const char* PhaseName(int p)
{
    switch (p) {
        case PHASE_INPUT:    return "INPUT (mid-update, inside GetCursorPos)";
        case PHASE_ENDSCENE: return "ENDSCENE (after the draw, into next frame)";
        default:             return "BEGINSCENE (after update, before draw)";
    }
}

static void ProbeCyclePhase(void)
{
    g_probePhase = (g_probePhase + 1) % 3;
    Log("PROBE    write phase -> %s", PhaseName(g_probePhase));
    if (g_probePhase == PHASE_INPUT)
        Log("PROBE    if yaw/pitch start working now, they are read during the"
            " update and must be written early.");
}


// ---------------------------------------------------------------------------
// XR STEERING - drive the confirmed camera fields from the UDP pose
//
// All five fields are confirmed [device, runs 8-10], so this is no longer a
// hunt: it is the real camera path, built in the probe because the probe
// already has the hooks, the logging and the input ownership it needs.
//
//   0x0048C3B0  Y          0x0048C3BC  yaw
//   0x0048C3B4  horiz 1    0x0048C3C4  pitch
//   0x0048C3B8  horiz 2
//
// Two conventions are NOT yet established and so are runtime-switchable
// rather than guessed: the sign of XOPS yaw and pitch, and which of the two
// horizontal fields is "X". Guessing either would produce a mod that looks
// broken for a reason that takes an hour to find; a key that flips it takes
// one run to settle.
//
// Keys (probe mode):
//   3   steering on/off
//   [   flip yaw sign        ]   flip pitch sign
//   ;   recentre (also sets the yaw/position reference from the game)
//   '   cycle how many DOF are driven: yaw+pitch -> +height -> full 6DOF
// ---------------------------------------------------------------------------

/* Which cluster to write. Default BOTH: writing B is what makes movement
   follow the head, and writing A as well makes the current frame's render
   correct immediately rather than one frame late. */
#define CLU_A       0
#define CLU_B       1
#define CLU_BOTH    2

#define DOF_LOOK    0       /* yaw + pitch only */
#define DOF_HEIGHT  1       /* + Y */
#define DOF_FULL    2       /* + both horizontal axes */

static int   g_dofMode    = DOF_FULL;
static float g_yawSign    = 1.0f;
/* Pitch sign -1 [device, run 13]. XOPS pitch is positive-DOWN, so the pose's
   positive-up value has to be negated.

   Recorded carefully because I got it wrong twice by reasoning instead of
   asking: runs 11 and 12 each ended on +1 after the owner flipped the sign,
   and I read that as evidence for +1. The flips were him comparing, not
   settling. The owner's direct report - "it was still up/down reverse" - is
   the evidence; a toggle history is not. */
static float g_pitchSign  = -1.0f;
static int   g_cluster    = CLU_BOTH;
static float g_posScale   = 1.0f;   /* engine units per metre; ~1 [derived] */

/* The reference captured at recentre: where the game had us, and the head
   pose we call "straight ahead". */
static BOOL  g_haveRef    = FALSE;
static float g_refYaw     = 0.0f;
static float g_refYawB    = 0.0f;
static float g_refPos[3]  = {0,0,0};   /* horiz1, Y, horiz2 - for the log */

/* The head offset we applied last frame, so this frame can apply only the
   difference. Without this, writing position cancels the game's own
   movement. */
static BOOL  g_havePrevHead = FALSE;
static float g_prevHeadP[3] = {0,0,0};

/* Height-clamp detection. Run 13 moved the head 1.4 m vertically and the
   game's Y moved 0.028, because XOPS re-clamps the player to the ground every
   frame - so a delta written in BeginScene is undone before the next one. */
static BOOL  g_wroteY       = FALSE;
static float g_yBeforeWrite = 0.0f;
static float g_yWriteWanted = 0.0f;
static int   g_yClamped     = 0;    /* consecutive frames the write was undone */

static XrPose g_pose;
static BOOL   g_posed     = FALSE;
static long   g_steerWrites = 0;

/* Watch-table indices. A: 0=Y 1=h1 2=h2 3=yaw 4=pitch.
   B: 5=h2 6=h1 7=yaw 8=pitch - note h2 comes FIRST in B, and B has no Y
   entry, so B's height is not driven. */
static UINT_PTR CamAddr(int i) { return WatchAddr(i); }
#define A_Y 0
#define A_H1 1
#define A_H2 2
#define A_YAW 3
#define A_PITCH 4
#define B_H2 5
#define B_H1 6
#define B_YAW 7
#define B_PITCH 8

static const char* ClusterName(int c)
{
    return c == CLU_A ? "A only (render copy - view turns, movement does not)"
         : c == CLU_B ? "B only (player orientation - movement follows, view is a frame late)"
                      : "BOTH (view and movement agree)";
}

/* Persist the conventions we have settled by hand, next to the DLL. Four key
   presses per session to re-establish what is already known is four chances
   to get it wrong and not notice - and the signs are exactly the sort of fact
   that should be recorded once rather than rediscovered. */
static void SteerIniPath(char* out, size_t n)
{
    char dll[MAX_PATH];
    char* slash;
    if (!GetModuleFileNameA(g_hSelf, dll, sizeof(dll))) { out[0] = 0; return; }
    slash = strrchr(dll, '\\');
    if (slash) *(slash + 1) = 0;
    _snprintf(out, n - 1, "%sxops_wxr_steer.ini", dll);
    out[n - 1] = 0;
}

static void SteerSave(void)
{
    char path[MAX_PATH];
    FILE* f;
    SteerIniPath(path, sizeof(path));
    if (!path[0]) return;
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# XOPS-WXR steering conventions, settled on hardware.\n");
    fprintf(f, "# Delete this file to go back to the built-in defaults.\n");
    fprintf(f, "yawSign %.0f\n",   g_yawSign);
    fprintf(f, "pitchSign %.0f\n", g_pitchSign);
    fprintf(f, "cluster %d\n",     g_cluster);
    /* The DOF mode is deliberately NOT saved: it is a choice about how
       much to drive in this session, not a fact about XOPS, and
       persisting it would quietly override a better default. */
    fclose(f);
}

static void SteerLoad(void)
{
    char path[MAX_PATH], key[32];
    float v;
    FILE* f;
    SteerIniPath(path, sizeof(path));
    if (!path[0]) return;
    f = fopen(path, "r");
    if (!f) { Log("STEER    no saved settings - using defaults"); return; }
    while (fscanf(f, "%31s", key) == 1) {
        if (key[0] == '#') { int c; while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (fscanf(f, "%f", &v) != 1) break;
        if      (!strcmp(key, "yawSign"))   g_yawSign   = v < 0 ? -1.0f : 1.0f;
        else if (!strcmp(key, "pitchSign")) g_pitchSign = v < 0 ? -1.0f : 1.0f;
        else if (!strcmp(key, "cluster"))   g_cluster   = ((int)v) % 3;
        /* "dof" appears in files written by an earlier build. Read and
           discard it rather than letting a stale mode win. */
        else if (!strcmp(key, "dof"))       { /* ignored */ }
    }
    fclose(f);
    Log("STEER    loaded %s", path);
    Log("STEER    yaw %+.0f  pitch %+.0f  cluster %s  dof %s",
        g_yawSign, g_pitchSign, ClusterName(g_cluster), DofName(g_dofMode));
}

static void SteerRecentre(void)
{
    float v;
    XrRequestRecenter();
    if (SafeRead(CamAddr(A_YAW), &v)) g_refYaw = v;
    if (SafeRead(CamAddr(B_YAW), &v)) g_refYawB = v;
    if (SafeRead(CamAddr(A_H1), &v)) g_refPos[0] = v;
    if (SafeRead(CamAddr(A_Y),  &v)) g_refPos[1] = v;
    if (SafeRead(CamAddr(A_H2), &v)) g_refPos[2] = v;
    g_haveRef = TRUE;
    /* Start the delta chain from where the head is now, so recentring never
       produces one large jump. */
    g_prevHeadP[0] = g_pose.valid ? g_pose.headP[0] : 0.0f;
    g_prevHeadP[1] = g_pose.valid ? g_pose.headP[1] : 0.0f;
    g_prevHeadP[2] = g_pose.valid ? g_pose.headP[2] : 0.0f;
    g_havePrevHead = g_pose.valid ? TRUE : FALSE;
    Log("STEER    recentred. yaw A %.4f rad (%.1f deg)  B %.4f, position"
        " h1=% .3f Y=% .3f h2=% .3f",
        g_refYaw, g_refYaw * 57.2957795f, g_refYawB,
        g_refPos[0], g_refPos[1], g_refPos[2]);
    Log("STEER    writing %s", ClusterName(g_cluster));
}

/* Called once per frame from BeginScene - the phase where a write to these
   fields is known to take effect [device, runs 8-10]. */
static void SteerTick(void)
{
    float yaw, pitch;

    if (!g_steer || !g_posed || !g_pose.valid) return;
    if (!g_haveRef) SteerRecentre();

    /* Yaw is relative: the pose's yaw is already measured from the recentre
       point, so add it to where the game had us when we recentred. Pitch is
       absolute against the horizon, so it is written outright. */
    yaw   = g_refYaw + g_yawSign * g_pose.headYaw;
    pitch = g_pitchSign * g_pose.headPitch;

    if (g_cluster == CLU_A || g_cluster == CLU_BOTH) {
        SafeWrite(CamAddr(A_YAW),   yaw);
        SafeWrite(CamAddr(A_PITCH), pitch);
    }
    if (g_cluster == CLU_B || g_cluster == CLU_BOTH) {
        /* B is the player's own orientation, so this is the write that makes
           WALKING follow the head - which is the whole point of the owner's
           input design: forward is where you are looking. */
        SafeWrite(CamAddr(B_YAW),   g_refYawB + g_yawSign * g_pose.headYaw);
        SafeWrite(CamAddr(B_PITCH), pitch);
    }

    /* Did last frame's height write survive? If the game clamps the player to
       the ground it will not, and no amount of writing will raise the view. */
    if (g_dofMode >= DOF_HEIGHT && g_wroteY) {
        float now;
        if (SafeRead(CamAddr(A_Y), &now)) {
            const float kept = now - g_yBeforeWrite;
            if (g_yWriteWanted != 0.0f) {
                if ((float)fabs(kept) < 0.25f * (float)fabs(g_yWriteWanted))
                    ++g_yClamped;
                else if (g_yClamped > 0) --g_yClamped;
            }
        }
    }

    if (g_dofMode >= DOF_HEIGHT && g_havePrevHead) {
        /* Add only what the head moved since last frame, to whatever height
           the game currently has - so gravity, stairs and crouching all keep
           working and the head merely contributes on top. */
        const float dY = g_posScale * (g_pose.headP[1] - g_prevHeadP[1]);
        float cur;
        g_wroteY = FALSE;
        if (dY != 0.0f && SafeRead(CamAddr(A_Y), &cur)) {
            SafeWrite(CamAddr(A_Y), cur + dY);
            g_yBeforeWrite = cur;
            g_yWriteWanted = dY;
            g_wroteY = TRUE;
        }
    }

    if (g_dofMode >= DOF_FULL && g_havePrevHead) {
        /* The head's sideways and forward motion is relative to the head, so
           it has to be rotated into the world by the yaw we are facing before
           it can be added to a world position: leaning left must move you
           left relative to your facing, not along a fixed world axis. */
        const float dx = g_posScale * (g_pose.headP[0] - g_prevHeadP[0]);
        const float dz = g_posScale * (g_pose.headP[2] - g_prevHeadP[2]);
        if (dx != 0.0f || dz != 0.0f) {
            const float c = (float)cos(yaw), sn = (float)sin(yaw);
            const float d1 =  dz * sn + dx * c;
            const float d2 =  dz * c  - dx * sn;
            float cur;
            if (SafeRead(CamAddr(A_H1), &cur)) {
                SafeWrite(CamAddr(A_H1), cur + d1);
                if (g_cluster == CLU_B || g_cluster == CLU_BOTH)
                    SafeWrite(CamAddr(B_H1), cur + d1);
            }
            if (SafeRead(CamAddr(A_H2), &cur)) {
                SafeWrite(CamAddr(A_H2), cur + d2);
                if (g_cluster == CLU_B || g_cluster == CLU_BOTH)
                    SafeWrite(CamAddr(B_H2), cur + d2);
            }
        }
    }

    g_prevHeadP[0] = g_pose.headP[0];
    g_prevHeadP[1] = g_pose.headP[1];
    g_prevHeadP[2] = g_pose.headP[2];
    g_havePrevHead = TRUE;
    ++g_steerWrites;
}

static const char* DofName(int m)
{
    return m == DOF_FULL   ? "full 6DOF (yaw, pitch, height, both horizontals)"
         : m == DOF_HEIGHT ? "yaw, pitch and height"
                           : "yaw and pitch only";
}

// ---------------------------------------------------------------------------
// STICK INPUT - the left thumbstick becomes W/A/S/D
//
// This needs no new unknowns. We own GetKeyboardState [device], so we SUPPLY
// the key state rather than synthesising events: set the high bit on the byte
// the game is about to read and it is indistinguishable from a real press.
// That is the route CLAUDE.md settled - "we supply input directly and never
// synthesise events".
//
// Head-relative movement falls out for free and does not need computing. The
// game moves the player along its own yaw field, and while steering we write
// that field from the head - so "forward" is already wherever the player is
// looking, which is the owner's input design. Nothing here has to know the
// head's direction.
//
// The stick axes are documented: left stick is (x right, y forward), so
// y > 0 is forward. No guessing needed, hence no sign toggle.
// ---------------------------------------------------------------------------

#define STICK_DEAD   0.35f      /* below this the stick is at rest */

static BOOL g_stickMove  = TRUE;    /* left stick drives W/A/S/D */
static long g_stickFrames = 0;      /* frames in which we supplied a key */
static BOOL g_prevRecentreBtn = FALSE;

/* Called from the GetKeyboardState hook, after the real call and after
   ProbeKeys has taken its own keys out. */

// ---------------------------------------------------------------------------
// VR-EXCLUSIVE INPUT - the desktop keyboard and mouse go dead in gameplay
//
// The owner's design (3 Nov 2026): in VR, all input comes from the headset
// and controllers over UDP. Desktop keys and mouse motion must not reach the
// game at all during play, because a stray WASD or mouse nudge fights the
// head and the hand. ESC is the one exception - it is how you get back to the
// menu and out of the game.
//
// Menus are the opposite case: there the player needs ordinary input, and
// MODE_VR 0 hands mouse control back to the runtime anyway. So suppression
// has to be gated on being in gameplay, and we can tell:
//
//   in gameplay  XOPS calls SetTransform 0 times per frame
//   title/menu   it calls it 25-35 times
//
// [device, every run since probe run 4]. No new instrumentation needed - this
// is the software-T&L fact reused as a state detector.
// ---------------------------------------------------------------------------

static long g_lastXforms  = 0;       /* SetTransform calls in the previous frame */
static long g_lastIndexed = 0;       /* DrawIndexedPrimitive calls, previous frame */

#define GAMEPLAY_MIN_INDEXED 60   /* gameplay 84-88, menus 26-48 [device] */

/* TRUE when the last completed frame looked like gameplay rather than a menu.
   Uses the previous frame because the current one is still being built when
   input is read.

   [unverified - heuristic] Two signals, because one is not enough:

     SetTransform == 0     rules out the title screen, which transforms 25-35
                           times a frame
     DrawIndexedPrimitive  rules out the in-mission menu, which is 2D only and
       >= 60               so ALSO transforms zero times - run 13 caught the
                           single-signal version calling that screen gameplay

   This wants replacing with the game's own menu/playing flag. Finding it is
   one walk-and-press-ESC scan away with the machinery already here, and until
   then VR-exclusive input must not be trusted to release the keyboard in
   menus. */
static BOOL InGameplay(void)
{
    return g_lastXforms == 0 && g_lastIndexed >= GAMEPLAY_MIN_INDEXED;
}

/* Called from the GetKeyboardState hook AFTER our own control keys have been
   read out, and BEFORE the stick supplies movement - so the owner keeps his
   probe keys, the game loses everything, and VR input is then added back. */
static void VrExclusiveKeys(PBYTE p)
{
    int vk;
    if (!g_vrExclusive || !InGameplay()) return;
    for (vk = 0; vk < 256; ++vk) {
        if (vk == VK_ESCAPE) continue;       /* the one way out */
        p[vk] = 0;
    }
}

static void StickKeys(PBYTE p)
{
    int pressed = 0;

    if (!g_stickMove || !g_posed || !g_pose.valid) return;

    /* Only while steering. Supplying movement keys when the head is not
       driving the view would walk the player sideways relative to where he is
       looking, which is worse than not working at all. */
    if (!g_steer) return;

    if (g_pose.moveY >  STICK_DEAD) { p['W'] |= 0x80; ++pressed; }
    if (g_pose.moveY < -STICK_DEAD) { p['S'] |= 0x80; ++pressed; }
    if (g_pose.moveX >  STICK_DEAD) { p['D'] |= 0x80; ++pressed; }
    if (g_pose.moveX < -STICK_DEAD) { p['A'] |= 0x80; ++pressed; }

    if (pressed) ++g_stickFrames;

    /* Right stick click recentres, the convention the Shogo client uses.
       Edge-triggered, or it would recentre every frame it is held. */
    {
        const BOOL now = XrButton(&g_pose, XRB_R_STICK) ? TRUE : FALSE;
        if (now && !g_prevRecentreBtn) {
            Log("STEER    recentre requested by the right stick click");
            SteerRecentre();
        }
        g_prevRecentreBtn = now;
    }
}

static void SteerLog(void)
{
    Log("XR       packets=%ld  steering=%s  dof=%s", XrPackets(),
        g_steer ? "ON" : "off", DofName(g_dofMode));
    Log("XR       cluster=%s", ClusterName(g_cluster));
    if (!XrRunning()) {
        Log("XR       NOT RUNNING: %s", XrLastError()[0] ? XrLastError() : "(no error)");
        return;
    }
    Log("XR       handshake dir %s   hmd %s %s", XrDir(), XrMake(), XrModel());
    if (XrPackets() == 0) {
        Log("XR       no packets yet. Start the XrAPI UDP Tester and point it");
        Log("XR       at 127.0.0.1:7872. Nothing else here can work until then.");
        return;
    }
    Log("XR       raw: %s", XrRawPacket());
    if (g_pose.valid) {
        Log("XR       head yaw % .4f rad (% .1f deg)  pitch % .4f (% .1f deg)"
            "  sync %.1f",
            g_pose.headYaw, g_pose.headYaw * 57.2957795f,
            g_pose.headPitch, g_pose.headPitch * 57.2957795f, g_pose.sync);
        Log("XR       head pos  % .3f % .3f % .3f m   ipd %.3f  fov %.1f x %.1f",
            g_pose.headP[0], g_pose.headP[1], g_pose.headP[2],
            g_pose.ipd, g_pose.fovx, g_pose.fovy);
        Log("XR       signs: yaw %+.0f  pitch %+.0f    writes so far %ld",
            g_yawSign, g_pitchSign, g_steerWrites);
        Log("XR       sticks: move % .2f,% .2f  turn % .2f,% .2f   bools %s",
            g_pose.moveX, g_pose.moveY, g_pose.turnX, g_pose.turnY,
            g_pose.bools[0] ? g_pose.bools : "(none)");
        Log("XR       stick->WASD %s, supplied keys on %ld frames",
            g_stickMove ? "on" : "OFF", g_stickFrames);
        Log("XR       VR-exclusive input %s; last frame %s"
            " (%ld SetTransforms, %ld indexed draws)",
            g_vrExclusive ? "ON" : "off",
            InGameplay() ? "GAMEPLAY" : "MENU/title",
            g_lastXforms, g_lastIndexed);
        /* Run 11's tester sent a fixed HMD position in floats 22-24, so the
           recentred offset was 0.000 every packet. Positional DOF cannot be
           judged against that, and a silent zero looks like broken code. */
        if (g_dofMode >= DOF_HEIGHT
            && g_pose.headP[0] == 0.0f && g_pose.headP[1] == 0.0f
            && g_pose.headP[2] == 0.0f)
            Log("XR       NOTE: head position is exactly 0,0,0 - the sender is"
                " not varying it, so positional DOF cannot be seen from here.");
        /* Run 12's symptom: position arriving every frame, DOF left at
           yaw+pitch, and so nothing moving. Silence there reads as a bug in
           the camera work, which it is not. */
        if (g_yClamped > 20)
            Log("XR       NOTE: height writes are being undone - %d frames in a"
                " row. XOPS re-clamps the player to the ground, so crouching"
                " cannot be driven by writing Y. Horizontal 6DOF is"
                " unaffected.", g_yClamped);
        if (g_dofMode == DOF_LOOK
            && (g_pose.headP[0] != 0.0f || g_pose.headP[1] != 0.0f
                || g_pose.headP[2] != 0.0f))
            Log("XR       NOTE: head POSITION is arriving but the DOF mode is"
                " yaw+pitch only, so it is being ignored. Press ' to include"
                " height and then full 6DOF.");
    }
}

static void ProbeRestore(void)
{
    if (!g_probeAddr) return;
    SafeWrite(g_probeAddr, g_probeOrig);
    g_probeAddr = 0;
}

static void ProbeSelect(int idx)
{
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);
    char ida[48];
    int tried = 0;

    ProbeRestore();
    if (g_probeN == 0) { Log("PROBE    no candidates - run the walk/stop scan first"); return; }

    /* Skip anything that has died since the list was built, rather than
       faulting on it. */
    while (tried < g_probeN) {
        while (idx < 0) idx += g_probeN;
        idx %= g_probeN;
        if (AddrWritable(g_probeList[idx]) &&
            SafeRead(g_probeList[idx], &g_probeOrig)) break;
        Log("PROBE    [%d/%d] 0x%08IX is gone - skipping", idx + 1, g_probeN, g_probeList[idx]);
        ++idx; ++tried;
    }
    if (tried >= g_probeN) { Log("PROBE    no addressable candidates left"); g_probeAddr = 0; return; }

    g_probeIdx  = idx;
    g_probeAddr = g_probeList[idx];

    /* Swing across the range this variable was actually seen to occupy. A
       value that lives between -47 and -40 gets swung across that, not by some
       arbitrary +/-1 that might mean nothing to it - or crash the game. */
    g_probeCentre = 0.5f * (g_probeMinV[idx] + g_probeMaxV[idx]);
    g_probeSwing  = 0.5f * (g_probeMaxV[idx] - g_probeMinV[idx]);
    if (g_probeSwing < 0.01f) g_probeSwing = 0.01f;

    ida[0] = 0;
    if (g_probeAddr >= exe && g_probeAddr < exe + 0x100000)
        _snprintf(ida, sizeof(ida), " IDA 0x%08IX", g_probeAddr - exe + 0x400000);
    if (g_probeShape[idx])
        Log("PROBE    this one has the XYZ shape (%s) - a strong candidate",
            g_probeShape[idx] == 2 ? "X, constant Y at +4" : "Z, constant Y at -4");
    if (g_probeWhat[idx])
        Log("PROBE    >>> %s", g_probeWhat[idx]);
    Log("PROBE    [%d/%d] 0x%08IX%s score=%d moves=%d orig=% .4f range % .4f..% .4f swing=%.4f x%.2f",
        idx + 1, g_probeN, g_probeAddr, ida, g_probeScore[idx], g_probeMoves[idx],
        g_probeOrig, g_probeMinV[idx], g_probeMaxV[idx], g_probeSwing, g_probeAmp);
}

/* Called from BeginScene, once per frame. */
static void ProbeTick(void)
{
    double ph;
    float v;
    static int faults = 0;

    if (!g_probeMode || !g_probeAddr) return;

    ph = 6.283185307 * (double)(g_frame % (long)PROBE_PERIOD) / (double)PROBE_PERIOD;
    v  = g_probeCentre + g_probeAmp * g_probeSwing * (float)sin(ph);

    /* Never write something wilder than the variable has already been seen to
       hold, give or take one swing. Most "writing to memory crashed the game"
       is really "writing an absurd value crashed the game". */
    {
        const float lo = g_probeMinV[g_probeIdx] - g_probeSwing;
        const float hi = g_probeMaxV[g_probeIdx] + g_probeSwing;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    }
    if (!(v == v)) return;

    if (!SafeWrite(g_probeAddr, v)) {
        Log("PROBE    write faulted at 0x%08IX - dropping it", g_probeAddr);
        g_probeAddr = 0;
        if (++faults > 8) { g_probeMode = FALSE; Log("PROBE    too many faults - mode OFF"); }
    }
}

static void ProbeTag(void)
{
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);
    char ida[48];
    if (!g_probeAddr) return;
    ++g_tagCount;
    ida[0] = 0;
    if (g_probeAddr >= exe && g_probeAddr < exe + 0x100000)
        _snprintf(ida, sizeof(ida), " IDA 0x%08IX", g_probeAddr - exe + 0x400000);
    Log("");
    Log("TAG #%d   index [%d] of %d   0x%08IX%s",
        g_tagCount, g_probeIdx + 1, g_probeN, g_probeAddr, ida);
    Log("         %s", g_probeWhat[g_probeIdx] ? g_probeWhat[g_probeIdx] : "(unlabelled)");
    if (g_probeShape[g_probeIdx])
        Log("         shape=%d  constant neighbour=% .4f",
            g_probeShape[g_probeIdx], g_probeHeight[g_probeIdx]);
    Log("         write phase %s", PhaseName(g_probePhase));
    Log("         orig=% .4f  observed range % .4f .. % .4f  swing=%.4f x%.2f",
        g_probeOrig, g_probeMinV[g_probeIdx], g_probeMaxV[g_probeIdx],
        g_probeSwing, g_probeAmp);
    Log("");
}


static const char* ClassName(char c)
{
    switch (c) {
        case 'U': return "PITCH - view TILTED up/down (horizon rotated)";
        case 'K': return "MOVE  - player LIFTED up/down (vertical, no tilt)";
        case 'L': return "YAW   - view turned left/right";
        case 'O': return "MOVE  - player slid front/back";
        case 'P': return "MOVE  - player slid left/right";
        default:  return "unclassified";
    }
}

/* Record what the owner saw this variable do, while it is still doing it. */
static void ProbeClassify(char kind)
{
    const UINT_PTR exe = (UINT_PTR)GetModuleHandleA(NULL);
    char ida[48];
    float yaw = 0.0f;
    BOOL haveYaw;

    if (!g_probeAddr || g_probeIdx < 0) {
        Log("PROBE    nothing selected - press 9 then 8 to step to a variable");
        return;
    }
    g_probeClass[g_probeIdx] = kind;
    ++g_tagCount;

    ida[0] = 0;
    if (g_probeAddr >= exe && g_probeAddr < exe + 0x100000)
        _snprintf(ida, sizeof(ida), " IDA 0x%08IX", g_probeAddr - exe + 0x400000);

    Log("");
    Log("TAG #%d   %s", g_tagCount, ClassName(kind));
    Log("         index [%d] of %d   0x%08IX%s", g_probeIdx + 1, g_probeN, g_probeAddr, ida);
    Log("         %s", g_probeWhat[g_probeIdx] ? g_probeWhat[g_probeIdx] : "(unlabelled)");
    Log("         orig=% .4f  swing=%.4f x%.2f  written at %s",
        g_probeOrig, g_probeSwing, g_probeAmp, PhaseName(g_probePhase));

    /* For a translation the owner can only report what he saw relative to
       himself - "it slid me sideways" - so record the yaw at that moment.
       front/back at yaw 0 and front/back at yaw 90 are different world axes,
       and this is what lets us resolve which. */
    haveYaw = SafeRead(WatchAddr(3), &yaw);
    if ((kind == 'O' || kind == 'P') && haveYaw)
        Log("         player yaw at this moment % .4f rad = %.1f deg"
            "  (needed to resolve which world axis)", yaw, yaw * 57.2957795f);
    Log("");
}

static void ProbeKeys(PBYTE p)
{
    /* Edge-triggered: act on the transition from up to down. */
    static BYTE prev[256];
    int vk;
    /* Number row and the two keys beside it. Nothing here is a movement key:
       swallowing W/A/S/D would mean the player cannot walk while probing, and
       walking is often exactly how you tell whether a variable tracks you. */
    const int watch[33] = { '9', '8', '7', '0', '6', '5', '4', '2', '3', '1',
                            VK_OEM_PERIOD,
                            VK_OEM_4, VK_OEM_6, VK_OEM_1, VK_OEM_7, VK_OEM_5,
                            VK_OEM_3,
                            'U', 'K', 'L', 'O', 'P', VK_OEM_MINUS, VK_OEM_PLUS,
                            VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
                            VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8,
                            VK_NUMPAD9 };
    BYTE now[256];
    int i, j;

    for (i = 0; i < 33; ++i) {
        vk = watch[i];
        now[vk] = (BYTE)(p[vk] & 0x80);
        if (now[vk] && !prev[vk]) {
            if (vk == '6') {
                BallisticArm();        /* works whether or not probe mode is on */
            } else if (vk == '5') {
                WatchDump("on demand (key 5)");
                /* 0x0048C3B0 is A+00, so start 4 floats earlier to catch
                   anything in front of it, and run well past pitch. */
                StructDump(WatchAddr(0) - 16, 16, "cluster A");
                StructDump(WatchAddr(5) - 16, 16, "cluster B");
            } else if (vk == '4') {
                /* Keep what we have unless the rescan actually produces
                   something. A list of confirmed addresses is worth more
                   than an empty one. */
                const int had = g_probeN;
                ProbeRestore();
                BuildProbeList();
                if (g_probeN == 0 && had > 0) {
                    Log("PROBE    rescan found nothing - keeping the watch list.");
                    BuildWatchList();
                }
                ProbeSelect(0);
            } else if (vk == '2') {
                ProbeCyclePhase();
            } else if (vk == '3') {
                g_steer = !g_steer;
                if (g_steer) {
                    ProbeRestore();     /* a sine wave fighting the head is noise */
                    SteerRecentre();
                }
                Log("STEER    %s  (%s)", g_steer ? "ON" : "off", DofName(g_dofMode));
                SteerLog();
            } else if (vk == VK_OEM_4) {
                g_yawSign = -g_yawSign;
                Log("STEER    yaw sign %+.0f", g_yawSign);
                SteerSave();
            } else if (vk == VK_OEM_6) {
                g_pitchSign = -g_pitchSign;
                Log("STEER    pitch sign %+.0f", g_pitchSign);
                SteerSave();
            } else if (vk == VK_OEM_PERIOD) {
                g_vrExclusive = !g_vrExclusive;
                Log("INPUT    VR-exclusive input: %s",
                    g_vrExclusive
                      ? "ON - in gameplay the game sees NO desktop keys or"
                        " mouse except ESC. Menus are unaffected."
                      : "off - the desktop keyboard and mouse work normally");
                Log("INPUT    (last frame: %ld SetTransform, %ld indexed draws"
                    " -> %s)", g_lastXforms, g_lastIndexed,
                    InGameplay() ? "GAMEPLAY" : "a MENU or the title");
            } else if (vk == '1') {
                g_stickMove = !g_stickMove;
                Log("STICK    left stick -> W/A/S/D: %s",
                    g_stickMove ? "ON (only while steering)" : "off");
            } else if (vk == VK_OEM_3) {
                g_cluster = (g_cluster + 1) % 3;
                SteerRecentre();
                Log("STEER    cluster -> %s", ClusterName(g_cluster));
                SteerSave();
            } else if (vk == VK_OEM_5) {
                g_noPause = !g_noPause;
                Log("FOCUS    run while unfocused: %s  (game window %p)",
                    g_noPause ? "YES - GetForegroundWindow is answered with the"
                                " game's own window" : "no - the game pauses normally",
                    g_gameWnd);
            } else if (vk == VK_OEM_1) {
                SteerRecentre();
            } else if (vk == VK_OEM_7) {
                g_dofMode = (g_dofMode + 1) % 3;
                SteerRecentre();
                Log("STEER    dof -> %s", DofName(g_dofMode));
            } else if (vk == '9') {
                g_probeMode = !g_probeMode;
                if (g_probeMode) {
                    /* Build ONCE, and only if there is nothing loaded. Toggling
                       must not reshuffle the list under the owner's feet. */
                    if (g_probeN == 0) BuildWatchList();
                    g_huntDone = TRUE;            /* freeze the scanner */
                    Log("PROBE    MODE ON   numpad 1-9 picks that variable directly");
                    Log("PROBE    (NumLock on). 8=next  7=prev  0=tag  -/+=swing  9=off");
                    Log("PROBE    2=change WHEN we write (phase)");
                    Log("PROBE    5=dump the watch list   4=rescan for new candidates");
                    Log("PROBE    while a sine wave runs, say what you SEE it do:");
                    Log("PROBE      U=TILTED my view up/down (horizon rotated)");
                    Log("PROBE      K=LIFTED me up/down (vertical, horizon level)");
                    Log("PROBE      L=turned me left/right");
                    Log("PROBE      O=slid me front/back   P=slid me left/right");
                    Log("PROBE    6=arm the ballistic window (height cheat + gravity)");
                    Log("PROBE    (W/A/S/D and the mouse still work normally)");
                    /* Resume where we left off rather than jumping to [1]. */
                    ProbeSelect(g_probeIdx < 0 ? 1 : g_probeIdx);
                } else {
                    ProbeRestore();
                    Log("PROBE    MODE OFF  (%d tags so far)", g_tagCount);
                }
            } else if (vk == '3' || vk == VK_OEM_4 || vk == VK_OEM_6
                       || vk == VK_OEM_1 || vk == VK_OEM_7 || vk == VK_OEM_5
                       || vk == VK_OEM_3 || vk == '1' || vk == VK_OEM_PERIOD) {
                /* handled above, before the probe-mode gate */
            } else if (g_probeMode) {
                if      (vk == '8') ProbeSelect(g_probeIdx + 1);
                else if (vk == '7') ProbeSelect(g_probeIdx - 1);
                else if (vk == '0') ProbeTag();
                else if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
                    ProbeSelect(vk - VK_NUMPAD1);   /* numpad N = variable N */
                else if (vk == 'U' || vk == 'K' || vk == 'L'
                         || vk == 'O' || vk == 'P')
                    ProbeClassify((char)vk);
                else if (vk == VK_OEM_MINUS) { g_probeAmp *= 0.5f; Log("PROBE    swing x%.4f", g_probeAmp); }
                else if (vk == VK_OEM_PLUS)  { g_probeAmp *= 2.0f; Log("PROBE    swing x%.4f", g_probeAmp); }
            }
        }
        prev[vk] = now[vk];
    }

    /* Swallow the control keys so the game never sees them: pressing W must
       step the probe, not walk the player somewhere else while you are trying
       to judge what changed. */
    /* Swallow only our own control keys. Movement is left completely alone, so
       the player can walk, strafe and turn normally while a variable is being
       wiggled - which is the point, since "does it track me when I move?" is
       the most useful question to ask of a candidate. */
    if (g_probeMode) {
        p['8'] = 0; p['7'] = 0; p['0'] = 0; p['4'] = 0;
        p['U'] = 0; p['K'] = 0; p['L'] = 0; p['O'] = 0; p['P'] = 0;
        p['2'] = 0;
        for (j = VK_NUMPAD1; j <= VK_NUMPAD9; ++j) p[j] = 0;
        p[VK_OEM_MINUS] = 0; p[VK_OEM_PLUS] = 0;
    }
    p['9'] = 0;
    p['6'] = 0;
    p['5'] = 0;
    /* Steering is not a probe experiment, so its keys are always swallowed
       and always live, whether or not probe mode is on. */
    p['3'] = 0;
    p[VK_OEM_4] = 0; p[VK_OEM_6] = 0; p[VK_OEM_1] = 0; p[VK_OEM_7] = 0;
    p[VK_OEM_5] = 0; p[VK_OEM_3] = 0; p['1'] = 0; p[VK_OEM_PERIOD] = 0;
}

// ---------------------------------------------------------------------------
// Init - deliberately NOT in DllMain. LoadLibrary under the loader lock is a
// deadlock risk on Windows and a worse one under Wine, so everything happens on
// the first call into an export instead.
// ---------------------------------------------------------------------------

static void EnsureInit(void)
{
    if (g_initDone) return;
    g_initDone = TRUE;

    char dir[MAX_PATH]; GetModuleFileNameA(g_hSelf, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0; else dir[0] = 0;

    char logPath[MAX_PATH];
    _snprintf(logPath, MAX_PATH, "%sxops_wxr_probe.log", dir);
    logPath[MAX_PATH - 1] = 0;
    g_log = fopen(logPath, "w");

    g_exeBase = (UINT_PTR)GetModuleHandleA(NULL);

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);

    Log("XOPS-WXR d3d8 probe  %s", kBuildStamp);      // line 1 is the stamp
    Log("host           %s", exe);
    Log("exe base       0x%08IX   (IDA addresses below assume 0x400000)", g_exeBase);
    Log("screen         %dx%d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    Log("");

    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
    char real[MAX_PATH];
    _snprintf(real, MAX_PATH, "%s\\d3d8.dll", sys);
    real[MAX_PATH - 1] = 0;

    g_hRealD3D8 = LoadLibraryA(real);
    if (!g_hRealD3D8) {
        Log("FATAL  could not load %s (err %lu)", real, GetLastError());
        return;
    }
    g_realCreate8 = (PFN_Direct3DCreate8)GetProcAddress(g_hRealD3D8, "Direct3DCreate8");
    Log("real d3d8      %s  Direct3DCreate8=%p", real, g_realCreate8);
    Log("");

    ResolveForwards();
    DumpImports();
    if (XrStart()) {
        Log("XR       client started. in 127.0.0.1:%d  out 127.0.0.1:%d", 7872, 7278);
        Log("XR       handshake dir %s   hmd %s %s", XrDir(), XrMake(), XrModel());
        Log("XR       the game is set to keep running while unfocused, so the");
        Log("XR       tester can hold focus. Backslash toggles that.");
        Log("XR       press 3 to steer the camera from the pose; ; recentres,");
        Log("XR       [ and ] flip the yaw and pitch signs, ' cycles the DOF,");
        Log("XR       ` cycles which cluster is written (A / B / both),");
        Log("XR       1 toggles the left stick driving W/A/S/D,");
        Log("XR       . toggles VR-exclusive input (desktop keys dead in play).");
        Log("XR       Anything settled with those keys is saved next to the DLL.");
    } else {
        Log("XR       client FAILED to start: %s", XrLastError());
    }
    SteerLoad();
    HookInput();
}

// ---------------------------------------------------------------------------
// vtable patching - we own the object, so swapping a function pointer is all
// that is needed. No inline detour, nothing to behave differently under Box86.
// ---------------------------------------------------------------------------

static BOOL PatchSlot(void* obj, int index, void* replacement, void** original)
{
    if (!obj) return FALSE;
    void** vtbl = *(void***)obj;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    if (original) *original = vtbl[index];
    vtbl[index] = replacement;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return TRUE;
}

static void NoteCallSite(DWORD type, void* ret)
{
    const UINT_PTR ida = ToIda(ret);
    for (int i = 0; i < g_siteCount; ++i)
        if (g_sites[i].type == type && g_sites[i].ida == ida) return;
    if (g_siteCount < MAX_SITES) {
        g_sites[g_siteCount].type = type;
        g_sites[g_siteCount].ida  = ida;
        ++g_siteCount;
        Log("CALLSITE  SetTransform(%s)  returns to IDA 0x%08IX  (frame %ld)",
            XformName(type), ida, g_frame);
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Full matrices are logged for the first few frames, then one frame every
// VERBOSE_EVERY frames, so a long session stays readable.
#define VERBOSE_FRAMES  3
#define VERBOSE_EVERY   120

static BOOL Verbose(void)
{
    return (g_frame < VERBOSE_FRAMES) || (g_frame % VERBOSE_EVERY == 0);
}

static HRESULT __stdcall hkSetTransform(void* dev, DWORD type, const D3DMATRIX8* m)
{
    NoteCallSite(type, _ReturnAddress());
    ++g_xformThisFrame;
    if      (type == D3DTS_VIEW)       { ++g_xfView;  if (m) { g_lastView = *m; g_haveView = TRUE; g_lastViewAt = g_frame; } }
    else if (type == D3DTS_PROJECTION) ++g_xfProj;
    else if (type == 256)              ++g_xfWorld;
    else                               ++g_xfOther;

    if (Verbose() && (type == D3DTS_VIEW || type == D3DTS_PROJECTION)) {
        Log("[f%ld] SetTransform(%s) from IDA 0x%08IX",
            g_frame, XformName(type), ToIda(_ReturnAddress()));
        LogMatrix(XformName(type), m);
        if (type == D3DTS_VIEW)       LogViewDecomposed(m);
        else                          LogProjDecomposed(m);
    }
    return o_SetTransform(dev, type, m);
}

static HRESULT __stdcall hkSetVertexShader(void* dev, DWORD handle)
{
    // In D3D8 an FVF code is passed here directly; a real shader is a handle.
    // D3DFVF_XYZRHW marks pre-transformed vertices, i.e. 2D / HUD drawing.
    if (handle < 0x10000) g_curFvf = handle;   // an FVF code, not a shader handle
    if (handle && handle < 0x10000) {
        BOOL seen = FALSE;
        for (int i = 0; i < g_fvfCount; ++i) if (g_fvfSeen[i] == handle) { seen = TRUE; break; }
        if (!seen && g_fvfCount < 32) {
            g_fvfSeen[g_fvfCount++] = handle;
            Log("FVF       0x%04lX%s  (first seen frame %ld, from IDA 0x%08IX)",
                handle, (handle & D3DFVF_XYZRHW) ? "  XYZRHW -> 2D/HUD" : "",
                g_frame, ToIda(_ReturnAddress()));
        }
    }
    return o_SetVertexShader(dev, handle);
}

static HRESULT __stdcall hkDrawPrimitiveUP(void* dev, DWORD prim, UINT count,
                                           const void* data, UINT stride)
{
    ++g_drawsThisFrame; ++g_drawsUP;
    NoteDraw(g_curFvf, stride);
    if (g_frame < VERBOSE_FRAMES && g_drawsThisFrame <= 4)
        Log("[f%ld] DrawPrimitiveUP  type=%lu count=%u stride=%u  from IDA 0x%08IX",
            g_frame, prim, count, stride, ToIda(_ReturnAddress()));
    return o_DrawPrimitiveUP(dev, prim, count, data, stride);
}

// The world may not come through DrawPrimitiveUP at all. Two XYZ formats
// (0x0142, 0x0102) are set by the game, and XYZ geometry needs the device's
// matrices - so if it is drawn from a vertex buffer through DrawPrimitive or
// DrawIndexedPrimitive, an earlier version of this probe would never have seen
// it, and the conclusion "every draw is XYZRHW" would have been an artefact of
// hooking only one of the three entry points. Hook all three.
static HRESULT __stdcall hkDrawPrimitive(void* dev, DWORD prim, UINT start, UINT count)
{
    ++g_drawsThisFrame; ++g_drawsVB;
    NoteDraw(g_curFvf, 0);
    return o_DrawPrimitive(dev, prim, start, count);
}

static HRESULT __stdcall hkDrawIndexedPrim(void* dev, DWORD prim, UINT minIdx,
                                           UINT numV, UINT startIdx, UINT primCount)
{
    ++g_drawsThisFrame; ++g_drawsIDX;
    NoteDraw(g_curFvf, 0);
    return o_DrawIndexedPrim(dev, prim, minIdx, numV, startIdx, primCount);
}

static HRESULT __stdcall hkBeginScene(void* dev)
{
    ++g_beginThisFrame;
    /* Poll first, then steer, then let the probe wiggle on top - so a probe
       experiment always wins over the pose and stays usable while steering. */
    if (XrRunning()) { g_posed = XrPoll(&g_pose) ? TRUE : g_posed; SteerTick(); }
    if (g_probePhase == PHASE_BEGINSCENE) ProbeTick();
    return o_BeginScene(dev);
}

static HRESULT __stdcall hkEndScene(void* dev)
{
    ++g_endThisFrame;
    return o_EndScene(dev);
}

static HRESULT __stdcall hkPresent(void* dev, const RECT* a, const RECT* b, HWND c, void* d)
{
    // Per-frame summary. The Begin/End counts answer "is EndScene once per
    // presented frame?", which decides where the sync square goes.
    if (Verbose()) {
        Log("[f%ld] PRESENT  BeginScene=%ld EndScene=%ld DrawPrimitiveUP=%ld SetTransform=%ld"
            "  (view=%ld proj=%ld world=%ld other=%ld)",
            g_frame, g_beginThisFrame, g_endThisFrame, g_drawsThisFrame, g_xformThisFrame,
            g_xfView, g_xfProj, g_xfWorld, g_xfOther);
        Log("[f%ld] DRAWPATH UP=%ld  DrawPrimitive=%ld  DrawIndexedPrimitive=%ld",
            g_frame, g_drawsUP, g_drawsVB, g_drawsIDX);

        // What the frame's geometry actually was.
        for (int i = 0; i < g_drawKinds; ++i)
            Log("[f%ld] DRAWS    FVF 0x%04lX stride=%u  x%ld   %s",
                g_frame, g_draws[i].fvf, g_draws[i].stride, g_draws[i].count,
                FvfKind(g_draws[i].fvf));

        // Where the camera is, according to the most recent view matrix - even
        // if that matrix was set many frames ago. A frozen eye position while
        // the player is moving is the proof that the camera lives elsewhere.
        if (g_haveView) {
            Log("[f%ld] CAMERA   last VIEW set at frame %ld%s",
                g_frame, g_lastViewAt,
                (g_lastViewAt < g_frame - 1) ? "   <- STALE: not set this frame" : "");
            LogViewDecomposed(&g_lastView);
        } else {
            Log("[f%ld] CAMERA   no VIEW matrix has ever been set", g_frame);
        }
        ReportCandidates();

        // Per-frame input call counts. A function called every frame is the
        // path the game actually reads input through - that is the one to
        // supply from, later.
        char ib[256]; ib[0] = 0;
        for (int i = 0; i < IN_COUNT; ++i) {
            if (!g_inFrame[i]) continue;
            char one[64];
            _snprintf(one, sizeof(one), "%s=%ld ", kInputNames[i], g_inFrame[i]);
            strncat(ib, one, sizeof(ib) - strlen(ib) - 1);
        }
        if (ib[0]) Log("[f%ld] INPUT    %s", g_frame, ib);
    }

    LogWindowIfChanged(c ? c : g_wnd);
    /* Keep sending every frame even when MODE_VR is 0, or the stream cannot
       be turned back on - the mode is only ever re-read from a packet we
       send. Mono first, per the settled decisions. */
    if (XrRunning())
        XrSendFrame(0.0f, 0.0f, g_steer ? XR_VR_ON : XR_VR_SCREEN,
                    XR_3D_MONO, 104.5f, 104.5f);

    if ((g_frame % 120) == 0 && g_frame > 60) { WatchDump("periodic"); SteerLog(); }
    BallisticTick();
    IdleTick();
    if ((g_frame % 5) == 0) SampleRanges();
    HuntTick();

    g_lastXforms  = g_xformThisFrame;
    g_lastIndexed = g_drawsIDX;
    for (int i = 0; i < IN_COUNT; ++i) g_inFrame[i] = 0;
    g_beginThisFrame = g_endThisFrame = g_drawsThisFrame = g_xformThisFrame = 0;
    g_xfView = g_xfProj = g_xfWorld = g_xfOther = 0;
    g_drawsUP = g_drawsVB = g_drawsIDX = 0;
    g_drawKinds = 0;
    ++g_frame;
    return o_Present(dev, a, b, c, d);
}

static void HookDevice(void* dev);

static HRESULT __stdcall hkReset(void* dev, D3DPRESENT_PARAMETERS8* pp)
{
    Log("");
    Log("RESET  requested %ux%u  windowed=%ld  fmt=%lu  flags=0x%08lX  (frame %ld)",
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        pp ? pp->Windowed : 0, pp ? pp->BackBufferFormat : 0,
        pp ? pp->Flags : 0, g_frame);
    HRESULT hr = o_Reset(dev, pp);
    Log("RESET  hr=0x%08lX", hr);
    // A Reset can replace the vtable in some implementations; re-patch cheaply.
    HookDevice(dev);
    return hr;
}

static void HookDevice(void* dev)
{
    if (!dev) return;
    void* tmp = NULL;

    if (PatchSlot(dev, VT_DEV_SETTRANSFORM,    (void*)hkSetTransform,    &tmp) && tmp != (void*)hkSetTransform) o_SetTransform    = (PFN_SetTransform)tmp;
    if (PatchSlot(dev, VT_DEV_PRESENT,         (void*)hkPresent,         &tmp) && tmp != (void*)hkPresent)       o_Present         = (PFN_Present)tmp;
    if (PatchSlot(dev, VT_DEV_BEGINSCENE,      (void*)hkBeginScene,      &tmp) && tmp != (void*)hkBeginScene)    o_BeginScene      = (PFN_BeginScene)tmp;
    if (PatchSlot(dev, VT_DEV_ENDSCENE,        (void*)hkEndScene,        &tmp) && tmp != (void*)hkEndScene)      o_EndScene        = (PFN_EndScene)tmp;
    if (PatchSlot(dev, VT_DEV_DRAWPRIMITIVEUP, (void*)hkDrawPrimitiveUP, &tmp) && tmp != (void*)hkDrawPrimitiveUP) o_DrawPrimitiveUP = (PFN_DrawPrimitiveUP)tmp;
    if (PatchSlot(dev, VT_DEV_DRAWPRIMITIVE,   (void*)hkDrawPrimitive,   &tmp) && tmp != (void*)hkDrawPrimitive)   o_DrawPrimitive   = (PFN_DrawPrimitive)tmp;
    if (PatchSlot(dev, VT_DEV_DRAWINDEXEDPRIM, (void*)hkDrawIndexedPrim, &tmp) && tmp != (void*)hkDrawIndexedPrim) o_DrawIndexedPrim = (PFN_DrawIndexedPrim)tmp;
    if (PatchSlot(dev, VT_DEV_SETVERTEXSHADER, (void*)hkSetVertexShader, &tmp) && tmp != (void*)hkSetVertexShader) o_SetVertexShader = (PFN_SetVertexShader)tmp;
    if (PatchSlot(dev, VT_DEV_RESET,           (void*)hkReset,           &tmp) && tmp != (void*)hkReset)         o_Reset           = (PFN_Reset)tmp;
}

static HRESULT __stdcall hkCreateDevice(void* self, UINT adapter, DWORD devType,
                                        HWND hFocus, DWORD behaviour,
                                        D3DPRESENT_PARAMETERS8* pp, void** outDev)
{
    Log("");
    Log("CREATEDEVICE  called from IDA 0x%08IX", ToIda(_ReturnAddress()));
    if (pp) {
        Log("  backbuffer    %ux%u  count=%u  format=%lu",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferCount, pp->BackBufferFormat);
        Log("  windowed      %ld        swapeffect=%lu  multisample=%lu",
            pp->Windowed, pp->SwapEffect, pp->MultiSampleType);
        Log("  hDeviceWindow %p  hFocusWindow=%p", pp->hDeviceWindow, hFocus);
        /* Whichever of the two is set is the window the game considers its
           own, and so the handle GetForegroundWindow must hand back. */
        if (!g_gameWnd) g_gameWnd = pp->hDeviceWindow ? pp->hDeviceWindow : hFocus;
        Log("  autodepth     %ld  fmt=%lu", pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat);
        Log("  flags         0x%08lX%s", pp->Flags,
            (pp->Flags & 1) ? "  LOCKABLE_BACKBUFFER" : "  (back buffer NOT lockable)");
        Log("  refresh       %u Hz  presentinterval=0x%08X",
            pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval);
        Log("  behaviour     0x%08lX  devtype=%lu  adapter=%u", behaviour, devType, adapter);
    }

    HRESULT hr = o_CreateDevice(self, adapter, devType, hFocus, behaviour, pp, outDev);
    Log("CREATEDEVICE  hr=0x%08lX  dev=%p", hr, (outDev && *outDev) ? *outDev : NULL);

    if (SUCCEEDED(hr) && outDev && *outDev) {
        HookDevice(*outDev);
        Log("device hooks installed: SetTransform(37) Present(15) BeginScene(34) "
            "EndScene(35) DrawPrimitive(70) DrawIndexedPrimitive(71) "
            "DrawPrimitiveUP(72) SetVertexShader(76) Reset(14)");
        LogWindowIfChanged(pp ? pp->hDeviceWindow : hFocus);
        Log("");
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

extern "C" void* __stdcall Direct3DCreate8(UINT sdkVersion)
{
    EnsureInit();
    if (!g_realCreate8) return NULL;

    void* d3d = g_realCreate8(sdkVersion);
    Log("Direct3DCreate8(SDKVersion=%u = 0x%X)  ->  %p  [0xDC = 220 = D3D8.1]",
        sdkVersion, sdkVersion, d3d);

    if (d3d) {
        void* tmp = NULL;
        if (PatchSlot(d3d, VT_D3D8_CREATEDEVICE, (void*)hkCreateDevice, &tmp) &&
            tmp != (void*)hkCreateDevice)
            o_CreateDevice = (PFN_CreateDevice)tmp;
    }
    return d3d;
}

// The other d3d8 exports, forwarded without knowing their signatures. A naked
// jmp preserves whatever calling convention and stack cleanup the real function
// uses, which a typed wrapper could not do safely.
#define FORWARD(name)                                                       \
    static FARPROC g_fp_##name = NULL;                                      \
    extern "C" __declspec(naked) void __stdcall name(void)                  \
    {                                                                       \
        __asm { jmp dword ptr [g_fp_##name] }                               \
    }

FORWARD(ValidateVertexShader)
FORWARD(ValidatePixelShader)
FORWARD(DebugSetMute)
FORWARD(Direct3D8EnableMaximizedWindowedModeShim)

static void ResolveForwards(void)
{
    if (!g_hRealD3D8) return;
    g_fp_ValidateVertexShader = GetProcAddress(g_hRealD3D8, "ValidateVertexShader");
    g_fp_ValidatePixelShader  = GetProcAddress(g_hRealD3D8, "ValidatePixelShader");
    g_fp_DebugSetMute         = GetProcAddress(g_hRealD3D8, "DebugSetMute");
    g_fp_Direct3D8EnableMaximizedWindowedModeShim =
        GetProcAddress(g_hRealD3D8, "Direct3D8EnableMaximizedWindowedModeShim");
}

// ---------------------------------------------------------------------------
// DllMain - minimal on purpose. See BUG_FIXES.md D-11: LoadLibrary under the
// loader lock is a deadlock risk, and a worse one under Wine.
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hMod;
        DisableThreadLibraryCalls(hMod);
        InitializeCriticalSection(&g_lock);
    } else if (reason == DLL_PROCESS_DETACH) {
        ProbeRestore();
        XrStop();
        if (g_log) {
            Log("");
            Log("SHUTDOWN  after %ld frames, %d distinct SetTransform call sites",
                g_frame, g_siteCount);
            for (int i = 0; i < g_siteCount; ++i)
                Log("  %-10s  IDA 0x%08IX", XformName(g_sites[i].type), g_sites[i].ida);
            Log("");
            {
                int i, n = 0;
                for (i = 0; i < g_probeN; ++i) if (g_probeClass[i]) ++n;
                if (n) {
                    Log("WHAT THE OWNER SAW  (%d of %d variables classified):", n, g_probeN);
                    for (i = 0; i < g_probeN; ++i) {
                        if (!g_probeClass[i]) continue;
                        Log("  0x%08IX  %-38s  %s",
                            g_probeList[i], ClassName(g_probeClass[i]),
                            g_probeWhat[i] ? g_probeWhat[i] : "");
                    }
                    Log("This is the map. Each line is an address whose effect was");
                    Log("watched on screen and named - not inferred from a correlation.");
                    Log("");
                }
            }
            Log("INPUT totals over the session:");
            for (int i = 0; i < IN_COUNT; ++i)
                Log("  %-18s %ld calls%s", kInputNames[i], g_inCalls[i],
                    g_inCalls[i] ? "   <- a live input path" : "");
            Log("A per-frame call count means that function is the live input");
            Log("path and we can answer it ourselves - no synthetic events.");
            fclose(g_log);
            g_log = NULL;
        }
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}

