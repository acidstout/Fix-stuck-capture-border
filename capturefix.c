/*
 * capturefix.c - Stuck Capture Border: detect and clear
 *
 * Native Win32 (C17) port of Fix-StuckCaptureBorder.ps1.
 *
 * The capture border is drawn by dwm.exe on behalf of an open
 * Windows.Graphics.Capture session. It is not an enumerable window, so there
 * is no API to query it. This tool probes the outer pixels of every monitor
 * using GDI (BitBlt into a DIB section), which does NOT open a capture
 * session itself and therefore cannot cause the condition it looks for.
 *
 * Because the border may be excluded from capture output on some systems, the
 * probe can return a false negative. A manual override and a two-step
 * calibration ("train on this PC") are provided for that reason.
 *
 * Config is written next to the executable if that folder is writable,
 * otherwise to %APPDATA%\StuckCaptureBorder\.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>

#include "resource.h"
#include "theme.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define APP_TITLE   L"Fix Stuck Capture Border - detect and clear"
#define APP_CLASS   L"StuckCaptureBorderWnd"
#define CONFIG_NAME L"StuckCaptureBorder.config.json"

#define EDGE_TOP    0
#define EDGE_BOTTOM 1
#define EDGE_LEFT   2
#define EDGE_RIGHT  3
static const wchar_t *const kEdgeName[4] = { L"Top", L"Bottom", L"Left", L"Right" };

#define MAX_MON     16
#define MAX_CAND    512
#define NBUCKET     512          /* 8 * 8 * 8 quantised colour buckets */

/* control ids */
#define IDC_STATUS      1001
#define IDC_DETAIL      1002
#define IDC_PROBE       1003
#define IDC_MANUAL      1004
#define IDC_AUTO        1005
#define IDC_GRPCAL      1006
#define IDC_CALBASE     1007
#define IDC_CALFRAME    1008
#define IDC_CALRESET    1009
#define IDC_SWATCH      1010
#define IDC_CALINFO     1011
#define IDC_CANDLABEL   1012
#define IDC_LIST        1013
#define IDC_REFRESH     1014
#define IDC_KILLSEL     1015
#define IDC_EXPLORER    1016
#define IDC_DWM         1017
#define IDC_LOG         1018
#define IDC_DARK        1019

/* tray menu commands */
#define IDM_SHOW        2001
#define IDM_STARTTRAY   2002
#define IDM_AUTORUN     2003
#define IDM_ABOUT       2004
#define IDM_EXIT        2005

#define IDT_AUTOPROBE   1
#define IDT_TRAYCLICK   2

#define TRAY_UID        1
#define AUTORUN_VALUE   L"FixStuckCaptureBorder"
#define AUTORUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* worker -> ui messages */
#define WM_APP_LOG      (WM_APP + 1)   /* lParam: heap wchar_t*     */
#define WM_APP_PROBE    (WM_APP + 2)   /* lParam: heap ProbeResult* */
#define WM_APP_SCAN     (WM_APP + 3)   /* lParam: heap ScanResult*  */
#define WM_APP_CALDONE  (WM_APP + 4)
#define WM_APP_JOBDONE  (WM_APP + 5)
#define WM_APP_TRAY     (WM_APP + 6)   /* notification icon callback */

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    BOOL    baseline[NBUCKET];   /* quantised colours seen with NO frame */
    int     baselineCount;
    BOOL    haveFrameColor;
    int     fr, fg, fb;          /* learned border colour */
    double  tolerance;           /* euclidean RGB distance for a pixel match */
    int     bandPx;              /* how deep from each edge to sample */
    int     sampleStride;        /* sample every Nth pixel along the edge */
    double  edgeCoverage;        /* fraction of an edge that must match */
    int     minHotEdges;         /* how many of the 4 edges make it "detected" */
    int     darkMode;            /* -1 follow Windows, 0 light, 1 dark */
    int     startInTray;         /* 1 = start minimised to the tray */
    wchar_t calibratedOn[40];
} Config;

typedef struct { unsigned long long count, sr, sg, sb; } Bucket;

typedef struct {
    wchar_t device[40];
    double  cov[4];
} MonResult;

typedef struct {
    int       nmon;
    MonResult mon[MAX_MON];
    BOOL      present;
    wchar_t   screen[40];
    int       hotEdges;
    BOOL      failed;
    wchar_t   error[200];
} ProbeResult;

typedef struct {
    DWORD   pid;
    wchar_t name[64];
    wchar_t signal[160];
    DWORD   sess;
    int     rank;
    BOOL    hasWgc;
} Cand;

typedef struct {
    int  n;
    Cand item[MAX_CAND];
} ScanResult;

typedef struct { int n; RECT r[MAX_MON]; wchar_t dev[MAX_MON][40]; } MonList;

/* job description handed to the worker thread */
#define ACT_NONE        0
#define ACT_CAL_BASE    1
#define ACT_CAL_FRAME   2
#define ACT_EXPLORER    3
#define ACT_DWM         4
#define ACT_KILL        5

#define MAX_KILL        64

typedef struct {
    int     action;
    BOOL    doProbe;
    BOOL    doScan;
    int     nkill;
    DWORD   killPid[MAX_KILL];
    wchar_t killName[MAX_KILL][64];
} Job;

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND  g_main, g_status, g_detail, g_probe, g_manual, g_auto, g_darkChk;
static HWND  g_grpCal, g_calBase, g_calFrame, g_calReset, g_swatch, g_calInfo;
static HWND  g_candLabel, g_list, g_refresh, g_killSel, g_explorer, g_dwm, g_log;
static HFONT g_fontUi, g_fontBold, g_fontMono;
static HBRUSH g_swatchBrush;
static HICON g_icoColor, g_icoGray;   /* tray: detected / resting */
static BOOL  g_trayShown;
static UINT  g_msgTaskbarCreated;     /* Explorer restart -> re-add the icon */
static int   g_dpi = 96;
static Config      g_cfg;
static wchar_t     g_cfgPath[MAX_PATH];
static ScanResult  g_scan;           /* backs the list view (UI thread only) */
static BOOL  g_probed   = FALSE;
static BOOL  g_detected = FALSE;
static BOOL  g_busy     = FALSE;
static BOOL  g_elevated = FALSE;
static DWORD g_session  = 0;

#define S(v) MulDiv((v), g_dpi, 96)

static const wchar_t *const kKnownSuspects[] = {
    L"explorer", L"ScreenClippingHost", L"SnippingTool", L"ScreenSketch",
    L"ShellExperienceHost", L"StartMenuExperienceHost", L"TextInputHost", L"SearchApp",
    L"remoting_host", L"remoting_desktop", L"remote_assistance_host"
};

static void UpdateFixButtons(void);
static void UpdateCalibrationDisplay(void);
static void StartJob(const Job *job);
static void ApplyTheme(void);
static void TrayRefresh(const wchar_t *detail);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void *xalloc(size_t n) { return calloc(1, n); }

/* Post a formatted line to the log box; safe from any thread. */
static void LogF(const wchar_t *fmt, ...)
{
    wchar_t *buf = (wchar_t *)xalloc(1024 * sizeof(wchar_t));
    if (!buf) return;
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    if (!PostMessageW(g_main, WM_APP_LOG, 0, (LPARAM)buf))
        free(buf);
}

static void AppendLog(const wchar_t *text)
{
    SYSTEMTIME st;
    wchar_t line[1200];
    GetLocalTime(&st);
    swprintf(line, 1200, L"[%02d:%02d:%02d] %s\r\n",
             st.wHour, st.wMinute, st.wSecond, text);
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void FormatError(DWORD err, wchar_t *out, size_t cch)
{
    out[0] = 0;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, out, (DWORD)cch, NULL);
    size_t n = wcslen(out);
    while (n && (out[n - 1] == L'\r' || out[n - 1] == L'\n' || out[n - 1] == L' '))
        out[--n] = 0;
    if (!out[0]) swprintf(out, cch, L"error %lu", err);
}

