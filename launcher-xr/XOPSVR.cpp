// XOPSVR.cpp - WinlatorXR launcher for XOPS-WXR.
//
// One window with Play and Options. Before launching it checks the release
// channel for a newer version and shows the release notes. For now that
// channel is Shogo WXR's AutoUpdate folder (so the check can be tested before
// XOPS-WXR has its own published files) and the module download step is
// deliberately disabled: this build never downloads a DLL.
//
// Play launches xops096.exe next to the launcher (or up to two folders up).
// Options edits config.txt one folder above the game, the same file the mod
// reads. A log is written to XOPSVR_update.log next to the EXE.

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>

#include "Resource.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

// --- release channel -------------------------------------------------------
// TEMP: test against Shogo WXR's published channel until XOPS-WXR has its own
// AutoUpdate files. The notes/version are real Shogo release data; the DLL
// download is disabled below and must never pull Shogo's CShell.dll.
const wchar_t* const kVersionUrl =
    L"https://raw.githubusercontent.com/WinlatorXR/Shogo-VR-WXR/refs/heads/main/AutoUpdate/version.txt";
const wchar_t* const kNotesUrl =
    L"https://raw.githubusercontent.com/WinlatorXR/Shogo-VR-WXR/refs/heads/main/AutoUpdate/notes.txt";

// Deliberately off for now: no DLL download, no version.txt write on YES.
const bool kEnableModDownload = false;

const wchar_t* const kAppTitle = L"XOPS-WXR";
const wchar_t* const kGameExe  = L"xops096.exe";

#define WM_APP_CHECK (WM_APP + 1)
#define ID_PLAY_ACTION 100

// --- config ----------------------------------------------------------------
struct Config
{
    std::wstring handedness = L"right";
    bool handFlip = false;          // config value: true = old/other-device roll
    bool haptics = true;
    std::wstring hapticsHand = L"dominant";
    int hapticFrames = 2;
    bool verbose = false;
    std::wstring stereo = L"mono";
    bool roll = true;
    int rollSign = -1;
    double posScale = 1.0;          // HMD translation gain, 0..2
    double hudScale = 1.1;          // replacement HUD visual scale, 0.5..2.0
};

Config g_cfg;
std::wstring g_exeDir;
std::wstring g_configPath;
std::wstring g_gameDir;
std::wstring g_gameExe;

// --- small helpers ---------------------------------------------------------
std::wstring SelfDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"" : p.substr(0, s + 1);
}

