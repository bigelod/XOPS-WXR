// ===========================================================================
//  xrapi.cpp - see xrapi.h
//
//  Adapted from Shogo-VR-WXR game/XrApiUDP.cpp [proven on a Quest 3].
//  Deviations from that client are marked DIFF and justified.
// ===========================================================================

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "xrapi.h"

#define XR_PORT_RECV      7872
#define XR_PORT_FALLBACK  7873
#define XR_PORT_SEND      7278
#define XR_PI             3.14159265358979323846

static SOCKET      g_sock       = INVALID_SOCKET;
static int         g_running    = 0;
static int         g_wsa        = 0;
static sockaddr_in g_sendAddr;
static int         g_haveSend   = 0;
static long        g_packets    = 0;
static char        g_err[160]   = {0};

/* Raw tracking-space values from the last packet. */
static int   g_haveRaw = 0;
static float g_lQuat[4], g_rQuat[4], g_hQuat[4];
static float g_lPos[3],  g_rPos[3],  g_hPos[3];
static float g_lStick[2], g_rStick[2];
static float g_ipd, g_fovx, g_fovy, g_sync;
static char  g_bools[24];
static char  g_raw[512];

/* Recentre point: the yaw-inverse quaternion and the position at centre. */
static int   g_haveCentre = 0;
static int   g_recentrePending = 0;
static float g_centreInv[4] = {0,0,0,1};
static float g_centrePos[3] = {0,0,0};

static char  g_dir[64]   = {0};
static char  g_make[64]  = {0};
static char  g_model[64] = {0};

// --------------------------------------------------------------- quaternions

