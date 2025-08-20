#include <objbase.h>
#include <shellapi.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Shell32.lib")

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static int RectW(const RECT *r) { return r->right - r->left; }
static int RectH(const RECT *r) { return r->bottom - r->top; }
static void NormalizeRect_(RECT *r) {
  if (r->left > r->right) {
    LONG t = r->left;
    r->left = r->right;
    r->right = t;
  }
  if (r->top > r->bottom) {
    LONG t = r->top;
    r->top = r->bottom;
    r->bottom = t;
  }
}

typedef enum {
  HT_NONE = -1,
  HT_TL = 0,
  HT_T,
  HT_TR,
  HT_L,
  HT_R,
  HT_BL,
  HT_B,
  HT_BR
} HANDLE_ID;

typedef struct {
  RECT virt;
  HBITMAP hbmCapture;
  HDC hdcCapture;
  HBITMAP hbmBlack;
  HDC hdcBlack;
  HBITMAP hbmBack;
  HDC hdcBack;
  int backW, backH;
  HFONT hFontSmall;

  HPEN hPenHandle;    // handle outline pen (white)
  HPEN hPenDotted;    // selection dotted pen (white)
  HBRUSH hBrushBlack; // black (for label bg)

  BLENDFUNCTION blendFn;

  // selection state
  BOOL haveSel, selecting, resizing, moving;
  RECT sel, resizeAnchor;
  POINT dragStart, dragCur, lastMouse, moveOffset;
  HANDLE_ID activeHandle;

  // overlay window handle (for closing only the overlay)
  HWND hwnd;
} OVERLAY;

static OVERLAY og;

static const BYTE OVERLAY_ALPHA = 100;
static const int HANDLE_SIZE = 5;
static const int BORDER_WIDTH = 2;
static const int HANDLE_BORDER_WIDTH = 2;
static const int MIN_SEL_SIZE = 2;

static void Overlay_DeleteBackBuffer(void) {
  if (og.hdcBack) {
    DeleteDC(og.hdcBack);
    og.hdcBack = NULL;
  }
  if (og.hbmBack) {
    DeleteObject(og.hbmBack);
    og.hbmBack = NULL;
  }
  og.backW = og.backH = 0;
}
static BOOL Overlay_EnsureBackBuffer(HWND hwnd, int w, int h) {
  if (og.hdcBack && w == og.backW && h == og.backH)
    return TRUE;
  Overlay_DeleteBackBuffer();
  HDC hdc = GetDC(hwnd);
  og.hdcBack = CreateCompatibleDC(hdc);
  og.hbmBack = CreateCompatibleBitmap(hdc, w, h);
  ReleaseDC(hwnd, hdc);
  if (!og.hdcBack || !og.hbmBack)
    return FALSE;
  SelectObject(og.hdcBack, og.hbmBack);
  og.backW = w;
  og.backH = h;
  return TRUE;
}

static BOOL Overlay_CaptureVirtual(void) {
  og.virt.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  og.virt.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  og.virt.right = og.virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  og.virt.bottom = og.virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  int W = RectW(&og.virt), H = RectH(&og.virt);

  HDC s = GetDC(NULL);
  if (!s)
    return FALSE;
  HDC mem = CreateCompatibleDC(s);
  HBITMAP bm = CreateCompatibleBitmap(s, W, H);
  if (!mem || !bm) {
    if (mem)
      DeleteDC(mem);
    ReleaseDC(NULL, s);
    return FALSE;
  }
  HGDIOBJ old = SelectObject(mem, bm);
  BitBlt(mem, 0, 0, W, H, s, og.virt.left, og.virt.top, SRCCOPY | CAPTUREBLT);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(NULL, s);

  og.hdcCapture = CreateCompatibleDC(NULL);
  SelectObject(og.hdcCapture, bm);
  og.hbmCapture = bm;

  // 1x1 black DIB for AlphaBlend
  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = 1;
  bi.bmiHeader.biHeight = 1;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *pv = NULL;
  og.hbmBlack = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pv, NULL, 0);
  if (!og.hbmBlack)
    return FALSE;
  *(DWORD *)pv = 0xFF000000;
  og.hdcBlack = CreateCompatibleDC(NULL);
  if (!og.hdcBlack) {
    DeleteObject(og.hbmBlack);
    og.hbmBlack = NULL;
    return FALSE;
  }
  SelectObject(og.hdcBlack, og.hbmBlack);

  // font
  LOGFONTW lf = {0};
  lf.lfHeight = -14;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
  og.hFontSmall = CreateFontIndirectW(&lf);

  // pens/brush and blend
  og.hPenHandle = CreatePen(PS_SOLID, HANDLE_BORDER_WIDTH, RGB(255, 255, 255));
  LOGBRUSH lb = {BS_SOLID, RGB(255, 255, 255), 0};
  og.hPenDotted =
      ExtCreatePen(PS_GEOMETRIC | PS_DOT | PS_ENDCAP_FLAT | PS_JOIN_MITER,
                   BORDER_WIDTH, &lb, 0, NULL);
  og.hBrushBlack = CreateSolidBrush(RGB(0, 0, 0));

  og.blendFn.BlendOp = AC_SRC_OVER;
  og.blendFn.BlendFlags = 0;
  og.blendFn.SourceConstantAlpha = OVERLAY_ALPHA;
  og.blendFn.AlphaFormat = 0;

  return (og.hPenHandle && og.hPenDotted && og.hBrushBlack);
}