static BOOL IsElevatedNow(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD got = 0;
    BOOL result = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &got))
            result = el.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return result;
}

static BOOL EnableDebugPrivilege(void)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BOOL ok;
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) return FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL) &&
         GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Config file: path, load, save                                       */
/* ------------------------------------------------------------------ */

static void BuildConfigPath(void)
{
    wchar_t dir[MAX_PATH], probe[MAX_PATH], *slash;
    HANDLE h;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    /* Is the folder holding the executable writable? */
    swprintf(probe, MAX_PATH, L"%s\\.wtest_%lu.tmp", dir, GetCurrentProcessId());
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        swprintf(g_cfgPath, MAX_PATH, L"%s\\%s", dir, CONFIG_NAME);
        return;
    }
    wchar_t appdata[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        wchar_t sub[MAX_PATH];
        swprintf(sub, MAX_PATH, L"%s\\StuckCaptureBorder", appdata);
        CreateDirectoryW(sub, NULL);
        swprintf(g_cfgPath, MAX_PATH, L"%s\\%s", sub, CONFIG_NAME);
    } else {
        swprintf(g_cfgPath, MAX_PATH, L"%s\\%s", dir, CONFIG_NAME);
    }
}

static void ConfigDefaults(Config *c)
{
    ZeroMemory(c, sizeof(*c));
    c->tolerance    = 30.0;
    c->bandPx       = 6;
    c->sampleStride = 8;
    c->edgeCoverage = 0.70;
    c->minHotEdges  = 3;
    c->darkMode     = -1;        /* follow the Windows app mode */
}

/* Minimal reader for the JSON this program writes - which uses the same
 * field names as the PowerShell original, so either file loads. */
static const char *FindKey(const char *json, const char *key)
{
    char pat[64];
    const char *p;
    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (!*p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static BOOL ReadNum(const char *json, const char *key, double *out)
{
    const char *p = FindKey(json, key);
    if (!p) return FALSE;
    if (*p != '-' && *p != '+' && *p != '.' && (*p < '0' || *p > '9')) return FALSE;
    *out = atof(p);
    return TRUE;
}

static void LoadConfig(void)
{
    HANDLE h;
    DWORD size, got = 0;
    char *buf;
    double v;
    const char *fc, *bk, *co;

    ConfigDefaults(&g_cfg);
    h = CreateFileW(g_cfgPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 4u * 1024u * 1024u) { CloseHandle(h); return; }
    buf = (char *)xalloc((size_t)size + 1);
    if (!buf) { CloseHandle(h); return; }
    ReadFile(h, buf, size, &got, NULL);
    buf[got] = 0;
    CloseHandle(h);

    if (ReadNum(buf, "Tolerance", &v) && v > 0)    g_cfg.tolerance    = v;
    if (ReadNum(buf, "BandPx", &v) && v >= 1)      g_cfg.bandPx       = (int)v;
    if (ReadNum(buf, "SampleStride", &v) && v >= 1) g_cfg.sampleStride = (int)v;
    if (ReadNum(buf, "EdgeCoverage", &v) && v > 0) g_cfg.edgeCoverage = v;
    if (ReadNum(buf, "MinHotEdges", &v) && v >= 1) g_cfg.minHotEdges  = (int)v;
    if (ReadNum(buf, "DarkMode", &v))
        g_cfg.darkMode = (v < 0) ? -1 : (v != 0);
    if (ReadNum(buf, "StartInTray", &v))
        g_cfg.startInTray = (v != 0);

    fc = FindKey(buf, "FrameColor");
    if (fc && *fc == '{') {
        double r, g, b;
        if (ReadNum(fc, "R", &r) && ReadNum(fc, "G", &g) && ReadNum(fc, "B", &b)) {
            g_cfg.fr = (int)r; g_cfg.fg = (int)g; g_cfg.fb = (int)b;
            g_cfg.haveFrameColor = TRUE;
        }
    }

    bk = FindKey(buf, "BaselineKeys");
    if (bk && *bk == '[') {
        const char *p = bk + 1;
        while (*p && *p != ']') {
            if (*p == '"') {
                int r = 0, g = 0, b = 0;
                if (sscanf(p + 1, "%d-%d-%d", &r, &g, &b) == 3 &&
                    r >= 0 && r < 8 && g >= 0 && g < 8 && b >= 0 && b < 8) {
                    int idx = (r << 6) | (g << 3) | b;
                    if (!g_cfg.baseline[idx]) { g_cfg.baseline[idx] = TRUE; g_cfg.baselineCount++; }
                }
                p++;
                while (*p && *p != '"') p++;
                if (*p) p++;
                continue;
            }
            p++;
        }
    }

    co = FindKey(buf, "CalibratedOn");
    if (co && *co == '"') {
        int i = 0;
        const char *p = co + 1;
        while (*p && *p != '"' && i < 39) g_cfg.calibratedOn[i++] = (wchar_t)(unsigned char)*p++;
        g_cfg.calibratedOn[i] = 0;
    }
    free(buf);
}

static void SaveConfig(void)
{
    char *out = (char *)xalloc(64 * 1024);
    size_t n = 0;
    int first = 1, i;
    HANDLE h;
    if (!out) return;

    n += (size_t)sprintf(out + n, "{\n  \"BaselineKeys\": [");
    for (i = 0; i < NBUCKET; i++) {
        if (!g_cfg.baseline[i]) continue;
        n += (size_t)sprintf(out + n, "%s\"%d-%d-%d\"", first ? "" : ",",
                             (i >> 6) & 7, (i >> 3) & 7, i & 7);
        first = 0;
    }
    n += (size_t)sprintf(out + n, "],\n");
    if (g_cfg.haveFrameColor)
        n += (size_t)sprintf(out + n,
                             "  \"FrameColor\": { \"R\": %d, \"G\": %d, \"B\": %d },\n",
                             g_cfg.fr, g_cfg.fg, g_cfg.fb);
    else
        n += (size_t)sprintf(out + n, "  \"FrameColor\": null,\n");
    n += (size_t)sprintf(out + n,
        "  \"Tolerance\": %g,\n  \"BandPx\": %d,\n  \"SampleStride\": %d,\n"
        "  \"EdgeCoverage\": %g,\n  \"MinHotEdges\": %d,\n  \"DarkMode\": %d,\n"
        "  \"StartInTray\": %d,\n",
        g_cfg.tolerance, g_cfg.bandPx, g_cfg.sampleStride,
        g_cfg.edgeCoverage, g_cfg.minHotEdges, g_cfg.darkMode, g_cfg.startInTray);
    if (g_cfg.calibratedOn[0]) {
        char tmp[80];
        WideCharToMultiByte(CP_UTF8, 0, g_cfg.calibratedOn, -1, tmp, 80, NULL, NULL);
        n += (size_t)sprintf(out + n, "  \"CalibratedOn\": \"%s\"\n}\n", tmp);
    } else {
        n += (size_t)sprintf(out + n, "  \"CalibratedOn\": null\n}\n");
    }

    h = CreateFileW(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, out, (DWORD)n, &wrote, NULL);
        CloseHandle(h);
    } else {
        LogF(L"Could not write config to %s (error %lu).", g_cfgPath, GetLastError());
    }
    free(out);
}

/* ------------------------------------------------------------------ */
/* Pixel probe                                                         */
/* ------------------------------------------------------------------ */

static int BucketOf(int r, int g, int b)
{
    return ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
}

/* Fallback heuristic used before calibration: strongly saturated yellow. */
static BOOL IsDefaultBorderColor(int r, int g, int b)
{
    return r > 170 && g > 130 && b < 110 && (r - b) > 80 && (g - b) > 50;
}

static BOOL IsCalibratedColor(const Config *c, int r, int g, int b)
{
    double dr = r - c->fr, dg = g - c->fg, db = b - c->fb;
    return sqrt(dr * dr + dg * dg + db * db) <= c->tolerance;
}

static BOOL MatchPixel(const Config *c, int r, int g, int b)
{
    return c->haveFrameColor ? IsCalibratedColor(c, r, g, b)
                             : IsDefaultBorderColor(r, g, b);
}

typedef struct { int x, y, w, h; } StripRect;

static StripRect EdgeStrip(RECT b, int edge, int band)
{
    StripRect s;
    int W = b.right - b.left, H = b.bottom - b.top;
    if (band > W) band = W;
    if (band > H) band = H;
    switch (edge) {
    case EDGE_TOP:    s.x = b.left;         s.y = b.top;           s.w = W;    s.h = band; break;
    case EDGE_BOTTOM: s.x = b.left;         s.y = b.bottom - band; s.w = W;    s.h = band; break;
    case EDGE_LEFT:   s.x = b.left;         s.y = b.top;           s.w = band; s.h = H;    break;
    default:          s.x = b.right - band; s.y = b.top;           s.w = band; s.h = H;    break;
    }
    return s;
}

/*
 * Grab one thin strip along a screen edge via BitBlt (plain GDI - this does
 * NOT create a Windows.Graphics.Capture session) and scan it.
 *
 * coverage = fraction of sampled positions along the edge that hold at least
 * one matching pixel. When tally is non-NULL the colours are tallied instead
 * and no matching is done.
 */
static BOOL ScanEdge(HDC screen, RECT bounds, int edge, const Config *cfg,
                     Bucket *tally, double *coverage)
{
    StripRect s = EdgeStrip(bounds, edge, cfg->bandPx);
    BITMAPINFO bi;
    void *bits = NULL;
    HDC mem;
    HBITMAP dib;
    HGDIOBJ old;
    BOOL ok;

    *coverage = 0.0;
    if (s.w <= 0 || s.h <= 0) return TRUE;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = s.w;
    bi.bmiHeader.biHeight      = -s.h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem = CreateCompatibleDC(screen);
    if (!mem) return FALSE;
    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(mem); return FALSE; }
    old = SelectObject(mem, dib);
    ok = BitBlt(mem, 0, 0, s.w, s.h, screen, s.x, s.y, SRCCOPY);
    GdiFlush();

    if (ok) {
        const BYTE *px = (const BYTE *)bits;
        size_t stride = (size_t)s.w * 4;
        BOOL horizontal = (edge == EDGE_TOP || edge == EDGE_BOTTOM);
        int lng  = horizontal ? s.w : s.h;
        int deep = horizontal ? s.h : s.w;
        int step = cfg->sampleStride > 0 ? cfg->sampleStride : 1;
        int positions = 0, hits = 0, i, d;

        for (i = 0; i < lng; i += step) {
            BOOL hit = FALSE;
            positions++;
            for (d = 0; d < deep; d++) {
                const BYTE *p = horizontal ? px + (size_t)d * stride + (size_t)i * 4
                                           : px + (size_t)i * stride + (size_t)d * 4;
                int b = p[0], g = p[1], r = p[2];
                if (tally) {
                    Bucket *bk = &tally[BucketOf(r, g, b)];
                    bk->count++;
                    bk->sr += (unsigned)r; bk->sg += (unsigned)g; bk->sb += (unsigned)b;
                } else if (!hit && MatchPixel(cfg, r, g, b)) {
                    hit = TRUE;
                }
            }
            if (hit) hits++;
        }
        *coverage = positions > 0 ? (double)hits / positions : 0.0;
    }

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    return ok;
}