static void QNormalise(float q[4])
{
    float n2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    float inv;
    if (n2 < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    inv = 1.0f / (float)sqrt(n2);
    q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
}

static void QMul(const float a[4], const float b[4], float r[4])
{
    r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    r[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    r[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
}

static void QRotate(const float q[4], const float v[3], float r[3])
{
    /* v' = v + 2w(u x v) + 2(u x (u x v)),  u = q.xyz */
    float u[3] = { q[0], q[1], q[2] };
    float t[3] = { 2.0f*(u[1]*v[2] - u[2]*v[1]),
                   2.0f*(u[2]*v[0] - u[0]*v[2]),
                   2.0f*(u[0]*v[1] - u[1]*v[0]) };
    r[0] = v[0] + q[3]*t[0] + (u[1]*t[2] - u[2]*t[1]);
    r[1] = v[1] + q[3]*t[1] + (u[2]*t[0] - u[0]*t[2]);
    r[2] = v[2] + q[3]*t[2] + (u[0]*t[1] - u[1]*t[0]);
}

static void QYaw(float angle, float q[4])      /* rotation about +Y */
{
    q[0] = 0.0f;
    q[1] = (float)sin(angle * 0.5f);
    q[2] = 0.0f;
    q[3] = (float)cos(angle * 0.5f);
}

static float YawOf(const float q[4])
{
    const float zf[3] = { 0.0f, 0.0f, -1.0f };   /* OpenXR forward */
    float fwd[3];
    QRotate(q, zf, fwd);
    return (float)atan2(-fwd[0], -fwd[2]);
}

// --------------------------------------------------------------- conversion

/* XrApi/OpenXR (right-handed, -Z forward) -> XOPS (left-handed, +Z forward).
   Flipping Z converts the handedness and turns -Z forward into +Z forward in
   one step, which is why only that one sign changes. */
static void ToGameSpace(const float qIn[4], const float pos[3],
                        float f[3], float u[3], float p[3])
{
    const float zf[3] = { 0.0f, 0.0f, -1.0f };
    const float yu[3] = { 0.0f, 1.0f,  0.0f };
    float q[4], d[3], pp[3], fw[3], up[3];

    QMul(g_centreInv, qIn, q);

    d[0] = pos[0] - g_centrePos[0];
    d[1] = pos[1] - g_centrePos[1];
    d[2] = pos[2] - g_centrePos[2];
    QRotate(g_centreInv, d, pp);

    QRotate(q, zf, fw);
    QRotate(q, yu, up);

    f[0] = fw[0]; f[1] = fw[1]; f[2] = -fw[2];
    u[0] = up[0]; u[1] = up[1]; u[2] = -up[2];
    p[0] = pp[0]; p[1] = pp[1]; p[2] = -pp[2];
}

static void Recentre(void)
{
    if (!g_haveRaw) return;
    QYaw(-YawOf(g_hQuat), g_centreInv);
    g_centrePos[0] = g_hPos[0];
    g_centrePos[1] = g_hPos[1];
    g_centrePos[2] = g_hPos[2];
    g_haveCentre = 1;
}

void XrRequestRecenter(void) { g_recentrePending = 1; }

// ------------------------------------------------------------------ handshake

static int EnsureDir(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    /* One level at a time: "Z:\tmp\xr" needs "Z:\tmp" first. */
    {
        char tmp[64];
        char* slash;
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        slash = strrchr(tmp, '\\');
        if (slash && slash > tmp + 2) { *slash = 0; EnsureDir(tmp); }
    }
    return CreateDirectoryA(path, NULL) ? 1 : (GetLastError() == ERROR_ALREADY_EXISTS);
}

static void TrimEol(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ')) s[--n] = 0;
}

static void WriteStartupFiles(void)
{
    char file[96];
    FILE* f;

    strcpy(g_dir, "Z:\\tmp\\xr");
    if (GetFileAttributesA("Z:\\") == INVALID_FILE_ATTRIBUTES) {
        /* DIFF: Shogo hardcodes D:\xrtemp. XOPS runs from E: on the owner's
           box, so take the drive the EXE is actually on and fall back to D:
           only if that cannot be determined. The directory is irrelevant when
           testing against the UDP tester - nothing reads it - but it must be
           right on the headset. */
        char exe[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe, sizeof(exe)) > 3 && exe[1] == ':')
            _snprintf(g_dir, sizeof(g_dir) - 1, "%c:\\xrtemp", exe[0]);
        else
            strcpy(g_dir, "D:\\xrtemp");
        g_dir[sizeof(g_dir) - 1] = 0;
    }

    if (!EnsureDir(g_dir)) {
        _snprintf(g_err, sizeof(g_err) - 1, "could not create %s", g_dir);
        return;
    }

    /* The runtime reads the FIRST LINE of `version` once, matching
       startsWith("0.1").."0.7". An unrecognised value means it never starts a
       UDP listener at all - total silence that looks like a socket bug. */
    _snprintf(file, sizeof(file) - 1, "%s\\version", g_dir); file[sizeof(file)-1] = 0;
    f = fopen(file, "w");
    if (f) { fputs("0.5", f); fclose(f); }

    _snprintf(file, sizeof(file) - 1, "%s\\vr", g_dir); file[sizeof(file)-1] = 0;
    f = fopen(file, "w");
    if (f) { fputs("VR", f); fclose(f); }

    /* `system` is the one we read. Seven lines; we want the first two, and
       line 2 is a product codename rather than a model name. */
    _snprintf(file, sizeof(file) - 1, "%s\\system", g_dir); file[sizeof(file)-1] = 0;
    f = fopen(file, "r");
    if (f) {
        char line[128];
        if (fgets(line, sizeof(line), f)) {
            TrimEol(line); strncpy(g_make, line, sizeof(g_make) - 1);
            g_make[sizeof(g_make) - 1] = 0;
        }
        if (fgets(line, sizeof(line), f)) {
            TrimEol(line); strncpy(g_model, line, sizeof(g_model) - 1);
            g_model[sizeof(g_model) - 1] = 0;
        }
        fclose(f);
    }
    if (!g_make[0])  strcpy(g_make, "META");
    if (!g_model[0]) strcpy(g_model, "QUEST 3");
    if (!strcmp(g_make, "OCULUS")) strcpy(g_make, "META");
    if (!strcmp(g_model, "EUREKA") || !strcmp(g_model, "PANTHER"))
        strcpy(g_model, "QUEST 3");
    else if (!strcmp(g_model, "HOLLYWOOD")) strcpy(g_model, "QUEST 2");
}

// -------------------------------------------------------------- start / stop

int XrStart(void)
{
    WSADATA wsa;
    sockaddr_in addr;
    u_long nb = 1;
    SOCKET s;

    if (g_running) return 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        strcpy(g_err, "WSAStartup failed");
        return 0;
    }
    g_wsa = 1;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        strcpy(g_err, "socket() failed");
        WSACleanup(); g_wsa = 0;
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(XR_PORT_RECV);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        /* 7873 is not a fallback in the protocol - the runtime sends to both -
           but it IS the right thing to try if 7872 is already taken, which
           usually means a second instance or a tester holding it. */
        addr.sin_port = htons(XR_PORT_FALLBACK);
        if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            _snprintf(g_err, sizeof(g_err) - 1,
                      "bind failed on both %d and %d (WSA %d)",
                      XR_PORT_RECV, XR_PORT_FALLBACK, WSAGetLastError());
            closesocket(s); WSACleanup(); g_wsa = 0;
            return 0;
        }
    }

    ioctlsocket(s, FIONBIO, &nb);       /* non-blocking: XrPoll drains */
    g_sock = s;

    memset(&g_sendAddr, 0, sizeof(g_sendAddr));
    g_sendAddr.sin_family = AF_INET;
    g_sendAddr.sin_port = htons(XR_PORT_SEND);
    g_sendAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_haveSend = 1;

    g_running = 1;
    WriteStartupFiles();
    return 1;
}

