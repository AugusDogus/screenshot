#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wchar.h>

#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Msimg32.lib")

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif

typedef enum {
    HT_NONE = -1,
    HT_TL = 0, HT_T, HT_TR,
    HT_L,          HT_R,
    HT_BL, HT_B, HT_BR
} HANDLE_ID;

typedef struct {
    // Virtual desktop capture
    RECT    virt;
    HBITMAP hbmCapture;
    HDC     hdcCapture;

    // 1x1 black DIB kept selected for AlphaBlend
    HBITMAP hbmBlack;
    HDC     hdcBlack;

    // Back buffer for double buffering
    HBITMAP hbmBack;
    HDC     hdcBack;
    int     backW, backH;

    // Selection state
    BOOL    selecting;
    BOOL    resizing;
    BOOL    moving;
    BOOL    haveSel;
    POINT   dragStart;
    POINT   dragCur;
    POINT   lastMouse;
    RECT    sel;            // normalized selection
    HANDLE_ID hotHandle;
    HANDLE_ID activeHandle;

    // Resize anchor: rect frozen at resize start (updated on flips)
    RECT    resizeAnchor;

    // Move state: mouse-down offset to selection's top-left
    POINT   moveOffset;

    // UI font
    HFONT   hFontSmall;

    // Cached GDI objects & blend params
    HPEN    hPenHandle;     // solid white (handle outlines)
    HPEN    hPenDotted;     // dotted white (selection border)
    HBRUSH  hBrushBlack;    // black (handles fill & label bg)
    BLENDFUNCTION blendFn;  // overlay alpha
} APPSTATE;

static APPSTATE g = {0};

// ---- Tuning knobs ----
static const BYTE OVERLAY_ALPHA = 100;      // 0..255 (higher = darker)
static const int  HANDLE_SIZE   = 3;        // half-size (px) for small squares
static const int  HANDLE_BORDER_WIDTH = 1;  // handle border width
static const int  BORDER_WIDTH  = 2;        // selection border & handle outline
static const int  MIN_SEL_SIZE  = 2;
// ----------------------

static RECT MakeRectFromPoints(POINT a, POINT b) {
    RECT r;
    r.left   = (a.x < b.x) ? a.x : b.x;
    r.right  = (a.x < b.x) ? b.x : a.x;
    r.top    = (a.y < b.y) ? a.y : b.y;
    r.bottom = (a.y < b.y) ? b.y : a.y;
    return r;
}
static void NormalizeRect(RECT* r){
    if (r->left > r->right){ LONG t=r->left; r->left=r->right; r->right=t; }
    if (r->top  > r->bottom){ LONG t=r->top;  r->top =r->bottom; r->bottom=t; }
}
static int RectW(const RECT* r){ return r->right - r->left; }
static int RectH(const RECT* r){ return r->bottom - r->top; }

static void ClampRectToClient(RECT* r, const RECT* client){
    if (r->left   < client->left)   r->left   = client->left;
    if (r->top    < client->top)    r->top    = client->top;
    if (r->right  > client->right)  r->right  = client->right;
    if (r->bottom > client->bottom) r->bottom = client->bottom;
}

