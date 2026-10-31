// XOPS-WXR - Monocular 6DOF VR injection for X Operations 0.96
// Target: WinlatorXR XrAPI (monocular mode) - same style as HWXR / Shogo-VR-WXR
// Build: VS2019, DirectX SDK June 2010, MinHook (https://github.com/TsudaKageyu/minhook)
// Output: dinput.dll (XOPS loads DirectInput) - drop next to XOPS.exe in WinlatorXR container
//
// How it maps to your IDA finds:
// .rdata:0045D1DC Direct3DCreate8
// .rdata:00463BA0 __imp_Direct3DCreate8
// sub_4035D0 = InitD3D -> CreateDevice -> g_pDevice = dword_48C5BC (102 xrefs)
// We hook CreateDevice at vtable[15] (0x3C) to get the device, then SetTransform at ~0xB0 (index 44)

#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <cstdio>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d8.lib")
#pragma comment(lib, "d3dx8.lib")
#include "MinHook.h"

#pragma comment(lib, "libMinHook.x86.lib") // or .x86.lib for 32-bit XOPS

// --- XrAPI (WinlatorXR) ---
// WinlatorXR sends pose on UDP 7872, game replies on 7278, and compositor syncs via red pixel at (0,0)
// Docs: "Head pose and controller state go out on port 7872. The game replies on port 7278 with a string of floats."
// And: "paint a sync counter into the top left corner of its own image as a shade of red"

struct HMDState {
    std::atomic<float> px{0}, py{0}, pz{0};
    std::atomic<float> qx{0}, qy{0}, qz{0}, qw{1};
    std::atomic<bool> valid{false};
} g_hmd;

static SOCKET g_sockRecv = INVALID_SOCKET;
static SOCKET g_sockSend = INVALID_SOCKET;
static sockaddr_in g_sendAddr{};
static std::atomic<int> g_frameId{0};

// Minimal string-of-floats parser - WinlatorXR sends e.g. "px,py,pz,qx,qy,qz,qw,..."
void XrThread() {
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    g_sockRecv = socket(AF_INET, SOCK_DGRAM, 0);
    g_sockSend = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in recvAddr{}; recvAddr.sin_family = AF_INET;
    recvAddr.sin_port = htons(7872);
    recvAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(g_sockRecv, (sockaddr*)&recvAddr, sizeof(recvAddr));

    g_sendAddr.sin_family = AF_INET;
    g_sendAddr.sin_port = htons(7278);
    g_sendAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    char buf[2048];
    while(true) {
        int len = recv(g_sockRecv, buf, sizeof(buf)-1, 0);
        if(len > 0) {
            buf[len]=0;
            float px=0,py=0,pz=0,qx=0,qy=0,qz=0,qw=1;
            // Try binary float array first, then CSV string
            if(len >= 28) {
                float* f = (float*)buf;
                // Heuristic: if values look sane
                if(fabs(f[0]) < 10 && fabs(f[1]) < 10) {
                    px=f[0]; py=f[1]; pz=f[2];
                    if(len >= 28) { qx=f[3]; qy=f[4]; qz=f[5]; qw=f[6]; }
                } else {
                    sscanf_s(buf, "%f,%f,%f,%f,%f,%f,%f", &px,&py,&pz,&qx,&qy,&qz,&qw);
                }
            } else {
                sscanf_s(buf, "%f,%f,%f,%f,%f,%f,%f", &px,&py,&pz,&qx,&qy,&qz,&qw);
            }
            g_hmd.px = px; g_hmd.py = py; g_hmd.pz = pz;
            g_hmd.qx = qx; g_hmd.qy = qy; g_hmd.qz = qz; g_hmd.qw = qw;
            g_hmd.valid = true;
        }
    }
}

// --- D3D8 Hooks ---
typedef IDirect3D8* (WINAPI* PFN_Direct3DCreate8)(UINT);
typedef HRESULT (WINAPI* PFN_CreateDevice)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
typedef HRESULT (WINAPI* PFN_SetTransform)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*);
typedef HRESULT (WINAPI* PFN_Present)(IDirect3DDevice8*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);

PFN_Direct3DCreate8 oDirect3DCreate8 = nullptr;
PFN_CreateDevice oCreateDevice = nullptr;
PFN_SetTransform oSetTransform = nullptr;
PFN_Present oPresent = nullptr;

// Forward
HRESULT WINAPI hkPresent(IDirect3DDevice8* self, CONST RECT* src, CONST RECT* dst, HWND wnd, CONST RGNDATA* rg);
HRESULT WINAPI hkSetTransform(IDirect3DDevice8* self, D3DTRANSFORMSTATETYPE Type, CONST D3DMATRIX* mat);
HRESULT WINAPI hkCreateDevice(IDirect3D8* self, UINT Adapter, D3DDEVTYPE Type, HWND hFocus, DWORD Behave, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** outDev);
IDirect3D8* WINAPI hkDirect3DCreate8(UINT sdkVer);

