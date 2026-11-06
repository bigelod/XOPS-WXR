// XOPS-WXR - d3d8 diagnostic probe
//
// A drop-in d3d8.dll proxy that logs everything we need to know about how XOPS
// drives Direct3D, then forwards every call to the real d3d8.dll.
//
// As of the 2026-11-05 pass it is no longer read-only: it performs the first
// two mod steps on the critical path - a borderless full-display window
// (stage 3) and the red sync pixel (stage 5). Both are observable on the
// desktop before any headset work. F toggles the window fit; the sync square
// appears only once a packet has arrived, because it echoes HMD_SYNC.
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
#include <ctype.h>
#include <stdlib.h>
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
static void VrInputKeys(PBYTE p);
static void VrExclusiveKeys(PBYTE p);
static BOOL InGameplay(void);
static BOOL g_steer = TRUE;    /* always-on by default; 3 is a debug toggle */

static void SteerRecentre(void);
static void SteerSave(void);
static void SteerLoad(void);
static const char* ClusterName(int c);
static const char* DofName(int m);

static void SteerLog(void);
static void FitWindow(HWND h);
static void DrawHandAndLaserWorld(void* dev);
static void ManualHandKeys(PBYTE p);
static void ManualHandApply(void);
static void RecaptureWeapon(void);
static void SteerAimWrite(void);
static void CaptureHudBatch(void* dev, UINT start, UINT count);
static void CaptureHudBatchUP(void* dev, const void* data, UINT count);
static void ReleaseHudBatches(void);
static void HandViewPose(const float* hp, const float* hf, const float* hu,
                         BOOL isRight, BOOL flip,
                         float vp[3], float vf[3], float vu[3], float vr[3]);
static void HandTwoHandedAim(const float offP[3], const float vp[3],
                             float vf[3], float vu[3], float vr[3]);
static UINT PrimVertexCount(DWORD prim, UINT primCount);
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
typedef struct { LONG x1, y1, x2, y2; } D3DRECT8;
typedef struct { int Pitch; void* pBits; } D3DLOCKED_RECT8;
typedef struct { UINT X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT8;
typedef struct { float x, y, z; DWORD color; float u, v; } CompVert3D;

typedef HRESULT (__stdcall *PFN_GetBackBuffer)(void*, UINT, DWORD, void**);
typedef HRESULT (__stdcall *PFN_CreateTexture)(void*, UINT, UINT, UINT, DWORD,
                                               DWORD, DWORD, void**);
typedef HRESULT (__stdcall *PFN_CreateDepthStencilSurface)(void*, UINT, UINT,
                                                           DWORD, DWORD, void**);
typedef HRESULT (__stdcall *PFN_CreateImageSurface)(void*, UINT, UINT, DWORD, void**);
typedef HRESULT (__stdcall *PFN_CopyRects)(void*, void*, const RECT*, UINT,
                                           void*, const POINT*);
typedef HRESULT (__stdcall *PFN_SetRenderTarget)(void*, void*, void*);
typedef HRESULT (__stdcall *PFN_GetRenderTarget)(void*, void**);
typedef HRESULT (__stdcall *PFN_GetDepthStencilSurface)(void*, void**);
typedef HRESULT (__stdcall *PFN_GetStreamSource)(void*, UINT, void**, UINT*);
typedef HRESULT (__stdcall *PFN_DrawIndexedPrimitiveUP)(void*, DWORD, UINT, UINT,
    UINT, const void*, DWORD, const void*, UINT);
static PFN_DrawIndexedPrimitiveUP o_DrawIndexedPrimitiveUP = NULL;
typedef HRESULT (__stdcall *PFN_GetIndices)(void*, void**, UINT*);
static PFN_GetIndices          o_GetIndices          = NULL;
typedef HRESULT (__stdcall *PFN_SetViewport)(void*, const D3DVIEWPORT8*);
typedef HRESULT (__stdcall *PFN_GetViewport)(void*, D3DVIEWPORT8*);
typedef HRESULT (__stdcall *PFN_SetRenderState)(void*, DWORD, DWORD);
typedef HRESULT (__stdcall *PFN_GetRenderState)(void*, DWORD, DWORD*);
typedef HRESULT (__stdcall *PFN_SetTexture)(void*, DWORD, void*);
typedef HRESULT (__stdcall *PFN_GetTexture)(void*, DWORD, void**);
typedef HRESULT (__stdcall *PFN_SetTextureStageState)(void*, DWORD, DWORD, DWORD);
typedef HRESULT (__stdcall *PFN_GetTextureStageState)(void*, DWORD, DWORD, DWORD*);
typedef HRESULT (__stdcall *PFN_CreateStateBlock)(void*, DWORD, DWORD*);
typedef HRESULT (__stdcall *PFN_ApplyStateBlock)(void*, DWORD);
typedef HRESULT (__stdcall *PFN_DeleteStateBlock)(void*, DWORD);

typedef HRESULT (__stdcall *PFN_TexGetSurfaceLevel)(void*, UINT, void**);
typedef HRESULT (__stdcall *PFN_TexLockRect)(void*, UINT, D3DLOCKED_RECT8*,
                                             const RECT*, DWORD);
typedef HRESULT (__stdcall *PFN_TexUnlockRect)(void*, UINT);
typedef HRESULT (__stdcall *PFN_SurfLockRect)(void*, D3DLOCKED_RECT8*,
                                              const RECT*, DWORD);
typedef HRESULT (__stdcall *PFN_SurfUnlockRect)(void*);
typedef ULONG   (__stdcall *PFN_Release)(void*);

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
    VT_DEV_GETBACKBUFFER      = 16,   // 0x40
    VT_DEV_CREATETEXTURE      = 20,   // 0x50
    VT_DEV_CREATEDEPTHSTENCIL = 26,   // 0x68
    VT_DEV_CREATEIMAGESURFACE = 27,   // 0x6C
    VT_DEV_COPYRECTS          = 28,   // 0x70
    VT_DEV_SETRENDERTARGET    = 31,   // 0x7C
    VT_DEV_GETRENDERTARGET    = 32,   // 0x80
    VT_DEV_GETDEPTHSTENCIL    = 33,   // 0x84
    VT_DEV_BEGINSCENE         = 34,   // 0x88
    VT_DEV_ENDSCENE           = 35,   // 0x8C
    VT_DEV_CLEAR              = 36,   // 0x90  <- sync pixel, no lock needed
    VT_DEV_SETTRANSFORM       = 37,   // 0x94  <- confirmed in XOPS.exe
    VT_DEV_GETTRANSFORM       = 38,   // 0x98
    VT_DEV_SETVIEWPORT        = 40,   // 0xA0
    VT_DEV_GETVIEWPORT        = 41,   // 0xA4
    VT_DEV_SETRENDERSTATE     = 50,   // 0xC8  <- confirmed in XOPS.exe
    VT_DEV_GETRENDERSTATE     = 51,   // 0xCC
    VT_DEV_APPLYSTATEBLOCK    = 54,   // 0xD8
    VT_DEV_DELETESTATEBLOCK   = 56,   // 0xE0
    VT_DEV_CREATESTATEBLOCK   = 57,   // 0xE4
    VT_DEV_SETTEXTURE         = 61,   // 0xF4
    VT_DEV_GETTEXTURESTAGESTATE = 62, // 0xF8
    VT_DEV_SETTEXTURESTAGESTATE = 63, // 0xFC
    VT_DEV_DRAWPRIMITIVE      = 70,   // 0x118
    VT_DEV_DRAWINDEXEDPRIM    = 71,   // 0x11C
    VT_DEV_DRAWPRIMITIVEUP    = 72,   // 0x120 <- the loc_401396 draw call
    VT_DEV_DRAWINDEXEDPRIMUP  = 73,   // 0x124
    VT_DEV_SETVERTEXSHADER    = 76    // 0x130
};

/* Texture / surface vtable slots. For these we only read the slots, never
   patch them; the composite is drawn with the originals. */
enum {
    VT_TEX_GETSURFACELEVEL = 15,
    VT_TEX_LOCKRECT        = 16,
    VT_TEX_UNLOCKRECT      = 17,
    VT_SURF_LOCKRECT       = 9,
    VT_SURF_UNLOCKRECT     = 10,
    VT_IUNKNOWN_RELEASE    = 2
};

#define D3DFMT_A8R8G8B8        21
#define D3DFMT_X8R8G8B8        22
#define D3DPOOL_DEFAULT        0
#define D3DPOOL_MANAGED        1
#define D3DPOOL_SYSTEMMEM      2
#define D3DUSAGE_DYNAMIC       0x00000200
#define D3DUSAGE_RENDERTARGET  0x00000001
#define D3DFMT_D16             80
#define D3DCLEAR_ZBUFFER       0x00000002
#define D3DPT_TRIANGLESTRIP     5
#define D3DPT_TRIANGLELIST      4
#define D3DCULL_NONE            1
#define D3DCMP_LESSEQUAL        4
#define D3DBLEND_SRCALPHA       5
#define D3DBLEND_INVSRCALPHA    6
#define D3DBLEND_ONE            2
#define D3DBLENDOP_ADD          1
#define D3DSHADE_GOURAUD        2
#define D3DFILL_SOLID           3
#define D3DTOP_DISABLE          1
#define D3DTOP_MODULATE         4
#define D3DTA_DIFFUSE           0
#define D3DTA_TEXTURE           2
#define D3DTSS_COLOROP          1
#define D3DTSS_COLORARG1        2
#define D3DTSS_COLORARG2        3
#define D3DTSS_ALPHAOP          4
#define D3DTSS_ALPHAARG1        5
#define D3DTSS_ALPHAARG2        6
#define D3DTSS_TEXCOORDINDEX    11
#define D3DTSS_ADDRESSU         13
#define D3DTSS_ADDRESSV         14
#define D3DTSS_MAGFILTER        16
#define D3DTSS_MINFILTER        17
#define D3DTSS_MIPFILTER        18
#define D3DTSS_TEXTURETRANSFORMFLAGS 24
#define D3DTEXF_NONE            0
#define D3DTEXF_POINT           1
#define D3DTADDRESS_CLAMP       3
#define D3DTADDRESS_WRAP        1
#define D3DRS_FILLMODE          8
#define D3DRS_SHADEMODE         9
#define D3DRS_ZENABLE           7
#define D3DRS_ZWRITEENABLE      14
#define D3DRS_ZFUNC             23
#define D3DRS_ALPHATESTENABLE   15
#define D3DRS_SRCBLEND          19
#define D3DRS_DESTBLEND         20
#define D3DRS_CULLMODE          22
#define D3DRS_ALPHABLENDENABLE  27
#define D3DRS_FOGENABLE         28
#define D3DRS_STENCILENABLE     52
#define D3DRS_LIGHTING          137
#define D3DRS_COLORVERTEX       141
#define D3DRS_CLIPPING          146
#define D3DRS_CLIPPLANEENABLE   151
#define D3DRS_COLORWRITEENABLE  168
#define D3DRS_BLENDOP           171
#define D3DRS_SCISSORTESTENABLE 174
#define D3DRS_WRAP0             128

#define D3DTS_VIEW        2
#define D3DTS_PROJECTION  3
#define D3DTS_WORLD       256
#define D3DFVF_XYZRHW     0x004
#define D3DCLEAR_TARGET   0x00000001
#define D3DFMT_INDEX16    101
#define D3DCOLOR_XRGB(r,g,b) ((DWORD)(((DWORD)(r) << 16) | \
                                      ((DWORD)(g) <<  8) |  \
                                       (DWORD)(b)))

/* XOPS's real projection, recovered from the matrix on device [device, run 1]:
   fovY 65.0 deg, fovX 80.7 deg, aspect 4:3. The outbound XrApi FOV fields are
   DEGREES and are applied only if > 1. Sending the old 104.5 pair asked the
   runtime to warp for a lens the game does not render. */
#define XOPS_FOV_X        80.7f
#define XOPS_FOV_Y        65.0f

/* The outbound FOV must describe the image the runtime actually receives. The
   projection-aspect patch below widens the horizontal FOV to match the
   window/container, so these start at the native 4:3 values and are recomputed
   when the patch is applied. */
static float g_outFovX = XOPS_FOV_X;
static float g_outFovY = XOPS_FOV_Y;
static BOOL  g_projPatched = FALSE;

typedef void* (__stdcall *PFN_Direct3DCreate8)(UINT);

typedef HRESULT (__stdcall *PFN_CreateDevice)(void*, UINT, DWORD, HWND, DWORD,
                                              D3DPRESENT_PARAMETERS8*, void**);
typedef HRESULT (__stdcall *PFN_Reset)(void*, D3DPRESENT_PARAMETERS8*);
typedef HRESULT (__stdcall *PFN_Present)(void*, const RECT*, const RECT*, HWND, void*);
typedef HRESULT (__stdcall *PFN_BeginScene)(void*);
typedef HRESULT (__stdcall *PFN_EndScene)(void*);
typedef HRESULT (__stdcall *PFN_Clear)(void*, DWORD, const D3DRECT8*, DWORD,
                                       DWORD, float, DWORD);