void XrStop(void)
{
    if (g_dir[0]) {
        char file[96];
        /* Leaving `vr` behind can convince the runtime a dead process still
           wants VR. */
        _snprintf(file, sizeof(file) - 1, "%s\\vr", g_dir); file[sizeof(file)-1] = 0;
        DeleteFileA(file);
    }
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    if (g_wsa) { WSACleanup(); g_wsa = 0; }
    g_running = 0;
}

int  XrRunning(void)        { return g_running; }
const char* XrDir(void)     { return g_dir[0] ? g_dir : "(none)"; }
const char* XrMake(void)    { return g_make; }
const char* XrModel(void)   { return g_model; }
long XrPackets(void)        { return g_packets; }
const char* XrRawPacket(void) { return g_raw; }
const char* XrLastError(void) { return g_err; }

// -------------------------------------------------------------------- parse

static int IsNumberToken(const char* t)
{
    if (!t || !*t) return 0;
    if (*t == '-' || *t == '+') ++t;
    return (*t >= '0' && *t <= '9') || *t == '.';
}

static int Parse(const char* data)
{
    char tok[64][40];
    int nTok = 0, i0, i;
    float f[29];
    const char* p = data;

    if (!data || !*data) return 0;

    strncpy(g_raw, data, sizeof(g_raw) - 1);
    g_raw[sizeof(g_raw) - 1] = 0;

    while (*p && nTok < 64) {
        int k = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && k < 39)
            tok[nTok][k++] = *p++;
        tok[nTok][k] = 0;
        ++nTok;
    }

    /* The lead token is "client0", not "client" - a prefix, so match it
       loosely by asking whether the first token is a number instead. */
    i0 = (nTok > 0 && !IsNumberToken(tok[0])) ? 1 : 0;
    if (nTok < i0 + 29) return 0;

    for (i = 0; i < 29; ++i) f[i] = (float)atof(tok[i0 + i]);

    g_lQuat[0]=f[0];  g_lQuat[1]=f[1];  g_lQuat[2]=f[2];  g_lQuat[3]=f[3];
    g_lStick[0]=f[4]; g_lStick[1]=f[5];
    g_lPos[0]=f[6];   g_lPos[1]=f[7];   g_lPos[2]=f[8];

    g_rQuat[0]=f[9];  g_rQuat[1]=f[10]; g_rQuat[2]=f[11]; g_rQuat[3]=f[12];
    g_rStick[0]=f[13];g_rStick[1]=f[14];
    g_rPos[0]=f[15];  g_rPos[1]=f[16];  g_rPos[2]=f[17];

    g_hQuat[0]=f[18]; g_hQuat[1]=f[19]; g_hQuat[2]=f[20]; g_hQuat[3]=f[21];
    g_hPos[0]=f[22];  g_hPos[1]=f[23];  g_hPos[2]=f[24];

    /* Take orientations as directions, not lengths: the UDP tester's
       quaternions are often not unit length. */
    QNormalise(g_lQuat);
    QNormalise(g_rQuat);
    QNormalise(g_hQuat);

    g_ipd  = f[25];
    g_fovx = f[26];
    g_fovy = f[27];
    g_sync = f[28];

    /* The bool run is 19 chars with the two flags as a separate token, so a
       parser that skips whitespace sees 21. Accept either and bounds-check. */
    g_bools[0] = 0;
    if (i0 + 29 < nTok) {
        strncpy(g_bools, tok[i0 + 29], sizeof(g_bools) - 1);
        g_bools[sizeof(g_bools) - 1] = 0;
    }

    g_haveRaw = 1;
    ++g_packets;

    if (!g_haveCentre || g_recentrePending) Recentre();
    g_recentrePending = 0;
    return 1;
}