static void Overlay_CleanupGDI(void) {
  if (og.hdcCapture) {
    DeleteDC(og.hdcCapture);
    og.hdcCapture = NULL;
  }
  if (og.hbmCapture) {
    DeleteObject(og.hbmCapture);
    og.hbmCapture = NULL;
  }
  if (og.hdcBlack) {
    DeleteDC(og.hdcBlack);
    og.hdcBlack = NULL;
  }
  if (og.hbmBlack) {
    DeleteObject(og.hbmBlack);
    og.hbmBlack = NULL;
  }
  if (og.hFontSmall) {
    DeleteObject(og.hFontSmall);
    og.hFontSmall = NULL;
  }
  if (og.hPenHandle) {
    DeleteObject(og.hPenHandle);
    og.hPenHandle = NULL;
  }
  if (og.hPenDotted) {
    DeleteObject(og.hPenDotted);
    og.hPenDotted = NULL;
  }
  if (og.hBrushBlack) {
    DeleteObject(og.hBrushBlack);
    og.hBrushBlack = NULL;
  }
  Overlay_DeleteBackBuffer();
}

// --- Handle helpers & swapping ---
static void GetHandleCenters(const RECT *r, POINT p[8]) {
  LONG cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
  p[HT_TL] = (POINT){r->left, r->top};
  p[HT_T] = (POINT){cx, r->top};
  p[HT_TR] = (POINT){r->right, r->top};
  p[HT_L] = (POINT){r->left, cy};
  p[HT_R] = (POINT){r->right, cy};
  p[HT_BL] = (POINT){r->left, r->bottom};
  p[HT_B] = (POINT){cx, r->bottom};
  p[HT_BR] = (POINT){r->right, r->bottom};
}
static RECT HR(POINT c) {
  int hs = HANDLE_SIZE;
  RECT rr = {c.x - hs, c.y - hs, c.x + hs, c.y + hs};
  return rr;
}
static HANDLE_ID HitTest(const RECT *r, POINT pt) {
  if (RectW(r) < 1 || RectH(r) < 1)
    return HT_NONE;
  POINT c[8];
  GetHandleCenters(r, c);
  for (int i = 0; i < 8; i++) {
    RECT h = HR(c[i]);
    if (PtInRect(&h, pt))
      return (HANDLE_ID)i;
  }
  const int EDGE = HANDLE_SIZE + 2;
  RECT top = {r->left + EDGE, r->top - EDGE, r->right - EDGE, r->top + EDGE};
  RECT bot = {r->left + EDGE, r->bottom - EDGE, r->right - EDGE,
              r->bottom + EDGE};
  RECT left = {r->left - EDGE, r->top + EDGE, r->left + EDGE, r->bottom - EDGE};
  RECT right = {r->right - EDGE, r->top + EDGE, r->right + EDGE,
                r->bottom - EDGE};
  if (PtInRect(&top, pt))
    return HT_T;
  if (PtInRect(&bot, pt))
    return HT_B;
  if (PtInRect(&left, pt))
    return HT_L;
  if (PtInRect(&right, pt))
    return HT_R;
  return HT_NONE;
}
static LPCSTR CursorForHandle(HANDLE_ID h) {
  switch (h) {
  case HT_T:
  case HT_B:
    return IDC_SIZENS;
  case HT_L:
  case HT_R:
    return IDC_SIZEWE;
  case HT_TL:
  case HT_BR:
    return IDC_SIZENWSE;
  case HT_TR:
  case HT_BL:
    return IDC_SIZENESW;
  default:
    return IDC_CROSS;
  }
}
static HANDLE_ID SwapH(HANDLE_ID h) {
  switch (h) {
  case HT_L:
    return HT_R;
  case HT_R:
    return HT_L;
  case HT_TL:
    return HT_TR;
  case HT_TR:
    return HT_TL;
  case HT_BL:
    return HT_BR;
  case HT_BR:
    return HT_BL;
  default:
    return h;
  }
}
static HANDLE_ID SwapV(HANDLE_ID h) {
  switch (h) {
  case HT_T:
    return HT_B;
  case HT_B:
    return HT_T;
  case HT_TL:
    return HT_BL;
  case HT_BL:
    return HT_TL;
  case HT_TR:
    return HT_BR;
  case HT_BR:
    return HT_TR;
  default:
    return h;
  }
}