typedef HRESULT (__stdcall *PFN_SetTransform)(void*, DWORD, const D3DMATRIX8*);
typedef HRESULT (__stdcall *PFN_GetTransform)(void*, DWORD, D3DMATRIX8*);
typedef HRESULT (__stdcall *PFN_DrawPrimitiveUP)(void*, DWORD, UINT, const void*, UINT);
typedef HRESULT (__stdcall *PFN_DrawPrimitive)(void*, DWORD, UINT, UINT);
typedef HRESULT (__stdcall *PFN_DrawIndexedPrim)(void*, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (__stdcall *PFN_SetVertexShader)(void*, DWORD);
typedef HRESULT (__stdcall *PFN_GetVertexShader)(void*, DWORD*);

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
static PFN_Clear            o_Clear            = NULL;
static PFN_SetTransform     o_SetTransform     = NULL;
static PFN_GetTransform     o_GetTransform     = NULL;
static PFN_DrawPrimitiveUP  o_DrawPrimitiveUP  = NULL;
static PFN_DrawPrimitive    o_DrawPrimitive    = NULL;
static PFN_DrawIndexedPrim  o_DrawIndexedPrim  = NULL;
static long g_drawsUP = 0, g_drawsVB = 0, g_drawsIDX = 0;
static PFN_SetVertexShader  o_SetVertexShader  = NULL;

/* Target-resolution composite (read-only slots, captured in HookDevice). */
static PFN_GetBackBuffer       o_GetBackBuffer       = NULL;
static PFN_GetStreamSource     o_GetStreamSource     = NULL;
static PFN_CreateTexture       o_CreateTexture       = NULL;
static PFN_CreateDepthStencilSurface o_CreateDepthStencilSurface = NULL;
static PFN_CreateImageSurface  o_CreateImageSurface  = NULL;
static PFN_CopyRects           o_CopyRects           = NULL;
static PFN_SetRenderTarget     o_SetRenderTarget     = NULL;
static PFN_GetRenderTarget     o_GetRenderTarget     = NULL;
static PFN_GetDepthStencilSurface o_GetDepthStencilSurface = NULL;
static PFN_SetViewport         o_SetViewport         = NULL;
static PFN_GetViewport         o_GetViewport         = NULL;
static PFN_SetRenderState      o_SetRenderState      = NULL;
static PFN_GetRenderState      o_GetRenderState      = NULL;
static PFN_SetTexture          o_SetTexture          = NULL;
static PFN_GetTexture          o_GetTexture          = NULL;
static PFN_SetTextureStageState o_SetTextureStageState = NULL;
static PFN_GetTextureStageState o_GetTextureStageState = NULL;

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
static D3DMATRIX8 g_lastProj;
static BOOL       g_haveProj   = FALSE;

// Which vertex format each draw used. XYZRHW means the game transformed the
// vertices itself on the CPU; XYZ means it relied on the device - and
// therefore on the matrices we can hook.
static DWORD g_curFvf = 0;
#define MAX_DRAWKINDS 16

/* HUD transform (stage 10 / comfort). The HUD is drawn as FVF 0x0144
   (XYZRHW|DIFFUSE|TEX1), stride 28, in the native 640x480 coordinate space.
   Scale it about the frame centre so the edge HUD lands in the middle square
   of the eye, independent of the world. H cycles the scale; 1.0 disables it. */
/* OFF until the HUD draws are unambiguously identified: stride 28 turned out
   to include world geometry, and scaling it tore the scene apart. H cycles it
   for experiments; V dumps the next 120 draws with their vertices. */
/* Visual scale of the HUD relative to its native 640x480 layout, as seen in
   the final client. The game-space X/Y scales are derived from this and the
   display stretch, so the HUD is not squeezed by a non-uniform Present. */
static float  g_hudVisualScale = 1.1f;
static float  g_hudOffsetX = 0.0f;
static float  g_hudOffsetY = 0.0f;
static BYTE*  g_hudBuf     = NULL;
static SIZE_T g_hudCap     = 0;
/* The replacement text HUD is drawn as a D3D texture (the old GDI-over-the-
   window prototype did not composite in a captured image). */
static BOOL   g_customHud = TRUE;

/* Target-resolution composite. When the device's back buffer is the desktop
   size, the game still draws its 640x480 frame in the top-left. We copy that
   frame into a texture, clear the target, draw it full-window, then overlay a
   resolution-independent HUD texture. */
static BOOL   g_compositeActive = FALSE;
static UINT   g_bbW = 640, g_bbH = 480;
static DWORD  g_bbFormat = D3DFMT_X8R8G8B8;
static void*  g_frameTex   = NULL;   /* 640x480 copy of the game frame */
static void*  g_frameSurf  = NULL;   /* its level-0 surface */
/* Off-screen 640x480 game render target. The game must keep its native
   viewport/projection or the FOV and the 3D HUD models drift. We bind this
   before the game's BeginScene, then composite it to the real back buffer. */
static void*  g_gameRT      = NULL;
static void*  g_gameRTSurf  = NULL;
static void*  g_gameDepth   = NULL;
static BOOL   g_gameRTActive = FALSE;
static BOOL   g_gameRTFirstScene = TRUE;
static void*  g_savedRT     = NULL;
static void*  g_savedDepth  = NULL;
static int    g_grabFrames = 0;      /* J: save the next composited frame */
static BOOL   g_grabPair   = FALSE;  /* also save the frame after it (AER L/R) */
static char   g_baseDir[MAX_PATH] = "";
static int    g_dumpDraws  = 0;
static BOOL   g_dumpArmed  = FALSE;
#define HUD_CX 320.0f
#define HUD_CY 240.0f
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

/* Learned from CreateDevice's hFocusWindow. Declared here because the custom
   HUD helper (defined earlier than the input block) needs it. */
static HWND  g_gameWnd      = NULL;

/* Borderless full-display window (stage 3). Declared here because the probe's
   key handler, far above FitWindow's definition, toggles it with F. */
static BOOL  g_fitWindow    = TRUE;
/* FALSE when the game created a FULLSCREEN device. In that mode the game owns
   the window and the display mode: resizing it or passing a Present dest rect
   fights its Reset and produces D3DERR_DEVICELOST (0x88760868). Confirmed on
   5 Nov when a title->level Reset failed and XOPS showed its "Reset" error. */
static BOOL  g_deviceWindowed = TRUE;

/* Target-resolution experiment, now the composite path. The game's config
   creates a 640x480 device; it still projects and draws its 640x480 frame in
   the top-left of a larger back buffer (confirmed 5 Nov). Asking for the
   desktop size gives us the surface on which the composite and the high-res
   HUD can exist. M toggles it; a change takes effect at the next Reset. */
static BOOL  g_maxRes   = TRUE;
static int   g_desktopW = 0;
static int   g_desktopH = 0;

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

/* Informative diagnostics are off by default; config.txt can turn them on.
   Errors and game-state changes always use Log(). */
static BOOL g_verboseDebug = FALSE;

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

/* Verbose-only logging: snapshots, probes, draw dumps, raw packets and other
   diagnostics. Suppressed unless config.txt has verbose_debug = true. */
static void LogD(const char* fmt, ...)
{
    va_list ap;
    if (!g_verboseDebug) return;
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
    if (!m) { LogD("    %s: (null)", tag); return; }
    for (int r = 0; r < 4; ++r)
        LogD("    %s[%d]  % .5f  % .5f  % .5f  % .5f",
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

    LogD("    eye      % .3f  % .3f  % .3f   (engine units)", eye[0], eye[1], eye[2]);
    LogD("    right    % .4f  % .4f  % .4f", m->m[0][0], m->m[1][0], m->m[2][0]);
    LogD("    up       % .4f  % .4f  % .4f", m->m[0][1], m->m[1][1], m->m[2][1]);
    LogD("    forward  % .4f  % .4f  % .4f", m->m[0][2], m->m[1][2], m->m[2][2]);

    // right x up should equal forward in a left-handed basis, and -forward in a
    // right-handed one. This single line settles the handedness question.
    const float cx = m->m[1][0] * m->m[2][1] - m->m[2][0] * m->m[1][1];
    const float cy = m->m[2][0] * m->m[0][1] - m->m[0][0] * m->m[2][1];
    const float cz = m->m[0][0] * m->m[1][1] - m->m[1][0] * m->m[0][1];
    const float dot = cx * m->m[0][2] + cy * m->m[1][2] + cz * m->m[2][2];
    LogD("    (right x up) . forward = % .4f  -> %s-handed",
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
        LogD("    fovY ~ %.2f deg   aspect ~ %.4f", fovY, (m00 != 0.0f) ? (m11 / m00) : 0.0f);
    }
    if (m22 != 1.0f && m22 != 0.0f) {
        // LH perspective: m22 = zf/(zf-zn), m32 = -zn*zf/(zf-zn).
        // So zn = -m32/m22 and zf = -m32/(m22-1). Both negations matter -
        // an earlier version dropped them and reported a negative near plane.
        const float zn = -m32 / m22;
        const float zf = -m32 / (m22 - 1.0f);
        LogD("    near ~ %.4f   far ~ %.2f", zn, zf);
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
/* The camera is solved [device, runs 8-13], so the differential hunt is no
   longer needed - and it is the one thing in this DLL that copies arbitrary
   game memory. On 5 Nov it crashed twice inside the CRT memcpy at RVA 0x985e:
   BuildSnapshot() raced a region the game freed between VirtualQuery and the
   copy. Default it OFF; the confirmed watch table is independent of it. The
   copy is also ReadProcessMemory now, so an explicit re-enable cannot fault. */
static BOOL g_huntDone  = TRUE;

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
        if (!g_snap) { LogD("CAMHUNT  could not reserve snapshot memory"); g_huntDone = TRUE; return; }
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
            /* ReadProcessMemory, not memcpy. VirtualQuery and the copy are
               not atomic: the game can free or reprotect a region in between,
               and a raw memcpy then faults inside the CRT (observed 5 Nov,
               RVA 0x985e). ReadProcessMemory reports the bad page instead. */
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress,
                                   g_snap + g_snapUsed, mbi.RegionSize, &got)
                || got != mbi.RegionSize) {
                LogD("CAMHUNT  skipped unreadable region %p (%u KB, got %u)",
                    mbi.BaseAddress, (unsigned)(mbi.RegionSize / 1024),
                    (unsigned)(got / 1024));
                /* Fall through to p = next; do not spin on this region. */
            } else {
                g_rgn[g_rgnCount].base    = (BYTE*)mbi.BaseAddress;
                g_rgn[g_rgnCount].size    = mbi.RegionSize;
                g_rgn[g_rgnCount].snapOff = g_snapUsed;
                g_snapUsed += mbi.RegionSize;
                ++g_rgnCount;
            }
        }
        if (next <= p) break;
        p = next;
    }
    LogD("CAMHUNT  snapshot: %d regions, %u KB  (of which %u KB is xops096.exe's"
        " own writable data - the probe itself is excluded)",
        g_rgnCount, (unsigned)(g_snapUsed / 1024), g_exeRgnKB);
    if (g_exeRgnKB == 0)
        LogD("CAMHUNT  WARNING: no EXE data section in the snapshot - a static"
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
            LogD("CAMHUNT  out of memory"); g_huntDone = TRUE; return;
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
    LogD("CAMHUNT  interval %d (MOVING): %d floats changed plausibly", g_intervals, g_candN);
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

static BOOL SafeReadInt(UINT_PTR a, int* out)
{
    __try { *out = *(const int*)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    return TRUE;
}

/* State candidates found by following the EXE's own UI strings:
     0x490A44  selected mission index (0x40 = title, -1 = none)
     0x490A48  mission result (set to 1 or 2 by the result screen)
     0x490CD0  in-game / menu mode flag
     0x490CC8  screen/mode companion
     0x48CB08  pause/menu candidate
     0x48DE3C  toggled mode flag (not the screen state)
     0x48DDD8  local player index (0..0x17)
     0x491A6C  state counter
   The T key logs these so one tagged run maps them to title/briefing/
   gameplay/death/victory. */
/* Dump the active player struct and the weapon-name table. The HUD values
   (health/armor, current/remaining ammo, slot contents) live in these fields;
   this maps them against the screenshot. */
static void PlayerDump(const char* why)
{
    const UINT_PTR b = g_exeBase;
    int idx = -1, i;
    if (!b) return;
    SafeReadInt(b + (0x48DDD8 - 0x400000), &idx);
    LogD("PLAYER %s  index=%d", why, idx);
    if (idx >= 0 && idx < 0x18) {
        const UINT_PTR base = b + (0x48DF00 - 0x400000) + (UINT_PTR)idx * 0x50;
        LogD("PLAYER base=0x%08IX (IDA 0x%08IX)",
            base, (UINT_PTR)(0x48DF00 + idx * 0x50));
        for (i = 0; i < 0x50; i += 4) {
            int v = 0;
            if (SafeReadInt(base + i, &v))
                LogD("PLAYER +%02X = %d (0x%08X)", i, v, v);
        }
        {
            int healthW = 0, selW = 0, w0W = 0, w1W = 0;
            int res0W = 0, res1W = 0, cur0W = 0, cur1W = 0;
            int health, sel, w0, w1, res0, res1, cur0, cur1;
            SafeReadInt(base + 0x04, &healthW);
            SafeReadInt(base + 0x06, &selW);
            SafeReadInt(base + 0x0A, &w0W);
            SafeReadInt(base + 0x0B, &w1W);
            SafeReadInt(base + 0x0C, &cur0W);
            SafeReadInt(base + 0x0D, &cur1W);
            SafeReadInt(base + 0x0E, &res0W);
            SafeReadInt(base + 0x0F, &res1W);
            health = healthW & 0xFFFF;
            sel    = selW & 0xFF;
            w0     = w0W & 0xFF;
            w1     = w1W & 0xFF;
            res0   = res0W & 0xFF;
            res1   = res1W & 0xFF;
            cur0   = cur0W & 0xFF;
            cur1   = cur1W & 0xFF;
            LogD("HUDVALS health=%d state=%s selectedSlot=%d"
                " slots=%d,%d  current=%d,%d reserve=%d,%d",
                health,
                health >= 0x50 ? "FINE" : health >= 0x28 ? "CAUTION"
                               : health > 0 ? "DANGER" : "DEAD",
                sel, w0, w1, cur0, cur1, res0, res1);
        }
    }
    for (i = 0; i < 32; ++i) {
        int ptr = 0;
        if (!SafeReadInt(b + (0x465DA0 - 0x400000) + i * 4, &ptr) || !ptr)
            continue;
        __try {
            const char* s = (const char*)((UINT_PTR)ptr - 0x400000 + b);
            LogD("WEAPON %2d ptr=0x%08IX '%.64s'", i, ptr, s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogD("WEAPON %2d ptr=0x%08IX <unreadable>", i, ptr);
        }
    }
}

// ---------------------------------------------------------------------------
// Custom HUD (target-resolution GDI overlay)
//
// XrApi is one image, so this is drawn over the same game window after
// Present, not an OpenVR/OpenXR overlay layer. The game's 2D HUD is suppressed
// in gameplay (its 3D weapon-slot models stay); this draws the text and panels
// at the window's real resolution with a normal font.
// ---------------------------------------------------------------------------

typedef struct {
    int health, sel, w0, w1, cur0, cur1, res0, res1;
    int dead;      /* mission result == 2: force the STATE readout to DEAD */
    int reload;    /* player action byte: 3 = RELOADING, 5 = CHANGING */
} HudVals;

static BOOL ReadHudVals(HudVals* v)
{
    const UINT_PTR b = g_exeBase;
    int idx = -1, baseW[8];
    UINT_PTR base;
    if (!v || !b) return FALSE;
    if (!SafeReadInt(b + (0x48DDD8 - 0x400000), &idx)) return FALSE;
    if (idx < 0 || idx >= 0x18) return FALSE;
    base = b + (0x48DF00 - 0x400000) + (UINT_PTR)idx * 0x50;
    if (!SafeReadInt(base + 0x04, &baseW[0]) ||
        !SafeReadInt(base + 0x06, &baseW[1]) ||
        !SafeReadInt(base + 0x0A, &baseW[2]) ||
        !SafeReadInt(base + 0x0B, &baseW[3]) ||
        !SafeReadInt(base + 0x0C, &baseW[4]) ||
        !SafeReadInt(base + 0x0D, &baseW[5]) ||
        !SafeReadInt(base + 0x0E, &baseW[6]) ||
        !SafeReadInt(base + 0x0F, &baseW[7]))
        return FALSE;
    v->health = baseW[0] & 0xFFFF;
    v->sel    = baseW[1] & 0xFF;
    v->w0     = baseW[2] & 0xFF;
    v->w1     = baseW[3] & 0xFF;
    /* The two low bytes at +0C/+0D are the loaded magazine; +0E/+0F are the
       reserve. Confirmed against the game's own HUD: slot 1 read "30 | 60"
       while +0D=30 and +0F=60. */
    v->cur0   = baseW[4] & 0xFF;
    v->cur1   = baseW[5] & 0xFF;
    v->res0   = baseW[6] & 0xFF;
    v->res1   = baseW[7] & 0xFF;
    {
        int resultW = 0, actionW = 0;
        if (SafeReadInt(b + (0x490A48 - 0x400000), &resultW))
            v->dead = (resultW == 2) ? 1 : 0;
        else
            v->dead = 0;
        /* The byte just before the player struct selects the HUD status:
           3 = RELOADING, 5 = CHANGING (push 4655A4h / 465598h at 0x405346 /
           0x405392). */
        if (SafeReadInt(base - 1, &actionW))
            v->reload = actionW & 0xFF;
        else
            v->reload = 0;
    }
    return TRUE;
}

static const char* WeaponNameShort(int id, char* out, size_t n)
{
    int ptr = 0;
    const char* s;
    if (!out || n == 0) return "?";
    strcpy(out, "?");
    if (id < 0 || id >= 32) return out;
    if (!SafeReadInt(g_exeBase + (0x465DA0 - 0x400000) + id * 4, &ptr) || !ptr)
        return out;
    s = (const char*)((UINT_PTR)ptr - 0x400000 + g_exeBase);
    __try {
        strncpy(out, s, n - 1);
        out[n - 1] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy(out, "?");
        return out;
    }
    {
        size_t len = strlen(out);
        while (len && out[len - 1] == ' ') out[--len] = 0;
    }
    return out;
}

static const char* ActiveWeaponName(void)
{
    static char nm[64] = "?";
    HudVals v;
    int id;
    if (!ReadHudVals(&v)) return nm;
    id = v.sel ? v.w1 : v.w0;
    WeaponNameShort(id, nm, sizeof(nm));
    return nm;
}

static const char* StateName(int health)
{
    return health >= 0x50 ? "FINE"
         : health >= 0x28 ? "CAUTION"
         : health > 0     ? "DANGER"
                          : "DEAD";
}

static COLORREF StateColor(int health)
{
    if (health >= 0x50)
        return RGB(0, 220, 0);                       /* FINE: green */
    if (health >= 0x28) {
        /* CAUTION: green -> yellow as health falls from 0x50 to 0x28. */
        const int t = (0x50 - health) * 255 / (0x50 - 0x28);
        return RGB((t * 230) / 255, 220, 0);
    }
    if (health > 0)
        return RGB(240, 140, 0);                     /* DANGER: orange */
    return RGB(230, 30, 30);                         /* DEAD: red */
}

/* ---------------------------------------------------------------------------
   Custom HUD: GDI renders the text at the target resolution into a 32bpp
   DIB, which is uploaded to a D3D texture and drawn as a quad inside the
   centre square of the window. Unlike the old GDI-over-the-window prototype,
   this is part of the D3D frame, so XrApi's single-image capture sees it.
   --------------------------------------------------------------------------- */

static void*   g_hudTex   = NULL;
static void*   g_panelTex = NULL;
static UINT    g_hudTexW  = 0, g_hudTexH = 0;
static int     g_hudDrawW = 0, g_hudDrawH = 0;
static BOOL    g_hudValid = FALSE;
static HudVals g_hudLast;

static HBITMAP g_hudDib   = NULL;
static HDC     g_hudDC    = NULL;
static HGDIOBJ g_hudOld   = NULL;
static void*   g_hudBits  = NULL;
static int     g_hudDibW  = 0, g_hudDibH = 0;
static HBRUSH  g_hudBlack = NULL;
static HFONT   g_hudBig   = NULL, g_hudSmall = NULL, g_hudStatus = NULL;
static int     g_hudBigH  = 0, g_hudSmallH = 0, g_hudStatusH = 0;
static HBRUSH  g_panelShadowBrush = NULL, g_panelFillBrush = NULL;
static HPEN    g_panelBorderPen   = NULL;

/* Virtual dominant hand + laser pointer. */
static void*   g_handTex   = NULL;
static void*   g_whiteTex  = NULL;
static int     g_handedness = 0;     /* 0 = right, 1 = left */
static BOOL    g_handFlip   = TRUE;  /* TRUE = apply the Quest 3 roll correction */

/* Haptics. The outbound packet carries, per hand, a vibration DURATION in
   frames - not an amplitude - and the runtime only stores values greater than
   zero, so sending 0 does not stop a buzz. Keep a per-hand countdown, top it
   up when the magazine counter drops, send the current count, then decay it:
   a single shot is a short click, automatic fire stays topped up and reads as
   up when the magazine counter drops, send the current count, then decay it:
   a single shot is a short click, and automatic fire tops it up per round for
   a continuous rumble. The value is deliberately low, matching SHOGO MAD WXR:
   2 frames is felt as a short kick, not a limp. */
#define HAPTIC_MAX_ROUNDS 8      /* a bigger drop is a reload/pickup, not fire */
#define HAPTIC_MAX_FRAMES 5      /* cap, so nothing can leave the motors on */
static BOOL  g_haptics      = TRUE;
static int   g_hapticHand   = 0;     /* 0 dominant, 1 both, 2 left, 3 right */
static int   g_hapticFrames = 2;     /* buzz length per shot, in frames */
static int   g_hapticL      = 0;     /* frames left to vibrate, per hand */
static int   g_hapticR      = 0;
static int   g_hapPrevCur0  = -1;    /* magazine counters, previous frame */
static int   g_hapPrevCur1  = -1;
static long  g_hapShots     = 0;
/* Two-handed aim: held while the left grip is down (not a config option).
   Both hands are on the weapon, so a shot buzzes both controllers; the pose
   work will hang the off hand off the same state. */
static BOOL  g_twoHanded    = FALSE;

/* Stereo experiment. 0 = mono (shipping), 1 = SBS, 2 = AER. AER alternates
   one eye per presented frame and offsets the view by +/- IPD/2 along the
   camera's right vector; SBS renders both eyes into the same frame. */
#define STEREO_MONO 0
#define STEREO_SBS  1
#define STEREO_AER  2
static int   g_stereoMode   = STEREO_MONO;
static BOOL  g_eyeRight     = FALSE;   /* the eye being rendered this frame */
static BOOL  g_hudModelPass = FALSE;   /* reached the 3D HUD weapon models */

/* AER reprojection scratch: the game's shared vertex buffers are never
   modified, each draw is copied here and re-submitted as a UP draw. */
#define AER_MAX_V 65536
#define AER_MAX_I 196608
static BYTE g_aerVerts[AER_MAX_V * 28];
static WORD g_aerIdx[AER_MAX_I];

/* Manual right-hand override for test driving, active only while the "0"
   user-control mode is ON. See ManualHandKeys for the key map.
   Positions/orientations are in XOPS space (+X right, +Y up, +Z forward),
   because that is what XrPoll already delivers. */
static BOOL    g_manualRight = FALSE;
static float   g_manualPos[3] = { 0.22f, -0.16f, 0.45f };
static float   g_manualYaw   = 0.0f;
static float   g_manualPitch = 0.0f;
static float   g_manualRoll  = 0.0f;

/* Per-weapon visual offset of the dominant hand box. The cloned gun stays at
   the base hand pose, so Q+numpad slides the hand against the gun; T saves
   the offset for the active weapon. */
static float   g_handAdj[3] = { 0.0f, 0.0f, 0.0f };
static float   g_handAdjTable[32][3];
static int     g_adjWeapon = -1;

/* Set while a shot is aimed at the hand so the view can be restored before
   the world is drawn. */
static BOOL    g_aimRestore = FALSE;
static float   g_aimSavedA[2] = { 0.0f, 0.0f };
static float   g_aimSavedB[2] = { 0.0f, 0.0f };
static BOOL    g_aimSavedBok = FALSE;
static BOOL    g_zoomOn     = FALSE; /* Y toggles the game's zoom; hide laser */
static BOOL    g_fireHeld   = FALSE; /* shoot trigger / mouse button this frame */
static long    g_flashFrames = 0;    /* frames left to show the muzzle flash */
static BOOL    g_handLogged = FALSE;
static BOOL    g_handDrawnThisFrame = FALSE;

/* Draw-elimination diagnostic: after the world/hand pass, number every draw
   and suppress one per 3 s. When the spinning inventory gun vanishes, the
   logged index is its draw. */
static int     g_3dIdx = 0;
static int     g_3dPrevN = 0;
static int     g_3dShown = -1;
static BOOL    g_3dFreeze = FALSE;   /* N marks the current draw */
static int     g_3dFreezeIdx = -1;
static BOOL    g_3dManual = FALSE;   /* J/K stepped the selection */
static int     g_3dSel    = 96;      /* identified inventory-gun draw */
static BOOL    g_3dAttach = TRUE;    /* capture the N-2 weapon draw automatically */
static BOOL    g_cloneValid = FALSE; /* one-shot weapon clone captured */
static BOOL    g_needCapture = FALSE;/* waiting for the next weapon draw */
static BOOL    g_tailDump = FALSE;   /* dump the tail draws once */

typedef struct { int idx; void* vb; UINT numV, prim; void* tex; } TailDraw;
static TailDraw g_tail[16];
static int g_tailHead = 0;

static void LogDrawSource(const void* ra, const char* path,
                          UINT count, UINT stride)
{
    if (!g_3dAttach || g_cloneValid || g_3dIdx != g_3dSel) return;
    LogD("HIDE     draw %d source: %s caller IDA 0x%08IX count=%u stride=%u",
        g_3dSel, path, ToIda((void*)ra), count, stride);
}

static BOOL HideThisDraw(void)
{
    const int idx = g_3dIdx++;
    if (g_3dAttach) return FALSE;      /* found it: keep the HUD copy */
    if (g_3dPrevN <= 0) return FALSE;
    if (g_3dManual) return idx == g_3dSel;
    if (idx != (int)((GetTickCount() / 3000u) % (DWORD)g_3dPrevN)) return FALSE;
    if (idx != g_3dShown) {
        g_3dShown = idx;
        LogD("HIDE     3D draw %d of %d", idx, g_3dPrevN);
    }
    return TRUE;
}

static void ReleaseIUnknown(void* p)
{
    if (p) ((PFN_Release)(*(void***)p)[VT_IUNKNOWN_RELEASE])(p);
}

static void AddRefIUnknown(void* p)
{
    if (p) ((ULONG (__stdcall*)(void*))(*(void***)p)[1])(p);
}

static HRESULT TexGetSurfaceLevel(void* tex, UINT level, void** out)
{
    if (!tex || !out) return E_FAIL;
    return ((PFN_TexGetSurfaceLevel)(*(void***)tex)[VT_TEX_GETSURFACELEVEL])
        (tex, level, out);
}

static HRESULT TexLock(void* tex, UINT level, D3DLOCKED_RECT8* lr,
                       const RECT* rc, DWORD flags)
{
    if (!tex || !lr) return E_FAIL;
    return ((PFN_TexLockRect)(*(void***)tex)[VT_TEX_LOCKRECT])
        (tex, level, lr, rc, flags);
}

static HRESULT TexUnlock(void* tex, UINT level)
{
    if (!tex) return E_FAIL;
    return ((PFN_TexUnlockRect)(*(void***)tex)[VT_TEX_UNLOCKRECT])(tex, level);
}

static HRESULT SurfLock(void* surf, D3DLOCKED_RECT8* lr,
                        const RECT* rc, DWORD flags)
{
    if (!surf || !lr) return E_FAIL;
    return ((PFN_SurfLockRect)(*(void***)surf)[VT_SURF_LOCKRECT])
        (surf, lr, rc, flags);
}

static HRESULT SurfUnlock(void* surf)
{
    if (!surf) return E_FAIL;
    return ((PFN_SurfUnlockRect)(*(void***)surf)[VT_SURF_UNLOCKRECT])(surf);
}

static int NextPow2(int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

static void HudReleaseDib(void)
{
    if (g_hudDC) {
        if (g_hudOld) SelectObject(g_hudDC, g_hudOld);
        DeleteDC(g_hudDC);
        g_hudDC = NULL;
    }
    g_hudOld = NULL;
    if (g_hudDib) {
        DeleteObject(g_hudDib);
        g_hudDib = NULL;
    }
    g_hudBits = NULL;
    g_hudDibW = g_hudDibH = 0;
}

static BOOL HudEnsureDib(int w, int h)
{
    BITMAPINFO bmi;

    if (g_hudDib && g_hudDibW == w && g_hudDibH == h && g_hudBits)
        return TRUE;

    HudReleaseDib();
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;       /* top-down, like the texture */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hudDib = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &g_hudBits,
                                NULL, 0);
    if (!g_hudDib || !g_hudBits) {
        Log("HUD      CreateDIBSection(%dx%d) failed", w, h);
        HudReleaseDib();
        return FALSE;
    }
    memset(g_hudBits, 0, (SIZE_T)w * h * 4);
    g_hudDC = CreateCompatibleDC(NULL);
    if (!g_hudDC) {
        Log("HUD      CreateCompatibleDC failed");
        HudReleaseDib();
        return FALSE;
    }
    g_hudOld = SelectObject(g_hudDC, g_hudDib);
    g_hudDibW = w;
    g_hudDibH = h;
    if (!g_hudBlack) g_hudBlack = CreateSolidBrush(RGB(0, 0, 0));
    return TRUE;
}

static HFONT HudMakeFont(int h)
{
    HFONT f = CreateFontA(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, "Consolas");
    if (!f)
        f = CreateFontA(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, "Arial");
    return f;
}

static BOOL HudEnsureFonts(int h, int* bigHOut, int* smallHOut)
{
    int bigH = h / 10;
    int smallH;
    int statusH;
    if (bigH < 18) bigH = 18;
    smallH = bigH * 2 / 3;
    if (smallH < 12) smallH = 12;
    statusH = bigH * 7 / 5;
    if (statusH < 24) statusH = 24;
    if (!g_hudBig || g_hudBigH != bigH) {
        if (g_hudBig) DeleteObject(g_hudBig);
        g_hudBig = HudMakeFont(bigH);
        g_hudBigH = bigH;
    }
    if (!g_hudSmall || g_hudSmallH != smallH) {
        if (g_hudSmall) DeleteObject(g_hudSmall);
        g_hudSmall = HudMakeFont(smallH);
        g_hudSmallH = smallH;
    }
    if (!g_hudStatus || g_hudStatusH != statusH) {
        if (g_hudStatus) DeleteObject(g_hudStatus);
        g_hudStatus = HudMakeFont(statusH);
        g_hudStatusH = statusH;
    }
    if (bigHOut) *bigHOut = bigH;
    if (smallHOut) *smallHOut = smallH;
    return g_hudBig && g_hudSmall && g_hudStatus;
}

/* Draw one text run with a blue silhouette underneath. The blue is converted
   to opaque black during upload, which gives the text a dark outline so it
   stays readable on the light panels and on bright walls. */
static void HudDrawText(HFONT font, COLORREF color, int x, int y,
                        const char* s, int align, int outline)
{
    static const int ox[8] = { -1,  0,  1, -1,  1, -1,  0,  1 };
    static const int oy[8] = { -1, -1, -1,  0,  0,  1,  1,  1 };
    int i, len;

    if (!g_hudDC || !font || !s) return;
    len = (int)strlen(s);
    SelectObject(g_hudDC, font);
    SetTextAlign(g_hudDC, align);
    if (outline > 0) {
        SetTextColor(g_hudDC, RGB(0, 0, 255));
        for (i = 0; i < 8; ++i)
            ExtTextOutA(g_hudDC, x + ox[i] * outline, y + oy[i] * outline,
                        0, NULL, s, (UINT)len, NULL);
    }
    SetTextColor(g_hudDC, color);
    ExtTextOutA(g_hudDC, x, y, 0, NULL, s, (UINT)len, NULL);
}

typedef struct {
    RECT left, right;
    int  radius;
} HudPanels;

static BOOL HudComputePanels(int w, int h, const HudVals* v, HudPanels* P)
{
    int bigH = 0, smallH = 0, margin, leftX, rightX, mainY, stateY;
    int cur, res, selEmpty, otherEmpty;
    int gap, leftW, rightW, pad;
    char curS[32], resS[32], stateS[64], nameA[64], nameB[64];
    SIZE szCur, szRes, szState, szA, szB;

    if (!P || !v || !g_hudDC) return FALSE;
    if (!HudEnsureFonts(h, &bigH, &smallH)) return FALSE;

    margin = w / 18;
    leftX  = margin;
    rightX = w - margin;
    mainY  = h * 60 / 100;
    stateY = h * 78 / 100;

    cur = v->sel ? v->cur1 : v->cur0;
    res = v->sel ? v->res1 : v->res0;
    selEmpty   = ((v->sel ? v->w1 : v->w0) == 0);
    otherEmpty = ((v->sel ? v->w0 : v->w1) == 0);

    _snprintf(curS, sizeof(curS), "%d", cur);
    _snprintf(resS, sizeof(resS), "/ %d", res);
    _snprintf(stateS, sizeof(stateS), "STATE %s",
              StateName(v->dead ? 0 : v->health));
    WeaponNameShort(v->sel ? v->w1 : v->w0, nameA, sizeof(nameA));
    if (selEmpty) _snprintf(nameA, sizeof(nameA), "None");
    WeaponNameShort(v->sel ? v->w0 : v->w1, nameB, sizeof(nameB));
    if (otherEmpty) _snprintf(nameB, sizeof(nameB), "None");

    SelectObject(g_hudDC, g_hudBig);
    GetTextExtentPoint32A(g_hudDC, curS, (int)strlen(curS), &szCur);
    GetTextExtentPoint32A(g_hudDC, nameA, (int)strlen(nameA), &szA);
    SelectObject(g_hudDC, g_hudSmall);
    GetTextExtentPoint32A(g_hudDC, resS, (int)strlen(resS), &szRes);
    GetTextExtentPoint32A(g_hudDC, stateS, (int)strlen(stateS), &szState);
    GetTextExtentPoint32A(g_hudDC, nameB, (int)strlen(nameB), &szB);

    gap = margin / 2;
    leftW = szCur.cx + gap + szRes.cx;
    if (szState.cx > leftW) leftW = szState.cx;
    rightW = szA.cx > szB.cx ? szA.cx : szB.cx;

    pad = margin / 2;
    if (pad < 6) pad = 6;

    P->left.left   = leftX - pad;
    P->left.top    = mainY - pad;
    P->left.right  = leftX + leftW + pad;
    P->left.bottom = stateY + smallH + pad;

    P->right.left   = rightX - rightW - pad;
    P->right.top    = mainY - pad;
    P->right.right  = rightX + pad;
    P->right.bottom = mainY + bigH + smallH / 3 + smallH + pad;

    if (P->left.left < 2) P->left.left = 2;
    if (P->left.top < 2) P->left.top = 2;
    if (P->left.right > w - 2) P->left.right = w - 2;
    if (P->left.bottom > h - 2) P->left.bottom = h - 2;
    if (P->right.left < 2) P->right.left = 2;
    if (P->right.top < 2) P->right.top = 2;
    if (P->right.right > w - 2) P->right.right = w - 2;
    if (P->right.bottom > h - 2) P->right.bottom = h - 2;

    P->radius = bigH / 3;
    if (P->radius < 6) P->radius = 6;
    if (P->radius > 28) P->radius = 28;
    return TRUE;
}

/* Panels are rendered with solid blue-channel codes so the upload can assign
   independent alpha values: 30 = soft shadow, 70 = white-ish fill,
   140 = light border. */
static void RenderPanelsToDib(int w, int h, const HudVals* v)
{
    HudPanels P;
    RECT all;

    if (!g_hudDC || !g_hudBits || !v) return;
    all.left = 0; all.top = 0; all.right = w; all.bottom = h;
    FillRect(g_hudDC, &all, g_hudBlack);
    if (!HudComputePanels(w, h, v, &P)) return;

    if (!g_panelShadowBrush) g_panelShadowBrush = CreateSolidBrush(RGB(0, 0, 30));
    if (!g_panelFillBrush)   g_panelFillBrush   = CreateSolidBrush(RGB(0, 0, 70));
    if (!g_panelBorderPen)   g_panelBorderPen   = CreatePen(PS_SOLID, 2, RGB(0, 0, 140));

    /* Soft drop shadow, then fill, then a 2px light border. The shadow only
       remains around the bottom/right edge because the fill is drawn over it. */
    SelectObject(g_hudDC, g_panelShadowBrush);
    SelectObject(g_hudDC, GetStockObject(NULL_PEN));
    RoundRect(g_hudDC, P.left.left + 3, P.left.top + 3,
              P.left.right + 3, P.left.bottom + 3, P.radius, P.radius);
    RoundRect(g_hudDC, P.right.left + 3, P.right.top + 3,
              P.right.right + 3, P.right.bottom + 3, P.radius, P.radius);

    SelectObject(g_hudDC, g_panelFillBrush);
    RoundRect(g_hudDC, P.left.left, P.left.top,
              P.left.right, P.left.bottom, P.radius, P.radius);
    RoundRect(g_hudDC, P.right.left, P.right.top,
              P.right.right, P.right.bottom, P.radius, P.radius);

    SelectObject(g_hudDC, GetStockObject(NULL_BRUSH));
    SelectObject(g_hudDC, g_panelBorderPen);
    RoundRect(g_hudDC, P.left.left, P.left.top,
              P.left.right, P.left.bottom, P.radius, P.radius);
    RoundRect(g_hudDC, P.right.left, P.right.top,
              P.right.right, P.right.bottom, P.radius, P.radius);
}

static void RenderHudToDib(int w, int h, const HudVals* v)
{
    RECT all;
    int bigH = 0, smallH = 0, margin, leftX, rightX, mainY, stateY;
    int cur, res, selEmpty, otherEmpty, stateHealth;
    int outlineBig, outlineSmall, outlineStatus, statusH;
    char curS[32], resS[32], stateS[64], nameA[64], nameB[64];
    SIZE sz;

    if (!g_hudDC || !g_hudBits || !v) return;

    all.left = 0; all.top = 0; all.right = w; all.bottom = h;
    FillRect(g_hudDC, &all, g_hudBlack);
    SetBkMode(g_hudDC, TRANSPARENT);
    SetTextAlign(g_hudDC, TA_LEFT | TA_TOP);
    if (!HudEnsureFonts(h, &bigH, &smallH)) return;

    margin = w / 18;
    leftX  = margin;
    rightX = w - margin;
    mainY  = h * 60 / 100;
    stateY = h * 78 / 100;

    outlineBig = bigH / 20;
    if (outlineBig < 1) outlineBig = 1;
    if (outlineBig > 3) outlineBig = 3;
    outlineSmall = smallH / 24;
    if (outlineSmall < 1) outlineSmall = 1;
    if (outlineSmall > 2) outlineSmall = 2;
    statusH = g_hudStatusH;
    outlineStatus = statusH / 20;
    if (outlineStatus < 1) outlineStatus = 1;
    if (outlineStatus > 3) outlineStatus = 3;

    cur = v->sel ? v->cur1 : v->cur0;
    res = v->sel ? v->res1 : v->res0;
    selEmpty   = ((v->sel ? v->w1 : v->w0) == 0);
    otherEmpty = ((v->sel ? v->w0 : v->w1) == 0);
    stateHealth = v->dead ? 0 : v->health;

    /* Left: current ammo, remaining ammo to its right, then STATE. */
    _snprintf(curS, sizeof(curS), "%d", cur);
    _snprintf(resS, sizeof(resS), "/ %d", res);
    SelectObject(g_hudDC, g_hudBig);
    GetTextExtentPoint32A(g_hudDC, curS, (int)strlen(curS), &sz);
    HudDrawText(g_hudBig, RGB(240, 240, 240), leftX, mainY, curS,
                TA_LEFT | TA_TOP, outlineBig);
    HudDrawText(g_hudSmall, RGB(150, 150, 150),
                leftX + sz.cx + margin / 2, mainY + (bigH - smallH), resS,
                TA_LEFT | TA_TOP, outlineSmall);

    _snprintf(stateS, sizeof(stateS), "STATE %s", StateName(stateHealth));
    HudDrawText(g_hudSmall, StateColor(stateHealth), leftX, stateY, stateS,
                TA_LEFT | TA_TOP, outlineSmall);

    /* Reload/change status, modelled on the game's own large centred text. */
    if (v->reload == 3 || v->reload == 5) {
        const char* status = (v->reload == 3) ? "RELOADING" : "CHANGING";
        HudDrawText(g_hudStatus, RGB(255, 255, 255), w / 2, h * 30 / 100,
                    status, TA_CENTER | TA_TOP, outlineStatus);
    }

    /* Right: selected weapon name, other slot name below. */
    WeaponNameShort(v->sel ? v->w1 : v->w0, nameA, sizeof(nameA));
    if (selEmpty) _snprintf(nameA, sizeof(nameA), "None");
    HudDrawText(g_hudBig, selEmpty ? RGB(255, 255, 255) : RGB(200, 200, 200),
                rightX, mainY, nameA, TA_RIGHT | TA_TOP, outlineBig);

    WeaponNameShort(v->sel ? v->w0 : v->w1, nameB, sizeof(nameB));
    if (otherEmpty) _snprintf(nameB, sizeof(nameB), "None");
    HudDrawText(g_hudSmall, otherEmpty ? RGB(255, 255, 255) : RGB(140, 140, 140),
                rightX, mainY + bigH + smallH / 3, nameB,
                TA_RIGHT | TA_TOP, outlineSmall);
    SetTextAlign(g_hudDC, TA_LEFT | TA_TOP);
}

static BOOL HudEnsureTexture(void* dev, int w, int h)
{
    int tw = NextPow2(w), th = NextPow2(h);
    HRESULT hr;

    if (g_hudTex && g_panelTex &&
        g_hudTexW == (UINT)tw && g_hudTexH == (UINT)th)
        return TRUE;

    if (g_hudTex) {
        ReleaseIUnknown(g_hudTex);
        g_hudTex = NULL;
    }
    if (g_panelTex) {
        ReleaseIUnknown(g_panelTex);
        g_panelTex = NULL;
    }
    g_hudTexW = g_hudTexH = 0;
    if (!o_CreateTexture) return FALSE;

    hr = o_CreateTexture(dev, tw, th, 1, 0, D3DFMT_A8R8G8B8,
                         D3DPOOL_MANAGED, &g_hudTex);
    if (FAILED(hr)) {
        hr = o_CreateTexture(dev, tw, th, 1, D3DUSAGE_DYNAMIC,
                             D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex);
    }
    if (FAILED(hr) || !g_hudTex) {
        Log("HUD      CreateTexture(%dx%d) failed hr=0x%08lX", tw, th, hr);
        g_hudTex = NULL;
        return FALSE;
    }
    hr = o_CreateTexture(dev, tw, th, 1, 0, D3DFMT_A8R8G8B8,
                         D3DPOOL_MANAGED, &g_panelTex);
    if (FAILED(hr)) {
        hr = o_CreateTexture(dev, tw, th, 1, D3DUSAGE_DYNAMIC,
                             D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_panelTex);
    }
    if (FAILED(hr) || !g_panelTex) {
        Log("HUD      CreateTexture(panel %dx%d) failed hr=0x%08lX",
            tw, th, hr);
        ReleaseIUnknown(g_hudTex);
        g_hudTex = NULL;
        g_panelTex = NULL;
        return FALSE;
    }
    g_hudTexW = tw;
    g_hudTexH = th;
    g_hudValid = FALSE;
    if (!HudEnsureDib(tw, th)) {
        ReleaseIUnknown(g_hudTex);
        ReleaseIUnknown(g_panelTex);
        g_hudTex = NULL;
        g_panelTex = NULL;
        g_hudTexW = g_hudTexH = 0;
        return FALSE;
    }
    Log("HUD      textures %dx%d (text + panels, draw region %dx%d)",
        tw, th, w, h);
    return TRUE;
}

/* Copy the GDI BGR pixels into A8R8G8B8. GDI leaves the alpha byte at zero,
   so derive coverage from brightness: text drawn on black becomes a clean
   alpha mask, and the bright text keeps its colour. The pure-blue silhouette
   from HudDrawText becomes opaque black, i.e. a dark outline. */
static void HudUpload(void* dev, int w, int h)
{
    D3DLOCKED_RECT8 lr;
    RECT rc;
    int x, y;

    if (!g_hudTex || !g_hudBits) return;
    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
    if (TexLock(g_hudTex, 0, &lr, &rc, 0) != S_OK || !lr.pBits) return;
    for (y = 0; y < h; ++y) {
        const DWORD* src = (const DWORD*)((const BYTE*)g_hudBits +
                                          (SIZE_T)y * g_hudDibW * 4);
        DWORD* dst = (DWORD*)((BYTE*)lr.pBits + (SIZE_T)y * lr.Pitch);
        for (x = 0; x < w; ++x) {
            const DWORD c = src[x];
            const DWORD b = c & 0xFF, g = (c >> 8) & 0xFF, r = (c >> 16) & 0xFF;
            if (r < 16 && g < 16 && b > 0) {
                /* Blue outline pass -> black with alpha from its coverage. */
                dst[x] = (b << 24);
            } else {
                DWORD a = r;
                if (g > a) a = g;
                if (b > a) a = b;
                if (a) { a = a * 6 / 5; if (a > 255) a = 255; }
                dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    TexUnlock(g_hudTex, 0);
}

/* Panel texture: the blue channel is the layer code, mapped to independent
   RGBA values so the fill/border can be translucent even though GDI cannot
   write alpha directly. */
static void HudUploadPanels(void* dev, int w, int h)
{
    D3DLOCKED_RECT8 lr;
    RECT rc;
    int x, y;

    if (!g_panelTex || !g_hudBits) return;
    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
    if (TexLock(g_panelTex, 0, &lr, &rc, 0) != S_OK || !lr.pBits) return;
    for (y = 0; y < h; ++y) {
        const DWORD* src = (const DWORD*)((const BYTE*)g_hudBits +
                                          (SIZE_T)y * g_hudDibW * 4);
        DWORD* dst = (DWORD*)((BYTE*)lr.pBits + (SIZE_T)y * lr.Pitch);
        for (x = 0; x < w; ++x) {
            const DWORD b = src[x] & 0xFF;
            DWORD out = 0;
            if (b >= 25 && b <= 35) {
                out = (50u << 24);                       /* soft shadow */
            } else if (b >= 65 && b <= 75) {
                out = (90u << 24) | (220u << 16) | (220u << 8) | 220u;
            } else if (b >= 135 && b <= 145) {
                out = (160u << 24) | (255u << 16) | (255u << 8) | 255u;
            }
            dst[x] = out;
        }
    }
    TexUnlock(g_panelTex, 0);
}

static BOOL PlayerDead(void)
{
    int result = -999;
    if (!g_exeBase) return FALSE;
    if (!SafeReadInt(g_exeBase + (0x490A48 - 0x400000), &result))
        return FALSE;
    return result == 2;
}

/* Gameplay, plus the death screen: keep the new HUD up so its STATE reads
   DEAD instead of dropping back to the game's own HUD for the final frames. */
static BOOL HudShouldDraw(void)
{
    return InGameplay() || PlayerDead();
}

static BOOL UpdateHudTexture(void* dev)
{
    HudVals v;
    int w, h;

    if (!o_CreateTexture || !ReadHudVals(&v)) return FALSE;
    w = (int)g_bbW / 3;
    h = (int)g_bbH / 3;
    if (w < 160 || h < 96) return FALSE;
    if (!HudEnsureTexture(dev, w, h)) return FALSE;
    if (g_hudValid && g_hudDrawW == w && g_hudDrawH == h &&
        memcmp(&v, &g_hudLast, sizeof(v)) == 0)
        return TRUE;

    RenderPanelsToDib(w, h, &v);
    HudUploadPanels(dev, w, h);
    RenderHudToDib(w, h, &v);
    HudUpload(dev, w, h);
    g_hudLast  = v;
    g_hudDrawW = w;
    g_hudDrawH = h;
    g_hudValid = TRUE;
    return TRUE;
}

static void StateLog(const char* tag)
{
    const UINT_PTR b = g_exeBase;
    int a44 = -999, a48 = -999, cd0 = -999, cc8 = -999;
    int cb08 = -999, de3c = -999, ddd8 = -999, a6c = -999;

#define IDAVA(x) (b + ((UINT_PTR)(x) - 0x400000))
    SafeReadInt(IDAVA(0x490A44), &a44);
    SafeReadInt(IDAVA(0x490A48), &a48);
    SafeReadInt(IDAVA(0x490CD0), &cd0);
    SafeReadInt(IDAVA(0x490CC8), &cc8);
    SafeReadInt(IDAVA(0x48CB08), &cb08);
    SafeReadInt(IDAVA(0x48DE3C), &de3c);
    SafeReadInt(IDAVA(0x48DDD8), &ddd8);
    SafeReadInt(IDAVA(0x491A6C), &a6c);
#undef IDAVA

    Log("STATE %-12s A44=%d A48=%d CD0=%d CC8=%d CB08=%d DE3C=%d DDD8=%d 1A6C=%d",
        tag, a44, a48, cd0, cc8, cb08, de3c, ddd8, a6c);
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
    LogD("CAMHUNT  interval %d (%s): %d tracked, best score %d held by %d, "
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
    if (best < 3) { LogD("    CAMHUNT  best score only %d - keep walking and stopping", best); return; }

    LogD("    CAMHUNT  top candidates (best score %d of %d intervals):", best, g_intervals);
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
        LogD("      0x%08IX%s score=%d triple=%d  [% .3f % .3f % .3f % .3f]",
            g_cands[i], ida, g_score[i], trip, f[0], f[1], f[2], f[3]);
        ++shown;
    }
    LogD("      triple=2 means the next two floats score as well -> an x,y,z");
    LogD("      triple, which is what a stored position looks like.");
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
#define IN_SCREENTOCLIENT    6
#define IN_CLIENTTOSCREEN    7
#define IN_COUNT             8

static const char* kInputNames[IN_COUNT] = {
    "GetAsyncKeyState", "GetKeyboardState", "GetKeyState",
    "GetCursorPos", "SetCursorPos", "GetForegroundWindow",
    "ScreenToClient", "ClientToScreen"
};
static long  g_inCalls[IN_COUNT]  = {0,0,0,0,0,0,0,0};
static long  g_inFrame[IN_COUNT]  = {0,0,0,0,0,0,0,0};
static FARPROC g_inReal[IN_COUNT] = {0,0,0,0,0,0,0,0};

/* Keep running when the window is not focused. On by default: the whole point
   of the UDP tester is that something else has focus. */
/* TRUE = VR-only input during gameplay (desktop keyboard/mouse suppressed).
   This is the default now. Key 0 enables desktop keyboard/mouse for manual
   tests without a headset or fake XrApi data. */
static BOOL g_vrExclusive = TRUE;
static BOOL g_noPause  = TRUE;
/* g_gameWnd is declared with the window state above. */

/* Display stretch: the game still reasons in 640x480, but the window may be
   larger and Present stretches the frame to it. Input must be mapped back to
   640x480, or clicks at a stretched position land outside the game's logical
   UI. Computed in hkPresent from the client rect. */
static float g_displayScaleX = 1.0f;
static float g_displayScaleY = 1.0f;

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
typedef BOOL  (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL  (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);

/* The window's REAL foreground state, asked of the unhooked API: the hooked
   GetForegroundWindow deliberately lies so the game keeps running while the
   owner uses another window. That keeps the game alive, but it must not let
   input cross between windows. */
static BOOL g_inputMuted = FALSE;   /* set once per GetKeyboardState poll */

static BOOL GameWindowFocused(void)
{
    HWND w = g_gameWnd ? g_gameWnd : g_wnd;
    if (!w || !g_inReal[IN_GETFOREGROUNDWINDOW]) return TRUE;
    return ((PFN_GetForegroundWindow)g_inReal[IN_GETFOREGROUNDWINDOW])() == w;
}

/* Keys WinlatorXR injects for the controller, so the physical key must never
   also reach the game: A/B/X/Y always in VR mode (on any screen), the arrow
   keys whenever MODE_VR is 1. Manual PC mode (the 0 key) is a desktop keyboard
   and is deliberately exempt. */
static BOOL InputKeyBlocked(int vk)
{
    if (!g_vrExclusive) return FALSE;
    if (vk == 'A' || vk == 'B' || vk == 'X' || vk == 'Y') return TRUE;
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
        return InGameplay() ? TRUE : FALSE;
    return FALSE;
}

static SHORT WINAPI hkGetAsyncKeyState(int vk)
{
    ++g_inCalls[IN_GETASYNCKEYSTATE]; ++g_inFrame[IN_GETASYNCKEYSTATE];
    if (!GameWindowFocused() || InputKeyBlocked(vk)) return 0;
    return ((PFN_GetAsyncKeyState)g_inReal[IN_GETASYNCKEYSTATE])(vk);
}

static BOOL WINAPI hkGetKeyboardState(PBYTE p)
{
    BOOL r;
    ++g_inCalls[IN_GETKEYBOARDSTATE]; ++g_inFrame[IN_GETKEYBOARDSTATE];
    r = ((PFN_GetKeyboardState)g_inReal[IN_GETKEYBOARDSTATE])(p);
    if (r && p) {
        /* Focus is the boundary for DESKTOP input. While another window owns
           the keyboard and mouse the game keeps running (g_noPause), but no
           physical key, button or cursor movement may cross between windows.
           XrApi input is not desktop input: the runtime/ UDP Tester may hold
           focus while it drives the game, so StickKeys/VrInputKeys still run
           below and inject their keys into the emptied array. Debug keys stay
           muted (ProbeKeys). */
        g_inputMuted = !GameWindowFocused();
        if (g_inputMuted) ZeroMemory(p, 256);

        /* WinlatorXR sends A/B/X/Y for the controller buttons, so the physical
           keys must never also reach the game - on any screen. The arrow keys
           are its extra controls and are blocked whenever MODE_VR is 1.
           Manual PC mode (the 0 key) is a real desktop keyboard and is
           deliberately exempt, so A/B/X/Y/arrows work there. */
        if (g_vrExclusive) {
            p['A'] = 0; p['B'] = 0; p['X'] = 0; p['Y'] = 0;
            if (InGameplay()) {
                p[VK_UP] = 0; p[VK_DOWN] = 0;
                p[VK_LEFT] = 0; p[VK_RIGHT] = 0;
            }
        }

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
        StickKeys(p);          /* left stick -> movement */
        ManualHandKeys(p);     /* numpad right hand, 0 mode only */
        VrInputKeys(p);        /* buttons, two-hand grip, shoot, zoom, edges */
        /* Fire state for the muzzle flash, and the hand-aim shot. In VR the
           trigger sets LButton above; in user mode it is the real mouse. */
        g_fireHeld = (p[VK_LBUTTON] & 0x80) ? TRUE : FALSE;
        if (g_fireHeld) g_flashFrames = 4;
        /* User-control mode: fire with the mouse, but land the shot on the
           right hand's laser. The view is restored at BeginScene. */
        if (!g_vrExclusive && g_manualRight && g_fireHeld)
            SteerAimWrite();

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
                LogD("KEYS      vk 0x%02X ('%c') pressed at frame %ld", vk,
                    (vk >= 32 && vk < 127) ? (char)vk : '?', g_frame);
            }
        }
    }
    return r;
}

static SHORT WINAPI hkGetKeyState(int vk)
{
  ++g_inCalls[IN_GETKEYSTATE]; ++g_inFrame[IN_GETKEYSTATE];
  if (!GameWindowFocused() || InputKeyBlocked(vk)) return 0;
  return ((PFN_GetKeyState)g_inReal[IN_GETKEYSTATE])(vk);
}

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
        const BOOL steeringNow = g_steer && g_vrExclusive && InGameplay();
        const BOOL reallyFocused = GameWindowFocused();
        /* In menus/title/briefing/death/victory MODE_VR is 0 and the runtime
           owns the mouse again, so never suppress it there. When the window
           is unfocused, suppress it on every screen: no mouse input at all. */
        if (!reallyFocused || steeringNow || vrOwnsInput)
            *p = g_lastCursor;
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
    HWND fg = NULL;

    ++g_inCalls[IN_SETCURSORPOS]; ++g_inFrame[IN_SETCURSORPOS];
    g_lastCursor.x = x; g_lastCursor.y = y; g_cursorKnown = TRUE;

    /* Never move the real desktop cursor while another window has focus. The
       game still gets a consistent answer: hkGetCursorPos hands back exactly
       these coordinates, so its look delta stays zero, but the player's
       cursor stays where they left it and the desktop remains usable while
       the game keeps running in the background. */
    if (g_inReal[IN_GETFOREGROUNDWINDOW])
        fg = ((PFN_GetForegroundWindow)g_inReal[IN_GETFOREGROUNDWINDOW])();
    if (g_gameWnd && fg != g_gameWnd) return TRUE;
    return ((PFN_SetCursorPos)g_inReal[IN_SETCURSORPOS])(x, y);
}

/* The game's logical UI is 640x480. When Present stretches the frame to a
   larger client, ScreenToClient must return 640x480 coordinates, and
   ClientToScreen must turn the game's 640x480 coordinates back into physical
   client coordinates. Without both, a stretched menu is unclickable. */
static BOOL WINAPI hkScreenToClient(HWND h, LPPOINT p)
{
    BOOL r;
    ++g_inCalls[IN_SCREENTOCLIENT]; ++g_inFrame[IN_SCREENTOCLIENT];
    r = ((PFN_ScreenToClient)g_inReal[IN_SCREENTOCLIENT])(h, p);
    if (r && p && g_gameWnd && h == g_gameWnd &&
        g_displayScaleX > 0.0f && g_displayScaleY > 0.0f) {
        p->x = (LONG)(p->x / g_displayScaleX);
        p->y = (LONG)(p->y / g_displayScaleY);
    }
    return r;
}

static BOOL WINAPI hkClientToScreen(HWND h, LPPOINT p)
{
    POINT q;
    BOOL r;
    ++g_inCalls[IN_CLIENTTOSCREEN]; ++g_inFrame[IN_CLIENTTOSCREEN];
    if (!p) return ((PFN_ClientToScreen)g_inReal[IN_CLIENTTOSCREEN])(h, p);
    q = *p;
    if (g_gameWnd && h == g_gameWnd) {
        q.x = (LONG)(q.x * g_displayScaleX);
        q.y = (LONG)(q.y * g_displayScaleY);
    }
    r = ((PFN_ClientToScreen)g_inReal[IN_CLIENTTOSCREEN])(h, &q);
    if (r) *p = q;
    return r;
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

    LogD("IMPORTS of the host EXE");
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
         imp->Name; ++imp)
    {
        const char* mod = (const char*)(base + imp->Name);
        const char* note = "";
        if (_stricmp(mod, "dinput.dll")  == 0) note = "   <- DirectInput 7: proxy it and return our own device state";
        if (_stricmp(mod, "dinput8.dll") == 0) note = "   <- DirectInput 8: proxy it and return our own device state";
        if (_stricmp(mod, "d3d8.dll")    == 0) note = "   <- us";
        if (_stricmp(mod, "xinput1_3.dll") == 0) note = "   <- XInput";
        LogD("  %-24s%s", mod, note);

        // For the input modules, list the functions too - the specific entry
        // points are what a proxy has to implement.
        if (_strnicmp(mod, "dinput", 6) != 0) continue;
        IMAGE_THUNK_DATA* orig = (IMAGE_THUNK_DATA*)(base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                LogD("      ordinal %lu", (DWORD)(orig->u1.Ordinal & 0xFFFF));
            } else {
                IMAGE_IMPORT_BY_NAME* nm = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                LogD("      %s", (const char*)nm->Name);
            }
        }
    }
    LogD("");
}

// ---------------------------------------------------------------------------
// GDI font capture (HUD text clarity experiment)
//
// The HUD/text is rasterised at 640x480 and then stretched, so vertex scaling
// only magnifies the glyphs. The cheap experiment is to enlarge the font at
// creation so the atlas/DIB has more source pixels. Capture every
// CreateFontIndirectA first: the HUD font is identified from its height/face,
// then only that font is scaled.
//
// The hook is installed in DllMain (an IAT write, not LoadLibrary) because the
// game may create fonts before Direct3DCreate8/EnsureInit.
// ---------------------------------------------------------------------------

typedef HFONT (WINAPI *PFN_CreateFontIndirectA)(const LOGFONTA*);
static PFN_CreateFontIndirectA g_realCreateFontIndirectA = NULL;
static float   g_fontScale = 1.0f;      /* 1.25 looked worse; keep original */
static LOGFONTA g_fontLog[16];
static int     g_fontLogN = 0;

static void LogFontRequest(const LOGFONTA* lf)
{
    if (!lf || !g_log) return;
    LogD("FONT  h=%ld w=%ld weight=%ld italic=%d charset=%d face='%s'",
        lf->lfHeight, lf->lfWidth, lf->lfWeight, lf->lfItalic,
        lf->lfCharSet, lf->lfFaceName);
}

static HFONT WINAPI hkCreateFontIndirectA(const LOGFONTA* lf)
{
    LOGFONTA copy;
    if (lf) {
        copy = *lf;
        if (g_fontLogN < (int)(sizeof(g_fontLog)/sizeof(g_fontLog[0])))
            g_fontLog[g_fontLogN++] = copy;
        LogFontRequest(&copy);
        if (g_fontScale != 1.0f) {
            const LONG oldH = copy.lfHeight;
            copy.lfHeight = (LONG)(copy.lfHeight * g_fontScale);
            if (copy.lfWidth) copy.lfWidth = (LONG)(copy.lfWidth * g_fontScale);
            LogD("FONT  scale %.2f: h %ld -> %ld", g_fontScale, oldH, copy.lfHeight);
            lf = &copy;
        }
    }
    if (!g_realCreateFontIndirectA) return NULL;
    return g_realCreateFontIndirectA(lf);
}

// GDI text capture. The HUD text is drawn through DrawTextA/W into a DIB, so
// this gives the exact strings, rects and font the game uses. That is the map
// for hiding the default text and redrawing it at a higher resolution inside
// the single XrApi image.
typedef int (WINAPI *PFN_DrawTextA)(HDC, LPCSTR, int, LPRECT, UINT);
typedef int (WINAPI *PFN_DrawTextW)(HDC, LPCWSTR, int, LPRECT, UINT);
static PFN_DrawTextA g_realDrawTextA = NULL;
static PFN_DrawTextW g_realDrawTextW = NULL;
static long g_textLogs = 0;

static void LogTextCall(const char* which, HDC hdc, const char* a,
                        const wchar_t* w, LPRECT rc, UINT fmt, int count)
{
    char face[64] = {0};
    LONG fh = 0;
    if (g_log && g_textLogs < 400) {
        HFONT f = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        LOGFONTA lf;
        if (f && GetObjectA(f, sizeof(lf), &lf)) {
            fh = lf.lfHeight;
            strncpy(face, lf.lfFaceName, sizeof(face) - 1);
        }
        if (w)
            LogD("TEXT %s rc=%ld,%ld,%ld,%ld fmt=0x%X h=%ld face='%s' '%.80ls'",
                which, rc ? rc->left : 0, rc ? rc->top : 0,
                rc ? rc->right : 0, rc ? rc->bottom : 0, fmt, fh, face, w);
        else if (a) {
            char buf[160];
            int n = (count < 0) ? (int)strlen(a) : count;
            if (n > 159) n = 159;
            memcpy(buf, a, (size_t)n); buf[n] = 0;
            LogD("TEXT %s rc=%ld,%ld,%ld,%ld fmt=0x%X h=%ld face='%s' '%s'",
                which, rc ? rc->left : 0, rc ? rc->top : 0,
                rc ? rc->right : 0, rc ? rc->bottom : 0, fmt, fh, face, buf);
        }
        ++g_textLogs;
    }
}

static int WINAPI hkDrawTextA(HDC hdc, LPCSTR text, int count, LPRECT rc, UINT fmt)
{
    LogTextCall("A", hdc, text, NULL, rc, fmt, count);
    return g_realDrawTextA ? g_realDrawTextA(hdc, text, count, rc, fmt) : 0;
}

static int WINAPI hkDrawTextW(HDC hdc, LPCWSTR text, int count, LPRECT rc, UINT fmt)
{
    LogTextCall("W", hdc, NULL, text, rc, fmt, count);
    return g_realDrawTextW ? g_realDrawTextW(hdc, text, count, rc, fmt) : 0;
}

static void HookInput(void)
{
    HMODULE hExe = GetModuleHandleA(NULL);
    static const char* names[IN_COUNT] = {
        "GetAsyncKeyState", "GetKeyboardState", "GetKeyState",
        "GetCursorPos", "SetCursorPos", "GetForegroundWindow",
        "ScreenToClient", "ClientToScreen"
    };
    void* hooks[IN_COUNT] = {
        (void*)hkGetAsyncKeyState, (void*)hkGetKeyboardState, (void*)hkGetKeyState,
        (void*)hkGetCursorPos,     (void*)hkSetCursorPos,
        (void*)hkGetForegroundWindow,
        (void*)hkScreenToClient,   (void*)hkClientToScreen
    };

    LogD("INPUT  IAT patch results (a patched entry proves we can supply this");
    LogD("       function's answer later, not merely count it):");
    LogD("       GetForegroundWindow is how XOPS detects focus loss and pauses;");
    LogD("       patching it is what keeps the game running while the UDP");
    LogD("       tester has focus. Toggle with the backslash key.");
    for (int i = 0; i < IN_COUNT; ++i) {
        void* old = PatchIat(hExe, names[i], hooks[i]);
        g_inReal[i] = (FARPROC)old;
        LogD("  %-18s %s", names[i], old ? "imported - PATCHED" : "not imported");
    }
    LogD("");
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
        LogD("BALLISTIC  no candidate pool yet - walk and stop a few times first");
        return;
    }
    for (i = 0; i < g_candN; ++i) {
        if (!SafeRead(g_cands[i], &g_ballStart[i])) g_ballStart[i] = 0.0f;
        g_ballPeak[i] = g_ballStart[i];
    }
    g_ballFrames = BALL_FRAMES;
    ++g_ballRuns;
    LogD("");
    LogD("BALLISTIC  window %d open for %d frames (~%ds).", g_ballRuns,
        BALL_FRAMES, BALL_FRAMES / 60);
    LogD("BALLISTIC  press the height cheat NOW, then stand still and let");
    LogD("BALLISTIC  gravity bring you down. Do not walk during the window.");
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
    LogD("");
    LogD("BALLISTIC  window closed - scoring rise-then-return");
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
    LogD("BALLISTIC  %d candidates rose >= %.2f and returned to within %.2f",
        hits, BALL_RISE, BALL_SETTLE);
    if (hits == 0)
        LogD("BALLISTIC  nothing passed. Was the cheat actually applied, and was"
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

    LogD("PROBE    list built: %d candidates", g_probeN);
    LogD("PROBE    rejected %d that never moved, %d no longer addressable,",
        rejRange, rejDead);
    LogD("PROBE    and %d that changed while the player was idle (%d idle runs)",
        g_vetoTotal, g_idleRuns);
    if (g_idleRuns == 0)
        LogD("PROBE    WARNING: no idle run was ever observed, so the strongest"
            " filter never ran. Stand still - no keys, no mouse - for a few seconds.");
    for (i = 0; i < g_probeN && i < 24; ++i) {
        char where[40];
        where[0] = 0;
        if (g_probeList[i] >= g_exeLo && g_probeList[i] < g_exeHi)
            _snprintf(where, sizeof(where), " IDA 0x%08IX",
                      g_probeList[i] - g_exeLo + 0x400000);
        LogD("PROBE      [%2d] 0x%08IX%s score=%d moves=%d range % .3f..% .3f  %s",
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
        LogD("PROBE      [%2d] if this is Y, the position triple at 0x%08IX reads"
            " X=% .3f  Y=% .3f  Z=% .3f", i + 1, g_probeList[i] - 4, x, y, z);
    }
    for (i = 0; i < g_probeN && i < 24; ++i)
        if (g_probeShape[i] && !g_probeBall[i])
            LogD("PROBE      [%2d] its constant neighbour holds % .4f - that is the"
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
    LogD("STRUCT   %s at 0x%08IX, %d floats:", name, base, count);
    for (i = 0; i < count; ++i) {
        float v;
        const UINT_PTR a = base + (UINT_PTR)i * 4;
        char note[64];
        if (!SafeRead(a, &v)) { LogD("STRUCT     +%02X  unreadable", i * 4); continue; }
        note[0] = 0;
        if (v == 0.0f)
            _snprintf(note, sizeof(note), "  <- exactly zero (nothing writes it)");
        else if (v > -6.3f && v < 6.3f)
            _snprintf(note, sizeof(note), "  (%.2f deg if an angle)", v * 57.2957795f);
        LogD("STRUCT     +%02X  % 12.4f%s", i * 4, v, note);
    }
}

/* Print every watched float with its label. A value that is plausibly an angle
   in radians is also shown in degrees, because "0.597 rad" means nothing at a
   glance and "34.2 deg" means everything. */
static void WatchDump(const char* why)
{
    int i;
    LogD("WATCH    %s  [write phase: %s]", why, PhaseName(g_probePhase));
    for (i = 0; i < WATCH_N; ++i) {
        float v;
        char deg[32];
        if (!SafeRead(WatchAddr(i), &v)) { LogD("WATCH      0x%08IX  unreadable", g_watch[i].va); continue; }
        deg[0] = 0;
        if (g_watch[i].angle)
            _snprintf(deg, sizeof(deg), "  = %7.2f deg", v * 57.2957795f);
        LogD("WATCH      0x%08IX % 11.4f%-20s  %s", g_watch[i].va, v, deg, g_watch[i].what);
    }
    /* Read the two clusters out as positions, so they can be checked against
       something measured independently - the view matrix eye, or simply where
       you know you are standing. */
    {
        float ax, ay, az, bx, bz;
        if (SafeRead(WatchAddr(1), &ax) && SafeRead(WatchAddr(0), &ay)
            && SafeRead(WatchAddr(2), &az))
            LogD("WATCH      cluster A as a position:  X=% .3f  Y=% .3f  Z=% .3f", ax, ay, az);
        if (SafeRead(WatchAddr(6), &bx) && SafeRead(WatchAddr(5), &bz))
            LogD("WATCH      cluster B as a position:  X=% .3f  Z=% .3f", bx, bz);
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
            LogD("PROBE    watch entry 0x%08IX is not readable - skipped", g_watch[i].va);
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
    LogD("PROBE    watch list loaded: %d confirmed addresses, fixed order.", g_probeN);
    LogD("PROBE    8 steps through them. The order does not change, so [3] is");
    LogD("PROBE    always the same address - tag it with 0 and tell me the number.");
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
    LogD("PROBE    write phase -> %s", PhaseName(g_probePhase));
    if (g_probePhase == PHASE_INPUT)
        LogD("PROBE    if yaw/pitch start working now, they are read during the"
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

/* VR controller mapping (owner, 5 Nov 2026). Right-handed: left trigger =
   fire mode, right trigger = shoot. Left-handed swaps only those two; every
   other control is identical. Right-stick turning accumulates a yaw offset so
   it composes with head look instead of fighting it. */
static int   g_leftHanded = 0;
static float g_turnSpeed  = 120.0f; /* degrees per second */
static float g_turnYaw    = 0.0f;   /* accumulated right-stick yaw, radians */
static BYTE  g_prevVrBtn[24];
static BYTE  g_pulseVk[256];        /* frames remaining for edge-pressed keys */
/* Patch the game's hardcoded 4:3 projection aspect to the container aspect.
   OFF by default: the owner wants the game's default FOV untouched. The
   supported device configuration is a 4:3 container (1280x960), where the
   native frame fills the screen with no bars and no FOV change. The patch is
   retained as an opt-in experiment (`patchAspect 1` in the INI). */
static BOOL  g_patchAspect = FALSE;

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
    fprintf(f, "hudVisualScale %.2f\n", g_hudVisualScale);
    fprintf(f, "leftHanded %d\n",    g_leftHanded);
    fprintf(f, "turnSpeed %.0f\n",   g_turnSpeed);
    fprintf(f, "patchAspect %d\n",   g_patchAspect ? 1 : 0);
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
        else if (!strcmp(key, "hudVisualScale")) g_hudVisualScale = v;
        else if (!strcmp(key, "leftHanded")) g_leftHanded = v != 0;
        else if (!strcmp(key, "turnSpeed"))  g_turnSpeed  = v;
        else if (!strcmp(key, "patchAspect")) g_patchAspect = v != 0;
        /* Older files may carry game-space "hudScale"; ignore it rather than
           interpreting 0.33 as a visual scale. */
        else if (!strcmp(key, "hudScale"))  { /* superseded */ }
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
    g_turnYaw = 0.0f;
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
    if (!g_vrExclusive) return;     /* desktop test mode: mouse owns the view */
    if (!g_haveRef) SteerRecentre();

    /* Right stick turns the player body relative to the head. Accumulate a
       yaw offset so it composes with head look instead of being overwritten
       by it. 120 deg/s is smooth turn; a snap mode can replace this later. */
    if (fabs(g_pose.turnX) > 0.15f) {
        const float dt = 1.0f / 60.0f;
        g_turnYaw += g_yawSign * g_pose.turnX *
                     (g_turnSpeed * 0.0174532925f) * dt;
        while (g_turnYaw >  3.14159265f) g_turnYaw -= 6.2831853f;
        while (g_turnYaw < -3.14159265f) g_turnYaw += 6.2831853f;
    }

    /* Yaw is relative: the pose's yaw is already measured from the recentre
       point, so add it to where the game had us when we recentred. Pitch is
       absolute against the horizon, so it is written outright. */
    yaw   = g_refYaw + g_turnYaw + g_yawSign * g_pose.headYaw;
    pitch = g_pitchSign * g_pose.headPitch;

    if (g_cluster == CLU_A || g_cluster == CLU_BOTH) {
        SafeWrite(CamAddr(A_YAW),   yaw);
        SafeWrite(CamAddr(A_PITCH), pitch);
    }
    if (g_cluster == CLU_B || g_cluster == CLU_BOTH) {
        /* B is the player's own orientation, so this is the write that makes
           WALKING follow the head - which is the whole point of the owner's
           input design: forward is where you are looking. */
        SafeWrite(CamAddr(B_YAW),   g_refYawB + g_turnYaw + g_yawSign * g_pose.headYaw);
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

/* ---------------------------------------------------------------------------
 * HAND AIM - point the game's shot at the laser, render from the head
 *
 * The game computes its shot from the player's yaw/pitch. While the shoot
 * trigger is held we overwrite those fields with the direction from the eyes
 * to the dominant hand's laser endpoint. This runs during the input phase,
 * i.e. before the game's update/shot step. SteerTick runs again at
 * BeginScene and writes the HMD orientation back before the world is drawn,
 * so the shot follows the hand but the frame is still rendered from the
 * head's own 6DOF pose.
 * ------------------------------------------------------------------------- */

static long g_aimWrites = 0;

/* Manual right-hand pose. Only live while the "0" user-control mode is ON;
   in VR the XrApi pose always wins. */
static void ManualHandApply(void)
{
    const float cy = cosf(g_manualYaw), sy = sinf(g_manualYaw);
    const float cp = cosf(g_manualPitch), sp = sinf(g_manualPitch);
    const float cr = cosf(g_manualRoll),  sr = sinf(g_manualRoll);
    float fx, fy, fz, ux, uy, uz, cx, cy2, cz;

    g_pose.rP[0] = g_manualPos[0];
    g_pose.rP[1] = g_manualPos[1];
    g_pose.rP[2] = g_manualPos[2];
    /* XOPS axes: +X right, +Y up, +Z forward. Positive yaw turns right. */
    fx =  sy * cp;
    fy =  sp;
    fz =  cy * cp;
    ux = -sy * sp;
    uy =  cp;
    uz = -cy * sp;
    /* Roll = rotate the up vector around the forward axis (Rodrigues;
       up is perpendicular to forward, so the parallel term drops out). */
    cx = fy * uz - fz * uy;
    cy2 = fz * ux - fx * uz;
    cz = fx * uy - fy * ux;
    g_pose.rF[0] = fx;  g_pose.rF[1] = fy;  g_pose.rF[2] = fz;
    g_pose.rU[0] = ux * cr + cx * sr;
    g_pose.rU[1] = uy * cr + cy2 * sr;
    g_pose.rU[2] = uz * cr + cz * sr;
}

/* Default desktop-test pose: head at the origin, both hands out in front.
   Used when user-control mode is enabled without any XrApi stream. */
static void ManualDefaultPose(void)
{
    ZeroMemory(&g_pose, sizeof(g_pose));
    g_pose.valid = TRUE;
    g_pose.headF[2] = 1.0f;
    g_pose.headU[1] = 1.0f;

    g_pose.lP[0] = -0.22f; g_pose.lP[1] = -0.16f; g_pose.lP[2] = 0.45f;
    g_pose.lF[2] = 1.0f;
    g_pose.lU[1] = 1.0f;

    g_pose.rP[0] =  0.22f; g_pose.rP[1] = -0.16f; g_pose.rP[2] = 0.45f;
    g_pose.rF[2] = 1.0f;
    g_pose.rU[1] = 1.0f;

    g_manualPos[0] = 0.22f;
    g_manualPos[1] = -0.16f;
    g_manualPos[2] = 0.45f;
    g_manualYaw = 0.0f;
    g_manualPitch = 0.0f;
    g_manualRoll = 0.0f;

    g_posed = TRUE;
    ManualHandApply();
}

/* Copy the live XrApi right-hand pose into the manual values. */
static void ManualHandSync(void)
{
    const float fx = g_pose.rF[0], fy = g_pose.rF[1], fz = g_pose.rF[2];
    float baseUp[3], liveUp[3], cross[3], dot;
    float cy, sy, cp, sp;

    g_manualPos[0] = g_pose.rP[0];
    g_manualPos[1] = g_pose.rP[1];
    g_manualPos[2] = g_pose.rP[2];

    if (fx*fx + fy*fy + fz*fz < 1e-6f) return;
    g_manualYaw   = atan2f(fx, fz);          /* XOPS: +X is right */
    g_manualPitch = asinf(fy < -1.0f ? -1.0f : fy > 1.0f ? 1.0f : fy);
    g_manualRoll  = 0.0f;

    /* Recover any roll the controller already had, so the hand does not snap
       upright on the first frame of user mode. */
    cy = cosf(g_manualYaw);  sy = sinf(g_manualYaw);
    cp = cosf(g_manualPitch); sp = sinf(g_manualPitch);
    baseUp[0] = -sy * sp;
    baseUp[1] =  cp;
    baseUp[2] = -cy * sp;
    liveUp[0] = g_pose.rU[0];
    liveUp[1] = g_pose.rU[1];
    liveUp[2] = g_pose.rU[2];
    cross[0] = baseUp[1]*liveUp[2] - baseUp[2]*liveUp[1];
    cross[1] = baseUp[2]*liveUp[0] - baseUp[0]*liveUp[2];
    cross[2] = baseUp[0]*liveUp[1] - baseUp[1]*liveUp[0];
    dot = baseUp[0]*liveUp[0] + baseUp[1]*liveUp[1] + baseUp[2]*liveUp[2];
    g_manualRoll = atan2f(cross[0]*fx + cross[1]*fy + cross[2]*fz, dot);
}

/* Numpad test drive, "0" user-control mode only:
     no E              hold E
     8  pitch up       8  move up   (+Y)
     5  pitch down     5  move down (-Y)
     4  yaw left       4  move left (-X)
     6  yaw right      6  move right(+X)
     7  roll anticlock 7  move back (+Z)
     9  roll clockwise 9  move fwd  (-Z)
   E is used instead of Shift because Shift is bound in the game (the
   Shift+numpad combination was reaching XOPS and showing its own model).
   The numpad keys are blocked from the game whenever user control is ON. */
static void ManualHandKeys(PBYTE p)
{
    const float dt = 1.0f / 60.0f;
    const float angRate = 0.7853982f;   /* 45 deg/s */
    const float moveRate = 0.35f;       /* m/s */
    const BOOL move = (p['E'] & 0x80) ? TRUE : FALSE;
    const BOOL adj  = (p['Q'] & 0x80) ? TRUE : FALSE;
    const BOOL k8 = (p[VK_NUMPAD8] & 0x80) ? TRUE : FALSE;
    const BOOL k5 = (p[VK_NUMPAD5] & 0x80) ? TRUE : FALSE;
    const BOOL k4 = (p[VK_NUMPAD4] & 0x80) ? TRUE : FALSE;
    const BOOL k6 = (p[VK_NUMPAD6] & 0x80) ? TRUE : FALSE;
    const BOOL k7 = (p[VK_NUMPAD7] & 0x80) ? TRUE : FALSE;
    const BOOL k9 = (p[VK_NUMPAD9] & 0x80) ? TRUE : FALSE;
    static long logCount = 0;

    if (g_inputMuted) return;
    if (!g_manualRight && !adj) return;

    /* Block the numpads so the game never sees them while user control is
       on, whether E is held or not. Read them into locals first. */
    p[VK_NUMPAD5] = 0; p[VK_NUMPAD7] = 0; p[VK_NUMPAD8] = 0;
    p[VK_NUMPAD9] = 0; p[VK_NUMPAD4] = 0; p[VK_NUMPAD6] = 0;

    /* Q + numpad slides the hand box only; the cloned gun stays put. */
    if (adj) {
        const float r = moveRate * dt;
        p['Q'] = 0;
        if (k8) g_handAdj[1] += r;
        if (k5) g_handAdj[1] -= r;
        if (k4) g_handAdj[0] -= r;
        if (k6) g_handAdj[0] += r;
        if (k9) g_handAdj[2] += r;
        if (k7) g_handAdj[2] -= r;
        if ((++logCount % 30) == 0)
            LogD("HAND     adj % .3f % .3f % .3f  (Q+numpad, T saves)",
                g_handAdj[0], g_handAdj[1], g_handAdj[2]);
        return;
    }

    if (!InGameplay()) return;

    if (k8) {
        if (move) g_manualPos[1] += moveRate * dt;
        else      g_manualPitch += angRate  * dt;
    }
    if (k5) {
        if (move) g_manualPos[1] -= moveRate * dt;
        else      g_manualPitch -= angRate  * dt;
    }
    if (k4) {
        if (move) g_manualPos[0] -= moveRate * dt;
        else      g_manualYaw   -= angRate  * dt;   /* left  */
    }
    if (k6) {
        if (move) g_manualPos[0] += moveRate * dt;
        else      g_manualYaw   += angRate  * dt;   /* right */
    }
    if (k7) {
        if (move) g_manualPos[2] -= moveRate * dt;   /* back  */
        else      g_manualRoll  += angRate  * dt;    /* roll A */
    }
    if (k9) {
        if (move) g_manualPos[2] += moveRate * dt;   /* fwd   */
        else      g_manualRoll  -= angRate  * dt;    /* roll C */
    }

    if (g_manualPitch >  1.4f) g_manualPitch =  1.4f;
    if (g_manualPitch < -1.4f) g_manualPitch = -1.4f;
    while (g_manualRoll >  3.14159265f) g_manualRoll -= 6.2831853f;
    while (g_manualRoll < -3.14159265f) g_manualRoll += 6.2831853f;

    ManualHandApply();
    if ((++logCount % 60) == 0)
        LogD("HAND     manual R pos % .3f % .3f % .3f"
            "  yaw % .3f pitch % .3f roll % .3f",
            g_manualPos[0], g_manualPos[1], g_manualPos[2],
            g_manualYaw, g_manualPitch, g_manualRoll);
}

static void SteerAimWrite(void)
{
    float vp[3], vf[3], vu[3], vr[3];
    float ovp[3], ovf[3], ovu[3], ovr[3];
    float dir[3], len;
    float yawBaseA, yawBaseB, pitchBase, yawOff, pitchOff;
    BOOL domRight;

    if (!g_steer) return;
    if (!g_posed || !g_pose.valid) return;
    if (!g_vrExclusive && !g_manualRight) return;
    if (!g_haveRef) SteerRecentre();

    domRight = (g_handedness == 0) ? TRUE : FALSE;
    if (domRight)
        HandViewPose(g_pose.rP, g_pose.rF, g_pose.rU, TRUE, g_handFlip,
                     vp, vf, vu, vr);
    else
        HandViewPose(g_pose.lP, g_pose.lF, g_pose.lU, FALSE, g_handFlip,
                     vp, vf, vu, vr);

    /* Two-handed aim: the shot follows the gun, which points from the
       dominant hand at the off hand (same basis the draw pass uses). */
    if (g_twoHanded) {
        if (domRight)
            HandViewPose(g_pose.lP, g_pose.lF, g_pose.lU, FALSE, FALSE,
                         ovp, ovf, ovu, ovr);
        else
            HandViewPose(g_pose.rP, g_pose.rF, g_pose.rU, TRUE, FALSE,
                         ovp, ovf, ovu, ovr);
        HandTwoHandedAim(ovp, vp, vf, vu, vr);
    }

    /* The laser runs 30 m along the hand's forward; aim at that endpoint,
       measured from the eyes at the view-space origin. */
    dir[0] = vp[0] + vf[0] * 30.0f;
    dir[1] = vp[1] + vf[1] * 30.0f;
    dir[2] = vp[2] + vf[2] * 30.0f;
    len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (len < 0.001f) return;
    dir[0] /= len; dir[1] /= len; dir[2] /= len;
    if (dir[2] <= 0.0f) return;        /* hand pointing behind the player */

    if (g_vrExclusive) {
        /* VR: the HMD orientation is the render pose; SteerTick writes it
           back at BeginScene. */
        yawBaseA  = g_refYaw  + g_turnYaw + g_yawSign * g_pose.headYaw;
        yawBaseB  = g_refYawB + g_turnYaw + g_yawSign * g_pose.headYaw;
        pitchBase = g_pitchSign * g_pose.headPitch;
    } else {
        /* User-control mode: the view is mouse-driven and there is no
           SteerTick, so remember it and restore it before the draw. */
        float ay, ap, by, bp;
        if (!SafeRead(CamAddr(A_YAW), &ay)) return;
        if (!SafeRead(CamAddr(A_PITCH), &ap)) return;
        yawBaseA  = ay;
        pitchBase = ap;
        yawBaseB  = ay;
        g_aimSavedA[0] = ay;
        g_aimSavedA[1] = ap;
        g_aimSavedBok = (SafeRead(CamAddr(B_YAW),   &by) &&
                         SafeRead(CamAddr(B_PITCH), &bp)) ? TRUE : FALSE;
        if (g_aimSavedBok) {
            g_aimSavedB[0] = by;
            g_aimSavedB[1] = bp;
            yawBaseB = by;
        }
        g_aimRestore = TRUE;
    }
    yawOff    = g_yawSign * atan2f(dir[0], dir[2]);
    pitchOff  = g_pitchSign * asinf(dir[1] < -1.0f ? -1.0f
                                  : dir[1] >  1.0f ?  1.0f : dir[1]);

    if (g_cluster == CLU_A || g_cluster == CLU_BOTH) {
        SafeWrite(CamAddr(A_YAW),   yawBaseA + yawOff);
        SafeWrite(CamAddr(A_PITCH), pitchBase + pitchOff);
    }
    if (g_cluster == CLU_B || g_cluster == CLU_BOTH) {
        SafeWrite(CamAddr(B_YAW),   yawBaseB + yawOff);
        SafeWrite(CamAddr(B_PITCH), pitchBase + pitchOff);
    }
    ++g_aimWrites;
    if (g_aimWrites <= 3 || (g_aimWrites % 60) == 0)
        LogD("AIM      shot -> hand laser: yaw %+ .4f rad (%+ .1f deg)"
            "  pitch %+ .4f (%+ .1f deg)  [writes %ld]",
            yawOff, yawOff * 57.2957795f,
            pitchOff, pitchOff * 57.2957795f, g_aimWrites);
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
static long g_lastUP      = 0;       /* DrawPrimitiveUP calls, previous frame */

/* Draw-count fallback only. The real detector now uses the game's state
   globals; the threshold is set to clear the in-mission menu (~28 indexed)
   while still accepting small levels (DUEL_EXT was 39-53). */
#define GAMEPLAY_MIN_INDEXED 30

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
    /* The game's own state, found by following its UI strings:
         0x490A44 = selected mission index, 0x40 (64) on the title
         0x490A48 = mission result, 1 = success, 2 = failure (result screen)
       Both are fixed addresses in this binary (ASLR off) and rebased here.
       Fall back to the old draw signature only if they cannot be read. */
    if (g_exeBase) {
        int mission = -1, result = -1, player = -1;
        const UINT_PTR b = g_exeBase;
        if (SafeReadInt(b + (0x490A44 - 0x400000), &mission) &&
            SafeReadInt(b + (0x490A48 - 0x400000), &result) &&
            SafeReadInt(b + (0x48DDD8 - 0x400000), &player)) {
            if (mission == 0x40) return FALSE;   /* title */
            if (result != 0)     return FALSE;   /* death / victory / result */
            if (mission < 0)     return FALSE;   /* no mission loaded */
            /* 0x48DDD8 is the local player index: -1 on the title, mission
               select and briefing; 0..0x17 while a player exists. This is
               what separated the 48-indexed mission-select screen from real
               gameplay in the 5 Nov log. */
            if (player < 0)      return FALSE;
            /* In-mission menu is ~28 indexed; the smallest real level seen
               (DUEL_EXT) was 39-53. Briefing/2D screens are 0. */
            return g_lastIndexed >= GAMEPLAY_MIN_INDEXED;
        }
    }
    return g_lastXforms == 0 && g_lastIndexed >= GAMEPLAY_MIN_INDEXED;
}

/* Haptics are driven by the magazine counter falling. Both slots are watched
   because the player can switch weapons, but only the firing slot's count can
   drop during play. Counts and the game state are re-read every frame, not
   just when the custom HUD redraws, so a disabled HUD still buzzes.

   The pulse is decayed BEFORE it is sent, which is the SHOGO MAD WXR order: a
   shot sets 2, that frame sends 1 and the next sends 0 (which the runtime
   ignores). One frame of vibration per shot, not a string of them. */
static void HapticTick(BOOL inGameplay, float* outL, float* outR)
{
    HudVals v;
    int fires = 0, dur;

    if (!outL || !outR) return;
    *outL = 0.0f;
    *outR = 0.0f;

    if (!g_haptics) {
        g_hapticL = g_hapticR = 0;
        g_hapPrevCur0 = g_hapPrevCur1 = -1;
        return;
    }

    /* On any screen that is not gameplay, forget the counters. A level change,
       respawn or the death screen can reset a magazine, and that must not read
       as a burst of fire on the first gameplay frame. A buzz already running
       is allowed to finish its short countdown. */
    if (!inGameplay || !ReadHudVals(&v)) {
        g_hapPrevCur0 = g_hapPrevCur1 = -1;
        if (g_hapticL > 0) --g_hapticL;
        if (g_hapticR > 0) --g_hapticR;
        *outL = (float)g_hapticL;
        *outR = (float)g_hapticR;
        return;
    }

    if (g_hapPrevCur0 >= 0 && g_hapPrevCur1 >= 0) {
        const int d0 = g_hapPrevCur0 - v.cur0;
        const int d1 = g_hapPrevCur1 - v.cur1;
        fires = d0 > d1 ? d0 : d1;
        if (fires < 0) fires = 0;
        /* A reload, pickup or a drop can move a count backwards by more than a
           magazine. That is bookkeeping, not fire. */
        if (fires > HAPTIC_MAX_ROUNDS) {
            LogD("HAPTIC   ignored ammo drop of %d (reload/pickup/respawn)", fires);
            fires = 0;
        }
    }
    g_hapPrevCur0 = v.cur0;
    g_hapPrevCur1 = v.cur1;

    if (fires > 0) {
        BOOL buzzL = FALSE, buzzR = FALSE;

        /* One pulse per shot, matching SHOGO MAD WXR: the duration is fixed
           and small (2 frames is a short kick). Automatic fire feels stronger
           because the pulses come faster, not because each one is longer. */
        dur = g_hapticFrames;
        if (dur < 1) dur = 1;
        if (dur > HAPTIC_MAX_FRAMES) dur = HAPTIC_MAX_FRAMES;

        switch (g_twoHanded ? 1 : g_hapticHand) {
        case 1:  buzzL = buzzR = TRUE; break;             /* both */
        case 2:  buzzL = TRUE; break;                     /* left */
        case 3:  buzzR = TRUE; break;                     /* right */
        default: if (g_leftHanded) buzzL = TRUE;          /* dominant */
                 else             buzzR = TRUE;
                 break;
        }
        if (buzzL) g_hapticL = dur;
        if (buzzR) g_hapticR = dur;

        ++g_hapShots;
        LogD("HAPTIC   fires=%d dur=%d frames  hand=%s  slot %d weapon %d  ammo %d/%d"
            "  (shots %ld)",
            fires, dur, buzzL && buzzR ? "both" : buzzL ? "left" : "right",
            v.sel, v.sel ? v.w1 : v.w0, v.cur0, v.cur1, g_hapShots);
    }

    if (g_hapticL > 0) --g_hapticL;
    if (g_hapticR > 0) --g_hapticR;
    *outL = (float)g_hapticL;
    *outR = (float)g_hapticR;
}

/* Called from the GetKeyboardState hook AFTER our own control keys have been
   read out, and BEFORE the stick supplies movement - so the owner keeps his
   probe keys, the game loses everything, and VR input is then added back. */
static void VrExclusiveKeys(PBYTE p)
{
    int vk;
    /* Unfocused: input is already muted, and synthetic keys must not appear
       either. */
    if (g_inputMuted) return;
    /* Only gameplay is VR-exclusive. MODE_VR is 0 on the title, objective,
       menus, death and victory screens, and desktop input must always pass
       through there. */
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
    if (!InGameplay()) return;      /* never move the player in a menu/screen */
    if (!g_vrExclusive) return;     /* desktop test mode: no synthetic input */

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

/* Edge-press a game key for a few frames. The game polls GetKeyboardState
   once per frame, so a one-frame edge can be missed; three is reliable. */
static void VrPulse(int vk)
{
    if (vk >= 0 && vk < 256) g_pulseVk[vk] = 3;
}

static BOOL VrEdge(int idx)
{
    const BOOL now = XrButton(&g_pose, idx) ? TRUE : FALSE;
    const BOOL was = (idx >= 0 && idx < 24 && g_prevVrBtn[idx]) ? TRUE : FALSE;
    if (idx >= 0 && idx < 24) g_prevVrBtn[idx] = (BYTE)now;
    return now && !was;
}

/* The owner's mapping (6 Nov). Held: left stick move, right stick turn, left
   grip two-handed aim. Edge: left trigger fire mode, right trigger shoot,
   right grip jump, B reload, A switch, X drop, Y zoom. */
static void VrInputKeys(PBYTE p)
{
    int vk;
    g_twoHanded = FALSE;
    if (!g_posed || !g_pose.valid || !InGameplay()) return;
    if (!g_vrExclusive) return;     /* desktop test mode: no synthetic input */

    /* Left grip held = two-handed aim: the off hand supports the weapon, so
       the weapon's kick is felt in both controllers and the off-hand pose can
       follow the dominant hand. */
    g_twoHanded = XrButton(&g_pose, XRB_L_GRIP) ? TRUE : FALSE;

    if (VrEdge(XRB_Y)) {
        g_zoomOn = !g_zoomOn;
        VrPulse(VK_SHIFT);                                  /* zoom toggle */
        Log("HAND     zoom %s - laser pointer %s", g_zoomOn ? "ON" : "off",
            g_zoomOn ? "hidden" : "visible");
    }

    if (!g_leftHanded) {
        if (XrButton(&g_pose, XRB_R_TRIGGER)) {
            p[VK_LBUTTON] |= 0x80;
            SteerAimWrite();                                 /* shot -> laser */
        }
        if (VrEdge(XRB_L_TRIGGER)) VrPulse('X');             /* fire mode */
    } else {
        if (XrButton(&g_pose, XRB_L_TRIGGER)) {
            p[VK_LBUTTON] |= 0x80;
            SteerAimWrite();                                 /* shot -> laser */
        }
        if (VrEdge(XRB_R_TRIGGER)) VrPulse('X');             /* fire mode */
    }

    if (VrEdge(XRB_R_GRIP)) VrPulse(VK_SPACE);               /* jump */
    if (VrEdge(XRB_B))      VrPulse('R');                    /* reload */
    if (VrEdge(XRB_A))      VrPulse('Z');                    /* switch weapon */
    if (VrEdge(XRB_X))      VrPulse('G');                    /* drop weapon */

    for (vk = 0; vk < 256; ++vk)
        if (g_pulseVk[vk]) { p[vk] |= 0x80; --g_pulseVk[vk]; }
}

static void SteerLog(void)
{
    LogD("XR       packets=%ld  steering=%s  dof=%s", XrPackets(),
        g_steer ? "ON" : "off", DofName(g_dofMode));
    LogD("XR       cluster=%s", ClusterName(g_cluster));
    if (!XrRunning()) {
        Log("XR       NOT RUNNING: %s", XrLastError()[0] ? XrLastError() : "(no error)");
        return;
    }
    LogD("XR       handshake dir %s   hmd %s %s", XrDir(), XrMake(), XrModel());
    if (XrPackets() == 0) {
        LogD("XR       no packets yet. Start the XrAPI UDP Tester and point it");
        LogD("XR       at 127.0.0.1:7872. Nothing else here can work until then.");
        return;
    }
    LogD("XR       raw: %s", XrRawPacket());
    if (g_pose.valid) {
        LogD("XR       head yaw % .4f rad (% .1f deg)  pitch % .4f (% .1f deg)"
            "  sync %.1f",
            g_pose.headYaw, g_pose.headYaw * 57.2957795f,
            g_pose.headPitch, g_pose.headPitch * 57.2957795f, g_pose.sync);
        LogD("XR       head pos  % .3f % .3f % .3f m   ipd %.3f  fov %.1f x %.1f",
            g_pose.headP[0], g_pose.headP[1], g_pose.headP[2],
            g_pose.ipd, g_pose.fovx, g_pose.fovy);
        LogD("HAND     raw L % .3f % .3f % .3f   R % .3f % .3f % .3f",
            g_pose.lP[0], g_pose.lP[1], g_pose.lP[2],
            g_pose.rP[0], g_pose.rP[1], g_pose.rP[2]);
        LogD("XR       signs: yaw %+.0f  pitch %+.0f    writes so far %ld",
            g_yawSign, g_pitchSign, g_steerWrites);
        LogD("XR       sticks: move % .2f,% .2f  turn % .2f,% .2f   bools %s",
            g_pose.moveX, g_pose.moveY, g_pose.turnX, g_pose.turnY,
            g_pose.bools[0] ? g_pose.bools : "(none)");
        LogD("XR       stick->WASD %s, supplied keys on %ld frames",
            g_stickMove ? "on" : "OFF", g_stickFrames);
        LogD("INPUT    hand=%s turn=%.0f deg/s  Lgrip=two-hand Ltrig=%s"
            " Rtrig=%s Rgrip=jump B=reload A=switch X=drop Y=zoom",
            g_leftHanded ? "left" : "right", g_turnSpeed,
            g_leftHanded ? "shoot" : "fire mode",
            g_leftHanded ? "fire mode" : "shoot");
        LogD("INPUT    %s; last frame %s"
            " (%ld SetTransforms, %ld indexed, %ld UP draws)",
            g_vrExclusive ? "VR input only (0 for desktop)"
                          : "desktop input enabled",
            InGameplay() ? "GAMEPLAY" : "MENU/title",
            g_lastXforms, g_lastIndexed, g_lastUP);
        /* Run 11's tester sent a fixed HMD position in floats 22-24, so the
           recentred offset was 0.000 every packet. Positional DOF cannot be
           judged against that, and a silent zero looks like broken code. */
        if (g_dofMode >= DOF_HEIGHT
            && g_pose.headP[0] == 0.0f && g_pose.headP[1] == 0.0f
            && g_pose.headP[2] == 0.0f)
            LogD("XR       NOTE: head position is exactly 0,0,0 - the sender is"
                " not varying it, so positional DOF cannot be seen from here.");
        /* Run 12's symptom: position arriving every frame, DOF left at
           yaw+pitch, and so nothing moving. Silence there reads as a bug in
           the camera work, which it is not. */
        if (g_yClamped > 20)
            LogD("XR       NOTE: height writes are being undone - %d frames in a"
                " row. XOPS re-clamps the player to the ground, so crouching"
                " cannot be driven by writing Y. Horizontal 6DOF is"
                " unaffected.", g_yClamped);
        if (g_dofMode == DOF_LOOK
            && (g_pose.headP[0] != 0.0f || g_pose.headP[1] != 0.0f
                || g_pose.headP[2] != 0.0f))
            LogD("XR       NOTE: head POSITION is arriving but the DOF mode is"
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
    if (g_probeN == 0) { LogD("PROBE    no candidates - run the walk/stop scan first"); return; }

    /* Skip anything that has died since the list was built, rather than
       faulting on it. */
    while (tried < g_probeN) {
        while (idx < 0) idx += g_probeN;
        idx %= g_probeN;
        if (AddrWritable(g_probeList[idx]) &&
            SafeRead(g_probeList[idx], &g_probeOrig)) break;
        LogD("PROBE    [%d/%d] 0x%08IX is gone - skipping", idx + 1, g_probeN, g_probeList[idx]);
        ++idx; ++tried;
    }
    if (tried >= g_probeN) { LogD("PROBE    no addressable candidates left"); g_probeAddr = 0; return; }

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
        LogD("PROBE    this one has the XYZ shape (%s) - a strong candidate",
            g_probeShape[idx] == 2 ? "X, constant Y at +4" : "Z, constant Y at -4");
    if (g_probeWhat[idx])
        LogD("PROBE    >>> %s", g_probeWhat[idx]);
    LogD("PROBE    [%d/%d] 0x%08IX%s score=%d moves=%d orig=% .4f range % .4f..% .4f swing=%.4f x%.2f",
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
        LogD("PROBE    write faulted at 0x%08IX - dropping it", g_probeAddr);
        g_probeAddr = 0;
        if (++faults > 8) { g_probeMode = FALSE; LogD("PROBE    too many faults - mode OFF"); }
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
    LogD("");
    LogD("TAG #%d   index [%d] of %d   0x%08IX%s",
        g_tagCount, g_probeIdx + 1, g_probeN, g_probeAddr, ida);
    LogD("         %s", g_probeWhat[g_probeIdx] ? g_probeWhat[g_probeIdx] : "(unlabelled)");
    if (g_probeShape[g_probeIdx])
        LogD("         shape=%d  constant neighbour=% .4f",
            g_probeShape[g_probeIdx], g_probeHeight[g_probeIdx]);
    LogD("         write phase %s", PhaseName(g_probePhase));
    LogD("         orig=% .4f  observed range % .4f .. % .4f  swing=%.4f x%.2f",
        g_probeOrig, g_probeMinV[g_probeIdx], g_probeMaxV[g_probeIdx],
        g_probeSwing, g_probeAmp);
    LogD("");
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
        LogD("PROBE    nothing selected - press 9 then 8 to step to a variable");
        return;
    }
    g_probeClass[g_probeIdx] = kind;
    ++g_tagCount;

    ida[0] = 0;
    if (g_probeAddr >= exe && g_probeAddr < exe + 0x100000)
        _snprintf(ida, sizeof(ida), " IDA 0x%08IX", g_probeAddr - exe + 0x400000);

    LogD("");
    LogD("TAG #%d   %s", g_tagCount, ClassName(kind));
    LogD("         index [%d] of %d   0x%08IX%s", g_probeIdx + 1, g_probeN, g_probeAddr, ida);
    LogD("         %s", g_probeWhat[g_probeIdx] ? g_probeWhat[g_probeIdx] : "(unlabelled)");
    LogD("         orig=% .4f  swing=%.4f x%.2f  written at %s",
        g_probeOrig, g_probeSwing, g_probeAmp, PhaseName(g_probePhase));

    /* For a translation the owner can only report what he saw relative to
       himself - "it slid me sideways" - so record the yaw at that moment.
       front/back at yaw 0 and front/back at yaw 90 are different world axes,
       and this is what lets us resolve which. */
    haveYaw = SafeRead(WatchAddr(3), &yaw);
    if ((kind == 'O' || kind == 'P') && haveYaw)
        LogD("         player yaw at this moment % .4f rad = %.1f deg"
            "  (needed to resolve which world axis)", yaw, yaw * 57.2957795f);
    LogD("");
}

static void ProbeKeys(PBYTE p)
{
    /* Edge-triggered: act on the transition from up to down. */
    static BYTE prev[256];
    int vk;
    /* Only 0 remains from the old number-key set. 1-9 and the numpad are
       deliberately passed through to the game. The letters/punctuation below
       are the VR/debug controls. */
    const int watch[] = {
        '0', 'F', 'M', 'H', 'V', 'T', 'N', 'J', 'K',
        VK_OEM_4, VK_OEM_6, VK_OEM_1, VK_OEM_7, VK_OEM_5, VK_OEM_3,
        VK_OEM_PERIOD
    };
    const int watchN = (int)(sizeof(watch) / sizeof(watch[0]));
    BYTE now[256];
    int i;

    /* Debug keys only belong to the game while it owns the input. When the
       window is unfocused hkGetKeyboardState has already emptied p; keep the
       edge table in step so a key held across the focus change is not a new
       press when focus returns. */
    if (g_inputMuted) {
        for (i = 0; i < watchN; ++i)
            prev[watch[i]] = (BYTE)(p[watch[i]] & 0x80);
        return;
    }

    for (i = 0; i < watchN; ++i) {
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
                    LogD("PROBE    rescan found nothing - keeping the watch list.");
                    BuildWatchList();
                }
                ProbeSelect(0);
            } else if (vk == '2') {
                ProbeCyclePhase();
            } else if (vk == 'F') {
                g_fitWindow = !g_fitWindow;
                Log("WINDOW  fit-to-display: %s",
                    g_fitWindow
                      ? "ON - borderless, full display, re-applied every frame"
                      : "off - no longer re-applied (the window is not restored)");
                if (g_fitWindow) FitWindow(g_gameWnd ? g_gameWnd : g_wnd);
            } else if (vk == 'M') {
                g_maxRes = !g_maxRes;
                Log("RESOLUTION target-res composite: %s  (applies at next Reset)",
                    g_maxRes ? "ON - device asked for the desktop size"
                             : "off - the game's own 640x480 mode");
            } else if (vk == 'J') {
                /* Hunt mode: stop attaching, hide the previous draw. */
                g_3dManual = TRUE;
                g_3dAttach = FALSE;
                RecaptureWeapon();
                if (g_3dPrevN > 0) {
                    g_3dSel = (g_3dSel <= 0 ? g_3dPrevN - 1 : g_3dSel - 1);
                    Log("HIDE     draw -> %d of %d  (J prev, K next, N attach)  weapon '%s'",
                        g_3dSel, g_3dPrevN, ActiveWeaponName());
                }
            } else if (vk == 'K' && !g_probeMode) {
                /* Hunt mode: stop attaching, hide the next draw. */
                g_3dManual = TRUE;
                g_3dAttach = FALSE;
                RecaptureWeapon();
                if (g_3dPrevN > 0) {
                    g_3dSel = (g_3dSel + 1) % g_3dPrevN;
                    Log("HIDE     draw -> %d of %d  (J prev, K next, N attach)  weapon '%s'",
                        g_3dSel, g_3dPrevN, ActiveWeaponName());
                }
            } else if (vk == 'H') {
                static const float kHudScales[] = { 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 0.9f };
                static int hudIdx = 1;   /* default is 1.1 */
                hudIdx = (hudIdx + 1) % (int)(sizeof(kHudScales)/sizeof(kHudScales[0]));
                g_hudVisualScale = kHudScales[hudIdx];
                Log("HUD      visual scale -> %.2f; 1.00 = native size", g_hudVisualScale);
                SteerSave();
            } else if (vk == 'V') {
                g_dumpDraws = 300;
                LogD("DRAWDump armed: next 300 draws, with the first UP vertices");
                PlayerDump("V");
            } else if (vk == 'T') {
                /* Save the current hand-vs-gun offset for this weapon. */
                {
                    int id = -1;
                    HudVals hv;
                    if (ReadHudVals(&hv)) id = hv.sel ? hv.w1 : hv.w0;
                    if (id >= 0 && id < 32) {
                        g_handAdjTable[id][0] = g_handAdj[0];
                        g_handAdjTable[id][1] = g_handAdj[1];
                        g_handAdjTable[id][2] = g_handAdj[2];
                    }
                    Log("HAND     saved adj '%s' (id %d): % .3f % .3f % .3f",
                        ActiveWeaponName(), id,
                        g_handAdj[0], g_handAdj[1], g_handAdj[2]);
                }
                Log("STATE TAG  xforms=%ld indexed=%ld UP=%ld  -> %s",
                    g_lastXforms, g_lastIndexed, g_lastUP,
                    InGameplay() ? "GAMEPLAY" : "NOT-GAMEPLAY");
                StateLog("T");
            } else if (vk == 'N') {
                /* Attach and capture this draw. */
                g_3dManual = TRUE;
                g_3dAttach = TRUE;
                RecaptureWeapon();
                Log("HIDE     ATTACH draw %d of %d  weapon '%s'",
                    g_3dSel, g_3dPrevN, ActiveWeaponName());
            } else if (vk == 'H') {
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
            } else if (vk == '0' && !g_probeMode) {
                g_vrExclusive = !g_vrExclusive;
                if (g_vrExclusive) {
                    SteerRecentre();
                    /* Back to VR: the XrApi pose owns the hand again. */
                    if (g_manualRight) {
                        g_manualRight = FALSE;
                        Log("HAND     user control OFF - XrApi pose restored");
                    }
                } else {
                    /* User control just turned ON: default both hands in
                       front of the player so no XrApi stream is needed. */
                    ManualDefaultPose();
                    g_manualRight = TRUE;
                    {
                        HWND w = g_gameWnd ? g_gameWnd : g_wnd;
                        RECT rc;
                        POINT pt;
                        if (w && GetClientRect(w, &rc)) {
                            pt.x = (rc.right - rc.left) / 2;
                            pt.y = (rc.bottom - rc.top) / 2;
                            if (ClientToScreen(w, &pt)) {
                                SetCursorPos(pt.x, pt.y);
                                g_lastCursor = pt;
                                g_cursorKnown = TRUE;
                            }
                        }
                    }
                    Log("HAND     user control ON (numpad drives the right hand;"
                        " hold E to move; cursor centred once; incoming 6DOF ignored)");
                }
                Log("INPUT    %s",
                    g_vrExclusive
                      ? "VR input only - desktop keys/mouse suppressed in"
                        " GAMEPLAY; MODE_VR 0 screens always keep desktop input"
                      : "desktop keyboard/mouse enabled for manual tests;"
                        " VR steering and synthetic input are paused");
                Log("INPUT    (last frame: %ld SetTransform, %ld indexed, %ld UP"
                    " draws -> %s)", g_lastXforms, g_lastIndexed, g_lastUP,
                    InGameplay() ? "GAMEPLAY" : "a MENU or the title");
            } else if (vk == '1') {
                g_stickMove = !g_stickMove;
                Log("STICK    left stick -> W/A/S/D: %s",
                    g_stickMove ? "ON (only while steering)" : "off");
            } else if (vk == VK_OEM_PERIOD) {
                /* '.' grabs the next composited frame, then the one after it
                   (a stereo pair under AER) - the only reliable way to inspect
                   the D3D output on this box. */
                g_grabFrames = 1;
                g_grabPair = TRUE;
                Log("DUMP     armed: next two composited frames -> xops_frame_<n>.bmp");
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
                    LogD("PROBE    (NumLock on). 8=next  7=prev  0=tag  -/+=swing  9=off");
                    LogD("PROBE    2=change WHEN we write (phase)");
                    LogD("PROBE    5=dump the watch list   4=rescan for new candidates");
                    LogD("PROBE    while a sine wave runs, say what you SEE it do:");
                    LogD("PROBE      U=TILTED my view up/down (horizon rotated)");
                    LogD("PROBE      K=LIFTED me up/down (vertical, horizon level)");
                    LogD("PROBE      L=turned me left/right");
                    LogD("PROBE      O=slid me front/back   P=slid me left/right");
                    LogD("PROBE    6=arm the ballistic window (height cheat + gravity)");
                    LogD("PROBE    (W/A/S/D and the mouse still work normally)");
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
                else if (vk == VK_OEM_MINUS) { g_probeAmp *= 0.5f; LogD("PROBE    swing x%.4f", g_probeAmp); }
                else if (vk == VK_OEM_PLUS)  { g_probeAmp *= 2.0f; LogD("PROBE    swing x%.4f", g_probeAmp); }
            }
        }
        prev[vk] = now[vk];
    }

    /* Swallow only the remaining VR/debug controls. Number keys 1-9 and the
       numpad are deliberately passed through to the game. */
    p['0'] = 0;             /* desktop-input toggle */
    p[VK_OEM_4] = 0; p[VK_OEM_6] = 0; p[VK_OEM_1] = 0; p[VK_OEM_7] = 0;
    p[VK_OEM_5] = 0; p[VK_OEM_3] = 0;
    p[VK_OEM_PERIOD] = 0;
    /* Stage-3 comparison switch, always live so it works before probe mode. */
    p['F'] = 0;
    /* Fullscreen-resolution experiment; applies at the next Reset. */
    p['M'] = 0;
    /* Back-buffer BMP grab. */
    p['J'] = 0;
    /* Custom-HUD comparison toggle (K still classifies in probe mode). */
    p['K'] = 0;
    /* HUD scale cycle. */
    p['H'] = 0;
    /* Draw-order dump for HUD identification. */
    p['V'] = 0;
    /* Manual state tag (press on death/victory screens). */
    p['T'] = 0;
    /* Handedness toggle. */
    p['N'] = 0;
}

// ---------------------------------------------------------------------------
// Init - deliberately NOT in DllMain. LoadLibrary under the loader lock is a
// deadlock risk on Windows and a worse one under Wine, so everything happens on
// the first call into an export instead.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Projection aspect
//
// XOPS hardcodes 4:3 when it builds the perspective matrix:
//
//     00403A91  68 AB AA AA 3F   push 1.333333
//
// Patching that immediate to the window/container aspect widens the rendered
// horizontal FOV. The 640x480 frame becomes anamorphic: when it is stretched
// to the target window, the output pixels are square again and the geometry
// is correct, with no letterbox and no squashed world. The outbound FOV is
// recomputed to match, so the runtime warps the image consistently.
// ---------------------------------------------------------------------------
static void PatchProjectionAspect(void)
{
    BYTE* p;
    DWORD old = 0;
    float aspect, oldAspect;

    if (!g_exeBase || g_desktopW < 1 || g_desktopH < 1) return;
    p = (BYTE*)g_exeBase + (0x403A92 - 0x400000);
    if (*(DWORD*)p != 0x3FAAAAAB) {
        Log("PROJECTION aspect patch: expected 0x3FAAAAAB, found 0x%08lX at"
            " IDA 0x403A92 - skipped", *(DWORD*)p);
        return;
    }
    aspect = (float)g_desktopW / (float)g_desktopH;
    if (aspect < 1.0f) aspect = 1.0f;
    if (aspect > 2.4f) aspect = 2.4f;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("PROJECTION aspect patch: VirtualProtect failed (err %lu)",
            GetLastError());
        return;
    }
    oldAspect = *(float*)p;
    *(float*)p = aspect;
    VirtualProtect(p, 4, old, &old);
    g_projPatched = TRUE;

    {
        const float pi = 3.14159265358979323846f;
        const float halfY = XOPS_FOV_Y * pi / 360.0f;
        g_outFovY = XOPS_FOV_Y;
        g_outFovX = 2.0f * atanf(tanf(halfY) * aspect) * 180.0f / pi;
    }
    Log("PROJECTION aspect patch: %.4f -> %.4f at IDA 0x403A92;"
        " outbound FOV %.2f x %.2f",
        oldAspect, aspect, g_outFovX, g_outFovY);
}

// ---------------------------------------------------------------------------
// Virtual hand / laser config
//
// `config.txt` lives one directory above the game, so a game update does not
// overwrite it. If it is missing, it is created with the defaults.
//
//     handedness = right | left
//     hand_flip  = false | true
//
// Quest 3 hand data arrives 180 degrees rolled relative to the other headsets
// (HWXR, Shogo WXR and IMVR all hit this). The default therefore APPLIES the
// roll correction: hand_flip = false is the Quest 3 layout. Set hand_flip =
// true to recreate the old, uncorrected gun roll on devices other than a
// Quest 3. Only the dominant hand/gun is rolled, not pitch or yaw.
// ---------------------------------------------------------------------------
static char* HandTrim(char* s)
{
    char* e;
    while (*s && isspace((unsigned char)*s)) ++s;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = 0;
    return s;
}

static void HandLoadConfig(void)
{
    char parent[MAX_PATH], path[MAX_PATH], alt[MAX_PATH];
    char line[256];
    FILE* f;
    size_t n;
    char* slash;

    if (!g_baseDir[0]) return;

    strncpy(parent, g_baseDir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    n = strlen(parent);
    while (n && (parent[n-1] == '\\' || parent[n-1] == '/')) parent[--n] = 0;
    slash = strrchr(parent, '\\');
    if (slash && slash > parent + 2) *(slash + 1) = 0;
    else parent[0] = 0;

    if (parent[0]) _snprintf(path, sizeof(path), "%sconfig.txt", parent);
    else           _snprintf(path, sizeof(path), "config.txt");
    path[sizeof(path) - 1] = 0;
    _snprintf(alt, sizeof(alt), "%sconfig.txt", g_baseDir);
    alt[sizeof(alt) - 1] = 0;

    f = fopen(path, "r");
    if (!f) {
        /* Be forgiving if the owner put it next to the game instead. */
        f = fopen(alt, "r");
        if (f) _snprintf(path, sizeof(path), "%s", alt);
    }
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs("# XOPS-WXR virtual hand / laser\n", f);
            fputs("# handedness: right (default) or left\n", f);
            fputs("# hand_flip: false (default) = Quest 3 hand roll (corrected);\n", f);
            fputs("#            true = recreate the old/uncorrected roll on other devices\n", f);
            fputs("handedness = right\n", f);
            fputs("hand_flip = false\n", f);
            fputs("# haptics: buzz on firing, from the magazine counter\n", f);
            fputs("# haptics_hand: dominant (default), both, left or right\n", f);
            fputs("# haptic_frames: buzz length per shot, in frames (1..5, 2 is a short kick)\n", f);
            fputs("haptics = true\n", f);
            fputs("haptics_hand = dominant\n", f);
            fputs("haptic_frames = 2\n", f);
            fputs("# verbose_debug: true writes diagnostic detail (probes, dumps,\n", f);
            fputs("# raw packets). false (default) keeps errors and state changes only.\n", f);
            fputs("verbose_debug = false\n", f);
            fputs("# stereo: mono (default), sbs, or aer (experimental)\n", f);
            fputs("stereo = mono\n", f);
            fclose(f);
            Log("HAND     created %s with defaults", path);
        } else {
            Log("HAND     could not create %s (err %lu)", path, GetLastError());
        }
        g_handedness = 0;
        g_handFlip = TRUE;      /* Quest 3 roll correction applied by default */
        g_leftHanded = 0;
        g_haptics = TRUE;
        g_hapticHand = 0;
        g_hapticFrames = 2;
        g_twoHanded = FALSE;
        g_verboseDebug = FALSE;
        g_stereoMode = STEREO_MONO;
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char* key;
        char* val;
        char* eq;
        key = HandTrim(line);
        if (!*key || *key == '#' || *key == ';') continue;
        eq = strchr(key, '=');
        if (!eq) continue;
        *eq = 0;
        val = HandTrim(eq + 1);
        key = HandTrim(key);
        if (!_stricmp(key, "handedness")) {
            if (!_stricmp(val, "left") || !strcmp(val, "1"))
                g_handedness = 1;
            else if (!_stricmp(val, "right") || !strcmp(val, "0"))
                g_handedness = 0;
        } else if (!_stricmp(key, "hand_flip")) {
            /* Inverted on purpose: false (default) APPLIES the Quest 3 roll
               correction; true reproduces the old roll on other devices. */
            const BOOL oldRoll = (!_stricmp(val, "true") ||
                                  !_stricmp(val, "yes") ||
                                  !strcmp(val, "1")) ? TRUE : FALSE;
            g_handFlip = oldRoll ? FALSE : TRUE;
        } else if (!_stricmp(key, "haptics")) {
            g_haptics = (!_stricmp(val, "true") || !_stricmp(val, "yes") ||
                         !strcmp(val, "1")) ? TRUE : FALSE;
        } else if (!_stricmp(key, "haptics_hand")) {
            if (!_stricmp(val, "both") || !strcmp(val, "1"))      g_hapticHand = 1;
            else if (!_stricmp(val, "left") || !strcmp(val, "2"))  g_hapticHand = 2;
            else if (!_stricmp(val, "right") || !strcmp(val, "3")) g_hapticHand = 3;
            else                                                   g_hapticHand = 0;
        } else if (!_stricmp(key, "haptic_frames")) {
            g_hapticFrames = atoi(val);
            if (g_hapticFrames < 1)  g_hapticFrames = 1;
            if (g_hapticFrames > HAPTIC_MAX_FRAMES)
                g_hapticFrames = HAPTIC_MAX_FRAMES;
        } else if (!_stricmp(key, "verbose_debug")) {
            g_verboseDebug = (!_stricmp(val, "true") || !_stricmp(val, "yes") ||
                              !strcmp(val, "1")) ? TRUE : FALSE;
        } else if (!_stricmp(key, "stereo")) {
            if (!_stricmp(val, "sbs") || !strcmp(val, "1"))      g_stereoMode = STEREO_SBS;
            else if (!_stricmp(val, "aer") || !strcmp(val, "2")) g_stereoMode = STEREO_AER;
            else                                                 g_stereoMode = STEREO_MONO;
        }
    }
    fclose(f);

    /* Dominant hand also selects which trigger shoots. The in-session N key
       can still toggle this, but the config is the startup authority. */
    g_leftHanded = (g_handedness == 1) ? 1 : 0;
    Log("HAND     config %s  handedness=%s  hand_flip=%s  (roll: %s)", path,
        g_handedness ? "left" : "right",
        g_handFlip ? "false" : "true",
        g_handFlip ? "Quest 3 corrected" : "old / other device");
    Log("HAPTIC   %s  hand=%s  frames=%d", g_haptics ? "on" : "off",
        g_hapticHand == 1 ? "both" : g_hapticHand == 2 ? "left" :
        g_hapticHand == 3 ? "right" : "dominant", g_hapticFrames);
    Log("LOG      verbose_debug=%s", g_verboseDebug ? "true" : "false");
    Log("STEREO   %s", g_stereoMode == STEREO_SBS ? "SBS (experimental)"
                      : g_stereoMode == STEREO_AER ? "AER (experimental)"
                                                   : "mono");
}

static void EnsureInit(void)
{
    if (g_initDone) return;
    g_initDone = TRUE;

    char dir[MAX_PATH]; GetModuleFileNameA(g_hSelf, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0; else dir[0] = 0;
    _snprintf(g_baseDir, MAX_PATH, "%s", dir);
    g_baseDir[MAX_PATH - 1] = 0;

    char logPath[MAX_PATH];
    _snprintf(logPath, MAX_PATH, "%sxops_wxr_probe.log", dir);
    logPath[MAX_PATH - 1] = 0;
    g_log = fopen(logPath, "w");

    g_exeBase = (UINT_PTR)GetModuleHandleA(NULL);

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);

    Log("XOPS-WXR d3d8 probe  %s", kBuildStamp);      // line 1 is the stamp
    Log("host           %s", exe);
    Log("exe base       0x%08IX   (IDA addresses below assume 0x400000)", g_exeBase);
    /* Loaded before the diagnostic startup block so verbose_debug is known
       before anything is written through LogD(). */
    HandLoadConfig();
    Log("FONT  CreateFontIndirectA IAT patched: %s",
        g_realCreateFontIndirectA ? "yes" : "no");
    for (int fi = 0; fi < g_fontLogN; ++fi) LogFontRequest(&g_fontLog[fi]);
    g_fontLogN = 0;
    /* Capture the desktop mode BEFORE the game enters fullscreen at 640x480. */
    g_desktopW = GetSystemMetrics(SM_CXSCREEN);
    g_desktopH = GetSystemMetrics(SM_CYSCREEN);
    Log("screen         %dx%d", g_desktopW, g_desktopH);
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
    LogD("real d3d8      %s  Direct3DCreate8=%p", real, g_realCreate8);
    Log("");

    ResolveForwards();
    DumpImports();
    if (XrStart()) {
        Log("XR       client started. in 127.0.0.1:%d  out 127.0.0.1:%d", 7872, 7278);
        LogD("XR       handshake dir %s   hmd %s %s", XrDir(), XrMake(), XrModel());
        LogD("XR       the game is set to keep running while unfocused, so the");
        LogD("XR       tester can hold focus. Backslash toggles that.");
        LogD("XR       steering is ON by default; 3 toggles it for debugging,");
        LogD("XR       ; recentres, [ ] flip the signs, ' cycles DOF, ` cluster,");
        LogD("XR       1 toggles left stick W/A/S/D, N toggles handedness.");
        LogD("XR       0 enables desktop keyboard/mouse for manual tests;");
        LogD("XR       MODE_VR 0 screens (title/objective/menu/death/victory)");
        LogD("XR       always keep desktop input regardless of that toggle.");
        LogD("WINDOW   borderless full-display fit is ON - F toggles it.");
        LogD("SYNC     the red square is drawn last in the frame once a packet");
        LogD("SYNC     has arrived; its value is HMD_SYNC echoed verbatim.");
        LogD("CAMHUNT  differential scanner is OFF (camera already confirmed).");
        LogD("CAMHUNT  This removes the raw memory copy that crashed on 5 Nov.");
        LogD("RESOLUTION target-resolution composite is ON: the device asks for");
        LogD("RESOLUTION %dx%d and the 640x480 game frame is stretched to fill it.",
            g_desktopW, g_desktopH);
        LogD("RESOLUTION M toggles it; the replacement HUD is drawn as a D3D");
        LogD("RESOLUTION texture in the centre square. J saves a back-buffer BMP.");
    } else {
        Log("XR       client FAILED to start: %s", XrLastError());
    }
    SteerLoad();
    if (g_patchAspect) PatchProjectionAspect();
    else Log("PROJECTION aspect patch disabled by xops_wxr_steer.ini");
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
        LogD("CALLSITE  SetTransform(%s)  returns to IDA 0x%08IX  (frame %ld)",
            XformName(type), ida, g_frame);
    }
}

// ---------------------------------------------------------------------------
// Window: borderless, filling the display (stage 3)
//
// This is a prerequisite for the sync pixel, not cosmetics: WinlatorXR samples
// pixel (0,0) of the captured window, and with a title bar there that pixel is
// chrome. The game's back buffer stays 640x480 - the engine projects to that
// itself - but Direct3D8::Present stretches it to the client rect in windowed
// mode. That is the documented fallback: sync corner valid, sharpness lost.
//
// Re-applied every frame because engines rebuild their window on mode changes,
// alt-tab and device loss. g_fitWindow is a comparison switch (F), not an undo:
// turning it off stops re-applying, it does not restore the old chrome.
// ---------------------------------------------------------------------------

static int  g_fitChanges = 0;

static void FitWindow(HWND h)
{
    LONG style, want;
    int sw, sh;
    RECT wr;

    if (!g_fitWindow) return;
    if (!g_deviceWindowed) return;      /* fullscreen: the game owns the window */
    if (!h) h = g_gameWnd ? g_gameWnd : g_wnd;
    if (!h || !IsWindow(h)) return;

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    style = GetWindowLongA(h, GWL_STYLE);
    want  = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME))
            | WS_POPUP | WS_VISIBLE;

    GetWindowRect(h, &wr);
    if (style == want && wr.left == 0 && wr.top == 0 &&
        wr.right == sw && wr.bottom == sh)
        return;                     /* idempotent: nothing to do on a good frame */

    SetWindowLongA(h, GWL_STYLE, want);
    SetWindowPos(h, NULL, 0, 0, sw, sh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    ++g_fitChanges;
    if (g_fitChanges <= 6 || (g_fitChanges % 60) == 0)
        Log("WINDOW  fit-to-display #%d: style 0x%08lX -> 0x%08lX,"
            " %ldx%ld -> %dx%d, no chrome",
            g_fitChanges, style, GetWindowLongA(h, GWL_STYLE),
            wr.right - wr.left, wr.bottom - wr.top, sw, sh);
}

/* Ask the device for the desktop/container size instead of 640x480. Called
   before the real CreateDevice and before the real Reset. The desktop size was
   captured at init, before the game switched the display mode. This applies in
   windowed mode too: the window is made borderless and full-display, so a
   desktop-sized back buffer is what we want there as well. */
static void ApplyTargetRes(D3DPRESENT_PARAMETERS8* pp)
{
    if (!pp || !g_maxRes || g_desktopW < 640 || g_desktopH < 480)
        return;
    g_bbFormat = pp->BackBufferFormat ? pp->BackBufferFormat
                                      : D3DFMT_X8R8G8B8;
    if (pp->BackBufferWidth == (UINT)g_desktopW &&
        pp->BackBufferHeight == (UINT)g_desktopH) {
        g_bbW = (UINT)g_desktopW;
        g_bbH = (UINT)g_desktopH;
        return;
    }
    Log("RESOLUTION target-res override: %ux%u -> %dx%d (%s)",
        pp->BackBufferWidth, pp->BackBufferHeight, g_desktopW, g_desktopH,
        pp->Windowed ? "windowed" : "fullscreen");
    pp->BackBufferWidth  = (UINT)g_desktopW;
    pp->BackBufferHeight = (UINT)g_desktopH;
    g_bbW = (UINT)g_desktopW;
    g_bbH = (UINT)g_desktopH;
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
    return g_verboseDebug &&
           ((g_frame < VERBOSE_FRAMES) || (g_frame % VERBOSE_EVERY == 0));
}

/* Eye separation in metres. The runtime's IPD is authoritative; fall back to
   a Quest-class default if the packet carries nonsense. */
static float EyeIpd(void)
{
    const float v = g_pose.ipd;
    if (v > 0.01f && v < 0.12f) return v;
    return 0.064f;
}

static HRESULT __stdcall hkSetTransform(void* dev, DWORD type, const D3DMATRIX8* m)
{
    NoteCallSite(type, _ReturnAddress());
    ++g_xformThisFrame;
    if      (type == D3DTS_VIEW)       { ++g_xfView;  if (m) { g_lastView = *m; g_haveView = TRUE; g_lastViewAt = g_frame; } }
    else if (type == D3DTS_PROJECTION) { ++g_xfProj; if (m) { g_lastProj = *m; g_haveProj = TRUE; } }
    else if (type == 256)              ++g_xfWorld;
    else                               ++g_xfOther;

    if (Verbose() && (type == D3DTS_VIEW || type == D3DTS_PROJECTION)) {
        LogD("[f%ld] SetTransform(%s) from IDA 0x%08IX",
            g_frame, XformName(type), ToIda(_ReturnAddress()));
        LogMatrix(XformName(type), m);
        if (type == D3DTS_VIEW)       LogViewDecomposed(m);
        else                          LogProjDecomposed(m);
    }
    /* AER: render one eye per frame by shifting the camera +/- IPD/2 along
       its own right vector. In a row-vector view matrix m[3][0] is the
       camera-space translation along right, so moving the camera right by d
       subtracts d there. The projection and everything else stay untouched;
       the runtime holds the other eye's frame. */
    if (type == D3DTS_VIEW && m &&
        g_stereoMode == STEREO_AER && InGameplay()) {
        D3DMATRIX8 adj = *m;
        const float d = (g_eyeRight ? 0.5f : -0.5f) * EyeIpd();
        adj.m[3][0] -= d;
        if ((g_frame % 120) == 0)
            LogD("STEREO   view offset eye=%s d=%+.4f  m30 %+.4f -> %+.4f",
                 g_eyeRight ? "right" : "left", d, m->m[3][0], adj.m[3][0]);
        return o_SetTransform(dev, type, &adj);
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
            LogD("FVF       0x%04lX%s  (first seen frame %ld, from IDA 0x%08IX)",
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
    /* 3D HUD/inventory models arrive as stride-24 XYZ+DIFFUSE+TEX1 UP draws
       (the 2D HUD is stride 28). Keep them for the identification row. */
    if (prim == 4 && stride == 24 && count >= 8 && count <= 600)
        CaptureHudBatchUP(dev, data, count);

    /* Fallback if the 3D HUD model draws did not trigger the hand pass: the
       first 2D HUD draw is still after the world and before the HUD layer. */
    if (g_compositeActive && g_gameRTActive && !g_handDrawnThisFrame &&
        InGameplay() && g_curFvf == 0x0144 && stride == 28) {
        DrawHandAndLaserWorld(dev);
        g_handDrawnThisFrame = TRUE;
    }
    if (g_handDrawnThisFrame && HideThisDraw()) return 0;

    /* Crosshair: four separate prim=3 (LINESTRIP), count=1 UP draws at
       (317,240), (320,237), (323,240), (320,243). Remove it for VR; a
       controller laser pointer replaces it later. */
    if (g_curFvf == 0x0144 && stride == 28 && prim == 3 && count == 1 && data) {
        const float* v = (const float*)data;
        if (v && v[0] >= 310.0f && v[0] <= 330.0f &&
                 v[1] >= 230.0f && v[1] <= 250.0f) {
            static long crossSkipped = 0;
            if (crossSkipped++ == 0)
                Log("HUD      crosshair draws suppressed");
            return 0;                  /* S_OK, draw nothing */
        }
    }

    /* In gameplay the original 2D HUD is replaced by the texture HUD drawn in
       CompositeFrame(). Its 2D layer is all stride-28 UP; the 3D weapon-slot
       models are indexed draws and stay visible. Menus/death/victory are
       MODE_VR 0 and keep their own HUD. */
    if (g_customHud && g_compositeActive && g_hudValid && HudShouldDraw() &&
        g_curFvf == 0x0144 && stride == 28) {
        static long hud2d = 0;
        if (hud2d++ == 0)
            Log("HUD      game 2D HUD suppressed; custom HUD active");
        return 0;
    }
    if (g_frame < VERBOSE_FRAMES && g_drawsThisFrame <= 4)
        LogD("[f%ld] DrawPrimitiveUP  type=%lu count=%u stride=%u  from IDA 0x%08IX",
            g_frame, prim, count, stride, ToIda(_ReturnAddress()));
    if (g_dumpDraws > 0) {
        const float* v = (const float*)data;
        if (v && count > 0)
            LogD("DRAWDump UP   fvf=0x%04lX stride=%u count=%u prim=%lu"
                " v0=%.1f,%.1f,%.1f,%.1f v1=%.1f,%.1f",
                g_curFvf, stride, count, prim,
                v[0], v[1], v[2], v[3],
                (count > 1 && stride >= 8) ? v[stride/4] : 0.0f,
                (count > 1 && stride >= 8) ? v[stride/4 + 1] : 0.0f);
        else
            LogD("DRAWDump UP   fvf=0x%04lX stride=%u count=%u prim=%lu (no data)",
                g_curFvf, stride, count, prim);
        --g_dumpDraws;
    }

    /* HUD = XYZRHW + stride 28. The tricky part: DrawPrimitiveUP's `count` is
       the PRIMITIVE count, not the vertex count. A HUD triangle fan has
       prim=6, count=2 -> 4 vertices. Copying count*stride leaves the extra
       vertices uninitialized and tears the HUD across the screen (seen 5 Nov). */
    if (g_hudVisualScale != 1.0f && InGameplay() &&
        g_curFvf == 0x0144 && stride == 28 &&
        count > 0 && count < 100000 && data) {
        const UINT verts = PrimVertexCount(prim, count);
        const SIZE_T bytes = (SIZE_T)verts * stride;
        const float sx = g_hudVisualScale / (g_displayScaleX > 0.0f ? g_displayScaleX : 1.0f);
        const float sy = g_hudVisualScale / (g_displayScaleY > 0.0f ? g_displayScaleY : 1.0f);
        if (!g_hudBuf || bytes > g_hudCap) {
            if (g_hudBuf) { VirtualFree(g_hudBuf, 0, MEM_RELEASE); g_hudBuf = NULL; }
            g_hudBuf = (BYTE*)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
            g_hudCap = g_hudBuf ? bytes : 0;
        }
        if (g_hudBuf) {
            UINT i;
            memcpy(g_hudBuf, data, bytes);
            for (i = 0; i < verts; ++i) {
                float* p = (float*)(g_hudBuf + (SIZE_T)i * stride);
                p[0] = (p[0] - HUD_CX) * sx + HUD_CX + g_hudOffsetX;
                p[1] = (p[1] - HUD_CY) * sy + HUD_CY + g_hudOffsetY;
            }
            {
                static long hudDraws = 0;
                if (hudDraws++ == 0)
                    LogD("HUD      visual %.2f -> game scale %.3f,%.3f"
                        " (prim %lu, %u verts)", g_hudVisualScale,
                        sx, sy, prim, verts);
            }
            return o_DrawPrimitiveUP(dev, prim, count, g_hudBuf, stride);
        }
    }
    return o_DrawPrimitiveUP(dev, prim, count, data, stride);
}

// The world may not come through DrawPrimitiveUP at all. Two XYZ formats
// (0x0142, 0x0102) are set by the game, and XYZ geometry needs the device's
// matrices - so if it is drawn from a vertex buffer through DrawPrimitive or
// DrawIndexedPrimitive, an earlier version of this probe would never have seen
// it, and the conclusion "every draw is XYZRHW" would have been an artefact of
// hooking only one of the three entry points. Hook all three.
/* Vertex count from a D3D8 primitive type and PrimitiveCount. DrawPrimitiveUP
   takes a primitive count; the buffer holds the expanded vertex count. */
static UINT PrimVertexCount(DWORD prim, UINT primCount)
{
    switch (prim) {
        case 1: return primCount;          /* POINTLIST   */
        case 2: return primCount * 2;      /* LINELIST    */
        case 3: return primCount + 1;      /* LINESTRIP   */
        case 4: return primCount * 3;      /* TRIANGLELIST*/
        case 5: return primCount + 2;      /* TRIANGLESTRIP */
        case 6: return primCount + 2;      /* TRIANGLEFAN */
        default: return primCount;
    }
}

typedef HRESULT (__stdcall *PFN_VBLock)(void*, UINT, UINT, BYTE**, DWORD);
typedef HRESULT (__stdcall *PFN_VBUnlock)(void*);

/* Bounds of a DrawPrimitive vertex-buffer batch. The 2D HUD gun icon is in
   one of these, so this finds it by its screen position. */
static void DumpVBBounds(void* dev, UINT start, UINT count)
{
    void* vb = NULL;
    UINT stride = 0;
    BYTE* data = NULL;
    PFN_VBLock lock;
    PFN_VBUnlock unlock;
    float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
    UINT i, n;

    if (!o_GetStreamSource || count < 1) return;
    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return;
    if (stride < 8) { ReleaseIUnknown(vb); return; }
    lock   = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, start * stride, count * stride, &data, 0x10) != S_OK || !data) {
        if (lock(vb, 0, 0, &data, 0x10) != S_OK || !data) {
            ReleaseIUnknown(vb);
            return;
        }
        data += start * stride;
    }
    n = count > 600 ? 600 : count;
    for (i = 0; i < n; ++i) {
        const float* f = (const float*)(data + (SIZE_T)i * stride);
        if (f[0] < mnx) mnx = f[0];
        if (f[0] > mxx) mxx = f[0];
        if (f[1] < mny) mny = f[1];
        if (f[1] > mxy) mxy = f[1];
    }
    unlock(vb);
    ReleaseIUnknown(vb);
    {
        D3DMATRIX8 wm;
        void* tex = NULL;
        char wtxt[64] = "";
        if (o_GetTransform && o_GetTransform(dev, D3DTS_WORLD, &wm) == S_OK)
            _snprintf(wtxt, sizeof(wtxt), "  world %.2f %.2f %.2f",
                      wm.m[3][0], wm.m[3][1], wm.m[3][2]);
        if (o_GetTexture) o_GetTexture(dev, 0, &tex);
        LogD("DRAWDump DIR  bounds x %.0f..%.0f  y %.0f..%.0f  stride=%u"
            "  tex=%p%s", mnx, mxx, mny, mxy, stride, tex, wtxt);
        if (tex) ReleaseIUnknown(tex);
    }
}

static HRESULT __stdcall hkDrawIndexedPrimitiveUP(void* dev, DWORD prim,
    UINT minIndex, UINT numVertices, UINT primCount, const void* indexData,
    DWORD indexFormat, const void* vertexData, UINT vertexStride)
{
    ++g_drawsThisFrame; ++g_drawsUP;
    NoteDraw(g_curFvf, vertexStride);
    if (vertexData && vertexStride == 24 && prim == 4 &&
        numVertices >= 8 && numVertices <= 600)
        CaptureHudBatchUP(dev, vertexData, numVertices);
    if (g_compositeActive && g_gameRTActive && !g_handDrawnThisFrame &&
        InGameplay() && vertexStride == 24) {
        DrawHandAndLaserWorld(dev);
        g_handDrawnThisFrame = TRUE;
    }
    if (g_handDrawnThisFrame && HideThisDraw()) return 0;
    return o_DrawIndexedPrimitiveUP(dev, prim, minIndex, numVertices, primCount,
                                    indexData, indexFormat, vertexData,
                                    vertexStride);
}

/* AER reprojection for the non-indexed path (the world comes through here as
   camera-space XYZ lists). Same lateral eye shift as the indexed path. */
static HRESULT AerDrawPrimitive(void* dev, DWORD prim, UINT start, UINT count)
{
    void* vb = NULL;
    UINT stride = 0, i;
    BYTE* data = NULL;
    PFN_VBLock lock;
    PFN_VBUnlock unlock;
    float d;
    static long logged = 0;

    if (!o_GetStreamSource || !o_DrawPrimitiveUP) return E_FAIL;
    if (count == 0 || count > AER_MAX_V) return E_FAIL;
    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return E_FAIL;
    if (stride != 20 && stride != 24) { ReleaseIUnknown(vb); return E_FAIL; }

    lock   = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, 0, 0, &data, 0x10) != S_OK || !data) {
        ReleaseIUnknown(vb);
        return E_FAIL;
    }
    data += (SIZE_T)start * stride;
    /* The non-indexed world path is in centimetres (a room spans ~500 units),
       so the metre eye offset is scaled by 100 here. */
    d = (g_eyeRight ? 0.5f : -0.5f) * EyeIpd() * 100.0f;
    for (i = 0; i < count; ++i) {
        const BYTE* src = data + (SIZE_T)i * stride;
        BYTE* dst = g_aerVerts + (SIZE_T)i * stride;
        memcpy(dst, src, stride);
        *(float*)(dst + 0) = *(const float*)(src + 0) - d;
    }
    unlock(vb);
    ReleaseIUnknown(vb);

    if (logged < 20 || (g_frame % 600) == 0) {
        ++logged;
        LogD("STEREO   AER primitive: count=%u stride=%u eye=%s d=%+.4f",
             count, stride, g_eyeRight ? "right" : "left", d);
    }
    return o_DrawPrimitiveUP(dev, prim, count, g_aerVerts, stride);
}

static HRESULT __stdcall hkDrawPrimitive(void* dev, DWORD prim, UINT start, UINT count)
{
    ++g_drawsThisFrame; ++g_drawsVB;
    NoteDraw(g_curFvf, 0);
    /* The inventory / HUD 3D models come through here as expanded triangle
       lists with stride 24 (XYZ + DIFFUSE + TEX1). Keep a copy of the one
       draw that carries the active weapon model. */
    if (prim == 4 && count >= 8 && count <= 600 &&
        g_3dAttach && !g_cloneValid && g_3dIdx == g_3dSel) {
        CaptureHudBatch(dev, start, count);
    }
    if (g_dumpDraws > 0) {
        LogD("DRAWDump DIR  fvf=0x%04lX count=%u prim=%lu", g_curFvf, count, prim);
        DumpVBBounds(dev, start, count);
        --g_dumpDraws;
    }
    if (g_handDrawnThisFrame && HideThisDraw()) return 0;
    if (g_stereoMode == STEREO_AER && InGameplay() && !g_hudModelPass) {
        if (AerDrawPrimitive(dev, prim, start, count) == S_OK)
            return S_OK;
    }
    return o_DrawPrimitive(dev, prim, start, count);
}

static void DumpWorldZ(void* dev, UINT minIdx, UINT numV)
{
    void* vb = NULL;
    UINT stride = 0;
    BYTE* data = NULL;
    PFN_VBLock lock;
    PFN_VBUnlock unlock;
    float zmin = 1e30f, zmax = -1e30f;
    float sample[4] = { 0, 0, 0, 0 };
    UINT i, n;

    if (!o_GetStreamSource || numV < 1) return;
    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return;
    if (stride < 16) { ReleaseIUnknown(vb); return; }

    lock = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, minIdx * stride, numV * stride, &data, 0x10) != S_OK || !data) {
        if (lock(vb, 0, 0, &data, 0x10) != S_OK || !data) {
            LogD("WORLDZ  vertex buffer not readable (stride %u)", stride);
            ReleaseIUnknown(vb);
            return;
        }
        data += minIdx * stride;
    }
    n = numV > 64 ? 64 : numV;
    for (i = 0; i < n; ++i) {
        const float z = *(const float*)(data + (SIZE_T)i * stride + 8);
        if (z < zmin) zmin = z;
        if (z > zmax) zmax = z;
        if (i < 4) sample[i] = z;
    }
    LogD("WORLDZ  stride=%u minIdx=%u numV=%u  z %.4f..%.4f  first %.3f %.3f %.3f %.3f",
        stride, minIdx, numV, zmin, zmax, sample[0], sample[1], sample[2], sample[3]);
    for (i = 0; i < 3 && i < numV; ++i) {
        const float* f = (const float*)(data + (SIZE_T)i * stride);
        LogD("WORLDZ  v%u  % .4f % .4f % .4f % .4f % .4f",
            i, f[0], f[1], f[2], f[3], f[4]);
    }
    unlock(vb);
    ReleaseIUnknown(vb);
}

/* ---------------------------------------------------------------------------
 * HUD WEAPON MODEL: keep a copy of the geometry
 *
 * The game draws the HUD slot icons as small 3D models (primCount 28,
 * numV 34, XYZ + TEX1, stride 20). We read every candidate and keep the one
 * that is most elongated (a gun), then draw that copy at the hand. The can /
 * ammo icons score much lower, so they are ignored.
 * ------------------------------------------------------------------------- */

#define GUN_MAXV 2048
#define GUN_MAXI 8192

static void*  g_gunTex      = NULL;
static BOOL   g_gunCapValid = FALSE;
static UINT   g_gunCapN     = 0;
static float  g_gunCapV[GUN_MAXV][5];
static UINT   g_gunCapIN    = 0;
static WORD   g_gunCapI[GUN_MAXI];
static float  g_gunScore    = 0.0f;
static int    g_gunLong     = 1;    /* local axis mapped to forward */
static int    g_gunMid      = 2;    /* local axis mapped to up      */
static float  g_gunScale    = 1.0f;
static float  g_gunScaleMul = 1.0f;  /* per-weapon trim (grenade is smaller) */
static BOOL   g_gunVertical  = FALSE;/* grenade stands up instead of forward */

/* Multi-part weapon clone: one group of draws sharing a vertex buffer. */
#define GUN_PARTS 8
typedef struct {
    BOOL  valid;
    UINT  nV;
    float V[GUN_MAXV][5];
    UINT  nI;
    WORD  I[GUN_MAXI];
    void* tex;
    int   la, ma;
    float scale;
} GunPart;
static GunPart g_parts[GUN_PARTS];
static int     g_partN = 0;
static void*   g_weaponVB = NULL;
static BOOL    g_needParts = FALSE;

/* Re-capture whenever the active weapon changes. The active slot's model is
   the second-to-last 3D draw, so g_3dSel follows g_3dPrevN - 2. */
static int g_lastWeaponKey = -1;
static BOOL g_weaponEmpty = FALSE;
static BOOL g_dropLatch  = FALSE;   /* dropped/emptied: hands only */
static int  g_emptyCount = 0;
static BOOL g_haveInv = FALSE;       /* player inventory read at least once */
static int g_prevW0 = -1, g_prevW1 = -1, g_prevSel = -1;

static void RecaptureWeapon(void)
{
    int i;
    g_cloneValid = FALSE;
    g_gunCapValid = FALSE;
    g_gunScore = 0.0f;
    g_needCapture = TRUE;
    g_tailDump = TRUE;
    g_needParts = TRUE;
    g_weaponVB = NULL;
    for (i = 0; i < g_partN; ++i) {
        if (g_parts[i].tex) ReleaseIUnknown(g_parts[i].tex);
        g_parts[i].tex = NULL;
        g_parts[i].valid = FALSE;
    }
    g_partN = 0;
    if (g_gunTex) { ReleaseIUnknown(g_gunTex); g_gunTex = NULL; }
}

static void ClearWeaponParts(void)
{
    int i;
    for (i = 0; i < g_partN; ++i) {
        if (g_parts[i].tex) ReleaseIUnknown(g_parts[i].tex);
        g_parts[i].tex = NULL;
        g_parts[i].valid = FALSE;
    }
    g_partN = 0;
    g_needParts = FALSE;
    g_weaponVB = NULL;
    if (g_gunTex) { ReleaseIUnknown(g_gunTex); g_gunTex = NULL; }
}

static void AmmoSanity(const HudVals* v)
{
    if (g_dropLatch) return;
    if (!g_weaponEmpty) return;
    if ((v->cur0 + v->cur1 + v->res0 + v->res1) <= 0) return;
    g_weaponEmpty = FALSE;
    g_3dManual = FALSE;
    g_3dAttach = TRUE;
    RecaptureWeapon();
    Log("HIDE     sanity: ammo %d/%d/%d/%d with empty hand - showing weapon",
        v->cur0, v->cur1, v->res0, v->res1);
}

static void WatchWeaponSwap(void)
{
    HudVals v;
    int active, key;
    const int prevW0 = g_prevW0, prevW1 = g_prevW1, prevSel = g_prevSel;
    BOOL dropped = FALSE, gained = FALSE;

    if (!ReadHudVals(&v)) return;
    g_haveInv = TRUE;

    active = v.sel ? v.w1 : v.w0;
    key = (v.sel & 0xFF) | ((v.w0 & 0xFF) << 8) | ((v.w1 & 0xFF) << 16);

    if (prevW0 >= 0) {
        dropped = ((prevW0 > 0 && v.w0 == 0) ||
                   (prevW1 > 0 && v.w1 == 0)) ? TRUE : FALSE;
        gained  = (((prevW0 == 0 && v.w0 > 0) ||
                    (prevW1 == 0 && v.w1 > 0) ||
                    (prevW0 != v.w0 && v.w0 > 0) ||
                    (prevW1 != v.w1 && v.w1 > 0))) ? TRUE : FALSE;
    }

    if (active == 0 && (v.cur0 + v.cur1 + v.res0 + v.res1) == 0) {
        if (g_emptyCount < 1000) ++g_emptyCount;
    } else {
        g_emptyCount = 0;
    }
    g_weaponEmpty = (g_emptyCount > 10) ? TRUE : FALSE;

    /* A deliberate drop, an explicit switch to the empty slot, or the
       grenade emptying to NONE latches hands-only until a pickup or a
       switch to a non-empty slot. */
    if (dropped || (prevSel >= 0 && v.sel != prevSel && active == 0) ||
        (g_weaponEmpty && active == 0)) {
        if (!g_dropLatch)
            Log("HIDE     weapon gone (drop/empty): hands only");
        g_dropLatch = TRUE;
    }
    if (g_dropLatch &&
        ((gained && active != 0) || (v.sel != prevSel && active != 0))) {
        g_dropLatch = FALSE;
        g_emptyCount = 0;
        Log("HIDE     pickup/switch after drop: recapturing");
    }

    if (g_dropLatch) {
        g_weaponEmpty = TRUE;
        g_needParts = FALSE;
        g_weaponVB = NULL;
        g_cloneValid = TRUE;
        g_gunCapValid = FALSE;
        if (g_partN > 0) ClearWeaponParts();
        if (g_gunTex) { ReleaseIUnknown(g_gunTex); g_gunTex = NULL; }
        g_lastWeaponKey = key;
        g_prevW0 = v.w0; g_prevW1 = v.w1; g_prevSel = v.sel;
        return;
    }

    g_weaponEmpty = FALSE;

    if (active != g_adjWeapon) {
        g_adjWeapon = active;
        if (active >= 0 && active < 32) {
            g_handAdj[0] = g_handAdjTable[active][0];
            g_handAdj[1] = g_handAdjTable[active][1];
            g_handAdj[2] = g_handAdjTable[active][2];
        } else {
            g_handAdj[0] = g_handAdj[1] = g_handAdj[2] = 0.0f;
        }
    }

    if (active > 0) {
        char nm[64];
        WeaponNameShort(active, nm, sizeof(nm));
        if (_strnicmp(nm, "GREN", 4) == 0) {
            g_gunScaleMul = 1.0f / 3.0f;
            g_gunVertical = TRUE;
        } else if (_strnicmp(nm, "MAC", 3) == 0) {
            g_gunScaleMul = 0.75f;
            g_gunVertical = FALSE;
        } else {
            g_gunScaleMul = 1.0f;
            g_gunVertical = FALSE;
        }
    }

    if (g_lastWeaponKey < 0) {
        g_lastWeaponKey = key;
        g_needParts = TRUE;
        g_weaponVB = NULL;
        g_prevW0 = v.w0; g_prevW1 = v.w1; g_prevSel = v.sel;
        return;
    }

    if (key != g_lastWeaponKey) {
        g_lastWeaponKey = key;
        g_3dManual = FALSE;
        g_3dAttach = TRUE;
        RecaptureWeapon();
        Log("HIDE     weapon %d in slot %d: recapturing model",
            active, v.sel);
    }

    AmmoSanity(&v);
    g_prevW0 = v.w0; g_prevW1 = v.w1; g_prevSel = v.sel;
}

static void CaptureGunModel(void* dev, UINT minIdx, UINT numV,
                            UINT startIdx, UINT primCount)
{
    void* vb = NULL;
    UINT stride = 0;
    BYTE* data = NULL;
    void* ib = NULL;
    UINT base = 0;
    PFN_VBLock lock;
    PFN_VBUnlock unlock;
    float v[GUN_MAXV][5];
    WORD idx[GUN_MAXI];
    UINT idxN = 0;
    void* tex = NULL;
    float mn[3] = { 1e9f, 1e9f, 1e9f };
    float mx[3] = { -1e9f, -1e9f, -1e9f };
    float ext[3];
    float big, mid, small, score;
    int longAxis = 1, midAxis = 2, shortAxis = 0;
    UINT i, k;

    if (!o_GetStreamSource || !o_GetIndices) return;
    if (numV > GUN_MAXV || numV < 4) {
        Log("HIDE     capture skipped: numV=%u (max %u)", numV, GUN_MAXV);
        return;
    }

    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return;
    if (stride < 20) { ReleaseIUnknown(vb); return; }

    lock   = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, minIdx * stride, numV * stride, &data, 0x10) != S_OK || !data) {
        if (lock(vb, 0, 0, &data, 0x10) != S_OK || !data) {
            ReleaseIUnknown(vb);
            return;
        }
        data += minIdx * stride;
    }
    for (i = 0; i < numV; ++i) {
        const float* f = (const float*)(data + (SIZE_T)i * stride);
        const int uo = (stride >= 28) ? 5 : (stride >= 24) ? 4 : 3;
        const int vo = uo + 1;
        v[i][0] = f[0];
        v[i][1] = f[1];
        v[i][2] = f[2];
        v[i][3] = f[uo];
        v[i][4] = f[vo];
        for (k = 0; k < 3; ++k) {
            if (v[i][k] < mn[k]) mn[k] = v[i][k];
            if (v[i][k] > mx[k]) mx[k] = v[i][k];
        }
    }
    unlock(vb);
    ReleaseIUnknown(vb);

    if (o_GetIndices(dev, &ib, &base) != S_OK || !ib) return;
    {
        PFN_VBLock ilock   = (PFN_VBLock)(*(void***)ib)[11];
        PFN_VBUnlock iunlock = (PFN_VBUnlock)(*(void***)ib)[12];
        BYTE* idata = NULL;
        if (ilock(ib, 0, 0, &idata, 0x10) == S_OK && idata) {
            UINT cnt = primCount * 3;
            if (cnt > GUN_MAXI) cnt = GUN_MAXI;
            memcpy(idx, idata + (SIZE_T)startIdx * 2, (SIZE_T)cnt * 2);
            idxN = cnt;
            iunlock(ib);
        }
        ReleaseIUnknown(ib);
    }
    if (idxN < 3) return;

    {
        static int fmtLogs = 0;
        if (fmtLogs < 30) {
            LogD("HIDE     model fmt: stride=%u fvf=0x%04lX"
                "  uv0 % .3f,% .3f  uv1 % .3f,% .3f  tex=%p",
                stride, g_curFvf, v[0][3], v[0][4], v[1][3], v[1][4], tex);
            ++fmtLogs;
        }
    }

    for (k = 0; k < 3; ++k) ext[k] = mx[k] - mn[k];
    big = ext[0]; mid = ext[1]; small = ext[2];
    longAxis = 0; midAxis = 1; shortAxis = 2;
    if (mid > big)   { float t = big; big = mid; mid = t; midAxis = 0; longAxis = 1; }
    if (small > mid) { float t = mid; mid = small; small = t; shortAxis = midAxis; midAxis = 2; }
    if (mid > big)   { float t = big; big = mid; mid = t; k = (UINT)longAxis; longAxis = midAxis; midAxis = (int)k; }
    if (small > big) { float t = big; big = small; small = t; k = (UINT)longAxis; longAxis = shortAxis; shortAxis = (int)k; }
    score = big / (small > 0.01f ? small : 0.01f);

    if (o_GetTexture) o_GetTexture(dev, 0, &tex);

    {
        static int logs = 0;
        if (logs < 24) {
            D3DMATRIX8 wm;
            char wtxt[64] = "";
            if (o_GetTransform && o_GetTransform(dev, D3DTS_WORLD, &wm) == S_OK)
                _snprintf(wtxt, sizeof(wtxt), "  world %.3f %.3f %.3f",
                          wm.m[3][0], wm.m[3][1], wm.m[3][2]);
            LogD("HAND     model cand %d: %u v %u i  ext %.3f/%.3f/%.3f"
                " score %.1f  tex=%p%s",
                logs, numV, idxN, ext[0], ext[1], ext[2], score, tex, wtxt);
            ++logs;
        }
    }

    if (score <= g_gunScore) {
        if (tex) ReleaseIUnknown(tex);
        return;
    }

    /* Better candidate: replace the stored model. */
    if (g_gunTex) ReleaseIUnknown(g_gunTex);
    g_gunTex = tex;
    memcpy(g_gunCapV, v, sizeof(float) * 5 * numV);
    memcpy(g_gunCapI, idx, sizeof(WORD) * idxN);
    g_gunCapN = numV;
    g_gunCapIN = idxN;
    g_gunLong = longAxis;
    g_gunMid = midAxis;
    g_gunScale = 0.525f / (big > 0.01f ? big : 1.0f);
    g_gunScore = score;
    g_gunCapValid = TRUE;
}