int XrPoll(XrPose* out)
{
    char buf[1024];

    if (g_running && g_sock != INVALID_SOCKET) {
        /* Drain everything queued; the newest packet wins. A frame behind is
           worse than a frame of work. */
        for (;;) {
            int n = recv(g_sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = 0;
            Parse(buf);
        }
    }

    if (!out) return g_haveRaw;
    if (!g_haveRaw) { out->valid = 0; return 0; }

    ToGameSpace(g_hQuat, g_hPos, out->headF, out->headU, out->headP);
    {
        float d;
        /* Yaw from the forward vector rather than from Euler extraction: it
           has no gimbal case and matches the convention the view matrix
           decode already uses. */
        out->headYaw = (float)atan2(out->headF[0], out->headF[2]);
        d = (float)sqrt(out->headF[0]*out->headF[0] + out->headF[2]*out->headF[2]);
        /* DIFF: Shogo negates this because LithTech pitch is positive-down.
           Left positive-up here and let the caller choose the sign, because
           XOPS's own pitch convention is not yet established. */
        out->headPitch = (float)atan2(out->headF[1], d);
    }

    {
        float u[3];
        ToGameSpace(g_rQuat, g_rPos, out->rF, u, out->rP);
        ToGameSpace(g_lQuat, g_lPos, out->lF, u, out->lP);
    }

    out->moveX = g_lStick[0]; out->moveY = g_lStick[1];
    out->turnX = g_rStick[0]; out->turnY = g_rStick[1];
    out->ipd   = g_ipd;
    out->fovx  = g_fovx;
    out->fovy  = g_fovy;
    out->sync  = g_sync;

    strncpy(out->bools, g_bools, sizeof(out->bools) - 1);
    out->bools[sizeof(out->bools) - 1] = 0;
    out->nBools = (int)strlen(out->bools);

    out->valid = 1;
    return 1;
}

// ------------------------------------------------------------------ outbound

void XrSendFrame(float lHaptic, float rHaptic, int modeVr, int mode3d,
                 float fovx, float fovy)
{
    char msg[128];
    int n;

    if (!g_running || !g_haveSend || g_sock == INVALID_SOCKET) return;

    /* FOV is in DEGREES and the runtime applies it only if > 1. A client that
       sends tangents of half-angles sends values the runtime ignores, and
       appears to work only because the default is kept. */
    n = _snprintf(msg, sizeof(msg) - 1, "%.1f %.1f %d %d %.1f %.1f",
                  lHaptic, rHaptic, modeVr, mode3d, fovx, fovy);
    if (n <= 0) return;
    msg[n] = 0;

    sendto(g_sock, msg, n, 0, (sockaddr*)&g_sendAddr, sizeof(g_sendAddr));
}