// --- Drawing helpers ---
static void DrawHandles(HDC hdc, const RECT *r) {
  POINT c[8];
  GetHandleCenters(r, c);
  HGDIOBJ oldPen = SelectObject(hdc, og.hPenHandle);
  HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
  for (int i = 0; i < 8; i++) {
    RECT hr = HR(c[i]);
    Rectangle(hdc, hr.left, hr.top, hr.right, hr.bottom);
  }
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
}

static RECT LabelBox(const RECT *sel, const RECT *client, HDC hdc) {
  RECT s = *sel;
  NormalizeRect_(&s);
  int w = RectW(&s), h = RectH(&s);
  wchar_t buf[64];
  swprintf(buf, 64, L"%dx%d", w, h);

  HFONT oldF = NULL;
  if (og.hFontSmall)
    oldF = (HFONT)SelectObject(hdc, og.hFontSmall);
  SIZE sz;
  GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);
  const int padX = 6, padY = 3, outside = HANDLE_SIZE + 6, inside = 6;
  int boxW = sz.cx + padX * 2, boxH = sz.cy + padY * 2;

  RECT box;
  box.right = s.left - outside;
  box.left = box.right - boxW;
  box.top = s.top;
  box.bottom = box.top + boxH;
  if (box.left >= client->left) {
    if (oldF)
      SelectObject(hdc, oldF);
    return box;
  }

  box.left = s.left;
  box.right = box.left + boxW;
  box.bottom = s.top - outside;
  box.top = box.bottom - boxH;
  if (box.top >= client->top) {
    if (oldF)
      SelectObject(hdc, oldF);
    return box;
  }

  box.left = s.left + inside;
  box.top = s.top + inside;
  box.right = box.left + boxW;
  box.bottom = box.top + boxH;
  if (box.left < client->left) {
    int dx = client->left - box.left;
    box.left += dx;
    box.right += dx;
  }
  if (box.top < client->top) {
    int dy = client->top - box.top;
    box.top += dy;
    box.bottom += dy;
  }
  if (box.right > client->right) {
    int dx = box.right - client->right;
    box.left -= dx;
    box.right -= dx;
  }
  if (box.bottom > client->bottom) {
    int dy = box.bottom - client->bottom;
    box.top -= dy;
    box.bottom -= dy;
  }
  if (oldF)
    SelectObject(hdc, oldF);
  return box;
}

static RECT InflateForUI(RECT r) {
  InflateRect(&r, 40, 40);
  return r;
}

static RECT ExactLabelRect(HWND hwnd, const RECT *sel, const RECT *client) {
  HDC hdc = og.hdcBack;
  BOOL temp = FALSE;
  if (!hdc) {
    hdc = GetDC(hwnd);
    temp = TRUE;
  }
  RECT box = LabelBox(sel, client, hdc);
  if (temp)
    ReleaseDC(hwnd, hdc);
  return box;
}

static void InvalidateSelChange(HWND hwnd, RECT oldSel, RECT newSel) {
  NormalizeRect_(&oldSel);
  NormalizeRect_(&newSel);
  RECT client = {0, 0, og.virt.right - og.virt.left,
                 og.virt.bottom - og.virt.top};
  RECT a = InflateForUI(oldSel), b = InflateForUI(newSel);
  RECT la = ExactLabelRect(hwnd, &oldSel, &client),
       lb = ExactLabelRect(hwnd, &newSel, &client);
  RECT u;
  u.left = MIN(MIN(a.left, b.left), MIN(la.left, lb.left));
  u.top = MIN(MIN(a.top, b.top), MIN(la.top, lb.top));
  u.right = MAX(MAX(a.right, b.right), MAX(la.right, lb.right));
  u.bottom = MAX(MAX(a.bottom, b.bottom), MAX(la.bottom, lb.bottom));
  InvalidateRect(hwnd, &u, FALSE);
}