static void HandDrawCapturedGun(void* dev, const float vp[3], const float vf[3],
                                const float vu[3], const float vr[3])
{
    static CompVert3D out[GUN_MAXI];
    float origin[3];
    int pi;

    if (g_partN <= 0) return;

    /* Weapon UVs can be negative (atlas offsets), so wrap instead of clamp. */
    o_SetTextureStageState(dev, 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    o_SetTextureStageState(dev, 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);

    origin[0] = vp[0] + vf[0] * 0.10f;
    origin[1] = vp[1] + vf[1] * 0.10f;
    origin[2] = vp[2] + vf[2] * 0.10f;

    for (pi = 0; pi < g_partN; ++pi) {
        GunPart* b = &g_parts[pi];
        const int la = b->la;
        const int ma = b->ma;
        const int sa = 3 - la - ma;
        const float S = b->scale * g_gunScaleMul;
        UINT n = 0, i;

        if (!b->valid || b->nI < 3) continue;
        for (i = 0; i < b->nI && n < GUN_MAXI; ++i) {
            const UINT vi = b->I[i];
            const float* v;
            if (vi >= b->nV) continue;
            v = b->V[vi];
            if (g_gunVertical) {
                out[n].x = origin[0] - vr[0]*v[sa]*S + vu[0]*v[la]*S - vf[0]*v[ma]*S;
                out[n].y = origin[1] - vr[1]*v[sa]*S + vu[1]*v[la]*S - vf[1]*v[ma]*S;
                out[n].z = origin[2] - vr[2]*v[sa]*S + vu[2]*v[la]*S - vf[2]*v[ma]*S;
            } else {
                out[n].x = origin[0] - vr[0]*v[sa]*S + vu[0]*v[ma]*S - vf[0]*v[la]*S;
                out[n].y = origin[1] - vr[1]*v[sa]*S + vu[1]*v[ma]*S - vf[1]*v[la]*S;
                out[n].z = origin[2] - vr[2]*v[sa]*S + vu[2]*v[ma]*S - vf[2]*v[la]*S;
            }
            out[n].color = 0xFFFFFFFFu;
            out[n].u = v[3];
            out[n].v = v[4];
            ++n;
        }
        if (n < 3) continue;
        o_SetTexture(dev, 0, b->tex ? b->tex : g_whiteTex);
        o_SetVertexShader(dev, 0x0142);
        o_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, n / 3, out, sizeof(CompVert3D));
    }
}