static BOOL CaptureVirtualScreen(void) {
    // Virtual desktop rect
    g.virt.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g.virt.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g.virt.right  = g.virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g.virt.bottom = g.virt.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int W = g.virt.right - g.virt.left;
    int H = g.virt.bottom - g.virt.top;

    // Capture desktop to compatible bitmap
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return FALSE;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, W, H);
    if (!hdcMem || !hbm) {
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BOOL ok = BitBlt(hdcMem, 0, 0, W, H, hdcScreen, g.virt.left, g.virt.top, SRCCOPY | CAPTUREBLT);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (!ok) { DeleteObject(hbm); return FALSE; }

    g.hdcCapture = CreateCompatibleDC(NULL);
    SelectObject(g.hdcCapture, hbm);
    g.hbmCapture = hbm;

    // --- 1x1 black DIB for AlphaBlend (kept selected in g.hdcBlack) ---
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = 1;
    bi.bmiHeader.biHeight      = 1;     // bottom-up
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;    // AlphaBlend-friendly
    bi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = NULL;
    g.hbmBlack  = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    if (!g.hbmBlack) return FALSE;
    *(DWORD*)pvBits = 0xFF000000;       // ARGB black

    g.hdcBlack = CreateCompatibleDC(NULL);
    if (!g.hdcBlack) { DeleteObject(g.hbmBlack); g.hbmBlack=NULL; return FALSE; }
    SelectObject(g.hdcBlack, g.hbmBlack); // keep selected

    // Small font for label
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -14;                    // ~11-12pt typical
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    g.hFontSmall = CreateFontIndirectW(&lf);

    // ---- Cached GDI assets ----
    g.hPenHandle  = CreatePen(PS_SOLID, HANDLE_BORDER_WIDTH, RGB(255,255,255));

    LOGBRUSH lb; lb.lbStyle = BS_SOLID; lb.lbColor = RGB(255,255,255); lb.lbHatch = 0;
    g.hPenDotted  = ExtCreatePen(PS_GEOMETRIC | PS_DOT | PS_ENDCAP_FLAT | PS_JOIN_MITER,
                                 BORDER_WIDTH, &lb, 0, NULL);

    g.hBrushBlack = CreateSolidBrush(RGB(0,0,0));

    g.blendFn.BlendOp = AC_SRC_OVER;
    g.blendFn.BlendFlags = 0;
    g.blendFn.SourceConstantAlpha = OVERLAY_ALPHA;
    g.blendFn.AlphaFormat = 0;

    return (g.hPenHandle && g.hPenDotted && g.hBrushBlack) ? TRUE : FALSE;
}

static void DestroyBackBuffer(void) {
    if (g.hdcBack) { DeleteDC(g.hdcBack); g.hdcBack = NULL; }
    if (g.hbmBack) { DeleteObject(g.hbmBack); g.hbmBack = NULL; }
    g.backW = g.backH = 0;
}

static BOOL EnsureBackBuffer(HWND hwnd, int w, int h) {
    if (g.hdcBack && w == g.backW && h == g.backH) return TRUE;

    DestroyBackBuffer();
    HDC hdc = GetDC(hwnd);
    g.hdcBack = CreateCompatibleDC(hdc);
    g.hbmBack = CreateCompatibleBitmap(hdc, w, h);
    ReleaseDC(hwnd, hdc);
    if (!g.hdcBack || !g.hbmBack) return FALSE;

    SelectObject(g.hdcBack, g.hbmBack);
    g.backW = w; g.backH = h;
    return TRUE;
}

static void Cleanup(void){
    if (g.hdcCapture){ DeleteDC(g.hdcCapture); g.hdcCapture=NULL; }
    if (g.hbmCapture){ DeleteObject(g.hbmCapture); g.hbmCapture=NULL; }

    if (g.hdcBlack){ DeleteDC(g.hdcBlack); g.hdcBlack=NULL; }
    if (g.hbmBlack){ DeleteObject(g.hbmBlack); g.hbmBlack=NULL; }

    if (g.hFontSmall){ DeleteObject(g.hFontSmall); g.hFontSmall=NULL; }

    if (g.hPenHandle)  { DeleteObject(g.hPenHandle);  g.hPenHandle  = NULL; }
    if (g.hPenDotted)  { DeleteObject(g.hPenDotted);  g.hPenDotted  = NULL; }
    if (g.hBrushBlack) { DeleteObject(g.hBrushBlack); g.hBrushBlack = NULL; }

    DestroyBackBuffer();
}