static BOOL CALLBACK MonCollect(HMONITOR mon, HDC dc, LPRECT rc, LPARAM data)
{
    MonList *ml = (MonList *)data;
    MONITORINFOEXW mi;
    (void)dc; (void)rc;
    if (ml->n >= MAX_MON) return FALSE;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
        ml->r[ml->n] = mi.rcMonitor;
        wcsncpy(ml->dev[ml->n], mi.szDevice, 39);
        ml->dev[ml->n][39] = 0;
        ml->n++;
    }
    return TRUE;
}

/* Probe every edge of every monitor. tally != NULL -> collect colours. */
static void Probe(const Config *cfg, ProbeResult *out, Bucket *tally)
{
    MonList ml;
    HDC screen;
    int m, e;

    ZeroMemory(&ml, sizeof(ml));
    ZeroMemory(out, sizeof(*out));
    EnumDisplayMonitors(NULL, NULL, MonCollect, (LPARAM)&ml);

    screen = GetDC(NULL);
    if (!screen) {
        out->failed = TRUE;
        wcscpy(out->error, L"GetDC(NULL) failed.");
        return;
    }
    for (m = 0; m < ml.n; m++) {
        wcscpy(out->mon[m].device, ml.dev[m]);
        for (e = 0; e < 4; e++) {
            double cov = 0.0;
            if (!ScanEdge(screen, ml.r[m], e, cfg, tally, &cov) && !out->failed) {
                out->failed = TRUE;
                swprintf(out->error, 200, L"Screen read failed on %s / %s (error %lu).",
                         ml.dev[m], kEdgeName[e], GetLastError());
            }
            out->mon[m].cov[e] = cov;
        }
        out->nmon = m + 1;
    }
    ReleaseDC(NULL, screen);

    if (!tally) {
        for (m = 0; m < out->nmon; m++) {
            int hot = 0;
            for (e = 0; e < 4; e++)
                if (out->mon[m].cov[e] >= cfg->edgeCoverage) hot++;
            if (hot >= cfg->minHotEdges) {
                out->present  = TRUE;
                out->hotEdges = hot;
                wcscpy(out->screen, out->mon[m].device);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Calibration                                                         */
/* ------------------------------------------------------------------ */

/* Step 1: record every quantised colour currently present at the edges. */
static int CalibrateBaseline(Config *cfg)
{
    Bucket *tally = (Bucket *)xalloc(sizeof(Bucket) * NBUCKET);
    ProbeResult pr;
    int i, count = 0;
    if (!tally) return -1;
    Probe(cfg, &pr, tally);
    ZeroMemory(cfg->baseline, sizeof(cfg->baseline));
    for (i = 0; i < NBUCKET; i++) {
        if (tally[i].count) { cfg->baseline[i] = TRUE; count++; }
    }
    cfg->baselineCount = count;
    free(tally);
    return count;
}

/* Step 2: whatever is at the edges now but was not in the baseline is the
 * frame; the most common such colour wins. */
static BOOL CalibrateFrame(Config *cfg, wchar_t *err, size_t errcch)
{
    Bucket *tally;
    ProbeResult pr;
    int i, best = -1;
    unsigned long long bestCount = 0;

    if (cfg->baselineCount == 0) {
        swprintf(err, errcch, L"No baseline recorded yet. Run step 1 while NO frame is visible.");
        return FALSE;
    }
    tally = (Bucket *)xalloc(sizeof(Bucket) * NBUCKET);
    if (!tally) { swprintf(err, errcch, L"Out of memory."); return FALSE; }
    Probe(cfg, &pr, tally);

    for (i = 0; i < NBUCKET; i++) {
        if (cfg->baseline[i]) continue;          /* present when clean -> not the border */
        if (tally[i].count > bestCount) { bestCount = tally[i].count; best = i; }
    }
    if (best < 0) {
        swprintf(err, errcch,
                 L"No new colours at the screen edges. Either the frame is not visible, "
                 L"or it is excluded from GDI capture on this machine - use the manual override.");
        free(tally);
        return FALSE;
    }
    cfg->fr = (int)(tally[best].sr / tally[best].count);
    cfg->fg = (int)(tally[best].sg / tally[best].count);
    cfg->fb = (int)(tally[best].sb / tally[best].count);
    cfg->haveFrameColor = TRUE;
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        swprintf(cfg->calibratedOn, 40, L"%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    free(tally);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Process candidates                                                  */
/* ------------------------------------------------------------------ */

static void StripExe(wchar_t *name)
{
    size_t n = wcslen(name);
    if (n > 4 && _wcsicmp(name + n - 4, L".exe") == 0) name[n - 4] = 0;
}

static BOOL IsKnownSuspect(const wchar_t *name)
{
    size_t i;
    for (i = 0; i < sizeof(kKnownSuspects) / sizeof(kKnownSuspects[0]); i++)
        if (_wcsicmp(name, kKnownSuspects[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL ModuleLooksLikeWgc(const wchar_t *mod)
{
    wchar_t low[MAX_PATH];
    size_t i, n = wcslen(mod);
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    for (i = 0; i < n; i++) low[i] = (wchar_t)towlower(mod[i]);
    low[n] = 0;
    if (wcsstr(low, L"windows.graphics.capture")) return TRUE;
    if (wcsncmp(low, L"graphicscapture", 15) == 0 && wcsstr(low, L".dll")) return TRUE;
    return FALSE;
}

/* TRUE if the module list could be read; *found says whether a capture DLL
 * was in it. */
static BOOL ProcessHasWgc(DWORD pid, BOOL *found)
{
    HANDLE snap;
    MODULEENTRY32W me;
    *found = FALSE;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (ModuleLooksLikeWgc(me.szModule)) { *found = TRUE; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return TRUE;
}

static int CandCompare(const void *a, const void *b)
{
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->rank != y->rank) return y->rank - x->rank;      /* rank descending */
    return _wcsicmp(x->name, y->name);
}

static void ScanCandidates(ScanResult *out)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    ZeroMemory(out, sizeof(*out));

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LogF(L"Process scan failed: could not snapshot the process list (error %lu).",
             GetLastError());
        return;
    }
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[64];
            BOOL hasWgc = FALSE, readable, known;
            DWORD sess = 0;
            Cand *c;

            if (pe.th32ProcessID <= 4) continue;
            wcsncpy(name, pe.szExeFile, 63);
            name[63] = 0;
            StripExe(name);

            readable = ProcessHasWgc(pe.th32ProcessID, &hasWgc);
            known    = IsKnownSuspect(name);
            if (!hasWgc && !known) continue;
            if (out->n >= MAX_CAND) break;

            if (!ProcessIdToSessionId(pe.th32ProcessID, &sess)) sess = (DWORD)-1;

            c = &out->item[out->n++];
            c->pid    = pe.th32ProcessID;
            c->sess   = sess;
            c->hasWgc = hasWgc;
            wcscpy(c->name, name);
            c->signal[0] = 0;
            if (hasWgc)   wcscat(c->signal, L"GraphicsCapture.dll loaded");
            if (known) {
                if (c->signal[0]) wcscat(c->signal, L"; ");
                wcscat(c->signal, L"known capture host");
            }
            if (!readable) {
                if (c->signal[0]) wcscat(c->signal, L"; ");
                wcscat(c->signal, L"modules unreadable");
            }
            c->rank = (hasWgc ? 10 : 0) + (known ? 5 : 0) + (sess == g_session ? 2 : 0);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    qsort(out->item, (size_t)out->n, sizeof(Cand), CandCompare);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static BOOL KillPid(DWORD pid, wchar_t *err, size_t errcch)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) { FormatError(GetLastError(), err, errcch); return FALSE; }
    if (!TerminateProcess(h, 1)) {
        FormatError(GetLastError(), err, errcch);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

/* Collect the pids of processes with the given image name in our session. */
static int FindProcesses(const wchar_t *exeName, DWORD sess, DWORD *pids, int max)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int n = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD s = 0;
            if (_wcsicmp(pe.szExeFile, exeName) != 0) continue;
            if (!ProcessIdToSessionId(pe.th32ProcessID, &s)) continue;
            if (s != sess) continue;
            if (n < max) pids[n++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

/*
 * Kill explorer.exe in this session and make sure it comes back.
 * AutoRestartShell usually revives it; if not, start it ourselves.
 */
static void ActRestartShell(void)
{
    DWORD pids[16];
    int n = FindProcesses(L"explorer.exe", g_session, pids, 16), i, killed = 0;
    wchar_t err[200];

    if (n == 0) { LogF(L"No explorer.exe running in this session."); return; }
    for (i = 0; i < n; i++)
        if (KillPid(pids[i], err, 200)) killed++;
    Sleep(2000);

    if (FindProcesses(L"explorer.exe", g_session, pids, 16) == 0) {
        SHELLEXECUTEINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.lpFile = L"explorer.exe";
        si.nShow  = SW_SHOWNORMAL;
        si.fMask  = SEE_MASK_NOASYNC;
        ShellExecuteExW(&si);
        Sleep(2000);
        LogF(L"Killed %d explorer process(es); restarted manually.", killed);
    } else {
        LogF(L"Killed %d explorer process(es); shell auto-restarted.", killed);
    }
}

static void ActRestartDwm(void)
{
    DWORD pids[16];
    int n, i, killed = 0;
    wchar_t err[200];

    if (!g_elevated) {
        LogF(L"DWM restart failed: DWM can only be restarted from an elevated process.");
        return;
    }
    EnableDebugPrivilege();
    n = FindProcesses(L"dwm.exe", g_session, pids, 16);
    if (n == 0) { LogF(L"DWM restart failed: no dwm.exe found for this session."); return; }
    for (i = 0; i < n; i++) {
        if (KillPid(pids[i], err, 200)) killed++;
        else LogF(L"Could not terminate dwm.exe (PID %lu): %s", pids[i], err);
    }
    Sleep(3000);
    if (killed) LogF(L"Terminated %d dwm.exe process(es); DWM restarts automatically.", killed);
}

static void ActKill(const Job *job)
{
    int i;
    wchar_t err[200];
    for (i = 0; i < job->nkill; i++) {
        if (KillPid(job->killPid[i], err, 200)) {
            LogF(L"Killed %s (PID %lu).", job->killName[i], job->killPid[i]);
            if (_wcsicmp(job->killName[i], L"explorer") == 0) {
                DWORD pids[16];
                Sleep(2000);
                if (FindProcesses(L"explorer.exe", g_session, pids, 16) == 0) {
                    SHELLEXECUTEINFOW si;
                    ZeroMemory(&si, sizeof(si));
                    si.cbSize = sizeof(si);
                    si.lpFile = L"explorer.exe";
                    si.nShow  = SW_SHOWNORMAL;
                    si.fMask  = SEE_MASK_NOASYNC;
                    ShellExecuteExW(&si);
                    LogF(L"Explorer restarted.");
                }
            }
        } else {
            LogF(L"Could not kill %s (PID %lu): %s", job->killName[i], job->killPid[i], err);
        }
    }
    Sleep(1000);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI WorkerProc(LPVOID param)
{
    Job *job = (Job *)param;

    switch (job->action) {
    case ACT_CAL_BASE: {
        int n = CalibrateBaseline(&g_cfg);
        if (n < 0) {
            LogF(L"Baseline calibration failed: out of memory.");
        } else {
            SaveConfig();
            PostMessageW(g_main, WM_APP_CALDONE, 0, 0);
            LogF(L"Baseline recorded: %d distinct edge colours. "
                 L"Now reproduce the frame and run step 2.", n);
        }
        break;
    }
    case ACT_CAL_FRAME: {
        wchar_t err[320];
        if (CalibrateFrame(&g_cfg, err, 320)) {
            SaveConfig();
            PostMessageW(g_main, WM_APP_CALDONE, 0, 0);
            LogF(L"Learned border colour RGB(%d,%d,%d). Probing to verify...",
                 g_cfg.fr, g_cfg.fg, g_cfg.fb);
        } else {
            LogF(L"Frame calibration failed: %s", err);
            job->doProbe = FALSE;
        }
        break;
    }
    case ACT_EXPLORER: ActRestartShell();  Sleep(1000); break;
    case ACT_DWM:      ActRestartDwm();    Sleep(1000); break;
    case ACT_KILL:     ActKill(job);                    break;
    default: break;
    }

    if (job->doProbe) {
        ProbeResult *pr = (ProbeResult *)xalloc(sizeof(ProbeResult));
        if (pr) {
            Probe(&g_cfg, pr, NULL);
            if (!PostMessageW(g_main, WM_APP_PROBE, 0, (LPARAM)pr)) free(pr);
        }
    }
    if (job->doScan) {
        ScanResult *sr = (ScanResult *)xalloc(sizeof(ScanResult));
        if (sr) {
            ScanCandidates(sr);
            if (!PostMessageW(g_main, WM_APP_SCAN, 0, (LPARAM)sr)) free(sr);
        }
    }

    free(job);
    PostMessageW(g_main, WM_APP_JOBDONE, 0, 0);
    return 0;
}

static void SetBusy(BOOL busy)
{
    g_busy = busy;
    EnableWindow(g_probe,    !busy);
    EnableWindow(g_refresh,  !busy);
    EnableWindow(g_calBase,  !busy);
    EnableWindow(g_calFrame, !busy);
    EnableWindow(g_calReset, !busy);
    UpdateFixButtons();
}

static void StartJob(const Job *job)
{
    Job *copy;
    HANDLE th;
    if (g_busy) return;
    copy = (Job *)xalloc(sizeof(Job));
    if (!copy) return;
    *copy = *job;
    SetBusy(TRUE);
    th = CreateThread(NULL, 0, WorkerProc, copy, 0, NULL);
    if (!th) {
        free(copy);
        SetBusy(FALSE);
        AppendLog(L"Could not start the worker thread.");
        return;
    }
    CloseHandle(th);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void UpdateFixButtons(void)
{
    BOOL manual  = (IsDlgButtonChecked(g_main, IDC_MANUAL) == BST_CHECKED);
    BOOL enabled = (manual || g_detected) && !g_busy;
    EnableWindow(g_explorer, enabled);
    EnableWindow(g_killSel,  enabled);
    EnableWindow(g_dwm,      enabled && g_elevated);
    SetWindowTextW(g_dwm, g_elevated ? L"Restart DWM (escalation)"
                                     : L"Restart DWM (needs admin)");
}

static void UpdateCalibrationDisplay(void)
{
    wchar_t text[MAX_PATH + 200];
    if (g_cfg.haveFrameColor) {
        swprintf(text, MAX_PATH + 200,
                 L"Calibrated: RGB(%d,%d,%d)  tolerance %g  |  baseline colours: %d  |  config: %s",
                 g_cfg.fr, g_cfg.fg, g_cfg.fb, g_cfg.tolerance, g_cfg.baselineCount, g_cfgPath);
    } else {
        swprintf(text, MAX_PATH + 200,
                 L"Not calibrated - using built-in yellow heuristic. "
                 L"Baseline colours: %d  |  config: %s",
                 g_cfg.baselineCount, g_cfgPath);
    }
    SetWindowTextW(g_calInfo, text);
    InvalidateRect(g_swatch, NULL, TRUE);
}

static void RenderProbe(const ProbeResult *pr)
{
    wchar_t detail[1024] = L"";
    int m, e;

    for (m = 0; m < pr->nmon; m++) {
        wchar_t part[160];
        swprintf(part, 160, L"%s%s [", m ? L"   " : L"", pr->mon[m].device);
        for (e = 0; e < 4; e++) {
            wchar_t one[24];
            swprintf(one, 24, L"%s%c:%.0f%%", e ? L" " : L"",
                     kEdgeName[e][0], pr->mon[m].cov[e] * 100.0);
            wcscat(part, one);
        }
        wcscat(part, L"]");
        if (wcslen(detail) + wcslen(part) < 1000) wcscat(detail, part);
    }
    SetWindowTextW(g_detail, detail);

    g_probed = TRUE;
    if (pr->failed) AppendLog(pr->error);

    if (pr->present) {
        g_detected = TRUE;
        SetWindowTextW(g_status, L"Status: BORDER DETECTED");
        LogF(L"Border detected on %s (%d hot edges).", pr->screen, pr->hotEdges);
    } else {
        g_detected = FALSE;
        SetWindowTextW(g_status, L"Status: clean (no border detected)");
        AppendLog(L"No border detected. If you can see one, tick the manual override.");
    }
    InvalidateRect(g_status, NULL, TRUE);
    UpdateFixButtons();

    if (pr->present) {
        wchar_t tip[128];
        swprintf(tip, ARRAYSIZE(tip), L"Capture frame detected on %s", pr->screen);
        TrayRefresh(tip);
    } else {
        TrayRefresh(NULL);
    }
}

static void RenderScan(const ScanResult *sr)
{
    int i;
    g_scan = *sr;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (i = 0; i < sr->n; i++) {
        LVITEMW it;
        wchar_t buf[32];
        ZeroMemory(&it, sizeof(it));
        it.mask     = LVIF_TEXT | LVIF_PARAM;
        it.iItem    = i;
        it.lParam   = (LPARAM)i;
        swprintf(buf, 32, L"%lu", sr->item[i].pid);
        it.pszText  = buf;
        ListView_InsertItem(g_list, &it);
        ListView_SetItemText(g_list, i, 1, (LPWSTR)sr->item[i].name);
        ListView_SetItemText(g_list, i, 2, (LPWSTR)sr->item[i].signal);
        if (sr->item[i].sess == (DWORD)-1) {
            ListView_SetItemText(g_list, i, 3, (LPWSTR)L"?");
        } else {
            swprintf(buf, 32, L"%lu", sr->item[i].sess);
            ListView_SetItemText(g_list, i, 3, buf);
        }
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
    LogF(L"Process scan complete: %d candidate(s).", sr->n);
}

static void DoLayout(int cw, int ch)
{
    int pad = S(12);
    int W   = cw - 2 * pad;
    int logTop = S(436);
    int logH   = ch - logTop - S(14);
    if (logH < S(60)) logH = S(60);

    MoveWindow(g_status,    pad,       S(12),  W,        S(32), TRUE);
    MoveWindow(g_detail,    S(14),     S(46),  cw - S(28), S(20), TRUE);
    MoveWindow(g_probe,     pad,       S(70),  S(130),   S(30), TRUE);
    MoveWindow(g_manual,    S(152),    S(76),  S(210),   S(24), TRUE);
    MoveWindow(g_auto,      S(370),    S(76),  S(170),   S(24), TRUE);
    MoveWindow(g_darkChk,   S(548),    S(76),  S(120),   S(24), TRUE);

    /* the calibration controls are siblings of the group box, so their
     * positions are relative to the client area, not to the box */
    {
        int gx = pad, gy = S(108);
        MoveWindow(g_grpCal,   gx,           gy,          W,         S(92), TRUE);
        MoveWindow(g_calBase,  gx + S(12),   gy + S(24),  S(210),    S(30), TRUE);
        MoveWindow(g_calFrame, gx + S(230),  gy + S(24),  S(210),    S(30), TRUE);
        MoveWindow(g_calReset, gx + S(448),  gy + S(24),  S(110),    S(30), TRUE);
        MoveWindow(g_swatch,   gx + S(570),  gy + S(24),  S(30),     S(30), TRUE);
        MoveWindow(g_calInfo,  gx + S(12),   gy + S(62),  W - S(24), S(20), TRUE);
    }

    MoveWindow(g_candLabel, pad,       S(208), S(400),   S(18), TRUE);
    MoveWindow(g_list,      pad,       S(228), W,        S(160), TRUE);
    MoveWindow(g_refresh,   pad,       S(396), S(130),   S(30), TRUE);
    MoveWindow(g_killSel,   S(150),    S(396), S(130),   S(30), TRUE);
    MoveWindow(g_explorer,  S(300),    S(396), S(190),   S(30), TRUE);
    MoveWindow(g_dwm,       S(498),    S(396), S(190),   S(30), TRUE);
    MoveWindow(g_log,       pad,       logTop, W,        logH,  TRUE);

    /* the signal column takes whatever width is left over */
    {
        int rest = W - S(70) - S(190) - S(50) - GetSystemMetrics(SM_CXVSCROLL) - S(4);
        if (rest < S(120)) rest = S(120);
        ListView_SetColumnWidth(g_list, 2, rest);
    }
}

static HWND MkCtl(const wchar_t *cls, const wchar_t *text, DWORD style,
                  int id, DWORD exStyle)
{
    /* WS_CLIPSIBLINGS matters for the owner-painted group box: without it its
       WM_PAINT would blank the buttons sitting inside it. */
    HWND h = CreateWindowExW(exStyle, cls, text,
                             style | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    return h;
}

static void CreateFonts(void)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        wcscpy(lf.lfFaceName, L"Segoe UI");
    }
    g_fontUi = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(14, g_dpi, 72);
    lf.lfWeight = FW_BOLD;
    g_fontBold  = CreateFontIndirectW(&lf);

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(9, g_dpi, 72);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Consolas");
    g_fontMono = CreateFontIndirectW(&lf);
}

static void CreateControls(void)
{
    LVCOLUMNW col;

    g_status = MkCtl(L"STATIC", L"Status: not probed yet", SS_LEFT, IDC_STATUS, 0);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    g_detail = MkCtl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_DETAIL, 0);

    g_probe  = MkCtl(L"BUTTON", L"Probe now", BS_PUSHBUTTON | WS_TABSTOP, IDC_PROBE, 0);
    g_manual = MkCtl(L"BUTTON", L"Manual override: I see the frame",
                     BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MANUAL, 0);
    g_auto   = MkCtl(L"BUTTON", L"Auto-probe every 10s",
                     BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTO, 0);
    g_darkChk = MkCtl(L"BUTTON", L"Dark mode",
                      BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DARK, 0);
    if (!Theme_Supported()) EnableWindow(g_darkChk, FALSE);

    g_grpCal   = MkCtl(L"BUTTON", L"Calibration (train on this PC)", BS_GROUPBOX, IDC_GRPCAL, 0);
    g_calBase  = MkCtl(L"BUTTON", L"1. Baseline (NO frame visible)",
                       BS_PUSHBUTTON | WS_TABSTOP, IDC_CALBASE, 0);
    g_calFrame = MkCtl(L"BUTTON", L"2. Learn frame (frame VISIBLE)",
                       BS_PUSHBUTTON | WS_TABSTOP, IDC_CALFRAME, 0);
    g_calReset = MkCtl(L"BUTTON", L"Reset", BS_PUSHBUTTON | WS_TABSTOP, IDC_CALRESET, 0);
    g_swatch   = MkCtl(L"STATIC", L"", SS_OWNERDRAW, IDC_SWATCH, 0);
    g_calInfo  = MkCtl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_CALINFO, 0);
    /* the group box is a sibling, so keep the buttons above it in z-order */
    SetWindowPos(g_grpCal, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    g_candLabel = MkCtl(L"STATIC", L"Candidate processes (ranked):", SS_LEFT, IDC_CANDLABEL, 0);

    g_list = MkCtl(WC_LISTVIEWW, L"",
                   LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER,
                   IDC_LIST, 0);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"PID";     col.cx = S(70);  ListView_InsertColumn(g_list, 0, &col);
    col.pszText = (LPWSTR)L"Process"; col.cx = S(190); ListView_InsertColumn(g_list, 1, &col);
    col.pszText = (LPWSTR)L"Signal";  col.cx = S(350); ListView_InsertColumn(g_list, 2, &col);
    col.pszText = (LPWSTR)L"Sess";    col.cx = S(50);  ListView_InsertColumn(g_list, 3, &col);

    g_refresh  = MkCtl(L"BUTTON", L"Rescan processes", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH, 0);
    g_killSel  = MkCtl(L"BUTTON", L"Kill selected", BS_PUSHBUTTON | WS_TABSTOP, IDC_KILLSEL, 0);
    g_explorer = MkCtl(L"BUTTON", L"Restart Explorer (primary fix)",
                       BS_PUSHBUTTON | WS_TABSTOP, IDC_EXPLORER, 0);
    g_dwm      = MkCtl(L"BUTTON", L"Restart DWM (escalation)",
                       BS_PUSHBUTTON | WS_TABSTOP, IDC_DWM, 0);

    g_log = MkCtl(L"EDIT", L"",
                  ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                  IDC_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
}

/* ------------------------------------------------------------------ */
/* Notification icon                                                   */
/* ------------------------------------------------------------------ */

static void TrayFill(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd   = g_main;
    nid->uID    = TRAY_UID;
}

/* Icon and tooltip follow the last verdict: colour means a frame is there. */
static void TrayRefresh(const wchar_t *detail)
{
    NOTIFYICONDATAW nid;
    if (!g_trayShown) return;
    TrayFill(&nid);
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon  = (g_probed && g_detected) ? g_icoColor : g_icoGray;
    swprintf(nid.szTip, ARRAYSIZE(nid.szTip), L"Fix Stuck Capture Border\n%s",
             detail ? detail
                    : (g_probed ? (g_detected ? L"Capture frame detected"
                                              : L"No capture frame detected")
                                : L"Not probed yet"));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid;
    TrayFill(&nid);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon            = (g_probed && g_detected) ? g_icoColor : g_icoGray;
    wcscpy(nid.szTip, L"Fix Stuck Capture Border");
    g_trayShown = Shell_NotifyIconW(NIM_ADD, &nid);
    if (g_trayShown) TrayRefresh(NULL);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayShown) return;
    TrayFill(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayShown = FALSE;
}

static void ShowMainWindow(void)
{
    ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main);
    /* The caption colour is only applied while the window is visible. */
    Theme_ApplyToMainWindow(g_main);
}

static void HideToTray(void)
{
    ShowWindow(g_main, SW_HIDE);
    if (!g_trayShown) TrayAdd();
}

/* ------------------------------------------------------------------ */
/* Run at Windows start (per user, no elevation needed)                */
/* ------------------------------------------------------------------ */

static void QuotedExePath(wchar_t *out, size_t cch)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    swprintf(out, cch, L"\"%s\"", path);
}

static BOOL IsAutoRunEnabled(void)
{
    HKEY k;
    wchar_t want[MAX_PATH + 4], have[MAX_PATH + 4];
    DWORD cb = sizeof(have), type = 0;
    BOOL match = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(k, AUTORUN_VALUE, NULL, &type, (LPBYTE)have, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        have[(cb / sizeof(wchar_t)) < ARRAYSIZE(have) ? cb / sizeof(wchar_t) : ARRAYSIZE(have) - 1] = 0;
        QuotedExePath(want, ARRAYSIZE(want));
        match = (_wcsicmp(have, want) == 0);
    }
    RegCloseKey(k);
    return match;
}

static BOOL SetAutoRun(BOOL enable)
{
    HKEY k;
    LONG rc;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    if (enable) {
        wchar_t cmd[MAX_PATH + 4];
        QuotedExePath(cmd, ARRAYSIZE(cmd));
        rc = RegSetValueExW(k, AUTORUN_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                            (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, AUTORUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

/* Re-applies the current mode to every control. Safe to call repeatedly. */
static void ApplyTheme(void)
{
    HWND buttons[] = { g_probe, g_calBase, g_calFrame, g_calReset,
                       g_refresh, g_killSel, g_explorer, g_dwm };
    HWND checks[]  = { g_manual, g_auto, g_darkChk };
    HWND labels[]  = { g_status, g_detail, g_calInfo, g_candLabel, g_swatch };
    size_t i;

    for (i = 0; i < ARRAYSIZE(buttons); i++) Theme_ApplyToButton(buttons[i]);
    for (i = 0; i < ARRAYSIZE(checks);  i++) Theme_ApplyToCheckBox(checks[i]);
    for (i = 0; i < ARRAYSIZE(labels);  i++) InvalidateRect(labels[i], NULL, TRUE);
    Theme_ApplyToGroupBox(g_grpCal);
    Theme_ApplyToListView(g_list);
    Theme_ApplyToEdit(g_log);
    Theme_ApplyToMainWindow(g_main);
    RedrawWindow(g_main, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void SetDarkMode(BOOL dark)
{
    Theme_SetDark(dark);
    CheckDlgButton(g_main, IDC_DARK, dark ? BST_CHECKED : BST_UNCHECKED);
    ApplyTheme();
}

static void OnManualToggle(void)
{
    UpdateFixButtons();
}

static void OnKillSelected(void)
{
    Job job;
    int i = -1;
    ZeroMemory(&job, sizeof(job));
    job.action  = ACT_KILL;
    job.doProbe = TRUE;
    job.doScan  = TRUE;

    for (;;) {
        LVITEMW it;
        int idx;
        i = ListView_GetNextItem(g_list, i, LVNI_SELECTED);
        if (i < 0) break;
        ZeroMemory(&it, sizeof(it));
        it.mask  = LVIF_PARAM;
        it.iItem = i;
        if (!ListView_GetItem(g_list, &it)) continue;
        idx = (int)it.lParam;
        if (idx < 0 || idx >= g_scan.n) continue;
        if (_wcsicmp(g_scan.item[idx].name, L"dwm") == 0) {
            AppendLog(L"Use the DWM button for dwm.exe.");
            continue;
        }
        if (job.nkill >= MAX_KILL) break;
        job.killPid[job.nkill] = g_scan.item[idx].pid;
        wcscpy(job.killName[job.nkill], g_scan.item[idx].name);
        job.nkill++;
    }
    if (job.nkill == 0) { AppendLog(L"Nothing selected."); return; }
    StartJob(&job);
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    UINT cmd;
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_cfg.startInTray ? MF_CHECKED : 0),
                IDM_STARTTRAY, L"Start minimised to tray");
    AppendMenuW(menu, MF_STRING | (IsAutoRunEnabled() ? MF_CHECKED : 0),
                IDM_AUTORUN, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(menu, IDM_SHOW, FALSE);

    GetCursorPos(&pt);
    /* Without the foreground switch the menu never closes on click-away. */
    SetForegroundWindow(g_main);
    cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                               pt.x, pt.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

static void ShowAbout(void)
{
    wchar_t text[1024];
    swprintf(text, ARRAYSIZE(text),
             L"Fix Stuck Capture Border %s\n"
             L"Copyright \x00A9 2026 Rekow IT\n\n"
             L"Detects and clears a stuck Windows Graphics Capture frame.\n"
             L"Probing uses GDI only and cannot itself start a capture session.\n\n"
             L"Configuration:\n%s",
             APP_VER_WSTRING, g_cfgPath);
    MessageBoxW(g_main, text, L"About Fix Stuck Capture Border",
                MB_OK | MB_ICONINFORMATION);
}

/* Double click on the tray icon: go straight for the primary fix. */
static void TrayFixNow(void)
{
    Job job;
    if (g_busy) { LogF(L"Tray: busy, ignoring the fix request."); return; }
    ZeroMemory(&job, sizeof(job));
    job.action  = ACT_EXPLORER;
    job.doProbe = TRUE;
    job.doScan  = TRUE;
    AppendLog(L"Tray: restarting Explorer to clear the frame.");
    StartJob(&job);
}

static void OnRestartDwm(void)
{
    Job job;
    int answer = MessageBoxW(g_main,
        L"This terminates dwm.exe. The screen will go black for a second or two "
        L"and DWM restarts automatically.\n\n"
        L"Over a remote session the connection may drop briefly. Continue?",
        L"Restart DWM", MB_YESNO | MB_ICONWARNING);
    if (answer != IDYES) return;
    ZeroMemory(&job, sizeof(job));
    job.action  = ACT_DWM;
    job.doProbe = TRUE;
    StartJob(&job);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Explorer recreates the tray after a restart - including one this very
       program triggered - and every icon has to be added again. */
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        g_trayShown = FALSE;
        TrayAdd();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        CreateFonts();
        CreateControls();
        return 0;

    case WM_SIZE:
        DoLayout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = S(700);
        mmi->ptMinTrackSize.y = S(600);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, Theme_BgBrush());
        return 1;
    }

    /* A read-only edit asks with WM_CTLCOLORSTATIC, not WM_CTLCOLOREDIT. */
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_log) {
            SetTextColor(dc, Theme_Text());
            SetBkColor(dc, Theme_Surface());
            return (LRESULT)Theme_SurfaceBrush();
        }
        if (ctl == g_status) {
            SetTextColor(dc, g_probed ? (g_detected ? Theme_Warn() : Theme_Ok())
                                      : Theme_Text());
        } else if (ctl == g_detail || ctl == g_calInfo) {
            SetTextColor(dc, Theme_DimText());
        } else {
            SetTextColor(dc, Theme_Text());
        }
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)Theme_BgBrush();
    }

    case WM_SETTINGCHANGE:
        /* Only follow Windows while the user has not chosen a mode here. */
        if (g_cfg.darkMode < 0 && Theme_IsColorSchemeChange(msg, lp))
            SetDarkMode(Theme_SystemPrefersDark());
        break;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlID == IDC_SWATCH) {
            HBRUSH br = g_cfg.haveFrameColor
                        ? CreateSolidBrush(RGB(g_cfg.fr, g_cfg.fg, g_cfg.fb))
                        : NULL;
            HBRUSH fr = CreateSolidBrush(Theme_Line());
            FillRect(di->hDC, &di->rcItem, br ? br : Theme_BgBrush());
            FrameRect(di->hDC, &di->rcItem, fr);
            DeleteObject(fr);
            if (br) DeleteObject(br);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                int idx = (int)cd->nmcd.lItemlParam;
                if (idx >= 0 && idx < g_scan.n && g_scan.item[idx].hasWgc) {
                    cd->clrTextBk = Theme_RowHighlight();
                    cd->clrText   = Theme_Text();
                }
                return CDRF_DODEFAULT;
            }
            default: return CDRF_DODEFAULT;
            }
        }
        break;
    }

    case WM_TIMER:
        if (wp == IDT_AUTOPROBE && !g_busy) {
            Job job;
            ZeroMemory(&job, sizeof(job));
            job.doProbe = TRUE;
            StartJob(&job);
        } else if (wp == IDT_TRAYCLICK) {
            /* No second click arrived, so it really was a single one. */
            KillTimer(hwnd, IDT_TRAYCLICK);
            ShowMainWindow();
        }
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            /* Hold the single-click action back until a double click can no
               longer follow, otherwise both fire. */
            SetTimer(hwnd, IDT_TRAYCLICK, GetDoubleClickTime(), NULL);
            return 0;
        case WM_LBUTTONDBLCLK:
            KillTimer(hwnd, IDT_TRAYCLICK);
            TrayFixNow();
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            return 0;
        default:
            return 0;
        }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            HideToTray();
            return 0;
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        Job job;
        ZeroMemory(&job, sizeof(job));
        switch (id) {
        case IDC_PROBE:
            job.doProbe = TRUE;
            StartJob(&job);
            return 0;
        case IDC_REFRESH:
            job.doScan = TRUE;
            StartJob(&job);
            return 0;
        case IDC_MANUAL:
            OnManualToggle();
            return 0;
        case IDM_SHOW:
            ShowMainWindow();
            return 0;
        case IDM_STARTTRAY:
            g_cfg.startInTray = !g_cfg.startInTray;
            SaveConfig();
            LogF(g_cfg.startInTray ? L"Will start minimised to the tray."
                                   : L"Will start with the window open.");
            return 0;
        case IDM_AUTORUN: {
            BOOL want = !IsAutoRunEnabled();
            if (SetAutoRun(want))
                LogF(want ? L"Added to the Windows startup items."
                          : L"Removed from the Windows startup items.");
            else
                LogF(L"Could not change the startup entry (error %lu).", GetLastError());
            return 0;
        }
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case IDC_DARK: {
            BOOL dark = (IsDlgButtonChecked(hwnd, IDC_DARK) == BST_CHECKED);
            g_cfg.darkMode = dark ? 1 : 0;
            SaveConfig();
            SetDarkMode(dark);
            AppendLog(dark ? L"Dark mode on." : L"Dark mode off.");
            return 0;
        }
        case IDC_AUTO:
            if (IsDlgButtonChecked(hwnd, IDC_AUTO) == BST_CHECKED) {
                SetTimer(hwnd, IDT_AUTOPROBE, 10000, NULL);
                AppendLog(L"Auto-probe on.");
            } else {
                KillTimer(hwnd, IDT_AUTOPROBE);
                AppendLog(L"Auto-probe off.");
            }
            return 0;
        case IDC_CALBASE:
            job.action = ACT_CAL_BASE;
            StartJob(&job);
            return 0;
        case IDC_CALFRAME:
            job.action  = ACT_CAL_FRAME;
            job.doProbe = TRUE;
            StartJob(&job);
            return 0;
        case IDC_CALRESET:
            ConfigDefaults(&g_cfg);
            SaveConfig();
            UpdateCalibrationDisplay();
            AppendLog(L"Calibration reset to defaults.");
            return 0;
        case IDC_EXPLORER:
            job.action  = ACT_EXPLORER;
            job.doProbe = TRUE;
            job.doScan  = TRUE;
            StartJob(&job);
            return 0;
        case IDC_DWM:
            OnRestartDwm();
            return 0;
        case IDC_KILLSEL:
            OnKillSelected();
            return 0;
        default: break;
        }
        break;
    }

    case WM_APP_LOG: {
        wchar_t *text = (wchar_t *)lp;
        if (text) { AppendLog(text); free(text); }
        return 0;
    }
    case WM_APP_PROBE: {
        ProbeResult *pr = (ProbeResult *)lp;
        if (pr) { RenderProbe(pr); free(pr); }
        return 0;
    }
    case WM_APP_SCAN: {
        ScanResult *sr = (ScanResult *)lp;
        if (sr) { RenderScan(sr); free(sr); }
        return 0;
    }
    case WM_APP_CALDONE:
        UpdateCalibrationDisplay();
        return 0;
    case WM_APP_JOBDONE:
        SetBusy(FALSE);
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, IDT_AUTOPROBE);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    HDC dc;
    Job job;
    int nmon;

    (void)prev; (void)cmdline;
    g_inst = inst;

    /* DPI awareness must be set before any window or GDI work, or screen
     * coordinates get scaled and the edge strips are sampled in the wrong
     * place on high-DPI setups. */
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        PFN_SPDAC spdac = u32 ? (PFN_SPDAC)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        /* DPI_AWARENESS_CONTEXT_SYSTEM_AWARE == (HANDLE)-2 */
        if (!spdac || !spdac((HANDLE)-2))
            SetProcessDPIAware();
    }

    dc = GetDC(NULL);
    if (dc) { g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;            /* WM_ERASEBKGND paints the theme */
    wc.lpszClassName = APP_CLASS;
    wc.hIcon   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    BuildConfigPath();
    LoadConfig();

    /* The mode has to be set before any control exists, or the controls come
       up light and only repaint on the first switch. */
    Theme_Init();
    Theme_SetDark(g_cfg.darkMode < 0 ? Theme_SystemPrefersDark()
                                     : g_cfg.darkMode != 0);

    g_elevated = IsElevatedNow();
    ProcessIdToSessionId(GetCurrentProcessId(), &g_session);

    {
        RECT r = { 0, 0, S(764), S(621) };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        g_main = CreateWindowExW(0, APP_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top,
                                 NULL, NULL, inst, NULL);
    }
    if (!g_main) return 1;

    /* centre on the work area of the monitor we landed on */
    {
        MONITORINFO mi;
        RECT wr;
        mi.cbSize = sizeof(mi);
        GetWindowRect(g_main, &wr);
        if (GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            int w = wr.right - wr.left, h = wr.bottom - wr.top;
            SetWindowPos(g_main, NULL,
                         mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2,
                         mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }

    CheckDlgButton(g_main, IDC_DARK, Theme_IsDark() ? BST_CHECKED : BST_UNCHECKED);
    ApplyTheme();

    g_icoColor = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    g_icoGray  = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON_GRAY), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_icoGray)  g_icoGray  = g_icoColor;
    if (!g_icoColor) g_icoColor = g_icoGray;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();

    if (g_cfg.startInTray) {
        ShowWindow(g_main, SW_HIDE);
    } else {
        ShowWindow(g_main, show);
        UpdateWindow(g_main);
        /* Again now that the window is visible: the caption only takes the new
           colour on an activation change. */
        Theme_ApplyToMainWindow(g_main);
    }

    nmon = GetSystemMetrics(SM_CMONITORS);
    LogF(L"Elevated: %s  |  Monitors: %d", g_elevated ? L"True" : L"False", nmon);
    AppendLog(L"Probe uses GDI (BitBlt) only - it cannot itself trigger a capture border.");
    UpdateCalibrationDisplay();
    UpdateFixButtons();

    ZeroMemory(&job, sizeof(job));
    job.doProbe = TRUE;
    job.doScan  = TRUE;
    StartJob(&job);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_fontUi)   DeleteObject(g_fontUi);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontMono) DeleteObject(g_fontMono);
    if (g_swatchBrush) DeleteObject(g_swatchBrush);
    return 0;
}