std::wstring ParentDir(const std::wstring& dir)
{
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    size_t s = d.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L"";
    return d.substr(0, s + 1);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    wchar_t last = dir[dir.size() - 1];
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring TrimW(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned)s[b] <= ' ') ++b;
    while (e > b && (unsigned)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s)
{
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
        nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
        &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool ReadFileText(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    out.clear();
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool WriteFileText(const std::wstring& path, const std::string& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) != 0
        && wrote == data.size();
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

void Log(const std::wstring& msg)
{
    HANDLE h = CreateFileW((g_exeDir + L"XOPSVR_update.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(stamp) + msg + L"\r\n";

    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
        nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
        &utf8[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    CloseHandle(h);
}

// --- HTTPS (version/notes only; no file download) --------------------------
bool HttpGetText(const std::wstring& url, std::string& text, std::wstring& err)
{
    const std::wstring httpsPrefix = L"https://";
    const std::wstring httpPrefix  = L"http://";
    bool secure = true;
    std::wstring rest;

    if (url.compare(0, httpsPrefix.size(), httpsPrefix) == 0)
        rest = url.substr(httpsPrefix.size());
    else if (url.compare(0, httpPrefix.size(), httpPrefix) == 0) {
        secure = false;
        rest = url.substr(httpPrefix.size());
    } else {
        err = L"unsupported URL";
        return false;
    }

    size_t slash = rest.find(L'/');
    std::wstring host = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    std::wstring path = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    if (host.empty()) { err = L"no host in the URL"; return false; }

    HINTERNET ses = WinHttpOpen(L"XOPSVR-Updater/0.1",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = L"WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(ses, 4000, 4000, 8000, 12000);

    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(),
        secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!con) {
        err = L"WinHttpConnect failed";
    } else {
        HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            err = L"WinHttpOpenRequest failed";
        } else {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status != 200) {
                    err = L"HTTP " + std::to_wstring(status);
                } else {
                    ok = true;
                    text.clear();
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) {
                            ok = false; err = L"read failed"; break;
                        }
                        if (!avail) break;
                        std::vector<char> chunk(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, &chunk[0], avail, &got)) {
                            ok = false; err = L"read failed"; break;
                        }
                        if (!got) break;
                        text.append(&chunk[0], got);
                    }
                }
            } else {
                err = L"request failed";
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

// --- version compare (same rules as the Shogo launcher) --------------------
bool ParseVersion(const std::string& s, std::vector<unsigned long long>& out)
{
    out.clear();
    bool any = false;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c >= '0' && c <= '9') {
            unsigned long long v = 0;
            while (i < s.size()) {
                unsigned char d = (unsigned char)s[i];
                if (d < '0' || d > '9') break;
                if (v <= 1000000ULL) v = v * 10 + (unsigned long long)(d - '0');
                ++i;
            }
            out.push_back(v);
            any = true;
        } else if (c == '.') {
            ++i;
        } else {
            break;
        }
    }
    return any;
}

bool IsNewer(const std::string& online, const std::string& local)
{
    std::vector<unsigned long long> a, b;
    if (!ParseVersion(online, a) || !ParseVersion(local, b)) return false;
    size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        unsigned long long x = i < a.size() ? a[i] : 0;
        unsigned long long y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    const size_t dot = online.find_first_of(".-+");
    const bool onlinePre = dot != std::string::npos
        && online.size() > dot + 1 && online[dot + 1] != '.';
    return !onlinePre;
}

// --- config file -----------------------------------------------------------
std::wstring FindConfigPath(const std::wstring& gameDir)
{
    std::wstring local = gameDir + L"config.txt";

    /* Config belongs in the game folder. If an older install still has it one
       level up, copy it in once so settings are not lost. */
    std::wstring parent = ParentDir(gameDir);
    if (!FileExists(local) && !parent.empty()) {
        std::wstring legacy = parent + L"config.txt";
        if (FileExists(legacy)) CopyFileW(legacy.c_str(), local.c_str(), FALSE);
    }
    return local;
}

bool ParseBool(const std::wstring& v)
{
    std::wstring s = Lower(TrimW(v));
    return s == L"true" || s == L"yes" || s == L"1" || s == L"on";
}

void LoadConfig()
{
    g_cfg = Config();

    std::string raw;
    if (!ReadFileText(g_configPath, raw)) return;

    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t end = raw.find('\n', pos);
        std::string lineA = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? raw.size() + 1 : end + 1;

        std::wstring line(lineA.begin(), lineA.end());
        std::wstring key = TrimW(line);
        if (key.empty() || key[0] == L'#' || key[0] == L';') continue;
        size_t eq = key.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring name = Lower(TrimW(key.substr(0, eq)));
        std::wstring val  = TrimW(key.substr(eq + 1));

        if (name == L"handedness") {
            g_cfg.handedness = (Lower(val) == L"left") ? L"left" : L"right";
        } else if (name == L"hand_flip") {
            g_cfg.handFlip = ParseBool(val);
        } else if (name == L"haptics") {
            g_cfg.haptics = ParseBool(val);
        } else if (name == L"haptics_hand") {
            std::wstring v = Lower(val);
            if (v == L"both" || v == L"left" || v == L"right") g_cfg.hapticsHand = v;
            else g_cfg.hapticsHand = L"dominant";
        } else if (name == L"haptic_frames") {
            int n = _wtoi(val.c_str());
            if (n < 1) n = 1;
            if (n > 5) n = 5;
            g_cfg.hapticFrames = n;
        } else if (name == L"verbose_debug") {
            g_cfg.verbose = ParseBool(val);
        } else if (name == L"stereo") {
            g_cfg.stereo = (Lower(val) == L"aer") ? L"aer" : L"mono";
        } else if (name == L"roll") {
            g_cfg.roll = ParseBool(val);
        } else if (name == L"roll_sign") {
            g_cfg.rollSign = (_wtoi(val.c_str()) > 0) ? 1 : -1;
        } else if (name == L"pos_scale") {
            double v = _wtof(val.c_str());
            if (v < 0.0) v = 0.0;
            if (v > 2.0) v = 2.0;
            g_cfg.posScale = v;
        } else if (name == L"hud_scale") {
            double v = _wtof(val.c_str());
            if (v < 0.5) v = 0.5;
            if (v > 2.0) v = 2.0;
            g_cfg.hudScale = v;
        }
    }

    /* Head roll is always on; it is not a user setting. */
    g_cfg.roll = true;
}