static void DrawDimsLabel(HDC hdc, const RECT *sel, const RECT *client) {
  RECT s = *sel;
  NormalizeRect_(&s);
  int w = RectW(&s), h = RectH(&s);
  wchar_t buf[64];
  swprintf(buf, 64, L"%dx%d", w, h);
  HFONT oldF = NULL;
  if (og.hFontSmall)
    oldF = (HFONT)SelectObject(hdc, og.hFontSmall);

  SIZE sz;
  GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &sz);
  const int padX = 6, padY = 3;
  RECT box = LabelBox(sel, client, hdc);

  FillRect(hdc, &box, og.hBrushBlack);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(240, 240, 240));
  RECT txt = {box.left + padX, box.top + padY, box.right - padX,
              box.bottom - padY};
  DrawTextW(hdc, buf, -1, &txt,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  if (oldF)
    SelectObject(hdc, oldF);
}

static void PaintOverlay(HWND hwnd, HDC hdc) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  int W = rc.right, H = rc.bottom;
  BitBlt(hdc, 0, 0, W, H, og.hdcCapture, 0, 0, SRCCOPY);
  AlphaBlend(hdc, 0, 0, W, H, og.hdcBlack, 0, 0, 1, 1, og.blendFn);

  if (og.haveSel) {
    RECT s = og.sel;
    NormalizeRect_(&s);
    BitBlt(hdc, s.left, s.top, RectW(&s), RectH(&s), og.hdcCapture, s.left,
           s.top, SRCCOPY);
    HGDIOBJ oldPen = SelectObject(hdc, og.hPenDotted);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, s.left, s.top, s.right, s.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DrawHandles(hdc, &s);
    DrawDimsLabel(hdc, &s, &rc);
  }
}

static BOOL CopySelectionToClipboard(HWND hwnd) {
  if (!og.haveSel)
    return FALSE;
  RECT s = og.sel;
  NormalizeRect_(&s);
  int w = RectW(&s), h = RectH(&s);
  if (w <= 0 || h <= 0)
    return FALSE;
  HDC hdc = GetDC(hwnd);
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, og.hdcCapture, s.left, s.top, SRCCOPY);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(hwnd, hdc);
  if (!OpenClipboard(hwnd)) {
    DeleteObject(bmp);
    return FALSE;
  }
  EmptyClipboard();
  SetClipboardData(CF_BITMAP, bmp);
  CloseClipboard();
  return TRUE;
}

// robust resize (same as before)
static void ResizeRobust(HANDLE_ID *hIO, POINT p, RECT *anchor, RECT *outSel) {
  HANDLE_ID h = *hIO;
  int L = anchor->left, R = anchor->right, T = anchor->top, B = anchor->bottom;
  if (h == HT_R || h == HT_TR || h == HT_BR) {
    if (p.x < anchor->left) {
      R = anchor->left;
      L = R;
      h = SwapH(h);
      anchor->right = R;
    } else
      R = (p.x < anchor->left + MIN_SEL_SIZE) ? (anchor->left + MIN_SEL_SIZE)
                                              : p.x;
  } else if (h == HT_L || h == HT_TL || h == HT_BL) {
    if (p.x > anchor->right) {
      L = anchor->right;
      R = L;
      h = SwapH(h);
      anchor->left = L;
    } else
      L = (p.x > anchor->right - MIN_SEL_SIZE) ? (anchor->right - MIN_SEL_SIZE)
                                               : p.x;
  }
  if (h == HT_B || h == HT_BL || h == HT_BR) {
    if (p.y < anchor->top) {
      B = anchor->top;
      T = B;
      h = SwapV(h);
      anchor->bottom = B;
    } else
      B = (p.y < anchor->top + MIN_SEL_SIZE) ? (anchor->top + MIN_SEL_SIZE)
                                             : p.y;
  } else if (h == HT_T || h == HT_TL || h == HT_TR) {
    if (p.y > anchor->bottom) {
      T = anchor->bottom;
      B = T;
      h = SwapV(h);
      anchor->top = T;
    } else
      T = (p.y > anchor->bottom - MIN_SEL_SIZE)
              ? (anchor->bottom - MIN_SEL_SIZE)
              : p.y;
  }
  RECT r = {L, T, R, B};
  NormalizeRect_(&r);
  *outSel = r;
  *hIO = h;
}