// --- Handle helpers & swapping ---
static void GetHandleCenters(const RECT* r, POINT p[8]){
    LONG cx = (r->left + r->right)/2;
    LONG cy = (r->top  + r->bottom)/2;
    p[HT_TL].x = r->left;  p[HT_TL].y = r->top;
    p[HT_T].x  = cx;       p[HT_T].y  = r->top;
    p[HT_TR].x = r->right; p[HT_TR].y = r->top;
    p[HT_L].x  = r->left;  p[HT_L].y  = cy;
    p[HT_R].x  = r->right; p[HT_R].y  = cy;
    p[HT_BL].x = r->left;  p[HT_BL].y = r->bottom;
    p[HT_B].x  = cx;       p[HT_B].y  = r->bottom;
    p[HT_BR].x = r->right; p[HT_BR].y = r->bottom;
}
static RECT HandleRectAt(POINT c){
    int hs = HANDLE_SIZE;
    RECT rr = { c.x - hs, c.y - hs, c.x + hs, c.y + hs };
    return rr;
}
static HANDLE_ID HitTestHandles(POINT pt, const RECT* r){
    if (RectW(r) < 1 || RectH(r) < 1) return HT_NONE;
    POINT centers[8]; GetHandleCenters(r, centers);
    for (int i=0;i<8;i++){
        RECT h = HandleRectAt(centers[i]);
        if (PtInRect(&h, pt)) return (HANDLE_ID)i;
    }
    // Thin edge hitboxes for convenience
    const int EDGE = HANDLE_SIZE + 2;
    RECT top = { r->left+EDGE, r->top-EDGE, r->right-EDGE, r->top+EDGE };
    RECT bot = { r->left+EDGE, r->bottom-EDGE, r->right-EDGE, r->bottom+EDGE };
    RECT lef = { r->left-EDGE, r->top+EDGE, r->left+EDGE, r->bottom-EDGE };
    RECT rig = { r->right-EDGE, r->top+EDGE, r->right+EDGE, r->bottom-EDGE };
    if (PtInRect(&top, pt)) return HT_T;
    if (PtInRect(&bot, pt)) return HT_B;
    if (PtInRect(&lef, pt)) return HT_L;
    if (PtInRect(&rig, pt)) return HT_R;
    return HT_NONE;
}
static LPCWSTR CursorForHandle(HANDLE_ID h){
    switch(h){
        case HT_T: case HT_B: return IDC_SIZENS;
        case HT_L: case HT_R: return IDC_SIZEWE;
        case HT_TL: case HT_BR: return IDC_SIZENWSE;
        case HT_TR: case HT_BL: return IDC_SIZENESW;
        default: return IDC_CROSS;
    }
}
static HANDLE_ID SwapH(HANDLE_ID h){
    switch(h){
        case HT_L: return HT_R; case HT_R: return HT_L;
        case HT_TL: return HT_TR; case HT_TR: return HT_TL;
        case HT_BL: return HT_BR; case HT_BR: return HT_BL;
        default: return h;
    }
}
static HANDLE_ID SwapV(HANDLE_ID h){
    switch(h){
        case HT_T: return HT_B; case HT_B: return HT_T;
        case HT_TL: return HT_BL; case HT_BL: return HT_TL;
        case HT_TR: return HT_BR; case HT_BR: return HT_TR;
        default: return h;
    }
}

// --- Drawing helpers ---
static void DrawHandles(HDC hdc, const RECT* r){
    POINT centers[8]; GetHandleCenters(r, centers);
    HGDIOBJ oldPen = SelectObject(hdc, g.hPenHandle);
    HGDIOBJ oldBr  = SelectObject(hdc, g.hBrushBlack);
    for (int i=0;i<8;i++){
        RECT hr = HandleRectAt(centers[i]);
        Rectangle(hdc, hr.left, hr.top, hr.right, hr.bottom);
    }
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
}