HRESULT WINAPI hkSetTransform(IDirect3DDevice8* self, D3DTRANSFORMSTATETYPE Type, CONST D3DMATRIX* mat) {
    if(g_hmd.valid && Type == D3DTS_VIEW && mat) {
        // Monocular 6DOF: add HMD translation to view matrix.
        // XOPS units ~ 50x smaller than meters, tune scale.
        const float scale = 50.0f;
        D3DMATRIX m = *mat;
        // m._41, _42, _43 = translation
        m._41 += g_hmd.px * scale;
        m._42 += g_hmd.py * scale;
        m._43 += g_hmd.pz * scale;

        // Optional: apply HMD rotation to basis vectors (AX,AY,AZ)
        // Build quat->matrix and multiply
        D3DXMATRIX rot;
        D3DXQUATERNION q(g_hmd.qx, g_hmd.qy, g_hmd.qz, g_hmd.qw);
        D3DXMatrixRotationQuaternion(&rot, &q);
        D3DXMATRIX result;
        D3DXMatrixMultiply(&result, (D3DXMATRIX*)&m, &rot);
        return oSetTransform(self, Type, &result);
    }
    return oSetTransform(self, Type, mat);
}

HRESULT WINAPI hkPresent(IDirect3DDevice8* self, CONST RECT* src, CONST RECT* dst, HWND wnd, CONST RGNDATA* rg) {
    // Paint sync counter for WinlatorXR compositor: top-left pixel = red shade = frameId % 255
    int frame = g_frameId.fetch_add(1) % 255;
    // Try to lock backbuffer and write pixel (0,0)
    IDirect3DSurface8* bb = nullptr;
    if(SUCCEEDED(self->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DLOCKED_RECT lr;
        if(SUCCEEDED(bb->LockRect(&lr, nullptr, 0))) {
            // Assuming X8R8G8B8: 0x00RRGGBB
            if(lr.pBits) {
                DWORD* p = (DWORD*)lr.pBits;
                p[0] = (frame << 16); // R = frame
            }
            bb->UnlockRect();
        }
        bb->Release();
    }
    // Reply on 7278 so WinlatorXR knows we rendered
    if(g_sockSend != INVALID_SOCKET) {
        char reply[64];
        sprintf_s(reply, "%d,0,0,0", frame);
        sendto(g_sockSend, reply, (int)strlen(reply), 0, (sockaddr*)&g_sendAddr, sizeof(g_sendAddr));
    }
    return oPresent(self, src, dst, wnd, rg);
}

HRESULT WINAPI hkCreateDevice(IDirect3D8* self, UINT Adapter, D3DDEVTYPE Type, HWND hFocus, DWORD Behave, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** outDev) {
    HRESULT hr = oCreateDevice(self, Adapter, Type, hFocus, Behave, pp, outDev);
    if(SUCCEEDED(hr) && outDev && *outDev) {
        IDirect3DDevice8* dev = *outDev;
        void** vtbl = *(void***)dev;
        // vtable[15] = Present (0x3C), vtable[44] = SetTransform (0xB0) - verify in your IDA (call [ecx+B0h])
        // If your XOPS build differs, adjust index by looking at your SetTransform xref.
        MH_CreateHook(vtbl[15], &hkPresent, (void**)&oPresent);
        MH_CreateHook(vtbl[44], &hkSetTransform, (void**)&oSetTransform);
        MH_EnableHook(MH_ALL_HOOKS);

        // Start XrAPI UDP thread once
        static bool started = false;
        if(!started) { started = true; std::thread(XrThread).detach(); }
    }
    return hr;
}

IDirect3D8* WINAPI hkDirect3DCreate8(UINT sdkVer) {
    IDirect3D8* pD3D = oDirect3DCreate8(sdkVer);
    if(pD3D) {
        void** vtbl = *(void***)pD3D;
        // IDirect3D8::CreateDevice = index 15 = 0x3C - matches your loc_403645: call [edx+3Ch]
        MH_CreateHook(vtbl[15], &hkCreateDevice, (void**)&oCreateDevice);
        MH_EnableHook(vtbl[15]);
    }
    return pD3D;
}

// dinput.dll proxy - XOPS loads dinput, so this DLL gets loaded automatically
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        MH_Initialize();
        // Hook Direct3DCreate8 export from d3d8.dll
        HMODULE hd3d = GetModuleHandleA("d3d8.dll");
        if(!hd3d) hd3d = LoadLibraryA("d3d8.dll");
        if(hd3d) {
            void* p = GetProcAddress(hd3d, "Direct3DCreate8");
            if(p) {
                MH_CreateHook(p, &hkDirect3DCreate8, (void**)&oDirect3DCreate8);
                MH_EnableHook(p);
            }
        }
    }
    return TRUE;
}