static LRESULT CALLBACK Overlay_WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    og.hwnd = hwnd;
    if (!Overlay_CaptureVirtual()) {
      DestroyWindow(hwnd);
      return 0;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    Overlay_EnsureBackBuffer(hwnd, rc.right, rc.bottom);
    SetCursor(LoadCursor(NULL, IDC_CROSS));
    og.haveSel = og.selecting = og.resizing = og.moving = FALSE;
    return 0;
  }
  case WM_SIZE: {
    int w = LOWORD(lParam), h = HIWORD(lParam);
    Overlay_EnsureBackBuffer(hwnd, w, h);
    return 0;
  }
  case WM_SETCURSOR: {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    og.lastMouse = pt;
    LPCSTR cur = IDC_CROSS;
    if (og.haveSel && !og.selecting && !og.resizing && !og.moving) {
      HANDLE_ID hh = HitTest(&og.sel, pt);
      if (hh != HT_NONE)
        cur = CursorForHandle(hh);
      else if (PtInRect(&(RECT){MIN(og.sel.left, og.sel.right),
                                MIN(og.sel.top, og.sel.bottom),
                                MAX(og.sel.left, og.sel.right),
                                MAX(og.sel.top, og.sel.bottom)},
                        pt))
        cur = IDC_SIZEALL;
    }
    SetCursor(LoadCursor(NULL, cur));
    return TRUE;
  }
  case WM_LBUTTONDOWN: {
    POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    og.lastMouse = p;
    SetCapture(hwnd);
    if (og.haveSel) {
      HANDLE_ID h = HitTest(&og.sel, p);
      if (h != HT_NONE) {
        og.resizing = TRUE;
        og.selecting = og.moving = FALSE;
        og.activeHandle = h;
        og.resizeAnchor = og.sel;
        return 0;
      }
      RECT s = og.sel;
      NormalizeRect_(&s);
      if (PtInRect(&s, p)) {
        og.moving = TRUE;
        og.selecting = og.resizing = FALSE;
        og.moveOffset.x = p.x - s.left;
        og.moveOffset.y = p.y - s.top;
        return 0;
      }
    }
    // new selection
    og.selecting = TRUE;
    og.resizing = og.moving = FALSE;
    og.haveSel = TRUE;
    og.dragStart = og.dragCur = p;
    og.sel = (RECT){p.x, p.y, p.x, p.y};
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    og.lastMouse = p;
    if (og.selecting) {
      RECT old = og.sel;
      og.dragCur = p;
      og.sel = (RECT){og.dragStart.x, og.dragStart.y, p.x, p.y};
      InvalidateSelChange(hwnd, old, og.sel);
    } else if (og.resizing) {
      RECT old = og.sel;
      ResizeRobust(&og.activeHandle, p, &og.resizeAnchor, &og.sel);
      // clamp to client
      RECT client = {0, 0, og.virt.right - og.virt.left,
                     og.virt.bottom - og.virt.top};
      if (og.sel.left < client.left)
        og.sel.left = client.left;
      if (og.sel.top < client.top)
        og.sel.top = client.top;
      if (og.sel.right > client.right)
        og.sel.right = client.right;
      if (og.sel.bottom > client.bottom)
        og.sel.bottom = client.bottom;
      InvalidateSelChange(hwnd, old, og.sel);
    } else if (og.moving) {
      RECT old = og.sel;
      RECT s = old;
      NormalizeRect_(&s);
      int w = RectW(&s), h = RectH(&s);
      int nl = p.x - og.moveOffset.x, nt = p.y - og.moveOffset.y;
      RECT client = {0, 0, og.virt.right - og.virt.left,
                     og.virt.bottom - og.virt.top};
      if (nl < client.left)
        nl = client.left;
      if (nt < client.top)
        nt = client.top;
      if (nl + w > client.right)
        nl = client.right - w;
      if (nt + h > client.bottom)
        nt = client.bottom - h;
      og.sel = (RECT){nl, nt, nl + w, nt + h};
      InvalidateSelChange(hwnd, old, og.sel);
    }
    return 0;
  }
  case WM_LBUTTONUP: {
    POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    og.lastMouse = p;
    og.selecting = og.resizing = og.moving = FALSE;
    ReleaseCapture();
    return 0;
  }
  case WM_KEYDOWN: {
    if (wParam == VK_ESCAPE || wParam == VK_RBUTTON) {
      DestroyWindow(hwnd);
    } else if (wParam == VK_RETURN ||
               ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C')) {
      if (CopySelectionToClipboard(hwnd)) {
        /* silent */
      }
      DestroyWindow(hwnd); // close overlay only, app keeps running
    }
    return 0;
  }
  case WM_RBUTTONDOWN: {
    DestroyWindow(hwnd);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC h = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    Overlay_EnsureBackBuffer(hwnd, rc.right, rc.bottom);
    PaintOverlay(hwnd, og.hdcBack);
    BitBlt(h, ps.rcPaint.left, ps.rcPaint.top,
           ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, og.hdcBack, ps.rcPaint.left,
           ps.rcPaint.top, SRCCOPY);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_DESTROY: {
    Overlay_CleanupGDI();
    og.hwnd = NULL;
    return 0;
  }
  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

// Create and show the overlay over the virtual desktop
static void LaunchOverlay(HINSTANCE hInst) {
  // compute virtual desktop + size
  RECT virt;
  virt.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  virt.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  int W = RectW(&virt), H = RectH(&virt);

  static const wchar_t *kOverlayClass = L"OverlayCaptureClass_Tray";
  static BOOL classRegd = FALSE;
  if (!classRegd) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = Overlay_WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = kOverlayClass;
    RegisterClassW(&wc);
    classRegd = TRUE;
  }

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass,
                              L"CaptureOverlay", WS_POPUP, virt.left, virt.top,
                              W, H, NULL, NULL, hInst, NULL);
  if (!hwnd)
    return;

  SetWindowPos(hwnd, HWND_TOPMOST, virt.left, virt.top, W, H, SWP_SHOWWINDOW);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
}

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_UID 1001
#define IDM_TRAY_CAPTURE 2001
#define IDM_TRAY_EXIT 2002
#define HOTKEY_ID_PRINT 3001