std::wstring BoolStr(bool b) { return b ? L"true" : L"false"; }

std::wstring FloatStr(double v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", v);
    return buf;
}

void SaveConfig()
{
    std::vector<std::wstring> lines;
    std::string raw;
    if (ReadFileText(g_configPath, raw)) {
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t end = raw.find('\n', pos);
            std::string lineA = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = (end == std::string::npos) ? raw.size() : end + 1;
            if (!lineA.empty() && lineA.back() == '\r') lineA.pop_back();
            lines.push_back(Widen(lineA));
        }
    }

    struct KeyVal { const wchar_t* key; std::wstring val; bool seen; };
    std::vector<KeyVal> kv = {
        { L"handedness",    g_cfg.handedness, false },
        { L"hand_flip",     BoolStr(g_cfg.handFlip), false },
        { L"haptics",       BoolStr(g_cfg.haptics), false },
        { L"haptics_hand",  g_cfg.hapticsHand, false },
        { L"haptic_frames", std::to_wstring(g_cfg.hapticFrames), false },
        { L"verbose_debug", BoolStr(g_cfg.verbose), false },
        { L"stereo",        g_cfg.stereo, false },
        { L"roll",          BoolStr(g_cfg.roll), false },
        { L"roll_sign",     std::to_wstring(g_cfg.rollSign), false },
        { L"pos_scale",     FloatStr(g_cfg.posScale), false },
        { L"hud_scale",     FloatStr(g_cfg.hudScale), false },
    };

    for (std::wstring& line : lines) {
        std::wstring t = TrimW(line);
        if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring name = Lower(TrimW(t.substr(0, eq)));
        for (KeyVal& k : kv) {
            if (name == k.key) {
                line = std::wstring(k.key) + L" = " + k.val;
                k.seen = true;
                break;
            }
        }
    }

    bool haveAny = false;
    for (const KeyVal& k : kv) if (k.seen) haveAny = true;
    if (!haveAny) lines.clear();            // new file: write a clean one

    bool missing = false;
    for (const KeyVal& k : kv) if (!k.seen) missing = true;
    if (missing) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back(L"");
        for (const KeyVal& k : kv)
            if (!k.seen) lines.push_back(std::wstring(k.key) + L" = " + k.val);
    }

    std::string out;
    for (const std::wstring& line : lines) {
        std::string a = Narrow(line);
        out += a;
        out += "\r\n";
    }
    if (!WriteFileText(g_configPath, out))
        Log(L"config: could not write " + g_configPath);
    else
        Log(L"config: saved " + g_configPath);
}

// --- dialogs ---------------------------------------------------------------
void PrepareDialog(HWND h)
{
    RECT r;
    if (!GetWindowRect(h, &r)) return;
    const int w = r.right - r.left, hgt = r.bottom - r.top;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(h, HWND_TOPMOST, (sw - w) / 2, (sh - hgt) / 2, 0, 0,
        SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}

int ComboIndex(HWND h, int id, const std::wstring& value)
{
    HWND c = GetDlgItem(h, id);
    int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        wchar_t buf[64] = {};
        SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)buf);
        if (Lower(buf) == Lower(value)) return i;
    }
    return 0;
}

std::wstring ComboText(HWND h, int id)
{
    HWND c = GetDlgItem(h, id);
    int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
    if (i < 0) return L"";
    wchar_t buf[64] = {};
    SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)buf);
    return buf;
}

void AddCombo(HWND h, int id, const wchar_t* const* items, int n)
{
    HWND c = GetDlgItem(h, id);
    for (int i = 0; i < n; ++i) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
}