#define HUD_BMAX 16
#define HUD_BV  600

typedef struct {
    BOOL  valid;
    UINT  n;
    float v[HUD_BV][5];
    void* tex;
    float scale;
    int   la, ma;
} HudBatch;

static HudBatch  g_hbSetA[HUD_BMAX];
static HudBatch  g_hbSetB[HUD_BMAX];
static HudBatch* g_hbCur  = g_hbSetA;   /* being filled this frame */
static HudBatch* g_hbPrev = g_hbSetB;   /* drawn by the hand pass  */
static int       g_hbCurN  = 0;
static int       g_hbPrevN = 0;

/* One-shot clone of the identified weapon model. Frozen after the first
   capture so the game's spinning copy does not affect ours. */
static HudBatch  g_clone;

static void CloneFromBatch(const HudBatch* b)
{
    if (g_cloneValid || !b || !b->valid) return;
    g_clone = *b;
    if (g_clone.tex) AddRefIUnknown(g_clone.tex);
    g_cloneValid = TRUE;
    LogD("HIDE     clone captured: n=%u tex=%p", b->n, b->tex);
}

/* Called at BeginScene: drop the set from two frames ago, then swap the
   captured set into the one the hand pass will draw. */
static void ReleaseHudBatches(void)
{
    int i;
    for (i = 0; i < g_hbPrevN; ++i) {
        if (g_hbPrev[i].tex) {
            ReleaseIUnknown(g_hbPrev[i].tex);
            g_hbPrev[i].tex = NULL;
        }
        g_hbPrev[i].valid = FALSE;
    }
    {
        HudBatch* t = g_hbPrev;
        g_hbPrev = g_hbCur;
        g_hbCur  = t;
    }
    g_hbPrevN = g_hbCurN;
    g_hbCurN  = 0;
}