static NOTIFYICONDATAW g_nid;
static HWND g_hwndCtl = NULL;

static void Tray_Add(HWND hwnd) {
  ZeroMemory(&g_nid, sizeof(g_nid));
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = hwnd;
  g_nid.uID = TRAY_UID;
  g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  g_nid.uCallbackMessage = WM_TRAYICON;
  g_nid.hIcon = (HICON)LoadIcon(NULL, IDI_APPLICATION);
  lstrcpynW(g_nid.szTip, L"Screenshot (PrintScreen to capture)",
            ARRAYSIZE(g_nid.szTip));
  Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void Tray_Delete(void) {
  if (g_nid.hWnd)
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
  g_nid.hWnd = NULL;
}

static void Tray_ShowMenu(HWND hwnd) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, IDM_TRAY_CAPTURE, L"Take Screenshot\tPrtSc");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd,
                 NULL);
  DestroyMenu(m);
}

static LRESULT CALLBACK Ctl_WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    g_hwndCtl = hwnd;
    Tray_Add(hwnd);
    // Register PrintScreen as a global hotkey (no modifiers)
    RegisterHotKey(hwnd, HOTKEY_ID_PRINT, 0, VK_SNAPSHOT);
    return 0;
  case WM_HOTKEY:
    if ((int)wParam == HOTKEY_ID_PRINT) {
      LaunchOverlay((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
    }
    return 0;
  case WM_TRAYICON:
    switch (LOWORD(lParam)) {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
      LaunchOverlay((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
      return 0;
    case WM_RBUTTONUP:
      Tray_ShowMenu(hwnd);
      return 0;
    }
    return 0;
  case WM_COMMAND: {
    switch (LOWORD(wParam)) {
    case IDM_TRAY_CAPTURE:
      LaunchOverlay((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
      return 0;
    case IDM_TRAY_EXIT:
      DestroyWindow(hwnd);
      return 0;
    }
    return 0;
  }
  case WM_DESTROY:
    UnregisterHotKey(hwnd, HOTKEY_ID_PRINT);
    Tray_Delete();
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd,
                      int nShow) {
  (void)hPrev;
  (void)lpCmd;
  (void)nShow;

  // Controller window (hidden) for tray + hotkey
  const wchar_t kCtlClass[] = L"ScreenshotCtlClass";
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = Ctl_WndProc;
  wc.hInstance = hInst;
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wc.lpszClassName = kCtlClass;
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0, kCtlClass, L"Screenshot Controller",
                              WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                              NULL, NULL, hInst, NULL);
  if (!hwnd)
    return 1;

  // Hide controller window; tray carries UI
  ShowWindow(hwnd, SW_HIDE);

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return (int)msg.wParam;
}