INT_PTR CALLBACK OptionsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t* handed[] = { L"right", L"left" };
        static const wchar_t* haph[]   = { L"dominant", L"both", L"left", L"right" };
        static const wchar_t* stereo[] = { L"mono", L"aer" };
        static const wchar_t* rsign[]  = { L"-1", L"+1" };

        AddCombo(h, IDC_HANDED, handed, 2);
        AddCombo(h, IDC_HAPHAND, haph, 4);
        AddCombo(h, IDC_STEREO, stereo, 2);
        AddCombo(h, IDC_ROLLSIGN, rsign, 2);

        SendMessageW(GetDlgItem(h, IDC_HANDED), CB_SETCURSEL,
            ComboIndex(h, IDC_HANDED, g_cfg.handedness), 0);
        SendMessageW(GetDlgItem(h, IDC_HAPHAND), CB_SETCURSEL,
            ComboIndex(h, IDC_HAPHAND, g_cfg.hapticsHand), 0);
        SendMessageW(GetDlgItem(h, IDC_STEREO), CB_SETCURSEL,
            ComboIndex(h, IDC_STEREO, g_cfg.stereo), 0);
        SendMessageW(GetDlgItem(h, IDC_ROLLSIGN), CB_SETCURSEL,
            g_cfg.rollSign > 0 ? 1 : 0, 0);

        CheckDlgButton(h, IDC_HANDFLIP, g_cfg.handFlip ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_HAPTICS,  g_cfg.haptics  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_VERBOSE,  g_cfg.verbose  ? BST_CHECKED : BST_UNCHECKED);

        SetDlgItemTextW(h, IDC_POSSCALE, FloatStr(g_cfg.posScale).c_str());
        SetDlgItemTextW(h, IDC_HUDSCALE, FloatStr(g_cfg.hudScale).c_str());

        PrepareDialog(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            g_cfg.handedness = ComboText(h, IDC_HANDED);
            if (g_cfg.handedness != L"left") g_cfg.handedness = L"right";

            g_cfg.hapticsHand = ComboText(h, IDC_HAPHAND);
            if (g_cfg.hapticsHand.empty()) g_cfg.hapticsHand = L"dominant";

            g_cfg.stereo = ComboText(h, IDC_STEREO);
            if (g_cfg.stereo != L"aer") g_cfg.stereo = L"mono";

            g_cfg.rollSign = (ComboText(h, IDC_ROLLSIGN) == L"+1") ? 1 : -1;

            g_cfg.handFlip = IsDlgButtonChecked(h, IDC_HANDFLIP) == BST_CHECKED;
            g_cfg.haptics  = IsDlgButtonChecked(h, IDC_HAPTICS)  == BST_CHECKED;
            g_cfg.verbose  = IsDlgButtonChecked(h, IDC_VERBOSE)  == BST_CHECKED;

            {
                wchar_t scale[32] = {};
                GetDlgItemTextW(h, IDC_POSSCALE, scale, 32);
                if (scale[0]) {
                    double v = _wtof(scale);
                    if (v < 0.0) v = 0.0;
                    if (v > 2.0) v = 2.0;
                    g_cfg.posScale = v;
                }
            }
            {
                wchar_t scale[32] = {};
                GetDlgItemTextW(h, IDC_HUDSCALE, scale, 32);
                if (scale[0]) {
                    double v = _wtof(scale);
                    if (v < 0.5) v = 0.5;
                    if (v > 2.0) v = 2.0;
                    g_cfg.hudScale = v;
                }
            }

            /* Head roll is not a user option: always on. haptic_frames is
               preserved from config.txt; it has no UI. */
            g_cfg.roll = true;

            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

std::wstring LocalVersion()
{
    std::string v;
    if (!ReadFileText(g_exeDir + L"version.txt", v)) return L"";
    std::string t = Trim(v);
    return Widen(t);
}

void CheckForUpdate(HWND h)
{
    SetDlgItemTextW(h, IDC_STATUS, L"Checking for updates...");

    std::string onlineRaw;
    std::wstring err;
    if (!HttpGetText(kVersionUrl, onlineRaw, err)) {
        Log(L"update check skipped (" + err + L")");
        SetDlgItemTextW(h, IDC_STATUS, L"Update check skipped (offline or unreachable).");
        return;
    }
    std::string online = Trim(onlineRaw);
    if (online.empty()) {
        Log(L"update check skipped (empty online version)");
        SetDlgItemTextW(h, IDC_STATUS, L"Update check skipped (empty online version).");
        return;
    }

    std::wstring local = LocalVersion();
    std::string localA = Narrow(local);
    Log(L"versions: online=" + Widen(online) + L" local=" + (local.empty() ? L"(none)" : local));

    if (!local.empty() && !IsNewer(online, localA)) {
        SetDlgItemTextW(h, IDC_STATUS, (L"Up to date (online " + Widen(online)
            + L", local " + local + L").").c_str());
        Log(L"update check: online is not newer, no prompt");
        return;
    }

    std::string notesRaw;
    if (!HttpGetText(kNotesUrl, notesRaw, err)) notesRaw.clear();
    std::string notesA = Trim(notesRaw);
    std::wstring notes(notesA.begin(), notesA.end());

    std::wstring prompt = L"A new update is available, download now?\r\nVersion "
        + Widen(online);
    if (!notes.empty()) prompt += L": " + notes;

    if (MessageBoxW(h, prompt.c_str(), kAppTitle,
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES) {
        Log(L"update: declined");
        SetDlgItemTextW(h, IDC_STATUS, L"Update available; declined.");
        return;
    }

    if (!kEnableModDownload) {
        Log(L"update: download disabled in this test build (Shogo test channel)");
        MessageBoxW(h,
            L"The module download is disabled in this test build.\r\n"
            L"Nothing was changed. This check only verifies the version and notes channel.",
            kAppTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        SetDlgItemTextW(h, IDC_STATUS, L"Update available (download disabled for now).");
        return;
    }

    // The real XOPS-WXR module download/install will live here once the
    // project has its own AutoUpdate/d3d8.dll. Never download Shogo's DLL.
    Log(L"update: no install path for this build");
}

bool FindGame(const std::wstring& self, std::wstring& game, std::wstring& exe)
{
    std::wstring probe = self;
    for (int depth = 0; depth < 3; ++depth) {
        std::wstring candidate = JoinPath(probe, kGameExe);
        if (FileExists(candidate)) {
            game = probe;
            exe = candidate;
            return true;
        }
        probe = ParentDir(probe);
        if (probe.empty()) break;
    }
    return false;
}

INT_PTR CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        PrepareDialog(h);
        std::wstring v = LocalVersion();
        std::wstring label = L"Version " + (v.empty() ? std::wstring(L"dev") : v);
        SetDlgItemTextW(h, IDC_VERSION, label.c_str());
        if (!FindGame(g_exeDir, g_gameDir, g_gameExe))
            SetDlgItemTextW(h, IDC_STATUS, L"xops096.exe was not found next to the launcher.");
        PostMessageW(h, WM_APP_CHECK, 0, 0);
        return TRUE;
    }
    case WM_APP_CHECK:
        CheckForUpdate(h);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PLAY:
            if (!FindGame(g_exeDir, g_gameDir, g_gameExe)) {
                MessageBoxW(h, L"Could not find xops096.exe next to the launcher.",
                    kAppTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
                return TRUE;
            }
            EndDialog(h, ID_PLAY_ACTION);
            return TRUE;
        case IDC_OPTIONS:
            LoadConfig();
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_OPTIONS),
                    h, OptionsProc, 0) == IDOK) {
                SaveConfig();
                SetDlgItemTextW(h, IDC_STATUS, L"Settings saved.");
            }
            return TRUE;
        case IDC_CHECK:
            CheckForUpdate(h);
            return TRUE;
        case IDCANCEL:
            EndDialog(h, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_exeDir = SelfDir();
    if (g_exeDir.empty()) return 1;
    g_configPath = FindConfigPath(g_exeDir);

    Log(L"launcher start");

    INT_PTR r = DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainProc, 0);
    if (r != ID_PLAY_ACTION) return 0;

    if (!FindGame(g_exeDir, g_gameDir, g_gameExe)) return 1;

    std::wstring cmd = L"\"" + g_gameExe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(g_gameExe.c_str(), &buf[0], nullptr, nullptr, FALSE, 0,
            nullptr, g_gameDir.c_str(), &si, &pi)) {
        Log(L"could not start " + g_gameExe);
        MessageBoxW(nullptr, L"Could not start xops096.exe.", kAppTitle,
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    Log(L"started " + g_gameExe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