static void CaptureHudBatch(void* dev, UINT start, UINT count)
{
    void* vb = NULL;
    UINT stride = 0;
    BYTE* data = NULL;
    PFN_VBLock lock;
    PFN_VBUnlock unlock;
    HudBatch* b;
    float mn[3] = { 1e9f, 1e9f, 1e9f };
    float mx[3] = { -1e9f, -1e9f, -1e9f };
    float ext[3], big;
    UINT i, k;

    if (!o_GetStreamSource || g_hbCurN >= HUD_BMAX) return;
    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return;
    if (stride != 24) { ReleaseIUnknown(vb); return; }

    lock   = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, start * stride, count * stride, &data, 0x10) != S_OK || !data) {
        ReleaseIUnknown(vb);
        return;
    }
    b = &g_hbCur[g_hbCurN];
    b->n = count > HUD_BV ? HUD_BV : count;
    for (i = 0; i < b->n; ++i) {
        const float* f = (const float*)(data + (SIZE_T)i * stride);
        for (k = 0; k < 5; ++k) b->v[i][k] = f[k];
        for (k = 0; k < 3; ++k) {
            if (b->v[i][k] < mn[k]) mn[k] = b->v[i][k];
            if (b->v[i][k] > mx[k]) mx[k] = b->v[i][k];
        }
    }
    unlock(vb);
    ReleaseIUnknown(vb);

    for (k = 0; k < 3; ++k) ext[k] = mx[k] - mn[k];
    big = ext[0]; b->la = 0; b->ma = 1;
    if (ext[1] > big) { big = ext[1]; b->la = 1; b->ma = 0; }
    if (ext[2] > big) { big = ext[2]; b->la = 2; b->ma = (ext[0] > ext[1] ? 0 : 1); }
    b->scale = 0.09f / (big > 0.01f ? big : 1.0f);
    b->tex = NULL;
    if (o_GetTexture) o_GetTexture(dev, 0, &b->tex);
    b->valid = TRUE;
    ++g_hbCurN;
    if (g_3dAttach) CloneFromBatch(b);
    {
        static int logs = 0;
        if (logs < 30) {
            LogD("HUD      batch %d: n=%u ext %.1f/%.1f/%.1f tex=%p",
                g_hbCurN - 1, b->n, ext[0], ext[1], ext[2], b->tex);
            ++logs;
        }
    }
}