// Compute label box with fallbacks: outside-left -> above -> inside (float).
static RECT ComputeLabelBoxTopLeftWithFallbackDims(const RECT* sel, const RECT* client,
                                                   int boxW, int boxH,
                                                   int outside, int insidePad)
{
    RECT s = *sel;
    if (s.left > s.right){ LONG t=s.left; s.left=s.right; s.right=t; }
    if (s.top  > s.bottom){ LONG t=s.top;  s.top =s.bottom; s.bottom=t; }

    // Try: outside-left, top-aligned
    RECT box;
    box.right  = s.left - outside;
    box.left   = box.right - boxW;
    box.top    = s.top;
    box.bottom = box.top + boxH;

    if (box.left >= client->left) return box;

    // Fallback 1: above top-left
    box.left   = s.left;
    box.right  = box.left + boxW;
    box.bottom = s.top - outside;
    box.top    = box.bottom - boxH;

    if (box.top >= client->top) return box;

    // Fallback 2: inside near top-left (float on top of the box)
    box.left   = s.left + insidePad;
    box.top    = s.top  + insidePad;
    box.right  = box.left + boxW;
    box.bottom = box.top + boxH;

    // Keep within client (never off-screen)
    if (box.left < client->left)   { int dx = client->left - box.left;   box.left += dx; box.right += dx; }
    if (box.top  < client->top)    { int dy = client->top  - box.top;    box.top  += dy; box.bottom+= dy; }
    if (box.right > client->right) { int dx = box.right - client->right; box.left -= dx; box.right -= dx; }
    if (box.bottom> client->bottom){ int dy = box.bottom- client->bottom;box.top  -= dy; box.bottom-= dy; }

    return box;
}

// Draw DIMENSIONS label at top-left (with fallbacks). Format: WIDTHxHEIGHT.
static void DrawDimsLabelTopLeftWithFallback(HDC hdc, const RECT* sel, const RECT* client){
    RECT s = *sel; NormalizeRect(&s);
    int w = RectW(&s), h = RectH(&s);

    wchar_t buf[64];
    swprintf(buf, 64, L"%dx%d", w, h); // dimensions

    HFONT oldF = NULL;
    if (g.hFontSmall) oldF = (HFONT)SelectObject(hdc, g.hFontSmall);

    SIZE sz;
    GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);

    const int padX = 6, padY = 3;
    const int outside = HANDLE_SIZE + 6; // distance from border/handle
    const int insidePad = 6;             // inside padding

    int boxW = sz.cx + padX*2;
    int boxH = sz.cy + padY*2;

    RECT box = ComputeLabelBoxTopLeftWithFallbackDims(sel, client, boxW, boxH, outside, insidePad);

    // Background (cached brush)
    FillRect(hdc, &box, g.hBrushBlack);

    // Text
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240,240,240));
    RECT txt = { box.left + padX, box.top + padY, box.right - padX, box.bottom - padY };
    DrawTextW(hdc, buf, -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (oldF) SelectObject(hdc, oldF);
}

static void PaintOverlay(HWND hwnd, HDC hdc){
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // 1) Base image
    BitBlt(hdc, 0, 0, W, H, g.hdcCapture, 0, 0, SRCCOPY);

    // 2) Darken with AlphaBlend (cached blendFn & black 1x1)
    AlphaBlend(hdc, 0, 0, W, H, g.hdcBlack, 0, 0, 1, 1, g.blendFn);

    // 3) Selection: restore brightness + dotted border + handles + label
    if (g.haveSel){
        RECT s = g.sel; NormalizeRect(&s);

        // Restore selection area from original capture
        BitBlt(hdc, s.left, s.top, RectW(&s), RectH(&s), g.hdcCapture, s.left, s.top, SRCCOPY);

        // Dotted white border (cached pen)
        HGDIOBJ oldPen = SelectObject(hdc, g.hPenDotted);
        HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, s.left, s.top, s.right, s.bottom);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);

        // Handles
        DrawHandles(hdc, &s);

        // Dimensions label with fallback placement
        DrawDimsLabelTopLeftWithFallback(hdc, &s, &rc);
    }
}

// --- Invalidation helpers ---
static RECT InflateForUI(RECT r) {
    const int pad = 40; // covers handles, dotted border, and label area
    InflateRect(&r, pad, pad);
    return r;
}

