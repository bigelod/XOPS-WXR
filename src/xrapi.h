// ===========================================================================
//  xrapi.h - in-process WinlatorXR XrApi client (protocol 0.4) for XOPS-WXR
//
//  Adapted from Shogo-VR-WXR's game/XrApiUDP.cpp, which is a working 0.4/0.5
//  client on a Quest 3. Rewritten in plain C-style C++ with no engine types,
//  and converting into XOPS space rather than LithTech space.
//
//  The `version` file advertises **0.4**. 0.5 is the same tracking packet plus
//  the InputAPI listener on 7728, which this client does not implement;
//  advertising 0.5 made the runtime expect that side of the protocol.
//
//  Three channels, per Docs/general_wxr/XRAPI_V0_4_DESC.md:
//
//    in   <- 127.0.0.1:7872   (7873 only if 7872 is already bound; the
//                              runtime sends the same packet to both, so
//                              binding both would receive everything twice)
//           "client <28 floats> <frame id> <19 or 21 T/F> <HMD name>"
//    out  -> 127.0.0.1:7278   "<Lhap> <Rhap> <MODE_VR> <MODE_3D> <FOVx> <FOVy>"
//    plus the red sync pixel, drawn by the caller - see XrPose::sync.
//
//  Poses arrive in the XrApi/OpenXR frame: right-handed, +Y up, -Z forward,
//  metres. They leave here in XOPS space: left-handed, +Y up, +Z forward,
//  and in engine units, which are ~1 metre each [derived, probe run 4].
// ===========================================================================

#ifndef XOPS_WXR_XRAPI_H
#define XOPS_WXR_XRAPI_H

/* One converted frame. Angles are radians; positions are engine units,
   measured from the recentre point. */
typedef struct {
    int   valid;            /* a packet has been parsed and converted */

    float headYaw;          /* relative to the recentre point, + = turned right */
    float headPitch;        /* absolute against the horizon, + = looking up    */
    float headF[3];         /* head forward, XOPS space */
    float headU[3];         /* head up */
    float headP[3];         /* head position, units from the recentre point */

    float rF[3], rP[3];     /* right (dominant) hand - this is what aims */
    float lF[3], lP[3];     /* left hand */
    float rU[3], lU[3];     /* hand up vectors, for the virtual hand roll */

    float moveX, moveY;     /* left stick  (x right, y forward) */
    float turnX, turnY;     /* right stick */

    float ipd;              /* metres */
    float fovx, fovy;       /* degrees, as the runtime wants them covered */
    float sync;             /* HMD_SYNC - echo this EXACTLY as the red channel */

    int   nBools;
    char  bools[24];        /* raw T/F run, spec order: 0=L_GRIP .. 18=R_TRIGGER */
} XrPose;

/* Buttons, by index into XrPose::bools. Treat this list as provisional and
   reconcile it against a device log before relying on it - the general docs
   are explicit that the indices beyond the common ones are unverified. */
#define XRB_L_GRIP        0
#define XRB_L_MENU        1
#define XRB_L_STICK       2
#define XRB_L_TRIGGER     7
#define XRB_X             8
#define XRB_Y             9
#define XRB_A            10
#define XRB_B            11
#define XRB_R_GRIP       12
#define XRB_R_STICK      13
#define XRB_R_TRIGGER    18

#define XrButton(p, i)  (((i) < (p)->nBools && (p)->bools[(i)] == 'T') ? 1 : 0)

/* MODE_VR */
#define XR_VR_OFF        0      /* runtime sends NO data at all - good for menus */
#define XR_VR_ON         1
#define XR_VR_SCREEN     2
#define XR_VR_SCREEN_MT  3

/* MODE_3D */
#define XR_3D_MONO       0
#define XR_3D_SBS        1
#define XR_3D_AER        2
#define XR_3D_USER     (-1)

int  XrStart(void);             /* sockets + the Z:\tmp\xr handshake */
void XrStop(void);              /* close sockets, delete the vr file */
int  XrRunning(void);

/* Drain every queued packet and fill `out` from the newest. Returns 1 once a
   packet has ever arrived, so a dropout holds the last good pose rather than
   snapping to zero. */
int  XrPoll(XrPose* out);

void XrSendFrame(float lHaptic, float rHaptic, int modeVr, int mode3d,
                 float fovx, float fovy);

void XrRequestRecenter(void);   /* applied when the next packet arrives */

const char* XrDir(void);        /* the handshake directory actually used */
const char* XrMake(void);
const char* XrModel(void);
long        XrPackets(void);    /* total packets received */
const char* XrRawPacket(void);  /* the last packet verbatim, for the log */
const char* XrLastError(void);

#endif /* XOPS_WXR_XRAPI_H */