static void CaptureHudBatchUP(void* dev, const void* data, UINT count)
{
    HudBatch* b;
    const BYTE* p = (const BYTE*)data;
    float mn[3] = { 1e9f, 1e9f, 1e9f };
    float mx[3] = { -1e9f, -1e9f, -1e9f };
    float ext[3], big;
    UINT i, k;

    if (!data || g_hbCurN >= HUD_BMAX) return;
    b = &g_hbCur[g_hbCurN];
    b->n = count > HUD_BV ? HUD_BV : count;
    for (i = 0; i < b->n; ++i) {
        const float* f = (const float*)(p + (SIZE_T)i * 24);
        for (k = 0; k < 5; ++k) b->v[i][k] = f[k];
        for (k = 0; k < 3; ++k) {
            if (b->v[i][k] < mn[k]) mn[k] = b->v[i][k];
            if (b->v[i][k] > mx[k]) mx[k] = b->v[i][k];
        }
    }
    for (k = 0; k < 3; ++k) ext[k] = mx[k] - mn[k];
    big = ext[0]; b->la = 0; b->ma = 1;
    if (ext[1] > big) { big = ext[1]; b->la = 1; b->ma = 0; }
    if (ext[2] > big) { big = ext[2]; b->la = 2; b->ma = (ext[0] > ext[1] ? 0 : 1); }
    b->scale = 0.09f / (big > 0.01f ? big : 1.0f);
    b->tex = NULL;
    if (o_GetTexture) o_GetTexture(dev, 0, &b->tex);
    b->valid = TRUE;
    ++g_hbCurN;
    if (g_3dAttach) CloneFromBatch(b);
    {
        static int logs = 0;
        if (logs < 30) {
            LogD("HUD      UP batch %d: n=%u ext %.1f/%.1f/%.1f tex=%p",
                g_hbCurN - 1, b->n, ext[0], ext[1], ext[2], b->tex);
            ++logs;
        }
    }
}

static void HandDrawHudBatchRow(void* dev)
{
    CompVert3D row[HUD_BV];
    static DWORD lastIdx = 0xFFFFFFFFu;
    DWORD idx;

    if (g_hbPrevN <= 0) return;
    /* Cycle the model shown at the top-left every 5 s, so two screenshots
       taken inside one window show the same batch at different times. */
    idx = (GetTickCount() / 5000u) % (DWORD)g_hbPrevN;
    if (idx != lastIdx) {
        LogD("HUD      showing batch %lu of %d", idx, g_hbPrevN);
        lastIdx = idx;
    }
    o_SetRenderState(dev, D3DRS_ZENABLE, 0);
    {
        HudBatch* b = &g_hbPrev[idx];
        const int la = b->la, ma = b->ma, sa = 3 - la - ma;
        const float cx = 0.0f, cy = 0.30f, cz = 0.60f;
        UINT n = 0, i;
        if (b->valid) {
            for (i = 0; i < b->n; ++i) {
                const float* v = b->v[i];
                row[n].x = cx + v[la] * b->scale;
                row[n].y = cy + v[ma] * b->scale;
                row[n].z = cz + v[sa] * b->scale;
                row[n].color = 0xFFFFFFFFu;
                row[n].u = v[3];
                row[n].v = v[4];
                ++n;
            }
            if (n >= 3) {
                o_SetTexture(dev, 0, b->tex ? b->tex : g_whiteTex);
                o_SetVertexShader(dev, 0x0142);
                o_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, n / 3, row, sizeof(CompVert3D));
            }
        }
    }
    o_SetRenderState(dev, D3DRS_ZENABLE, 1);
}

/* Draw the captured inventory gun at the dominant hand. */
static void HandDrawAttachedBatch(void* dev, const float vp[3], const float vf[3],
                                  const float vu[3], const float vr[3])
{
    CompVert3D out[HUD_BV];
    HudBatch* b;
    int la, ma, sa;
    float s, o[3];
    UINT n = 0, i;

    if (g_hbPrevN <= 0 && !g_cloneValid) return;
    b = g_cloneValid ? &g_clone : &g_hbPrev[0];
    if (!b->valid || b->n < 3) return;
    la = b->la; ma = b->ma; sa = 3 - la - ma;
    s = b->scale * (0.35f / 0.09f);
    o[0] = vp[0] + vf[0] * 0.10f;
    o[1] = vp[1] + vf[1] * 0.10f;
    o[2] = vp[2] + vf[2] * 0.10f;
    for (i = 0; i < b->n; ++i) {
        const float* v = b->v[i];
        out[n].x = o[0] + vr[0]*v[sa]*s + vu[0]*v[ma]*s + vf[0]*v[la]*s;
        out[n].y = o[1] + vr[1]*v[sa]*s + vu[1]*v[ma]*s + vf[1]*v[la]*s;
        out[n].z = o[2] + vr[2]*v[sa]*s + vu[2]*v[ma]*s + vf[2]*v[la]*s;
        out[n].color = 0xFFFFFFFFu;
        out[n].u = v[3];
        out[n].v = v[4];
        ++n;
    }
    if (n < 3) return;
    o_SetTexture(dev, 0, b->tex ? b->tex : g_whiteTex);
    o_SetVertexShader(dev, 0x0142);
    o_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, n / 3, out, sizeof(CompVert3D));
}

/* ---- AER reprojection ---------------------------------------------------
   XOPS pre-transforms the world on the CPU and submits XYZRHW vertices, so
   moving the D3D view has no effect. A lateral eye shift of d metres moves a
   vertex at camera-space depth z by -d * fpx / z pixels, and leaves y and the
   depth unchanged: exactly the parallax AER needs. The game's vertices are
   copied into a scratch buffer, so its shared vertex buffer is never touched.

   fpx and the depth constants come from the game's own projection:
   fpx = m00 * (width/2), A = m22, B = -m32, z = B / (A - zNdc). */
static HRESULT AerDrawIndexed(void* dev, DWORD prim, UINT minIdx, UINT numV,
                              UINT startIdx, UINT primCount)
{
    void* vb = NULL;
    void* ib = NULL;
    UINT stride = 0, base = 0, nI, i;
    BYTE* vdata = NULL;
    BYTE* idata = NULL;
    PFN_VBLock lock, ilock;
    PFN_VBUnlock unlock, iunlock;
    float A, B, fpx, d, zndc, zcam, dx;
    static long logged = 0;

    if (!o_GetStreamSource || !o_GetIndices || !o_DrawIndexedPrimitiveUP)
        return E_FAIL;
    if (prim != 4 /* D3DPT_TRIANGLELIST */ || !g_haveProj) return E_FAIL;
    if (!(g_curFvf & D3DFVF_XYZRHW)) return E_FAIL;
    if (numV == 0 || numV > AER_MAX_V) return E_FAIL;
    if (primCount == 0 || primCount * 3 > AER_MAX_I) return E_FAIL;

    if (o_GetStreamSource(dev, 0, &vb, &stride) != S_OK || !vb) return E_FAIL;
    if (stride != 20 && stride != 28) { ReleaseIUnknown(vb); return E_FAIL; }

    A = g_lastProj.m[2][2];
    B = -g_lastProj.m[3][2];
    fpx = g_lastProj.m[0][0] * 320.0f;
    if (stride == 28 && (A <= 0.0f || B <= 0.0f || fpx <= 0.0f)) {
        ReleaseIUnknown(vb);
        return E_FAIL;
    }
    d = (g_eyeRight ? 0.5f : -0.5f) * EyeIpd();

    lock   = (PFN_VBLock)(*(void***)vb)[11];
    unlock = (PFN_VBUnlock)(*(void***)vb)[12];
    if (lock(vb, 0, 0, &vdata, 0x10) != S_OK || !vdata) {
        ReleaseIUnknown(vb);
        return E_FAIL;
    }
    vdata += (SIZE_T)minIdx * stride;

    for (i = 0; i < numV; ++i) {
        const BYTE* src = vdata + (SIZE_T)i * stride;
        BYTE* dst = g_aerVerts + (SIZE_T)i * stride;
        float x;
        memcpy(dst, src, stride);
        x = *(const float*)(src + 0);
        if (stride == 20) {
            /* World vertices are camera-space XYZ + TEX1, so the eye shift is
               a plain lateral translation; D3D applies the fixed projection. */
            *(float*)(dst + 0) = x - d;
        } else {
            /* XYZRHW: pre-transformed. Move the vertex in screen space by the
               parallax for its depth. */
            float z = *(const float*)(src + 8);
            zndc = z;
            if (zndc < 0.0f) zndc = 0.0f;
            if (zndc > 0.9999f) zndc = 0.9999f;
            zcam = B / (A - zndc);
            if (zcam < 0.05f) zcam = 0.05f;
            dx = -d * fpx / zcam;
            if (dx >  60.0f) dx =  60.0f;
            if (dx < -60.0f) dx = -60.0f;
            *(float*)(dst + 0) = x + dx;
        }
    }
    unlock(vb);
    ReleaseIUnknown(vb);

    if (o_GetIndices(dev, &ib, &base) != S_OK || !ib) return E_FAIL;
    if (base != 0) { ReleaseIUnknown(ib); return E_FAIL; }
    nI = primCount * 3;
    ilock   = (PFN_VBLock)(*(void***)ib)[11];
    iunlock = (PFN_VBUnlock)(*(void***)ib)[12];
    if (ilock(ib, 0, 0, &idata, 0x10) != S_OK || !idata) {
        ReleaseIUnknown(ib);
        return E_FAIL;
    }
    idata += (SIZE_T)startIdx * 2;
    for (i = 0; i < nI; ++i) {
        UINT vi = ((const WORD*)idata)[i];
        if (vi < minIdx) vi = minIdx;             /* keep it in range */
        g_aerIdx[i] = (WORD)(vi - minIdx);
    }
    iunlock(ib);
    ReleaseIUnknown(ib);

    if (logged < 40 || (g_frame % 600) == 0) {
        ++logged;
        LogD("STEREO   AER reproject: numV=%u prim=%u eye=%s d=%+.4f fpx=%.1f",
             numV, primCount, g_eyeRight ? "right" : "left", d, fpx);
    }

    return o_DrawIndexedPrimitiveUP(dev, prim, 0, numV, primCount,
                                    g_aerIdx, D3DFMT_INDEX16,
                                    g_aerVerts, 28);
}

static HRESULT __stdcall hkDrawIndexedPrim(void* dev, DWORD prim, UINT minIdx,
                                           UINT numV, UINT startIdx, UINT primCount)
{    /* The world is drawn; the last three primCount=28/numV=34 draws are the
       3D HUD weapon-slot models. Draw the virtual hands/laser just before the
       first of those, so they are in the gameplay layer underneath the HUD
       layer, not after it. From that draw on, everything is HUD/view-model
       geometry and must not be eye-reprojected. */
    if (InGameplay() && primCount == 28 && numV == 34)
        g_hudModelPass = TRUE;
    if (g_compositeActive && g_gameRTActive && !g_handDrawnThisFrame &&
        InGameplay() && primCount == 28 && numV == 34) {
        DrawHandAndLaserWorld(dev);
        g_handDrawnThisFrame = TRUE;
    }
    /* Multi-part capture: gather every draw that uses the primary weapon's
       vertex buffer, one part per submesh. */
    if (g_needParts && g_weaponVB && g_partN < GUN_PARTS && !g_weaponEmpty) {
        void* vb = NULL;
        UINT st = 0;
        if (o_GetStreamSource) o_GetStreamSource(dev, 0, &vb, &st);
        if (vb == g_weaponVB) {
            g_gunScore = 0.0f;
            CaptureGunModel(dev, minIdx, numV, startIdx, primCount);
            if (g_gunCapValid) {
                GunPart* p = &g_parts[g_partN];
                p->nV = g_gunCapN;
                memcpy(p->V, g_gunCapV, sizeof(float) * 5 * g_gunCapN);
                p->nI = g_gunCapIN;
                memcpy(p->I, g_gunCapI, sizeof(WORD) * g_gunCapIN);
                p->tex = g_gunTex;
                g_gunTex = NULL;
                p->la = g_gunLong;
                p->ma = g_gunMid;
                p->scale = g_gunScale;
                p->valid = TRUE;
                ++g_partN;
                LogD("HIDE     part %d: %u v %u i tex=%p",
                    g_partN - 1, p->nV, p->nI, p->tex);
            }
        }
        if (vb) ReleaseIUnknown(vb);
    }
    if (g_handDrawnThisFrame && HideThisDraw()) return 0;
    {
        void* vb = NULL; UINT st = 0; void* tex = NULL;
        TailDraw* t = &g_tail[g_tailHead & 15];
        if (o_GetStreamSource) o_GetStreamSource(dev, 0, &vb, &st);
        if (o_GetTexture) o_GetTexture(dev, 0, &tex);
        t->idx = (int)g_3dIdx;
        t->vb = vb;
        t->numV = numV;
        t->prim = primCount;
        t->tex = tex;
        ++g_tailHead;
        if (vb) ReleaseIUnknown(vb);
        if (tex) ReleaseIUnknown(tex);
    }
    ++g_drawsThisFrame; ++g_drawsIDX;
    NoteDraw(g_curFvf, 0);
    if (g_dumpDraws > 0) {
        LogD("DRAWDump IDX  fvf=0x%04lX primCount=%u numV=%u",
            g_curFvf, primCount, numV);
        if (g_curFvf == 0x0144 && numV >= 20 && primCount >= 20)
            DumpWorldZ(dev, minIdx, numV);
        --g_dumpDraws;
    }
    /* AER: reproject the pre-transformed world into the current eye. The
       three primCount=28/numV=34 draws are the game's 3D HUD weapon slots and
       stay screen-fixed, like a view model. */
    if (g_stereoMode == STEREO_AER && InGameplay() && !g_hudModelPass) {
        if (AerDrawIndexed(dev, prim, minIdx, numV, startIdx, primCount) == S_OK)
            return S_OK;
    }
    return o_DrawIndexedPrim(dev, prim, minIdx, numV, startIdx, primCount);
}

// The red sync pixel (stage 5). Echo HMD_SYNC exactly as received: the runtime
// calibrates itself to whatever red values it observes, and its remapping is
// gated on green == 0. 10x10 at (0,0) survives a border or a scaling pass.
// Clear is the only route because the back buffer is not lockable [device],
// and it writes exact bytes with no filtering or blending. It is the last draw
// before we hand control to the real EndScene, so nothing overwrites it.
static void DrawSyncPixel(void* dev)
{
    static long drawn = 0;
    int r, b;
    D3DRECT8 rc;

    if (!dev || !o_Clear || !g_posed || !g_pose.valid) return;

    r = (int)(g_pose.sync + 0.5f);
    if (r < 0) r = 0; else if (r > 255) r = 255;

    /* AER carries the eye in blue: 0 = left, 255 = right. Every other mode
       keeps blue 0 (section 5 of the XrApi spec). */
    b = (g_stereoMode == STEREO_AER && InGameplay()) ? (g_eyeRight ? 255 : 0) : 0;

    rc.x1 = 0; rc.y1 = 0; rc.x2 = 10; rc.y2 = 10;
    o_Clear(dev, 1, &rc, D3DCLEAR_TARGET, D3DCOLOR_XRGB(r, 0, b), 0.0f, 0);

    if (drawn == 0)
        Log("SYNC    first sync square drawn: RGB(%d,0,%d), 10x10 at (0,0)"
            " - echo of HMD_SYNC", r, b);
    ++drawn;
}

// ---------------------------------------------------------------------------
// Target-resolution composite
//
// The device's back buffer is created at the desktop/container size. XOPS
// still renders its world and 3D HUD models into the top-left 640x480, so:
//   1. copy that 640x480 region into a texture,
//   2. draw it across the whole back buffer,
//   3. draw the replacement text HUD (also a texture) inside the centre square.
// This is all in the D3D frame, so WinlatorXR's single-image capture sees it.
// ---------------------------------------------------------------------------

#define COMP_RS_N   20
#define COMP_TS0_N  13
#define COMP_TS1_N  6

typedef struct {
    DWORD rs[COMP_RS_N];   BYTE rsOk[COMP_RS_N];
    DWORD ts0[COMP_TS0_N]; BYTE ts0Ok[COMP_TS0_N];
    DWORD ts1[COMP_TS1_N]; BYTE ts1Ok[COMP_TS1_N];
    D3DVIEWPORT8 vp;
    BOOL  haveVp;
} CompState;

static const DWORD kCompRs[COMP_RS_N] = {
    D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE,
    D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
    D3DRS_LIGHTING, D3DRS_COLORVERTEX, D3DRS_CLIPPING,
    D3DRS_CLIPPLANEENABLE, D3DRS_BLENDOP, D3DRS_COLORWRITEENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_WRAP0, D3DRS_ZFUNC
};
static const DWORD kCompTs0[COMP_TS0_N] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
    D3DTSS_ADDRESSU, D3DTSS_ADDRESSV,
    D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER
};
static const DWORD kCompTs1Stage[COMP_TS1_N] = { 1, 1, 2, 2, 3, 3 };
static const DWORD kCompTs1Type[COMP_TS1_N]  = {
    D3DTSS_COLOROP, D3DTSS_ALPHAOP,
    D3DTSS_COLOROP, D3DTSS_ALPHAOP,
    D3DTSS_COLOROP, D3DTSS_ALPHAOP
};

static void CompSaveState(void* dev, CompState* s)
{
    int i;
    if (!s) return;
    ZeroMemory(s, sizeof(*s));
    for (i = 0; i < COMP_RS_N; ++i)
        s->rsOk[i] = (o_GetRenderState &&
                      o_GetRenderState(dev, kCompRs[i], &s->rs[i]) == S_OK)
                     ? 1 : 0;
    for (i = 0; i < COMP_TS0_N; ++i)
        s->ts0Ok[i] = (o_GetTextureStageState &&
                       o_GetTextureStageState(dev, 0, kCompTs0[i],
                                              &s->ts0[i]) == S_OK) ? 1 : 0;
    for (i = 0; i < COMP_TS1_N; ++i)
        s->ts1Ok[i] = (o_GetTextureStageState &&
                       o_GetTextureStageState(dev, kCompTs1Stage[i],
                                              kCompTs1Type[i],
                                              &s->ts1[i]) == S_OK) ? 1 : 0;
    s->haveVp = (o_GetViewport && o_GetViewport(dev, &s->vp) == S_OK) ? TRUE : FALSE;
}

static void CompRestoreState(void* dev, const CompState* s)
{
    int i;
    if (!s) return;
    for (i = 0; i < COMP_RS_N; ++i)
        if (s->rsOk[i]) o_SetRenderState(dev, kCompRs[i], s->rs[i]);
    for (i = 0; i < COMP_TS0_N; ++i)
        if (s->ts0Ok[i]) o_SetTextureStageState(dev, 0, kCompTs0[i], s->ts0[i]);
    for (i = 0; i < COMP_TS1_N; ++i)
        if (s->ts1Ok[i]) o_SetTextureStageState(dev, kCompTs1Stage[i],
                                                kCompTs1Type[i], s->ts1[i]);
    if (s->haveVp) o_SetViewport(dev, &s->vp);
}