// EXACT label rect measurement (matches DrawDimsLabelTopLeftWithFallback)
static RECT ExactLabelRectForSel(HWND hwnd, const RECT* sel, const RECT* client){
    RECT s = *sel; NormalizeRect(&s);
    int w = RectW(&s), h = RectH(&s);

    wchar_t buf[64];
    swprintf(buf, 64, L"%dx%d", w, h);

    // Use existing backbuffer DC if available; otherwise a temp DC
    HDC hdc = g.hdcBack;
    BOOL temp = FALSE;
    if (!hdc) { hdc = GetDC(hwnd); temp = TRUE; }

    HFONT oldF = NULL;
    if (g.hFontSmall) oldF = (HFONT)SelectObject(hdc, g.hFontSmall);

    SIZE sz;
    GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);

    const int padX = 6, padY = 3;
    const int outside = HANDLE_SIZE + 6;
    const int insidePad = 6;

    int boxW = sz.cx + padX*2;
    int boxH = sz.cy + padY*2;

    RECT box = ComputeLabelBoxTopLeftWithFallbackDims(sel, client, boxW, boxH, outside, insidePad);

    if (oldF) SelectObject(hdc, oldF);
    if (temp) ReleaseDC(hwnd, hdc);
    return box;
}

static void InvalidateSelChange(HWND hwnd, RECT oldSel, RECT newSel) {
    NormalizeRect(&oldSel);
    NormalizeRect(&newSel);

    RECT client;
    client.left = 0; client.top = 0;
    client.right  = g.virt.right - g.virt.left;
    client.bottom = g.virt.bottom - g.virt.top;

    // Selection areas (inflated for handles/border)
    RECT a = InflateForUI(oldSel);
    RECT b = InflateForUI(newSel);

    // EXACT label areas using the same measurement & fallback logic as painting
    RECT la = ExactLabelRectForSel(hwnd, &oldSel, &client);
    RECT lb = ExactLabelRectForSel(hwnd, &newSel, &client);

    // Union them all
    RECT u;
    u.left   = MIN(MIN(a.left,  b.left),  MIN(la.left,  lb.left));
    u.top    = MIN(MIN(a.top,   b.top),   MIN(la.top,   lb.top));
    u.right  = MAX(MAX(a.right, b.right), MAX(la.right, lb.right));
    u.bottom = MAX(MAX(a.bottom,b.bottom),MAX(la.bottom,lb.bottom));

    InvalidateRect(hwnd, &u, FALSE);
}

// --- Interaction helpers ---
static BOOL PtInRectNorm(const RECT* r, POINT pt){
    RECT s = *r; NormalizeRect((RECT*)&s);
    return PtInRect(&s, pt);
}

static void BeginDrag(HWND hwnd, POINT p){
    RECT old = g.sel;      // remember old selection (if any) to clear it
    BOOL hadOld = g.haveSel;

    g.selecting = TRUE;
    g.resizing  = FALSE;
    g.moving    = FALSE;

    g.haveSel   = TRUE;
    g.dragStart = g.dragCur = p;
    g.lastMouse = p;
    g.sel = MakeRectFromPoints(p,p);

    if (hadOld) {
        InvalidateSelChange(hwnd, old, g.sel);
    } else {
        RECT r = InflateForUI(g.sel);
        InvalidateRect(hwnd, &r, FALSE);
    }
}
static void UpdateDrag(HWND hwnd, POINT p){
    g.lastMouse = p;
    RECT old = g.sel;
    g.dragCur = p;
    g.sel = MakeRectFromPoints(g.dragStart, g.dragCur);
    InvalidateSelChange(hwnd, old, g.sel);
}
static void EndDrag(HWND hwnd, POINT p){
    g.lastMouse = p;
    g.selecting = FALSE;
    UpdateDrag(hwnd, p);
}