static void CompSetStates(void* dev)
{
    int s;
    o_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    o_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    o_SetRenderState(dev, D3DRS_ZENABLE, 0);
    o_SetRenderState(dev, D3DRS_ZWRITEENABLE, 0);
    o_SetRenderState(dev, D3DRS_ALPHATESTENABLE, 0);
    o_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    o_SetRenderState(dev, D3DRS_LIGHTING, 0);
    o_SetRenderState(dev, D3DRS_FOGENABLE, 0);
    o_SetRenderState(dev, D3DRS_STENCILENABLE, 0);
    o_SetRenderState(dev, D3DRS_CLIPPING, 0);
    o_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    o_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    o_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xFFFFFFFF);
    o_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, 0);
    o_SetRenderState(dev, D3DRS_WRAP0, 0);
    o_SetRenderState(dev, D3DRS_COLORVERTEX, 0);
    o_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    o_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    o_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);

    o_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    o_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    o_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    o_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    o_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    o_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    o_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    o_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
    o_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    o_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    o_SetTextureStageState(dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    o_SetTextureStageState(dev, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    o_SetTextureStageState(dev, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    for (s = 1; s <= 3; ++s) {
        o_SetTextureStageState(dev, s, D3DTSS_COLOROP, D3DTOP_DISABLE);
        o_SetTextureStageState(dev, s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

typedef struct {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} CompVert;

static void CompDrawQuad(void* dev, float x0, float y0, float x1, float y1,
                         float u0, float v0, float u1, float v1)
{
    CompVert v[4];
    v[0].x = x0; v[0].y = y0; v[0].z = 0.0f; v[0].rhw = 1.0f;
    v[0].color = 0xFFFFFFFF; v[0].u = u0; v[0].v = v0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0.0f; v[1].rhw = 1.0f;
    v[1].color = 0xFFFFFFFF; v[1].u = u1; v[1].v = v0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0.0f; v[2].rhw = 1.0f;
    v[2].color = 0xFFFFFFFF; v[2].u = u0; v[2].v = v1;
    v[3].x = x1; v[3].y = y1; v[3].z = 0.0f; v[3].rhw = 1.0f;
    v[3].color = 0xFFFFFFFF; v[3].u = u1; v[3].v = v1;
    o_SetVertexShader(dev, 0x0144);       /* XYZRHW | DIFFUSE | TEX1 */
    o_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(CompVert));
}

static float HandDot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void HandCross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}

static float HandLen(const float a[3])
{
    return sqrtf(HandDot(a, a));
}

static void HandNorm(float a[3])
{
    float l = HandLen(a);
    if (l > 1e-6f) { a[0] /= l; a[1] /= l; a[2] /= l; }
    else           { a[0] = 0.0f; a[1] = 0.0f; a[2] = 1.0f; }
}

/* Two-handed aim (left grip held): the gun stays at the dominant hand but its
   forward points at the off hand, keeping the gun hand's roll. Mirrors SHOGO
   MAD WXR's UpdateTwoHanded - remove the forward component from the hand's up
   vector, renormalise it, then rebuild right = up x forward. A degenerate
   hand vector leaves the pose alone. */
static void HandTwoHandedAim(const float offP[3], const float vp[3],
                             float vf[3], float vu[3], float vr[3])
{
    float dir[3], len, d, ul;

    dir[0] = offP[0] - vp[0];
    dir[1] = offP[1] - vp[1];
    dir[2] = offP[2] - vp[2];
    len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (len < 0.001f) return;
    dir[0] /= len; dir[1] /= len; dir[2] /= len;

    d = dir[0]*vu[0] + dir[1]*vu[1] + dir[2]*vu[2];
    vu[0] -= dir[0]*d; vu[1] -= dir[1]*d; vu[2] -= dir[2]*d;
    ul = sqrtf(vu[0]*vu[0] + vu[1]*vu[1] + vu[2]*vu[2]);
    if (ul > 0.001f) {
        vu[0] /= ul; vu[1] /= ul; vu[2] /= ul;
    } else {
        vu[0] = 0.0f; vu[1] = 1.0f; vu[2] = 0.0f;
    }
    HandCross(vu, dir, vr);
    HandNorm(vr);

    vf[0] = dir[0];
    vf[1] = dir[1];
    vf[2] = dir[2];
}

/* Project a view-space point into the game's 640x480 image. The game view is
   +Z forward, +Y up, +X right. */
static BOOL HandProject(const float v[3], float* sx, float* sy)
{
    const float pi = 3.14159265358979323846f;
    float tx, ty, nx, ny;

    if (!sx || !sy || v[2] <= 0.05f) return FALSE;
    tx = tanf(g_outFovX * pi / 360.0f);
    ty = tanf(g_outFovY * pi / 360.0f);
    if (tx < 0.01f) tx = 0.01f;
    if (ty < 0.01f) ty = 0.01f;
    nx = v[0] / (v[2] * tx);
    ny = v[1] / (v[2] * ty);
    *sx = (nx * 0.5f + 0.5f) * 640.0f;
    *sy = (0.5f - ny * 0.5f) * 480.0f;
    return TRUE;
}

static void HandMapScreen(int gx, int gy, int gw, int gh, float* x, float* y)
{
    *x = (float)gx + (*x * (float)gw / 640.0f);
    *y = (float)gy + (*y * (float)gh / 480.0f);
}

static void HandDrawQuad(void* dev, const float* px, const float* py,
                         const float* pz, const float* pu, const float* pv,
                         DWORD color, void* tex)
{
    CompVert v[4];
    int i;

    if (!dev || !tex) return;
    for (i = 0; i < 4; ++i) {
        v[i].x = px[i]; v[i].y = py[i];
        v[i].z = pz ? pz[i] : 0.0f;
        v[i].rhw = 1.0f;
        v[i].color = color;
        v[i].u = pu[i]; v[i].v = pv[i];
    }
    o_SetTexture(dev, 0, tex);
    o_SetVertexShader(dev, 0x0144);       /* XYZRHW | DIFFUSE | TEX1 */
    o_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(CompVert));
}

static void HandDrawLine(void* dev, float x0, float y0, float z0,
                         float x1, float y1, float z1,
                         float width, DWORD color)
{
    float dx, dy, len, px, py;
    float px4[4], py4[4], pz4[4];
    const float pu[4] = { 0, 1, 0, 1 };
    const float pv[4] = { 0, 0, 1, 1 };

    if (!dev || !g_whiteTex) return;
    dx = x1 - x0; dy = y1 - y0;
    len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;
    px = -dy / len * width * 0.5f;
    py =  dx / len * width * 0.5f;
    px4[0] = x0 - px; py4[0] = y0 - py; pz4[0] = z0;
    px4[1] = x1 - px; py4[1] = y1 - py; pz4[1] = z1;
    px4[2] = x0 + px; py4[2] = y0 + py; pz4[2] = z0;
    px4[3] = x1 + px; py4[3] = y1 + py; pz4[3] = z1;
    HandDrawQuad(dev, px4, py4, pz4, pu, pv, color, g_whiteTex);
}

/* XYZ + DIFFUSE + TEX1, 24 bytes. D3D projects and depth-tests it with the
   game's own view/projection, so it matches the world depth buffer. */
static void HandDrawQuad3D(void* dev, const float p[4][3],
                           const float* pu, const float* pv,
                           DWORD color, void* tex)
{
    CompVert3D v[4];
    int i;

    if (!dev || !tex) return;
    for (i = 0; i < 4; ++i) {
        v[i].x = p[i][0]; v[i].y = p[i][1]; v[i].z = p[i][2];
        v[i].color = color;
        v[i].u = pu[i]; v[i].v = pv[i];
    }
    o_SetTexture(dev, 0, tex);
    o_SetVertexShader(dev, 0x0142);       /* XYZ | DIFFUSE | TEX1 */
    o_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(CompVert3D));
}

/* Camera-facing 3D quad for one laser segment. The width is scaled with
   distance so the beam stays roughly `pixels` wide on screen while still
   being real 3D geometry with real depth. */
static void HandDrawBeam3D(void* dev, const float p0[3], const float p1[3],
                           float pixels, DWORD color)
{
    const float u[4] = { 0, 1, 0, 1 };
    const float v[4] = { 0, 0, 1, 1 };
    float dir[3], right[3];
    const float viewF[3] = { 0.0f, 0.0f, 1.0f };
    float w0, w1, tanY;
    float corners[4][3];

    if (!dev || !g_whiteTex) return;
    dir[0] = p1[0] - p0[0];
    dir[1] = p1[1] - p0[1];
    dir[2] = p1[2] - p0[2];
    HandNorm(dir);
    /* view forward is (0,0,1); beam right is perpendicular to both */
    HandCross(viewF, dir, right);
    if (HandLen(right) < 0.001f) {
        right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    } else {
        HandNorm(right);
    }
    tanY = tanf(g_outFovY * 3.14159265358979323846f / 360.0f);
    if (tanY < 0.01f) tanY = 0.01f;
    w0 = pixels * p0[2] * 2.0f * tanY / 480.0f;
    w1 = pixels * p1[2] * 2.0f * tanY / 480.0f;

    corners[0][0] = p0[0] - right[0]*w0*0.5f;
    corners[0][1] = p0[1] - right[1]*w0*0.5f;
    corners[0][2] = p0[2] - right[2]*w0*0.5f;
    corners[1][0] = p1[0] - right[0]*w1*0.5f;
    corners[1][1] = p1[1] - right[1]*w1*0.5f;
    corners[1][2] = p1[2] - right[2]*w1*0.5f;
    corners[2][0] = p0[0] + right[0]*w0*0.5f;
    corners[2][1] = p0[1] + right[1]*w0*0.5f;
    corners[2][2] = p0[2] + right[2]*w0*0.5f;
    corners[3][0] = p1[0] + right[0]*w1*0.5f;
    corners[3][1] = p1[1] + right[1]*w1*0.5f;
    corners[3][2] = p1[2] + right[2]*w1*0.5f;
    HandDrawQuad3D(dev, corners, u, v, color, g_whiteTex);
}

/* Build a hand's view-space basis. Positions/orientations come from XrApi,
   already relative to the head; transform by the head basis. */
static void HandViewPose(const float* hp, const float* hf, const float* hu,
                         BOOL isRight, BOOL flip,
                         float vp[3], float vf[3], float vu[3], float vr[3])
{
    float headR[3], rel[3];
    BOOL noPos = (HandLen(hp) < 0.01f);
    BOOL noDir = (HandLen(hf) < 0.01f);
    float d;

    HandCross(g_pose.headU, g_pose.headF, headR);
    HandNorm(headR);

    if (noPos) {
        vp[0] = isRight ? 0.22f : -0.22f;
        vp[1] = -0.16f;
        vp[2] = 0.45f;
        if (!g_handLogged) {
            g_handLogged = TRUE;
            LogD("HAND     no controller hand position; using fallback offsets");
        }
    } else {
        rel[0] = hp[0] - g_pose.headP[0];
        rel[1] = hp[1] - g_pose.headP[1];
        rel[2] = hp[2] - g_pose.headP[2];
        vp[0] = HandDot(rel, headR);
        vp[1] = HandDot(rel, g_pose.headU);
        vp[2] = HandDot(rel, g_pose.headF);
    }

    if (noDir) {
        vf[0] = 0.0f; vf[1] = 0.0f; vf[2] = 1.0f;
        vu[0] = 0.0f; vu[1] = 1.0f; vu[2] = 0.0f;
    } else {
        vf[0] = HandDot(hf, headR);
        vf[1] = HandDot(hf, g_pose.headU);
        vf[2] = HandDot(hf, g_pose.headF);
        vu[0] = HandDot(hu, headR);
        vu[1] = HandDot(hu, g_pose.headU);
        vu[2] = HandDot(hu, g_pose.headF);
        HandNorm(vf);
        if (HandLen(vu) < 0.1f) {
            vu[0] = 0.0f; vu[1] = 1.0f; vu[2] = 0.0f;
        }
    }

    if (vp[2] < 0.18f) vp[2] = 0.18f;

    d = HandDot(vu, vf);
    vu[0] -= vf[0]*d; vu[1] -= vf[1]*d; vu[2] -= vf[2]*d;
    HandNorm(vu);
    HandCross(vu, vf, vr);
    HandNorm(vr);
    if (flip) {
        vu[0] = -vu[0]; vu[1] = -vu[1]; vu[2] = -vu[2];
        vr[0] = -vr[0]; vr[1] = -vr[1]; vr[2] = -vr[2];
    }
}

/* A box in the hand's own basis. Used for the palm and for the small finger
   and thumb slabs. Six textured faces, depth-tested against the world. */
static void HandDrawBoxBasis(void* dev, const float c[3], const float vf[3],
                             const float vu[3], const float vr[3],
                             float hw, float hh, float hl, DWORD color,
                             void* tex)
{
    const float u[4] = { 0, 1, 0, 1 };
    const float v[4] = { 0, 0, 1, 1 };
    const int faces[6][4] = {
        { 0, 1, 2, 3 },   /* back  */
        { 4, 5, 6, 7 },   /* front */
        { 0, 2, 4, 6 },   /* left  */
        { 1, 3, 5, 7 },   /* right */
        { 0, 1, 4, 5 },   /* bottom*/
        { 2, 3, 6, 7 }    /* top   */
    };
    float R[3], U[3], F[3];
    float corner[8][3];
    int f, i;

    R[0] = vr[0]*hw; R[1] = vr[1]*hw; R[2] = vr[2]*hw;
    U[0] = vu[0]*hh; U[1] = vu[1]*hh; U[2] = vu[2]*hh;
    F[0] = vf[0]*hl; F[1] = vf[1]*hl; F[2] = vf[2]*hl;

    for (i = 0; i < 8; ++i) {
        const float sr = (i & 1) ? 1.0f : -1.0f;
        const float su = (i & 2) ? 1.0f : -1.0f;
        const float sf = (i & 4) ? 1.0f : -1.0f;
        corner[i][0] = c[0] + R[0]*sr + U[0]*su + F[0]*sf;
        corner[i][1] = c[1] + R[1]*sr + U[1]*su + F[1]*sf;
        corner[i][2] = c[2] + R[2]*sr + U[2]*su + F[2]*sf;
    }

    for (f = 0; f < 6; ++f) {
        float q[4][3];
        for (i = 0; i < 4; ++i) {
            const int n = faces[f][i];
            q[i][0] = corner[n][0];
            q[i][1] = corner[n][1];
            q[i][2] = corner[n][2];
        }
        HandDrawQuad3D(dev, q, u, v, color, tex);
    }
}

/* A palm with four finger slabs and one thumb slab. The fingers and thumb are
   deliberately small and blocky: they exist so the roll/pitch/yaw of the hand
   is obvious at a glance, the way SixDOFinator's calibration hands are. */
static void HandDrawBox(void* dev, const float vp[3], const float vf[3],
                        const float vu[3], const float vr[3], BOOL isRight)
{
    const float hw = 0.016f, hh = 0.028f, hl = 0.028f, off = 0.023f;
    float c[3];
    int i;

    c[0] = vp[0] + vf[0]*(off + hl);
    c[1] = vp[1] + vf[1]*(off + hl);
    c[2] = vp[2] + vf[2]*(off + hl);

    /* Palm. */
    HandDrawBoxBasis(dev, c, vf, vu, vr, hw, hh, hl, 0xFFFFFFFFu, g_handTex);

    /* Four fingers: thin slabs forward of the palm, fanned slightly outward
       and with staggered lengths, so their direction is readable even when
       foreshortened. They sit toward the back of the hand so they stay
       visible above a grip. */
    {
        const float lens[4] = { 0.019f, 0.023f, 0.022f, 0.018f };
        for (i = 0; i < 4; ++i) {
            const float sideOff = (-1.5f + (float)i) * 0.0082f;
            float base[3], ff[3], fu[3], fr[3], fc[3], d, len;

            base[0] = c[0] + vf[0]*hl + vr[0]*sideOff + vu[0]*0.010f;
            base[1] = c[1] + vf[1]*hl + vr[1]*sideOff + vu[1]*0.010f;
            base[2] = c[2] + vf[2]*hl + vr[2]*sideOff + vu[2]*0.010f;

            ff[0] = vf[0] + vr[0]*sideOff*4.0f;
            ff[1] = vf[1] + vr[1]*sideOff*4.0f;
            ff[2] = vf[2] + vr[2]*sideOff*4.0f;
            HandNorm(ff);

            fu[0] = vu[0]; fu[1] = vu[1]; fu[2] = vu[2];
            d = fu[0]*ff[0] + fu[1]*ff[1] + fu[2]*ff[2];
            fu[0] -= ff[0]*d; fu[1] -= ff[1]*d; fu[2] -= ff[2]*d;
            HandNorm(fu);
            HandCross(fu, ff, fr);
            HandNorm(fr);

            len = lens[i];
            fc[0] = base[0] + ff[0]*(len + 0.001f);
            fc[1] = base[1] + ff[1]*(len + 0.001f);
            fc[2] = base[2] + ff[2]*(len + 0.001f);

            HandDrawBoxBasis(dev, fc, ff, fu, fr,
                             0.0034f, 0.0038f, len,
                             0xFFF4F4F4u, g_whiteTex);
        }
    }

    /* Thumb: on the inner side (right hand -> -right, left hand -> +right),
       angled forward and inward. */
    {
        const float side = isRight ? -1.0f : 1.0f;
        float tc[3], tf[3], tu[3], tr[3], len, d;

        tc[0] = c[0] + vr[0]*side*(hw + 0.004f)
                     + vu[0]*0.004f + vf[0]*(hl*0.18f);
        tc[1] = c[1] + vr[1]*side*(hw + 0.004f)
                     + vu[1]*0.004f + vf[1]*(hl*0.18f);
        tc[2] = c[2] + vr[2]*side*(hw + 0.004f)
                     + vu[2]*0.004f + vf[2]*(hl*0.18f);

        tf[0] = vf[0]*0.72f - vr[0]*side*0.69f;
        tf[1] = vf[1]*0.72f - vr[1]*side*0.69f;
        tf[2] = vf[2]*0.72f - vr[2]*side*0.69f;
        len = sqrtf(tf[0]*tf[0] + tf[1]*tf[1] + tf[2]*tf[2]);
        if (len > 0.001f) { tf[0] /= len; tf[1] /= len; tf[2] /= len; }
        else              { tf[0] = vf[0]; tf[1] = vf[1]; tf[2] = vf[2]; }

        tu[0] = vu[0]; tu[1] = vu[1]; tu[2] = vu[2];
        d = tu[0]*tf[0] + tu[1]*tf[1] + tu[2]*tf[2];
        tu[0] -= tf[0]*d; tu[1] -= tf[1]*d; tu[2] -= tf[2]*d;
        len = sqrtf(tu[0]*tu[0] + tu[1]*tu[1] + tu[2]*tu[2]);
        if (len > 0.001f) { tu[0] /= len; tu[1] /= len; tu[2] /= len; }
        else              { tu[0] = vu[0]; tu[1] = vu[1]; tu[2] = vu[2]; }
        HandCross(tu, tf, tr);
        HandNorm(tr);

        HandDrawBoxBasis(dev, tc, tf, tu, tr,
                         0.0050f, 0.0050f, 0.013f, 0xFFE8E8E8u, g_whiteTex);
    }
}

/* Continuous 3D beam: one triangle strip sharing a single camera-facing right
   vector, so segment joints do not gap. Width is scaled per point to keep the
   on-screen thickness roughly `pixels`. */
static void HandDrawBeam3DStrip(void* dev, const float* pts, int count,
                                float pixels, DWORD color)
{
    const float viewF[3] = { 0.0f, 0.0f, 1.0f };
    float dir[3], right[3], tanY;
    CompVert3D verts[50];
    int i;

    if (!dev || !g_whiteTex || count < 2 || count > 25) return;
    dir[0] = pts[(count - 1) * 3 + 0] - pts[0];
    dir[1] = pts[(count - 1) * 3 + 1] - pts[1];
    dir[2] = pts[(count - 1) * 3 + 2] - pts[2];
    HandNorm(dir);
    HandCross(viewF, dir, right);
    if (HandLen(right) < 0.001f) {
        right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    } else {
        HandNorm(right);
    }
    tanY = tanf(g_outFovY * 3.14159265358979323846f / 360.0f);
    if (tanY < 0.01f) tanY = 0.01f;

    for (i = 0; i < count; ++i) {
        float z = pts[i * 3 + 2];
        float w;
        if (z < 0.05f) z = 0.05f;
        w = pixels * z * 2.0f * tanY / 480.0f;
        verts[2*i].x = pts[i*3+0] - right[0]*w*0.5f;
        verts[2*i].y = pts[i*3+1] - right[1]*w*0.5f;
        verts[2*i].z = pts[i*3+2] - right[2]*w*0.5f;
        verts[2*i].color = color;
        verts[2*i].u = 0.0f;
        verts[2*i].v = (float)i / (float)(count - 1);
        verts[2*i+1].x = pts[i*3+0] + right[0]*w*0.5f;
        verts[2*i+1].y = pts[i*3+1] + right[1]*w*0.5f;
        verts[2*i+1].z = pts[i*3+2] + right[2]*w*0.5f;
        verts[2*i+1].color = color;
        verts[2*i+1].u = 1.0f;
        verts[2*i+1].v = (float)i / (float)(count - 1);
    }
    o_SetTexture(dev, 0, g_whiteTex);
    o_SetVertexShader(dev, 0x0142);
    o_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2 * count - 2,
                      verts, sizeof(CompVert3D));
}

static void HandMakeProjection(D3DMATRIX8* m, float fovY, float aspect,
                               float n, float f)
{
    const float pi = 3.14159265358979323846f;
    const float h = 1.0f / tanf(fovY * pi / 360.0f);
    ZeroMemory(m, sizeof(*m));
    m->m[0][0] = h / aspect;
    m->m[1][1] = h;
    m->m[2][2] = f / (f - n);
    m->m[2][3] = 1.0f;
    m->m[3][2] = -(f * n) / (f - n);
}

static void DrawHandAndLaserWorld(void* dev)
{
    float dvp[3], dvf[3], dvu[3], dvr[3];
    float ovp[3], ovf[3], ovu[3], ovr[3];
    const int segs = 24;
    BOOL domRight;
    CompState st;
    DWORD savedFvf;
    void* savedTex = NULL;
    D3DMATRIX8 view, proj, saveView, saveProj;
    D3DMATRIX8 world, saveWorld;
    BOOL haveView = FALSE, haveProj = FALSE, haveWorld = FALSE;
    int i;

    if (!dev || !InGameplay() || !g_posed || !g_pose.valid) return;
    if (!g_handTex || !g_whiteTex) return;

    domRight = (g_handedness == 0) ? TRUE : FALSE;
    if (domRight)
        HandViewPose(g_pose.rP, g_pose.rF, g_pose.rU, TRUE, g_handFlip,
                     dvp, dvf, dvu, dvr);
    else
        HandViewPose(g_pose.lP, g_pose.lF, g_pose.lU, FALSE, g_handFlip,
                     dvp, dvf, dvu, dvr);

    if (domRight)
        HandViewPose(g_pose.lP, g_pose.lF, g_pose.lU, FALSE, FALSE,
                     ovp, ovf, ovu, ovr);
    else
        HandViewPose(g_pose.rP, g_pose.rF, g_pose.rU, TRUE, FALSE,
                     ovp, ovf, ovu, ovr);

    /* Left grip held: mount the gun at the dominant hand and point it at the
       off hand. The dominant hand box turns with the gun (it is the gripping
       hand), while the off-hand box keeps its own controller pose. */
    {
        static BOOL wasTwoHanded = FALSE;
        if (g_twoHanded) {
            HandTwoHandedAim(ovp, dvp, dvf, dvu, dvr);
            if (!wasTwoHanded)
                Log("HAND     two-handed aim ON: dom %.3f %.3f %.3f  off %.3f %.3f %.3f"
                    "  aim %+.3f %+.3f %+.3f",
                    dvp[0], dvp[1], dvp[2], ovp[0], ovp[1], ovp[2],
                    dvf[0], dvf[1], dvf[2]);
        } else if (wasTwoHanded) {
            Log("HAND     two-handed aim off");
        }
        wasTwoHanded = g_twoHanded;
    }

    if ((g_frame % 120) == 0) {
        float dsx = 0, dsy = 0, osx = 0, osy = 0;
        BOOL dok = HandProject(dvp, &dsx, &dsy);
        BOOL ook = HandProject(ovp, &osx, &osy);
        LogD("HAND screen dom %s %.0f,%.0f  off %s %.0f,%.0f  (640x480)",
            dok ? "ok" : "BEHIND", dsx, dsy, ook ? "ok" : "BEHIND", osx, osy);
    }

    savedFvf = g_curFvf;
    CompSaveState(dev, &st);
    CompSetStates(dev);
    if (o_GetTexture) o_GetTexture(dev, 0, &savedTex);
    {
        /* The 3D HUD model pass may have set a small slot viewport. Draw the
           hands at the full game resolution. */
        D3DVIEWPORT8 vp;
        vp.X = 0; vp.Y = 0; vp.Width = 640; vp.Height = 480;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        o_SetViewport(dev, &vp);
    }
    o_SetRenderState(dev, D3DRS_ZENABLE, 1);
    o_SetRenderState(dev, D3DRS_ZWRITEENABLE, 0);
    o_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    o_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);

    /* Use the game's own 3D pipeline. The world vertices are already in camera
       space, so the view is identity; the captured game projection makes our
       depth values exactly comparable with the world's depth buffer. */
    ZeroMemory(&view, sizeof(view));
    view.m[0][0] = view.m[1][1] = view.m[2][2] = view.m[3][3] = 1.0f;
    /* AER: the hand/laser vertices are already in camera space, so the same
       eye shift becomes a translation of the view. */
    if (g_stereoMode == STEREO_AER && InGameplay())
        view.m[3][0] = (g_eyeRight ? -0.5f : 0.5f) * EyeIpd();
    /* World must stay identity: no leftover model matrix. Do not copy the
       translated view into it or the eye shift is applied twice. */
    ZeroMemory(&world, sizeof(world));
    world.m[0][0] = world.m[1][1] = world.m[2][2] = world.m[3][3] = 1.0f;
    if (g_haveProj) proj = g_lastProj;
    else HandMakeProjection(&proj, 65.0f, 4.0f / 3.0f, 0.1f, 80.0f);
    if (o_GetTransform) {
        haveView = (o_GetTransform(dev, D3DTS_VIEW, &saveView) == S_OK);
        haveProj = (o_GetTransform(dev, D3DTS_PROJECTION, &saveProj) == S_OK);
        haveWorld = (o_GetTransform(dev, D3DTS_WORLD, &saveWorld) == S_OK);
    }
    if (o_SetTransform) {
        o_SetTransform(dev, D3DTS_WORLD, &world);
        o_SetTransform(dev, D3DTS_VIEW, &view);
        o_SetTransform(dev, D3DTS_PROJECTION, &proj);
    }
    {
        static int matLog = 0;
        if (matLog < 1) {
            void* rt = NULL;
            void* ds = NULL;
            if (o_GetRenderTarget) o_GetRenderTarget(dev, &rt);
            if (o_GetDepthStencilSurface) o_GetDepthStencilSurface(dev, &ds);
            LogD("HAND     3D draw #%d frame %ld: draw %ld idx %ld begin %ld end %ld"
                "  target=%p gameRT=%p  depth=%p gameDepth=%p",
                matLog, g_frame, g_drawsThisFrame, g_drawsIDX,
                g_beginThisFrame, g_endThisFrame, rt, g_gameRTSurf, ds, g_gameDepth);
            if (rt) ReleaseIUnknown(rt);
            if (ds) ReleaseIUnknown(ds);
            ++matLog;
            if (matLog == 1) {
                LogD("HAND     3D draw: game view %s, projection %s",
                    haveView ? "read" : "unavailable",
                    g_haveProj ? "captured" : "built");
                if (haveView) LogMatrix("HAND view", &saveView);
                LogMatrix("HAND proj", &proj);
            }
        }
    }

    /* Both hands as solid 3D boxes. Z-write makes the nearest face win.
       The gun model and muzzle effects are the game's own assets; they will
       be duplicated onto the hand in a later pass. */
    o_SetRenderState(dev, D3DRS_ZWRITEENABLE, 1);
    HandDrawBox(dev, ovp, ovf, ovu, ovr, !domRight);
    {
        float hvp[3];
        hvp[0] = dvp[0] + dvr[0]*g_handAdj[0] + dvu[0]*g_handAdj[1] + dvf[0]*g_handAdj[2];
        hvp[1] = dvp[1] + dvr[1]*g_handAdj[0] + dvu[1]*g_handAdj[1] + dvf[1]*g_handAdj[2];
        hvp[2] = dvp[2] + dvr[2]*g_handAdj[0] + dvu[2]*g_handAdj[1] + dvf[2]*g_handAdj[2];
        HandDrawBox(dev, hvp, dvf, dvu, dvr, domRight);
    }
    if (g_partN > 0 && !g_weaponEmpty && !g_needParts)
        HandDrawCapturedGun(dev, dvp, dvf, dvu, dvr);
    o_SetRenderState(dev, D3DRS_ZWRITEENABLE, 0);

    /* Laser from the dominant hand. Segmented for depth. */
    if (!g_zoomOn) {
        float beam[25 * 3];
        for (i = 0; i <= segs; ++i) {
            const float t = (float)i / (float)segs;
            beam[i*3+0] = dvp[0] + dvf[0]*(0.20f + 30.0f*t);
            beam[i*3+1] = dvp[1] + dvf[1]*(0.20f + 30.0f*t);
            beam[i*3+2] = dvp[2] + dvf[2]*(0.20f + 30.0f*t);
        }
        HandDrawBeam3DStrip(dev, beam, segs + 1, 6.0f, 0x50FF2020u);
        HandDrawBeam3DStrip(dev, beam, segs + 1, 2.0f, 0xFFFF3030u);
    }

    /* Muzzle flash / smoke are the game's own effects; we do not draw our
       own. g_flashFrames stays as a "shot recently" counter for the pass
       that will move those effects onto the hand. */
    if (g_flashFrames > 0) { /* reserved for the game-effect relocation */ }

    if (o_SetTransform) {
        if (haveWorld) o_SetTransform(dev, D3DTS_WORLD, &saveWorld);
        if (haveView) o_SetTransform(dev, D3DTS_VIEW, &saveView);
        if (haveProj) o_SetTransform(dev, D3DTS_PROJECTION, &saveProj);
    }
    if (o_SetTexture) o_SetTexture(dev, 0, savedTex);
    if (savedTex) ReleaseIUnknown(savedTex);
    CompRestoreState(dev, &st);
    if (savedFvf) o_SetVertexShader(dev, savedFvf);
}

static void ReleaseSavedTargets(void);
static void BindGameRenderTarget(void* dev);
static void BindBackBuffer(void* dev);

static void ReleaseCompositeResources(void)
{
    ReleaseSavedTargets();
    if (g_gameRTSurf) { ReleaseIUnknown(g_gameRTSurf); g_gameRTSurf = NULL; }
    if (g_gameDepth)  { ReleaseIUnknown(g_gameDepth);  g_gameDepth  = NULL; }
    if (g_gameRT)     { ReleaseIUnknown(g_gameRT);     g_gameRT     = NULL; }
    if (g_frameSurf)  { ReleaseIUnknown(g_frameSurf);  g_frameSurf  = NULL; }
    if (g_frameTex)   { ReleaseIUnknown(g_frameTex);   g_frameTex   = NULL; }
    if (g_hudTex)     { ReleaseIUnknown(g_hudTex);     g_hudTex     = NULL; }
    if (g_panelTex)   { ReleaseIUnknown(g_panelTex);   g_panelTex   = NULL; }
    if (g_handTex)    { ReleaseIUnknown(g_handTex);    g_handTex    = NULL; }
    if (g_whiteTex)   { ReleaseIUnknown(g_whiteTex);   g_whiteTex   = NULL; }
    if (g_gunTex)     { ReleaseIUnknown(g_gunTex);     g_gunTex     = NULL; }
    if (g_clone.tex)  { ReleaseIUnknown(g_clone.tex);  g_clone.tex  = NULL; }
    g_cloneValid = FALSE;
    g_hudTexW = g_hudTexH = 0;
    g_hudValid = FALSE;
    g_gameRTActive = FALSE;
    g_compositeActive = FALSE;
}

static void* HandMakeTexture(void* dev, int w, int h, const DWORD* pixels)
{
    void* tex = NULL;
    D3DLOCKED_RECT8 lr;
    HRESULT hr;
    int y;

    if (!dev || !pixels || !o_CreateTexture) return NULL;
    hr = o_CreateTexture(dev, (UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
                         D3DPOOL_MANAGED, &tex);
    if (FAILED(hr) || !tex) {
        Log("HAND     CreateTexture(%dx%d) failed hr=0x%08lX", w, h, hr);
        return NULL;
    }
    if (TexLock(tex, 0, &lr, NULL, 0) != S_OK || !lr.pBits) {
        Log("HAND     LockRect(%dx%d) failed", w, h);
        ReleaseIUnknown(tex);
        return NULL;
    }
    for (y = 0; y < h; ++y)
        memcpy((BYTE*)lr.pBits + (SIZE_T)y * lr.Pitch,
               pixels + (SIZE_T)y * w, (SIZE_T)w * 4);
    TexUnlock(tex, 0);
    return tex;
}

static void EnsureHandTextures(void* dev)
{
    DWORD pix[64 * 64];
    DWORD white = 0xFFFFFFFF;
    int x, y;

    if (g_handTex && g_whiteTex) return;

    if (g_handTex)  { ReleaseIUnknown(g_handTex);  g_handTex = NULL; }
    if (g_whiteTex) { ReleaseIUnknown(g_whiteTex); g_whiteTex = NULL; }

    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 64; ++x) {
            int shade = 30 + (63 - y) * 45 / 63;
            if (y < 6 || y > 57 || x < 6 || x > 57) shade = 15;
            if (shade > 120) shade = 120;
            pix[y * 64 + x] = (0xFFu << 24) |
                              ((DWORD)shade << 16) |
                              ((DWORD)shade << 8) |
                               (DWORD)shade;
        }
    }
    g_handTex = HandMakeTexture(dev, 64, 64, pix);
    g_whiteTex = HandMakeTexture(dev, 1, 1, &white);
    if (!g_handTex || !g_whiteTex) {
        Log("HAND     hand/white texture creation failed; laser disabled");
        if (g_handTex)  { ReleaseIUnknown(g_handTex);  g_handTex = NULL; }
        if (g_whiteTex) { ReleaseIUnknown(g_whiteTex); g_whiteTex = NULL; }
    } else {
        Log("HAND     hand texture 64x64 + white 1x1 ready");
    }
}

static BOOL EnsureCompositeResources(void* dev)
{
    DWORD fmt;
    HRESULT hr;

    if (!dev || !o_CreateTexture || !o_CreateDepthStencilSurface ||
        !o_SetRenderTarget || !o_GetRenderTarget || !o_GetDepthStencilSurface ||
        !o_SetViewport || !o_GetBackBuffer || !o_CopyRects ||
        !o_CreateImageSurface)
        return FALSE;
    if (g_bbW < 640 || g_bbH < 480)
        return FALSE;
    if (g_bbW == 640 && g_bbH == 480) {
        Log("COMPOSITE not needed: back buffer is still 640x480");
        return FALSE;
    }
    if (!g_gameRT || !g_gameRTSurf || !g_gameDepth) {
        fmt = g_bbFormat ? g_bbFormat : D3DFMT_X8R8G8B8;
        hr = o_CreateTexture(dev, 640, 480, 1, D3DUSAGE_RENDERTARGET,
                             fmt, D3DPOOL_DEFAULT, &g_gameRT);
        if (FAILED(hr) || !g_gameRT) {
            Log("COMPOSITE CreateTexture(640x480 render target fmt=%lu)"
                " failed hr=0x%08lX", fmt, hr);
            g_gameRT = NULL;
            return FALSE;
        }
        hr = TexGetSurfaceLevel(g_gameRT, 0, &g_gameRTSurf);
        if (FAILED(hr) || !g_gameRTSurf) {
            Log("COMPOSITE GetSurfaceLevel(render target) failed hr=0x%08lX", hr);
            ReleaseCompositeResources();
            return FALSE;
        }
        hr = o_CreateDepthStencilSurface(dev, 640, 480, D3DFMT_D16, 0,
                                         &g_gameDepth);
        if (FAILED(hr) || !g_gameDepth) {
            Log("COMPOSITE CreateDepthStencilSurface(640x480 D16)"
                " failed hr=0x%08lX", hr);
            ReleaseCompositeResources();
            return FALSE;
        }
    }
    g_compositeActive = TRUE;
    g_gameRTFirstScene = TRUE;
    EnsureHandTextures(dev);
    BindGameRenderTarget(dev);
    Log("COMPOSITE active: back buffer %ux%u, game render target 640x480"
        " fmt=%lu", g_bbW, g_bbH, g_bbFormat);
    return TRUE;
}

static void CompositeFrame(void* dev)
{
    CompState st;
    D3DVIEWPORT8 vp;
    DWORD savedFvf = g_curFvf;
    int hw, hh, gw, gh, gx, gy;

    if (!g_compositeActive || !dev || !g_gameRT)
        return;
    if (g_gameRTActive)
        return;     /* EndScene should have restored the back buffer */

    CompSaveState(dev, &st);
    {
        static int vpLogs = 0;
        if (vpLogs++ == 0 && st.haveVp)
            LogD("COMPOSITE game viewport %ux%u at %u,%u (game RT 640x480)",
                st.vp.Width, st.vp.Height, st.vp.X, st.vp.Y);
    }
    if (o_BeginScene(dev) != S_OK) {
        CompRestoreState(dev, &st);
        DrawSyncPixel(dev);
        return;
    }
    vp.X = 0; vp.Y = 0; vp.Width = g_bbW; vp.Height = g_bbH;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    o_SetViewport(dev, &vp);
    CompSetStates(dev);

    /* Default: keep the game's default projection/FOV and show its native 4:3
       frame. On a 4:3 container (1280x960) the rectangle fills the window
       exactly - no bars, no crop, no stretch. On another aspect the letterbox
       is the honest way to keep the FOV unchanged.
       Opt-in experiment (`patchAspect 1`): the projection aspect is widened
       to the container, and the frame is stretched full-window instead. */
    if (g_projPatched) {
        gw = (int)g_bbW;
        gh = (int)g_bbH;
        gx = 0;
        gy = 0;
        o_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 0.0f, 0);
        {
            static int fillLog = 0;
            if (fillLog++ == 0)
                Log("COMPOSITE fill: %ux%u <- 640x480 anamorphic"
                    " (projection widened)", g_bbW, g_bbH);
        }
    } else {
        gw = (int)g_bbW;
        gh = (gw * 3) / 4;
        if (gh > (int)g_bbH) {
            gh = (int)g_bbH;
            gw = (gh * 4) / 3;
        }
        if (gw < 1) gw = 1;
        if (gh < 1) gh = 1;
        gx = ((int)g_bbW - gw) / 2;
        gy = ((int)g_bbH - gh) / 2;
        o_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 0.0f, 0);
        {
            static int fitLog = 0;
            if (fitLog++ == 0)
                Log("COMPOSITE native 4:3: %ux%u -> %dx%d at %d,%d"
                    " (bars %d,%d), default FOV", g_bbW, g_bbH, gw, gh,
                    gx, gy, (int)g_bbW - gw, (int)g_bbH - gh);
        }
    }

    o_SetTexture(dev, 0, g_gameRT);
    o_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    CompDrawQuad(dev, (float)gx, (float)gy,
                 (float)(gx + gw), (float)(gy + gh),
                 0.0f, 0.0f, 1.0f, 1.0f);

    if (g_customHud && HudShouldDraw() && UpdateHudTexture(dev)) {
        hw = (int)g_bbW / 3;
        hh = (int)g_bbH / 3;
        o_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);
        o_SetTexture(dev, 0, g_panelTex);
        CompDrawQuad(dev, (float)hw, (float)hh,
                     (float)(hw * 2), (float)(hh * 2),
                     0.0f, 0.0f,
                     (float)g_hudDrawW / (float)g_hudTexW,
                     (float)g_hudDrawH / (float)g_hudTexH);
        o_SetTexture(dev, 0, g_hudTex);
        CompDrawQuad(dev, (float)hw, (float)hh,
                     (float)(hw * 2), (float)(hh * 2),
                     0.0f, 0.0f,
                     (float)g_hudDrawW / (float)g_hudTexW,
                     (float)g_hudDrawH / (float)g_hudTexH);
    }

    o_EndScene(dev);

    /* Outside our scene, still before Present, and before restoring the game's
       scissor/clip state: the red square must survive the composite. */
    DrawSyncPixel(dev);

    CompRestoreState(dev, &st);
    if (savedFvf) o_SetVertexShader(dev, savedFvf);
}