// Robust resizing that flips handles after crossing and (1) outputs 0px at flip frame,
// (2) updates the resize anchor so the fixed side is the crossed edge.
static void DoResizeFromHandleRobust(HANDLE_ID* hIO, POINT p, RECT* anchor, RECT* outSel){
    HANDLE_ID h = *hIO;

    int left   = anchor->left;
    int right  = anchor->right;
    int top    = anchor->top;
    int bottom = anchor->bottom;

    // Horizontal component
    if (h == HT_L || h == HT_R || h == HT_TL || h == HT_TR || h == HT_BL || h == HT_BR){
        if (h == HT_R || h == HT_TR || h == HT_BR){
            // moving the right side relative to anchor->left
            if (p.x < anchor->left){
                right = anchor->left;
                left  = right;          // zero width this frame
                h = SwapH(h);
                anchor->right = right;  // crossed edge becomes fixed
            } else {
                right = (p.x < anchor->left + MIN_SEL_SIZE) ? (anchor->left + MIN_SEL_SIZE) : p.x;
            }
        } else if (h == HT_L || h == HT_TL || h == HT_BL){
            // moving the left side relative to anchor->right
            if (p.x > anchor->right){
                left  = anchor->right;
                right = left;           // zero width this frame
                h = SwapH(h);
                anchor->left = left;    // crossed edge becomes fixed
            } else {
                left = (p.x > anchor->right - MIN_SEL_SIZE) ? (anchor->right - MIN_SEL_SIZE) : p.x;
            }
        }
    }

    // Vertical component
    if (h == HT_T || h == HT_B || h == HT_TL || h == HT_TR || h == HT_BL || h == HT_BR){
        if (h == HT_B || h == HT_BL || h == HT_BR){
            if (p.y < anchor->top){
                bottom = anchor->top;
                top    = bottom;        // zero height this frame
                h = SwapV(h);
                anchor->bottom = bottom;
            } else {
                bottom = (p.y < anchor->top + MIN_SEL_SIZE) ? (anchor->top + MIN_SEL_SIZE) : p.y;
            }
        } else if (h == HT_T || h == HT_TL || h == HT_TR){
            if (p.y > anchor->bottom){
                top    = anchor->bottom;
                bottom = top;           // zero height this frame
                h = SwapV(h);
                anchor->top = top;
            } else {
                top = (p.y > anchor->bottom - MIN_SEL_SIZE) ? (anchor->bottom - MIN_SEL_SIZE) : p.y;
            }
        }
    }

    RECT r = { left, top, right, bottom };
    NormalizeRect(&r);
    *outSel = r;
    *hIO = h;
}

static void ResizeFromHandle(HWND hwnd, HANDLE_ID h, POINT p){
    g.lastMouse = p;
    RECT old = g.sel;

    RECT client;
    client.left = 0; client.top = 0;
    client.right  = g.virt.right - g.virt.left;
    client.bottom = g.virt.bottom - g.virt.top;

    DoResizeFromHandleRobust(&g.activeHandle, p, &g.resizeAnchor, &g.sel);
    ClampRectToClient(&g.sel, &client);

    InvalidateSelChange(hwnd, old, g.sel);
}

// --- Move helpers ---
static void MoveSelection(HWND hwnd, POINT p){
    g.lastMouse = p;
    RECT old = g.sel;

    int w = RectW(&old);
    int h = RectH(&old);

    int nl = p.x - g.moveOffset.x;
    int nt = p.y - g.moveOffset.y;

    RECT client = {0,0, g.virt.right - g.virt.left, g.virt.bottom - g.virt.top};

    if (nl < client.left) nl = client.left;
    if (nt < client.top ) nt = client.top;
    if (nl + w > client.right ) nl = client.right  - w;
    if (nt + h > client.bottom) nt = client.bottom - h;

    g.sel.left   = nl;
    g.sel.top    = nt;
    g.sel.right  = nl + w;
    g.sel.bottom = nt + h;

    InvalidateSelChange(hwnd, old, g.sel);
}

// --- Clipboard ---
static BOOL CopySelectionToClipboard(HWND hwnd){
    if (!g.haveSel) return FALSE;
    RECT s = g.sel; NormalizeRect(&s);
    int w = RectW(&s), h = RectH(&s);
    if (w<=0 || h<=0) return FALSE;

    HDC hdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0,0, w,h, g.hdcCapture, s.left, s.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(hwnd, hdc);

    if (!OpenClipboard(hwnd)){ DeleteObject(bmp); return FALSE; }
    EmptyClipboard();
    SetClipboardData(CF_BITMAP, bmp); // ownership passed to clipboard
    CloseClipboard();
    return TRUE;
}

// --- Window proc ---
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
    case WM_CREATE: {
        SetCursor(LoadCursor(NULL, IDC_CROSS));
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureBackBuffer(hwnd, rc.right, rc.bottom);
        g.lastMouse.x = 0; g.lastMouse.y = 0;
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        EnsureBackBuffer(hwnd, w, h);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        g.lastMouse = pt;

        LPCWSTR cur = IDC_CROSS;
        if (g.haveSel && !g.selecting && !g.resizing && !g.moving){
            HANDLE_ID hh = HitTestHandles(pt, &g.sel);
            if (hh != HT_NONE){
                cur = CursorForHandle(hh);
            } else if (PtInRectNorm(&g.sel, pt)) {
                cur = IDC_SIZEALL; // move selection cursor
            }
        }
        SetCursor(LoadCursor(NULL, cur));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g.lastMouse = p;
        SetCapture(hwnd);

        if (g.haveSel){
            HANDLE_ID h = HitTestHandles(p, &g.sel);
            if (h != HT_NONE){
                // Start resizing
                g.selecting = FALSE;
                g.moving    = FALSE;
                g.resizing  = TRUE;
                g.activeHandle = h;
                g.resizeAnchor = g.sel; // freeze anchor at start of resize
                return 0;
            }
            if (PtInRectNorm(&g.sel, p)){
                // Start moving
                g.selecting = FALSE;
                g.resizing  = FALSE;
                g.moving    = TRUE;
                g.moveOffset.x = p.x - g.sel.left;
                g.moveOffset.y = p.y - g.sel.top;
                return 0;
            }
        }
        // Outside: begin a new selection
        g.resizing = FALSE;
        g.moving   = FALSE;
        BeginDrag(hwnd, p);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g.lastMouse = p;
        if (g.selecting){
            UpdateDrag(hwnd, p);
        } else if (g.resizing){
            ResizeFromHandle(hwnd, g.activeHandle, p);
        } else if (g.moving){
            MoveSelection(hwnd, p);
        } else if (g.haveSel){
            g.hotHandle = HitTestHandles(p, &g.sel);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g.lastMouse = p;
        if (g.selecting) EndDrag(hwnd, p);
        g.resizing = FALSE;
        g.moving   = FALSE;
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONDOWN:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (wParam == VK_RETURN || ((GetKeyState(VK_CONTROL)&0x8000) && wParam=='C')){
            if (CopySelectionToClipboard(hwnd)){
                MessageBeep(MB_OK);
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureBackBuffer(hwnd, rc.right, rc.bottom);

        // Draw full scene into back buffer, then present only the invalidated region
        PaintOverlay(hwnd, g.hdcBack);
        BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
               ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top,
               g.hdcBack, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // no background erase (avoids flicker)
    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow){
    (void)hPrev; (void)lpCmdLine; (void)nShow;

    if (!CaptureVirtualScreen()){
        MessageBox(NULL, L"Failed to capture the virtual screen.", L"Error", MB_ICONERROR);
        return 1;
    }

    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.hCursor     = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"OverlayCaptureClass_DB11";
    // no background brush -> prevents erase flicker
    RegisterClass(&wc);

    int W = g.virt.right - g.virt.left;
    int H = g.virt.bottom - g.virt.top;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"CaptureOverlay",
        WS_POPUP,
        g.virt.left, g.virt.top, W, H,
        NULL, NULL, hInst, NULL);

    if (!hwnd){ Cleanup(); return 1; }

    SetWindowPos(hwnd, HWND_TOPMOST, g.virt.left, g.virt.top, W, H, SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