/* G: save the composited back buffer to a BMP next to the DLL. This is the
   only reliable way to inspect the frame on this box - screen captures of the
   D3D window come back black. */
static void DumpBackBuffer(void* dev)
{
    void* bb = NULL;
    void* sys = NULL;
    RECT r;
    POINT pt;
    D3DLOCKED_RECT8 lr;
    char path[MAX_PATH];
    FILE* f;
    DWORD rowBytes;
    int y;

    if (g_grabFrames <= 0) return;
    if (--g_grabFrames != 0) return;
    if (!o_GetBackBuffer || !o_CreateImageSurface || !o_CopyRects) return;

    if (g_bbFormat != D3DFMT_A8R8G8B8 && g_bbFormat != D3DFMT_X8R8G8B8) {
        LogD("DUMP     unsupported back-buffer format %lu", g_bbFormat);
        return;
    }
    if (o_GetBackBuffer(dev, 0, 0, &bb) != S_OK || !bb) {
        LogD("DUMP     GetBackBuffer failed");
        return;
    }
    if (o_CreateImageSurface(dev, g_bbW, g_bbH, g_bbFormat, &sys) != S_OK ||
        !sys) {
        LogD("DUMP     CreateImageSurface failed");
        ReleaseIUnknown(bb);
        return;
    }
    r.left = 0; r.top = 0; r.right = (LONG)g_bbW; r.bottom = (LONG)g_bbH;
    pt.x = 0; pt.y = 0;
    if (o_CopyRects(dev, bb, &r, 1, sys, &pt) != S_OK) {
        LogD("DUMP     CopyRects(back buffer -> system surface) failed");
        ReleaseIUnknown(sys); ReleaseIUnknown(bb);
        return;
    }
    if (SurfLock(sys, &lr, NULL, 0) != S_OK || !lr.pBits) {
        LogD("DUMP     LockRect(system surface) failed");
        ReleaseIUnknown(sys); ReleaseIUnknown(bb);
        return;
    }

    _snprintf(path, MAX_PATH, "%sxops_frame_%ld.bmp", g_baseDir, g_frame);
    path[MAX_PATH - 1] = 0;
    f = fopen(path, "wb");
    if (f) {
        BITMAPFILEHEADER bf;
        BITMAPINFOHEADER bi;
        rowBytes = g_bbW * 4;
        ZeroMemory(&bf, sizeof(bf));
        ZeroMemory(&bi, sizeof(bi));
        bf.bfType = 0x4D42;
        bf.bfOffBits = sizeof(bf) + sizeof(bi);
        bf.bfSize = bf.bfOffBits + rowBytes * g_bbH;
        bi.biSize = sizeof(bi);
        bi.biWidth = (LONG)g_bbW;
        bi.biHeight = -(LONG)g_bbH;       /* top-down */
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = rowBytes * g_bbH;
        fwrite(&bf, sizeof(bf), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        for (y = 0; y < (int)g_bbH; ++y)
            fwrite((BYTE*)lr.pBits + (SIZE_T)y * lr.Pitch, 1, rowBytes, f);
        fclose(f);
        LogD("DUMP     wrote %s (%ux%u)  eye=%s", path, g_bbW, g_bbH,
             (g_stereoMode == STEREO_AER && InGameplay())
                 ? (g_eyeRight ? "right" : "left") : "mono");
    } else {
        LogD("DUMP     could not open %s", path);
    }

    SurfUnlock(sys);
    ReleaseIUnknown(sys);
    ReleaseIUnknown(bb);

    /* Pair grab: the next presented frame is the other eye under AER. */
    if (g_grabPair) {
        g_grabPair = FALSE;
        g_grabFrames = 1;
    }
}

static HRESULT __stdcall hkSetViewport(void* dev, const D3DVIEWPORT8* vp)
{
    if (g_gameRTActive && vp && g_bbW > 0 && g_bbH > 0) {
        D3DVIEWPORT8 v = *vp;
        if (v.Width > 640 || v.Height > 480) {
            v.X = (UINT)((float)v.X * 640.0f / (float)g_bbW);
            v.Y = (UINT)((float)v.Y * 480.0f / (float)g_bbH);
            v.Width = (UINT)((float)v.Width * 640.0f / (float)g_bbW);
            v.Height = (UINT)((float)v.Height * 480.0f / (float)g_bbH);
            {
                static int n = 0;
                if (n++ < 6)
                    LogD("VIEWPORT game %ux%u at %u,%u -> %ux%u at %u,%u",
                        vp->Width, vp->Height, vp->X, vp->Y,
                        v.Width, v.Height, v.X, v.Y);
            }
        }
        return o_SetViewport(dev, &v);
    }
    return o_SetViewport(dev, vp);
}

static HRESULT __stdcall hkSetRenderTarget(void* dev, void* rt, void* depth)
{
    if (g_gameRTActive && (rt != g_gameRTSurf || depth != g_gameDepth)) {
        static int n = 0;
        if (n++ < 4)
            Log("VIEWPORT game tried to change the render target inside the"
                " frame - keeping the 640x480 RT");
        return o_SetRenderTarget(dev, g_gameRTSurf, g_gameDepth);
    }
    return o_SetRenderTarget(dev, rt, depth);
}

static HRESULT __stdcall hkClear(void* dev, DWORD count, const D3DRECT8* rects,
                                 DWORD flags, DWORD color, float z, DWORD stencil)
{
    if (g_handDrawnThisFrame) {
        static int n = 0;
        if (n++ < 12) {
            void* rt = NULL;
            if (o_GetRenderTarget) o_GetRenderTarget(dev, &rt);
            LogD("CLEAR    after hand draw frame %ld: flags=0x%08lX"
                " color=0x%08lX z=%.3f target=%p gameRT=%p"
                "  draws=%ld idx=%ld begin=%ld end=%ld",
                g_frame, flags, color, z, rt, g_gameRTSurf,
                g_drawsThisFrame, g_drawsIDX, g_beginThisFrame, g_endThisFrame);
            if (rt) ReleaseIUnknown(rt);
        }
    }
    return o_Clear(dev, count, rects, flags, color, z, stencil);
}

static void ReleaseSavedTargets(void)
{
    if (g_savedRT)    { ReleaseIUnknown(g_savedRT);    g_savedRT = NULL; }
    if (g_savedDepth) { ReleaseIUnknown(g_savedDepth); g_savedDepth = NULL; }
}

static void RestoreGameRenderTarget(void* dev)
{
    D3DVIEWPORT8 vp;

    if (!g_gameRTActive) return;
    if (o_SetRenderTarget) {
        if (g_savedRT) {
            o_SetRenderTarget(dev, g_savedRT, g_savedDepth);
        } else {
            void* bb = NULL;
            if (o_GetBackBuffer && o_GetBackBuffer(dev, 0, 0, &bb) == S_OK) {
                o_SetRenderTarget(dev, bb, NULL);
                ReleaseIUnknown(bb);
            }
        }
    }
    ReleaseSavedTargets();
    g_gameRTActive = FALSE;
    if (o_SetViewport) {
        vp.X = 0; vp.Y = 0; vp.Width = g_bbW; vp.Height = g_bbH;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        o_SetViewport(dev, &vp);
    }
}

static void BindGameRenderTarget(void* dev)
{
    D3DVIEWPORT8 vp;
    if (!g_compositeActive || !g_gameRTSurf || !g_gameDepth ||
        !o_SetRenderTarget)
        return;
    if (o_SetRenderTarget(dev, g_gameRTSurf, g_gameDepth) == S_OK) {
        g_gameRTActive = TRUE;
        vp.X = 0; vp.Y = 0; vp.Width = 640; vp.Height = 480;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        o_SetViewport(dev, &vp);
    }
}

static void BindBackBuffer(void* dev)
{
    void* bb = NULL;
    D3DVIEWPORT8 vp;
    if (!g_compositeActive) return;
    if (o_GetBackBuffer && o_GetBackBuffer(dev, 0, 0, &bb) == S_OK && bb) {
        g_gameRTActive = FALSE;
        o_SetRenderTarget(dev, bb, NULL);
        ReleaseIUnknown(bb);
        vp.X = 0; vp.Y = 0; vp.Width = g_bbW; vp.Height = g_bbH;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        o_SetViewport(dev, &vp);
    }
}

static HRESULT __stdcall hkBeginScene(void* dev)
{
    ++g_beginThisFrame;
    g_handDrawnThisFrame = FALSE;
    g_3dIdx = 0;
    g_tailHead = 0;
    ReleaseHudBatches();
    /* A shot aimed at the hand must not move the rendered view: put the
       mouse/HMD view back before the world is drawn. In VR SteerTick also
       writes it, but in user-control mode SteerTick is off. */
    if (g_aimRestore) {
        SafeWrite(CamAddr(A_YAW),   g_aimSavedA[0]);
        SafeWrite(CamAddr(A_PITCH), g_aimSavedA[1]);
        if (g_aimSavedBok) {
            SafeWrite(CamAddr(B_YAW),   g_aimSavedB[0]);
            SafeWrite(CamAddr(B_PITCH), g_aimSavedB[1]);
        }
        g_aimRestore = FALSE;
    }
    if (g_flashFrames > 0) --g_flashFrames;
    /* The active slot's model is the second-to-last 3D draw. Follow the
       count so it stays correct across levels, and re-capture on a swap. */
    WatchWeaponSwap();
    if (g_3dPrevN > 2 && !g_3dManual)
        g_3dSel = g_3dPrevN - 2;
    /* Poll first, then steer, then let the probe wiggle on top - so a probe
       experiment always wins over the pose and stays usable while steering. */
    if (XrRunning()) {
        /* In user-control mode the numpad pose is the only source of truth,
           so ignore incoming 6DOF packets entirely - otherwise a synthetic
           tester or a live headset keeps moving the head and off-hand. */
        if (!g_manualRight)
            g_posed = XrPoll(&g_pose) ? TRUE : g_posed;
        /* In "0" desktop mode the numpad owns the right hand; re-apply after
           the poll so the XrApi packet does not overwrite it. */
        if (g_manualRight) ManualHandApply();
        /* Only drive the camera while the previous frame looked like gameplay.
           In the title, briefing/objective, menus and death screens the game
           owns its own camera and the runtime is in MODE_VR 0. */
        if (InGameplay()) SteerTick();
    }
    if (g_probePhase == PHASE_BEGINSCENE) ProbeTick();
    /* The 640x480 target is bound persistently outside Present, so draws the
       title screen makes before BeginScene land in it too. Do not clear here:
       a pre-scene title background must survive. */
    return o_BeginScene(dev);
}

static HRESULT __stdcall hkEndScene(void* dev)
{
    HRESULT hr;
    ++g_endThisFrame;
    /* Normally the hand/laser pass already ran at the first 3D HUD model draw.
       Only use this end-of-scene fallback if no HUD/model draw matched. */
    if (g_compositeActive && InGameplay() && !g_handDrawnThisFrame) {
        DrawHandAndLaserWorld(dev);
        g_handDrawnThisFrame = TRUE;
    }
    if (g_tailDump) {
        int n = g_tailHead < 12 ? g_tailHead : 12;
        int i;
        LogD("TAIL     last %d indexed draws (of %d):", n, g_tailHead);
        for (i = 0; i < n; ++i) {
            TailDraw* t = &g_tail[(g_tailHead - n + i) & 15];
            LogD("TAIL     idx %d  vb=%p  numV=%u prim=%u tex=%p",
                t->idx, t->vb, t->numV, t->prim, t->tex);
        }
        g_tailDump = FALSE;
    }
    if (g_needParts) {
        if (!g_weaponVB && g_tailHead > 0) {
            g_weaponVB = g_tail[(g_tailHead - 1) & 15].vb;
            LogD("HIDE     weapon VB %p; capturing parts next frame", g_weaponVB);
        } else if (g_weaponVB && g_partN > 0) {
            g_needParts = FALSE;
            LogD("HIDE     assembled %d part(s) for '%s'",
                g_partN, ActiveWeaponName());
        }
    }
    g_3dPrevN = g_3dIdx;
    hr = o_EndScene(dev);
    /* Keep the game target bound - the title background is drawn between
       scenes. The back buffer is bound only in Present. */
    if (!g_compositeActive) DrawSyncPixel(dev);
    return hr;
}

static HRESULT __stdcall hkPresent(void* dev, const RECT* a, const RECT* b, HWND c, void* d)
{
    /* Re-apply the borderless full-display window every frame. Cheap when it
       is already correct; repairs it when the engine resets it. Done before
       Present so the frame lands in the new client rect. */
    FitWindow(g_gameWnd ? g_gameWnd : g_wnd);

    /* One automatic draw-order dump once the session is in its stride, so the
       HUD draws can be identified by path/vertices rather than guessed from
       stride. V arms another dump on demand. */
    if (!g_dumpArmed && g_frame >= 600) {
        g_dumpArmed = TRUE;
        g_dumpDraws = 300;
        LogD("DRAWDump auto-armed at frame %ld", g_frame);
        PlayerDump("auto");
    }

    /* D3D8 does NOT stretch a smaller back buffer to a larger client rect.
       Confirmed on screen 5 Nov 2026: a 1920x1080 borderless window showed the
       640x480 image 1:1 in the top-left with black everywhere else. Passing
       the client rect as the destination makes Present stretch it, which is
       what the fallback in the plan assumes. Respect any rect the game sets -
       this only fills in the null case. */
    /* The game's own Present passes an explicit 640x480 destination rect
       (0x403AFE/0x403B0E builds it), not NULL. Replacing only a NULL dest is
       why the earlier windowed stretch did nothing. Override the rect the game
       gives us with the full client rect, which makes D3D8 stretch the 640x480
       software output to the window. In fullscreen the client is 640x480, so
       this is a no-op there. */
    RECT dst;
    const RECT* pDest = b;
    const RECT* pSrc  = a;
    if (g_gameWnd && GetClientRect(g_gameWnd, &dst) &&
        dst.right > 0 && dst.bottom > 0) {
        g_displayScaleX = (float)dst.right / 640.0f;
        g_displayScaleY = (float)dst.bottom / 480.0f;
        if (b && (b->right - b->left != dst.right ||
                  b->bottom - b->top != dst.bottom)) {
            static int destLogs = 0;
            if (destLogs < 3) {
                Log("PRESENT dest override: %ldx%ld -> %ldx%ld",
                    b->right - b->left, b->bottom - b->top,
                    dst.right, dst.bottom);
                ++destLogs;
            }
        }
        if (g_deviceWindowed && g_fitWindow) pDest = &dst;
    }
    /* With the target-resolution composite the back buffer already holds the
       finished, full-window image: present all of it, unscaled, into the whole
       window. The game's own 640x480 source/dest rects must not crop it. */
    if (g_compositeActive) {
        pSrc = NULL;
        pDest = NULL;
        g_displayScaleX = (float)g_bbW / 640.0f;
        g_displayScaleY = (float)g_bbH / 480.0f;
    }

    // Per-frame summary. The Begin/End counts answer "is EndScene once per
    // presented frame?", which decides where the sync square goes.
    if (Verbose()) {
        LogD("[f%ld] PRESENT  BeginScene=%ld EndScene=%ld DrawPrimitiveUP=%ld SetTransform=%ld"
            "  (view=%ld proj=%ld world=%ld other=%ld)",
            g_frame, g_beginThisFrame, g_endThisFrame, g_drawsThisFrame, g_xformThisFrame,
            g_xfView, g_xfProj, g_xfWorld, g_xfOther);
        LogD("[f%ld] DRAWPATH UP=%ld  DrawPrimitive=%ld  DrawIndexedPrimitive=%ld",
            g_frame, g_drawsUP, g_drawsVB, g_drawsIDX);

        // What the frame's geometry actually was.
        for (int i = 0; i < g_drawKinds; ++i)
            LogD("[f%ld] DRAWS    FVF 0x%04lX stride=%u  x%ld   %s",
                g_frame, g_draws[i].fvf, g_draws[i].stride, g_draws[i].count,
                FvfKind(g_draws[i].fvf));

        // Where the camera is, according to the most recent view matrix - even
        // if that matrix was set many frames ago. A frozen eye position while
        // the player is moving is the proof that the camera lives elsewhere.
        if (g_haveView) {
            LogD("[f%ld] CAMERA   last VIEW set at frame %ld%s",
                g_frame, g_lastViewAt,
                (g_lastViewAt < g_frame - 1) ? "   <- STALE: not set this frame" : "");
            LogViewDecomposed(&g_lastView);
        } else {
            LogD("[f%ld] CAMERA   no VIEW matrix has ever been set", g_frame);
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
        if (ib[0]) LogD("[f%ld] INPUT    %s", g_frame, ib);
    }

    LogWindowIfChanged(c ? c : g_wnd);
    /* Keep sending every frame even when MODE_VR is 0, or the stream cannot
       be turned back on - the mode is only ever re-read from a packet we
       send. Mono first, per the settled decisions. */
    if (XrRunning()) {
        float hapL = 0.0f, hapR = 0.0f;
        /* MODE_VR 0 on the title, briefing/objective, menus and death screens:
           the runtime stops the pose stream, restores its own mouse control
           and presents flat. MODE_VR 1 only when the previous frame had the
           gameplay draw signature. Keep sending the packet in both modes, or
           the stream cannot be turned back on. */
        const int modeVr = InGameplay() ? XR_VR_ON : XR_VR_OFF;
        static int lastModeVr = -1;
        if (modeVr != lastModeVr) {
            Log("XR       MODE_VR -> %d  (%s)  last frame: %ld xforms,"
                " %ld indexed, %ld UP",
                modeVr,
                modeVr ? "in-game: immersive, pose stream on"
                       : "title/objective/menu/death: flat, runtime mouse",
                g_lastXforms, g_lastIndexed, g_lastUP);
            StateLog("MODE_VR");
            lastModeVr = modeVr;
        }
        /* Haptics ride in the first two fields: a duration in frames, per
           hand. Updated from the magazine counter, gameplay only. */
        HapticTick(modeVr == XR_VR_ON, &hapL, &hapR);
        {
            /* AER only exists in gameplay; menus stay mono so their 2D UI is
               not alternated between eyes. SBS is not implemented yet. */
            const int mode3d =
                (g_stereoMode == STEREO_AER && modeVr == XR_VR_ON)
                    ? XR_3D_AER : XR_3D_MONO;
            if (modeVr != XR_VR_ON) g_eyeRight = FALSE;
            XrSendFrame(hapL, hapR, modeVr, mode3d, g_outFovX, g_outFovY);
            if (g_stereoMode == STEREO_AER && modeVr == XR_VR_ON) {
                if ((g_frame % 120) == 0)
                    LogD("STEREO   AER eye=%s  ipd=%.3f m  mode3d=%d",
                         g_eyeRight ? "right" : "left", EyeIpd(), mode3d);
            }
        }
    }

    if ((g_frame % 120) == 0 && g_frame > 60) { WatchDump("periodic"); SteerLog(); }
    BallisticTick();
    IdleTick();
    if ((g_frame % 5) == 0) SampleRanges();
    HuntTick();

    g_lastXforms  = g_xformThisFrame;
    g_lastIndexed = g_drawsIDX;
    g_lastUP      = g_drawsUP;
    for (int i = 0; i < IN_COUNT; ++i) g_inFrame[i] = 0;
    g_beginThisFrame = g_endThisFrame = g_drawsThisFrame = g_xformThisFrame = 0;
    g_xfView = g_xfProj = g_xfWorld = g_xfOther = 0;
    g_drawsUP = g_drawsVB = g_drawsIDX = 0;
    g_drawKinds = 0;
    ++g_frame;

    {
        HRESULT hr;
        if (g_compositeActive) {
            BindBackBuffer(dev);
            CompositeFrame(dev);
            if (g_grabFrames > 0) DumpBackBuffer(dev);
        }
        hr = o_Present(dev, pSrc, pDest, c, d);
        /* Leave the game target bound for the next frame, including any
           title/demo draws that happen before BeginScene. */
        if (g_compositeActive) BindGameRenderTarget(dev);
        /* Flip the AER eye only after this frame has been rendered, composited
           (including the blue eye flag) and presented. Flipping earlier made
           the flag disagree with the image. */
        if (g_stereoMode == STEREO_AER && InGameplay())
            g_eyeRight = !g_eyeRight;
        g_hudModelPass = FALSE;
        return hr;
    }
}

static void HookDevice(void* dev);

static HRESULT __stdcall hkReset(void* dev, D3DPRESENT_PARAMETERS8* pp)
{
    Log("");
    Log("RESET  requested %ux%u  windowed=%ld  fmt=%lu  flags=0x%08lX  (frame %ld)",
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        pp ? pp->Windowed : 0, pp ? pp->BackBufferFormat : 0,
        pp ? pp->Flags : 0, g_frame);

    /* A minimized/hidden fullscreen window has a lost device; Reset then fails
       with D3DERR_DEVICELOST. Restore the window before handing Reset through,
       and retry once if it still comes back lost. This is deliberately not a
       ShowWindow IAT hook: blocking the game's own minimize calls changed its
       startup state and made frame 1 fail instead. */
    HWND hw = g_gameWnd ? g_gameWnd : g_wnd;
    if (hw) {
        RECT wr;
        BOOL min = IsIconic(hw);
        if (!min && GetWindowRect(hw, &wr) && wr.left <= -30000) min = TRUE;
        if (min) {
            Log("RESET  window is minimized or hidden - restoring it");
            ShowWindow(hw, SW_RESTORE);
            SetForegroundWindow(hw);
            Sleep(250);
        }
    }

    if (pp) g_deviceWindowed = pp->Windowed ? TRUE : FALSE;

    D3DPRESENT_PARAMETERS8 orig;
    BOOL resized = FALSE;
    if (pp) {
        orig = *pp;
        ApplyTargetRes(pp);
        resized = (pp->BackBufferWidth != orig.BackBufferWidth ||
                   pp->BackBufferHeight != orig.BackBufferHeight) ? TRUE : FALSE;
        g_bbW = pp->BackBufferWidth;
        g_bbH = pp->BackBufferHeight;
        g_bbFormat = pp->BackBufferFormat ? pp->BackBufferFormat
                                          : D3DFMT_X8R8G8B8;
    }

    /* Default-pool resources do not survive Reset. */
    ReleaseCompositeResources();

    HRESULT hr = o_Reset(dev, pp);
    if (FAILED(hr) && resized) {
        Log("RESET  target-res reset failed hr=0x%08lX - retrying at %ux%u",
            hr, orig.BackBufferWidth, orig.BackBufferHeight);
        *pp = orig;
        g_bbW = orig.BackBufferWidth;
        g_bbH = orig.BackBufferHeight;
        g_bbFormat = orig.BackBufferFormat ? orig.BackBufferFormat
                                           : D3DFMT_X8R8G8B8;
        hr = o_Reset(dev, pp);
    }
    if (hr == (HRESULT)0x88760868 && hw) {          /* D3DERR_DEVICELOST */
        Log("RESET  DEVICELOST - restoring the window and retrying once");
        ShowWindow(hw, SW_RESTORE);
        SetForegroundWindow(hw);
        Sleep(400);
        hr = o_Reset(dev, pp);
        Log("RESET  retry hr=0x%08lX", hr);
    }
    Log("RESET  hr=0x%08lX", hr);
    // A Reset can replace the vtable in some implementations; re-patch cheaply.
    HookDevice(dev);
    FitWindow(g_gameWnd ? g_gameWnd : g_wnd);
    if (SUCCEEDED(hr)) EnsureCompositeResources(dev);
    return hr;
}

static void HookDevice(void* dev)
{
    if (!dev) return;
    void* tmp = NULL;

    /* Clear is used for the sync square and is also probed for clears that
       happen after the hand pass. */
    {
        void** vtbl = *(void***)dev;
        if (vtbl) {
            /* Read-only slots used by the target-resolution composite. */
            o_GetBackBuffer        = (PFN_GetBackBuffer)vtbl[VT_DEV_GETBACKBUFFER];
            o_GetStreamSource      = (PFN_GetStreamSource)vtbl[84];
            o_GetIndices           = (PFN_GetIndices)vtbl[86];
            o_DrawIndexedPrimitiveUP = (PFN_DrawIndexedPrimitiveUP)vtbl[VT_DEV_DRAWINDEXEDPRIMUP];
            o_GetTransform         = (PFN_GetTransform)vtbl[VT_DEV_GETTRANSFORM];
            o_CreateTexture        = (PFN_CreateTexture)vtbl[VT_DEV_CREATETEXTURE];
            o_CreateDepthStencilSurface = (PFN_CreateDepthStencilSurface)vtbl[VT_DEV_CREATEDEPTHSTENCIL];
            o_CreateImageSurface   = (PFN_CreateImageSurface)vtbl[VT_DEV_CREATEIMAGESURFACE];
            o_CopyRects            = (PFN_CopyRects)vtbl[VT_DEV_COPYRECTS];
            o_GetRenderTarget      = (PFN_GetRenderTarget)vtbl[VT_DEV_GETRENDERTARGET];
            o_GetDepthStencilSurface = (PFN_GetDepthStencilSurface)vtbl[VT_DEV_GETDEPTHSTENCIL];
            o_GetViewport          = (PFN_GetViewport)vtbl[VT_DEV_GETVIEWPORT];
            o_SetRenderState       = (PFN_SetRenderState)vtbl[VT_DEV_SETRENDERSTATE];
            o_GetRenderState       = (PFN_GetRenderState)vtbl[VT_DEV_GETRENDERSTATE];
            o_SetTexture           = (PFN_SetTexture)vtbl[VT_DEV_SETTEXTURE];
            o_GetTexture           = (PFN_GetTexture)vtbl[60];
            o_SetTextureStageState = (PFN_SetTextureStageState)vtbl[VT_DEV_SETTEXTURESTAGESTATE];
            o_GetTextureStageState = (PFN_GetTextureStageState)vtbl[VT_DEV_GETTEXTURESTAGESTATE];
        }
    }

    if (PatchSlot(dev, VT_DEV_SETRENDERTARGET, (void*)hkSetRenderTarget, &tmp) && tmp != (void*)hkSetRenderTarget) o_SetRenderTarget = (PFN_SetRenderTarget)tmp;
    if (PatchSlot(dev, VT_DEV_CLEAR,          (void*)hkClear,          &tmp) && tmp != (void*)hkClear)          o_Clear          = (PFN_Clear)tmp;
    if (PatchSlot(dev, VT_DEV_SETVIEWPORT,     (void*)hkSetViewport,     &tmp) && tmp != (void*)hkSetViewport)     o_SetViewport     = (PFN_SetViewport)tmp;

    if (PatchSlot(dev, VT_DEV_SETTRANSFORM,    (void*)hkSetTransform,    &tmp) && tmp != (void*)hkSetTransform) o_SetTransform    = (PFN_SetTransform)tmp;
    if (PatchSlot(dev, VT_DEV_PRESENT,         (void*)hkPresent,         &tmp) && tmp != (void*)hkPresent)       o_Present         = (PFN_Present)tmp;
    if (PatchSlot(dev, VT_DEV_BEGINSCENE,      (void*)hkBeginScene,      &tmp) && tmp != (void*)hkBeginScene)    o_BeginScene      = (PFN_BeginScene)tmp;
    if (PatchSlot(dev, VT_DEV_ENDSCENE,        (void*)hkEndScene,        &tmp) && tmp != (void*)hkEndScene)      o_EndScene        = (PFN_EndScene)tmp;
    if (PatchSlot(dev, VT_DEV_DRAWPRIMITIVEUP, (void*)hkDrawPrimitiveUP, &tmp) && tmp != (void*)hkDrawPrimitiveUP) o_DrawPrimitiveUP = (PFN_DrawPrimitiveUP)tmp;
    if (PatchSlot(dev, VT_DEV_DRAWINDEXEDPRIMUP, (void*)hkDrawIndexedPrimitiveUP, &tmp) && tmp != (void*)hkDrawIndexedPrimitiveUP) o_DrawIndexedPrimitiveUP = (PFN_DrawIndexedPrimitiveUP)tmp;
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
    LogD("CREATEDEVICE  called from IDA 0x%08IX", ToIda(_ReturnAddress()));
    if (pp) {
        g_deviceWindowed = pp->Windowed ? TRUE : FALSE;
        LogD("  backbuffer    %ux%u  count=%u  format=%lu",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferCount, pp->BackBufferFormat);
        LogD("  windowed      %ld        swapeffect=%lu  multisample=%lu",
            pp->Windowed, pp->SwapEffect, pp->MultiSampleType);
        LogD("  hDeviceWindow %p  hFocusWindow=%p", pp->hDeviceWindow, hFocus);
        /* Whichever of the two is set is the window the game considers its
           own, and so the handle GetForegroundWindow must hand back. */
        if (!g_gameWnd) g_gameWnd = pp->hDeviceWindow ? pp->hDeviceWindow : hFocus;
        LogD("  autodepth     %ld  fmt=%lu", pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat);
        LogD("  flags         0x%08lX%s", pp->Flags,
            (pp->Flags & 1) ? "  LOCKABLE_BACKBUFFER" : "  (back buffer NOT lockable)");
        LogD("  refresh       %u Hz  presentinterval=0x%08X",
            pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval);
        LogD("  behaviour     0x%08lX  devtype=%lu  adapter=%u", behaviour, devType, adapter);
    }

    HRESULT hr;
    if (pp) {
        D3DPRESENT_PARAMETERS8 orig = *pp;
        BOOL resized;
        ApplyTargetRes(pp);
        resized = (pp->BackBufferWidth != orig.BackBufferWidth ||
                   pp->BackBufferHeight != orig.BackBufferHeight) ? TRUE : FALSE;
        g_bbW = pp->BackBufferWidth;
        g_bbH = pp->BackBufferHeight;
        g_bbFormat = pp->BackBufferFormat ? pp->BackBufferFormat
                                          : D3DFMT_X8R8G8B8;
        hr = o_CreateDevice(self, adapter, devType, hFocus, behaviour, pp, outDev);
        if (FAILED(hr) && resized) {
            Log("CREATEDEVICE target-res create failed hr=0x%08lX -"
                " retrying at %ux%u", hr,
                orig.BackBufferWidth, orig.BackBufferHeight);
            *pp = orig;
            g_bbW = orig.BackBufferWidth;
            g_bbH = orig.BackBufferHeight;
            g_bbFormat = orig.BackBufferFormat ? orig.BackBufferFormat
                                               : D3DFMT_X8R8G8B8;
            hr = o_CreateDevice(self, adapter, devType, hFocus, behaviour,
                                pp, outDev);
        }
    } else {
        hr = o_CreateDevice(self, adapter, devType, hFocus, behaviour, pp, outDev);
    }
    Log("CREATEDEVICE  hr=0x%08lX  dev=%p", hr, (outDev && *outDev) ? *outDev : NULL);

    if (SUCCEEDED(hr) && outDev && *outDev) {
        HookDevice(*outDev);
        /* Stage 3: take the chrome off while we are outside the frame loop,
           so the first SetWindowPos is not re-entered through Present. */
        FitWindow(g_gameWnd ? g_gameWnd : (pp ? pp->hDeviceWindow : hFocus));
        EnsureCompositeResources(*outDev);
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
    LogD("Direct3DCreate8(SDKVersion=%u = 0x%X)  ->  %p  [0xDC = 220 = D3D8.1]",
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
        /* Capture fonts created before Direct3DCreate8. This is an IAT write,
           not LoadLibrary, so it is safe under the loader lock. */
        g_realCreateFontIndirectA = (PFN_CreateFontIndirectA)
            PatchIat(GetModuleHandleA(NULL), "CreateFontIndirectA",
                     (void*)hkCreateFontIndirectA);
        g_realDrawTextA = (PFN_DrawTextA)
            PatchIat(GetModuleHandleA(NULL), "DrawTextA", (void*)hkDrawTextA);
        g_realDrawTextW = (PFN_DrawTextW)
            PatchIat(GetModuleHandleA(NULL), "DrawTextW", (void*)hkDrawTextW);
    } else if (reason == DLL_PROCESS_DETACH) {
        ProbeRestore();
        XrStop();
        if (g_log) {
            Log("");
            Log("SHUTDOWN  after %ld frames, %d distinct SetTransform call sites",
                g_frame, g_siteCount);
            for (int i = 0; i < g_siteCount; ++i)
                LogD("  %-10s  IDA 0x%08IX", XformName(g_sites[i].type), g_sites[i].ida);
            LogD("");
            {
                int i, n = 0;
                for (i = 0; i < g_probeN; ++i) if (g_probeClass[i]) ++n;
                if (n) {
                    LogD("WHAT THE OWNER SAW  (%d of %d variables classified):", n, g_probeN);
                    for (i = 0; i < g_probeN; ++i) {
                        if (!g_probeClass[i]) continue;
                        LogD("  0x%08IX  %-38s  %s",
                            g_probeList[i], ClassName(g_probeClass[i]),
                            g_probeWhat[i] ? g_probeWhat[i] : "");
                    }
                    LogD("This is the map. Each line is an address whose effect was");
                    LogD("watched on screen and named - not inferred from a correlation.");
                    LogD("");
                }
            }
            LogD("INPUT totals over the session:");
            for (int i = 0; i < IN_COUNT; ++i)
                LogD("  %-18s %ld calls%s", kInputNames[i], g_inCalls[i],
                    g_inCalls[i] ? "   <- a live input path" : "");
            LogD("A per-frame call count means that function is the live input");
            LogD("path and we can answer it ourselves - no synthetic events.");
            fclose(g_log);
            g_log = NULL;
        }
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}

