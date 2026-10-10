#include "mongoose.h"
#include "qrcodegen.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define DEFAULT_PORT 18900

/* 布局参数 */
#define PAD        16
#define LINE_H     22
#define QR_SIZE    260
#define CLIENT_W   320
#define CLIENT_H   350

#define IDT_COPY_TIP 1

static char s_listen_on[64] = "http://0.0.0.0:18900";
static char s_url[256] = "http://127.0.0.1:18900";
static struct mg_mgr s_mgr;
static int g_running = 1;
static int s_port = DEFAULT_PORT;

static HWND  g_hwnd = NULL;
static HFONT g_font_url  = NULL;
static HFONT g_font_hint = NULL;

static RECT g_rc_qr  = {0, 0, 0, 0};
static RECT g_rc_url = {0, 0, 0, 0};
static int  g_url_hot = 0;

/* ---- 自绘提示窗口 ---- */
static HWND    g_tip_hwnd = NULL;
static HFONT   g_font_tip = NULL;
static wchar_t g_tip_text[256] = L"";
static int     g_qr_hovered = 0;
static int     g_copy_tip_shown = 0;

/* ---------- UTF-8 -> UTF-16 ---------- */
static void u2w(const char *src, wchar_t *dst, int dst_len) {
  MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_len);
}

/* ---------- 内嵌 HTML ---------- */
static const char *s_html =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
    "<title>实时文本同步</title>"
    "<style>"
    "html,body{margin:0;padding:0;height:100%}"
    "body{padding:8px;padding-bottom:calc(8px + env(safe-area-inset-bottom,0px));"
    "font-family:system-ui,-apple-system,\"Segoe UI\",sans-serif;background:#f5f5f5;"
    "height:100vh;height:100dvh;box-sizing:border-box;display:flex;flex-direction:column}"
    "#status{font-size:12px;color:#666;margin-bottom:6px;height:16px;flex:0 0 auto}"
    "#status.ok{color:#2a9d2a}#status.err{color:#c33}"
    "textarea{flex:1 1 auto;width:100%;min-height:0;padding:12px;font-size:16px;"
    "border:1px solid #ccc;border-radius:8px;box-sizing:border-box;resize:none;"
    "font-family:inherit;outline:none;background:#fff}"
    "textarea:focus{border-color:#4a90e2;box-shadow:0 0 0 2px rgba(74,144,226,.15)}"
    "</style></head><body>"
    "<div id=\"status\">连接中...</div>"
    "<textarea id=\"text\" placeholder=\"在这里输入，另一端实时同步...\"></textarea>"
    "<script>"
    "(function(){"
    "var ta=document.getElementById('text');"
    "var statusEl=document.getElementById('status');"
    "var ws=new WebSocket('ws://'+location.host+'/ws');"
    "var ignore=false;"
    "function setStatus(t,c){statusEl.textContent=t;statusEl.className=c||'';}"
    "ws.onopen=function(){setStatus('已连接','ok');};"
    "ws.onclose=function(){setStatus('连接断开，重连中...','err');setTimeout(function(){location.reload();},2000);};"
    "ws.onerror=function(){setStatus('连接错误','err');};"
    "ws.onmessage=function(e){"
    "if(e.data===ta.value)return;"
    "ignore=true;ta.value=e.data;ignore=false;"
    "};"
    "ta.addEventListener('input',function(){"
    "if(ignore)return;"
    "if(ws.readyState===WebSocket.OPEN){ws.send(ta.value);}"
    "});"
    "})();"
    "</script></body></html>";

/* ---------- Mongoose 回调 ---------- */
static void broadcast(struct mg_mgr *mgr, const char *msg, size_t len) {
  struct mg_connection *c;
  for (c = mgr->conns; c != NULL; c = c->next) {
    if (c->is_websocket) {
      mg_ws_send(c, msg, len, WEBSOCKET_OP_TEXT);
    }
  }
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
      mg_ws_upgrade(c, hm, NULL);
    } else if (mg_match(hm->uri, mg_str("/"), NULL)) {
      mg_http_reply(c, 200, "Content-Type: text/html\r\n", "%s", s_html);
    } else {
      mg_http_reply(c, 404, "", "Not Found");
    }
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    broadcast(c->mgr, wm->data.buf, wm->data.len);
  }
}

/* ---------- 端口是否可用（独占方式探测） ---------- */
static int is_port_available(int port) {
  if (port <= 0 || port > 65535) return 0;

  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return 0;

  /* Windows 上必须用 SO_EXCLUSIVEADDRUSE：
     若别的进程用 SO_REUSEADDR 占了该端口，普通 bind 仍会“成功”，
     导致误判为可用。 */
  BOOL excl = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             (const char *) &excl, sizeof(excl));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons((u_short) port);

  int ok = (bind(s, (struct sockaddr *) &addr, sizeof(addr)) == 0);
  closesocket(s);
  return ok;
}

/* ---------- 让系统分配一个空闲端口 ---------- */
static int pick_random_port(void) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return 0;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = 0;   /* 0 = 由系统挑一个临时端口 */

  int port = 0;
  if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
    int alen = sizeof(addr);
    if (getsockname(s, (struct sockaddr *) &addr, &alen) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  closesocket(s);
  return port;
}

/* ---------- 尝试在指定端口启动监听，成功返回 1 ---------- */
static int start_listen(int port) {
  if (!is_port_available(port)) return 0;

  snprintf(s_listen_on, sizeof(s_listen_on), "http://0.0.0.0:%d", port);
  if (mg_http_listen(&s_mgr, s_listen_on, fn, NULL) == NULL) return 0;

  s_port = port;
  return 1;
}

/* ---------- 兜底方案：枚举网卡 ---------- */
static int get_ip_by_adapters(char *out, size_t out_len) {
  ULONG outBufLen = 15000;
  PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(outBufLen);
  if (pAddresses == NULL) return 0;

  int found = 0;
  if (GetAdaptersAddresses(
          AF_INET,
          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX,
          NULL, pAddresses, &outBufLen) == NO_ERROR) {
    PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
    while (pCurr && !found) {
      if (pCurr->OperStatus == IfOperStatusUp &&
          pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
        PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress;
        while (pUnicast) {
          if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
            struct sockaddr_in *sa =
                (struct sockaddr_in *) pUnicast->Address.lpSockaddr;
            const char *ip = inet_ntoa(sa->sin_addr);
            if (strcmp(ip, "127.0.0.1") != 0 && strcmp(ip, "0.0.0.0") != 0) {
              strncpy(out, ip, out_len - 1);
              out[out_len - 1] = '\0';
              found = 1;
              break;
            }
          }
          pUnicast = pUnicast->Next;
        }
      }
      pCurr = pCurr->Next;
    }
  }
  free(pAddresses);
  return found;
}

/* ---------- 获取本机局域网 IPv4（两段式） ---------- */
static void get_local_ip(char *buf, size_t len, int port) {
  /* 默认回环 */
  snprintf(buf, len, "http://127.0.0.1:%d", port);

  /* 第一段：UDP socket trick，只查路由表，不发包，通常 <1ms */
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s != INVALID_SOCKET) {
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family      = AF_INET;
    target.sin_port        = htons(53);
    target.sin_addr.s_addr = inet_addr("8.8.8.8");

    if (connect(s, (struct sockaddr *) &target, sizeof(target)) == 0) {
      struct sockaddr_in local;
      int llen = sizeof(local);
      if (getsockname(s, (struct sockaddr *) &local, &llen) == 0) {
        if (local.sin_addr.s_addr != 0 &&
            local.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
          char ip[INET_ADDRSTRLEN];
          if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip))) {
            snprintf(buf, len, "http://%s:%d", ip, port);
            closesocket(s);
            return;
          }
        }
      }
    }
    closesocket(s);
  }

  /* 第二段：兜底，枚举网卡 */
  {
    char ip[INET_ADDRSTRLEN];
    if (get_ip_by_adapters(ip, sizeof(ip))) {
      snprintf(buf, len, "http://%s:%d", ip, port);
    }
  }
}

/* ---------- 复制 UTF-8 文本到剪贴板 ---------- */
static void copy_text_to_clipboard(HWND hwnd, const char *utf8) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  if (wlen <= 0) return;

  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
  if (hMem == NULL) return;

  wchar_t *p = (wchar_t *) GlobalLock(hMem);
  if (p == NULL) {
    GlobalFree(hMem);
    return;
  }
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, p, wlen);
  GlobalUnlock(hMem);

  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, hMem) == NULL) {
      GlobalFree(hMem);
    }
    CloseClipboard();
  } else {
    GlobalFree(hMem);
  }
}

/* ---------- 字体 ---------- */
static HFONT create_font(int height, int weight, BOOL underline) {
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, underline, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Microsoft YaHei UI");
}

/* ---------- 自绘提示窗口 ---------- */
static LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);

      /* 经典淡黄底 (255,255,225) */
      HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 225));
      FillRect(hdc, &rc, hBrush);
      DeleteObject(hBrush);

      /* 深灰细边框 */
      HPEN hPen = CreatePen(PS_SOLID, 1, RGB(118, 118, 118));
      HPEN oldPen = (HPEN) SelectObject(hdc, hPen);
      HBRUSH oldBrush = (HBRUSH) SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, 0, 0, rc.right, rc.bottom);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBrush);
      DeleteObject(hPen);

      /* 黑字 */
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(0, 0, 0));
      HFONT oldFont = (HFONT) SelectObject(hdc, g_font_tip);
      RECT trc = rc;
      trc.left += 8;
      trc.right -= 8;
      DrawTextW(hdc, g_tip_text, -1, &trc,
                DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_SINGLELINE);
      SelectObject(hdc, oldFont);

      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void create_tip_window(HINSTANCE hInst) {
  WNDCLASSEXW wc = {0};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = TipWndProc;
  wc.hInstance     = hInst;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = L"TextSyncTip";
  RegisterClassExW(&wc);

  g_font_tip = create_font(13, FW_NORMAL, FALSE);

  g_tip_hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
      L"TextSyncTip", L"",
      WS_POPUP,
      0, 0, 10, 10,
      NULL, NULL, hInst, NULL);
}

/* 在屏幕坐标 (sx, sy) 处显示提示，文本为 text */
static void show_tip_at(const wchar_t *text, int sx, int sy) {
  if (g_tip_hwnd == NULL) return;

  wcsncpy(g_tip_text, text, 255);
  g_tip_text[255] = L'\0';

  /* 根据文字计算窗口大小 */
  HDC hdc = GetDC(g_tip_hwnd);
  HFONT oldFont = (HFONT) SelectObject(hdc, g_font_tip);
  RECT rc = {0, 0, 500, 0};
  DrawTextW(hdc, g_tip_text, -1, &rc,
            DT_CALCRECT | DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
  SelectObject(hdc, oldFont);
  ReleaseDC(g_tip_hwnd, hdc);

  int w = rc.right - rc.left + 16;
  int h = rc.bottom - rc.top + 8;
  if (w < 60) w = 60;
  if (h < 26) h = 26;

  /* 避免超出屏幕右/下边缘 */
  int sw = GetSystemMetrics(SM_CXSCREEN);
  int sh = GetSystemMetrics(SM_CYSCREEN);
  if (sx + w > sw) sx = sw - w - 2;
  if (sy + h > sh) sy = sy - h - 24;

  SetWindowPos(g_tip_hwnd, HWND_TOPMOST, sx, sy, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(g_tip_hwnd, NULL, TRUE);
  UpdateWindow(g_tip_hwnd);
}

static void hide_tip_window(void) {
  if (g_tip_hwnd != NULL) ShowWindow(g_tip_hwnd, SW_HIDE);
}

/* ---------- 计算布局 ---------- */
static void compute_layout(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  int W = rc.right;
  if (W <= 0) W = CLIENT_W;

  int qs = QR_SIZE;
  if (qs > W - 2 * PAD) qs = W - 2 * PAD;
  if (qs < 40) qs = 40;

  int top = PAD;
  g_rc_qr.left   = (W - qs) / 2;
  g_rc_qr.top    = top;
  g_rc_qr.right  = g_rc_qr.left + qs;
  g_rc_qr.bottom = top + qs;

  wchar_t wurl[256];
  u2w(s_url, wurl, 256);

  int cx = 0, cy = 24;
  if (g_font_url != NULL) {
    HDC hdc = GetDC(hwnd);
    HFONT old = (HFONT) SelectObject(hdc, g_font_url);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(hdc, wurl, (int) wcslen(wurl), &sz);
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    cx = sz.cx;
    if (sz.cy > 0) cy = sz.cy;
  }

  int uy = g_rc_qr.bottom + 12;
  g_rc_url.left   = (W - cx) / 2;
  g_rc_url.top    = uy;
  g_rc_url.right  = g_rc_url.left + cx;
  g_rc_url.bottom = uy + cy;
}

/* ---------- 主窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      g_font_url  = create_font(17, FW_NORMAL, TRUE);
      g_font_hint = create_font(13, FW_NORMAL, FALSE);
      create_tip_window(((LPCREATESTRUCTW) lParam)->hInstance);
      compute_layout(hwnd);
      return 0;

    case WM_SIZE:
      compute_layout(hwnd);
      return 0;

    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect((HDC) wParam, &rc, (HBRUSH) GetStockObject(WHITE_BRUSH));
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, (HBRUSH) GetStockObject(WHITE_BRUSH));
      SetBkMode(hdc, TRANSPARENT);

      RECT r;

      /* ---- 二维码 ---- */
      {
        uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
        uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
        BOOL ok = qrcodegen_encodeText(s_url, tempBuffer, qrcode,
                                       qrcodegen_Ecc_MEDIUM,
                                       qrcodegen_VERSION_MIN,
                                       qrcodegen_VERSION_MAX,
                                       qrcodegen_Mask_AUTO, true);
        if (ok) {
          int size = qrcodegen_getSize(qrcode);
          int quiet = 4;
          int total = size + quiet * 2;
          int w = g_rc_qr.right - g_rc_qr.left;
          int h = g_rc_qr.bottom - g_rc_qr.top;
          int side = (w < h ? w : h);
          int module_px = side / total;
          if (module_px < 1) module_px = 1;
          int qr_px = module_px * total;
          int ox = g_rc_qr.left + (w - qr_px) / 2 + quiet * module_px;
          int oy = g_rc_qr.top  + (h - qr_px) / 2 + quiet * module_px;

          HBRUSH hBlack = (HBRUSH) GetStockObject(BLACK_BRUSH);
          for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
              if (qrcodegen_getModule(qrcode, x, y)) {
                RECT q;
                q.left   = ox + x * module_px;
                q.top    = oy + y * module_px;
                q.right  = ox + (x + 1) * module_px;
                q.bottom = oy + (y + 1) * module_px;
                FillRect(hdc, &q, hBlack);
              }
            }
          }
        }
      }

      /* ---- URL（可点击，居中，带下划线） ---- */
      {
        wchar_t wurl[256];
        u2w(s_url, wurl, 256);
        SelectObject(hdc, g_font_url);
        SetTextColor(hdc, g_url_hot ? RGB(214, 69, 65) : RGB(0, 102, 204));
        r.left = 0; r.right = rc.right;
        r.top = g_rc_url.top; r.bottom = g_rc_url.bottom;
        DrawTextW(hdc, wurl, -1, &r,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
      }

      /* ---- URL 下方提示（动态显示实际端口） ---- */
      SelectObject(hdc, g_font_hint);
      SetTextColor(hdc, RGB(150, 150, 150));
      r.left = 0; r.right = rc.right;
      r.top = g_rc_url.bottom + 8;
      r.bottom = r.top + LINE_H;
      {
        wchar_t hint[128];
        wsprintfW(hint,
                  L"\u670D\u52A1\u7AEF\u53E3 %d \u53EF\u5728\u542F\u52A8\u65F6\u6307\u5B9A",
                  s_port);
        DrawTextW(hdc, hint, -1, &r,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
      }

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&g_rc_url, pt) || PtInRect(&g_rc_qr, pt)) {
          SetCursor(LoadCursor(NULL, IDC_HAND));
        } else {
          SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return TRUE;
      }
      break;

    case WM_MOUSEMOVE: {
      POINT pt;
      pt.x = GET_X_LPARAM(lParam);
      pt.y = GET_Y_LPARAM(lParam);

      int in_qr  = PtInRect(&g_rc_qr, pt)  ? 1 : 0;
      int in_url = PtInRect(&g_rc_url, pt) ? 1 : 0;

      int hot = (in_qr || in_url) ? 1 : 0;
      if (hot != g_url_hot) {
        g_url_hot = hot;
        InvalidateRect(hwnd, NULL, FALSE);
      }

      /* 二维码悬停：显示操作提示 */
      if (in_qr) {
        if (!g_copy_tip_shown) {
          POINT sp = pt;
          ClientToScreen(hwnd, &sp);
          show_tip_at(L"\u5DE6\u952E\u6253\u5F00URL  \u53F3\u952E\u590D\u5236URL",
                      sp.x + 16, sp.y + 20);
        }
      } else {
        if (g_qr_hovered) {
          g_qr_hovered = 0;
          if (!g_copy_tip_shown) hide_tip_window();
        }
      }
      g_qr_hovered = in_qr;

      /* 请求 WM_MOUSELEAVE */
      {
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      if (g_qr_hovered) {
        g_qr_hovered = 0;
        if (!g_copy_tip_shown) hide_tip_window();
      }
      if (g_url_hot) {
        g_url_hot = 0;
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;

    /* 左键：二维码区 / URL 区 -> 打开网页 */
    case WM_LBUTTONUP: {
      POINT pt;
      pt.x = GET_X_LPARAM(lParam);
      pt.y = GET_Y_LPARAM(lParam);
      if (PtInRect(&g_rc_url, pt) || PtInRect(&g_rc_qr, pt)) {
        ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
      }
      return 0;
    }

    /* 右键：二维码区 / URL 区 -> 复制 URL，显示“已复制” */
    case WM_RBUTTONUP: {
      POINT pt;
      pt.x = GET_X_LPARAM(lParam);
      pt.y = GET_Y_LPARAM(lParam);
      if (PtInRect(&g_rc_qr, pt) || PtInRect(&g_rc_url, pt)) {
        copy_text_to_clipboard(hwnd, s_url);

        POINT sp = pt;
        ClientToScreen(hwnd, &sp);
        show_tip_at(L"\u5DF2\u590D\u5236", sp.x + 16, sp.y + 20);

        g_copy_tip_shown = 1;
        SetTimer(hwnd, IDT_COPY_TIP, 1300, NULL);
      }
      return 0;
    }

    case WM_TIMER:
      if (wParam == IDT_COPY_TIP) {
        KillTimer(hwnd, IDT_COPY_TIP);
        g_copy_tip_shown = 0;
        /* 鼠标若仍在二维码上，恢复显示操作提示 */
        if (g_qr_hovered) {
          POINT sp;
          GetCursorPos(&sp);
          show_tip_at(L"\u5DE6\u952E\u6253\u5F00URL  \u53F3\u952E\u590D\u5236URL",
                      sp.x + 16, sp.y + 20);
        } else {
          hide_tip_window();
        }
      }
      return 0;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      KillTimer(hwnd, IDT_COPY_TIP);
      if (g_tip_hwnd) { DestroyWindow(g_tip_hwnd); g_tip_hwnd = NULL; }
      if (g_font_tip) { DeleteObject(g_font_tip); g_font_tip = NULL; }
      if (g_font_url)  { DeleteObject(g_font_url);  g_font_url  = NULL; }
      if (g_font_hint) { DeleteObject(g_font_hint); g_font_hint = NULL; }
      g_hwnd = NULL;
      g_running = 0;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------- 解析命令行端口；返回 0 表示未指定 ---------- */
static int parse_port(const char *cmdline) {
  if (cmdline == NULL) return 0;
  while (*cmdline == ' ' || *cmdline == '\t' || *cmdline == '"') cmdline++;
  if (*cmdline == '\0') return 0;
  int p = atoi(cmdline);
  if (p > 0 && p < 65536) return p;
  return 0;
}

/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  (void) hPrevInstance;
  (void) nCmdShow;

  /* 先初始化 Winsock，端口探测和 get_local_ip 里的 UDP trick 都需要 */
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  mg_mgr_init(&s_mgr);
  mg_log_set(MG_LL_NONE);

  /* 期望端口：命令行指定优先，否则默认 18900 */
  int desired = parse_port(lpCmdLine);
  if (desired <= 0) desired = DEFAULT_PORT;

  /* 先试期望端口；被占用就改用系统随机分配的端口 */
  int listening = start_listen(desired);
  if (!listening) {
    for (int i = 0; i < 20 && !listening; i++) {
      int rp = pick_random_port();
      if (rp > 0) listening = start_listen(rp);
    }
  }
  if (!listening) {
    MessageBoxW(NULL,
                L"\u65E0\u6CD5\u76D1\u542C\u4EFB\u4F55\u7AEF\u53E3",
                L"\u9519\u8BEF", MB_OK | MB_ICONERROR);
    mg_mgr_free(&s_mgr);
    WSACleanup();
    return 1;
  }

  /* 两段式获取本机 IP：UDP trick 快命中，失败回退枚举网卡 */
  get_local_ip(s_url, sizeof(s_url), s_port);

  WNDCLASSEXW wc = {0};
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = WndProc;
  wc.hInstance     = hInstance;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH) GetStockObject(WHITE_BRUSH);
  wc.lpszClassName = L"TextSyncQR";
  if (!RegisterClassExW(&wc)) {
    mg_mgr_free(&s_mgr);
    WSACleanup();
    return 1;
  }

  RECT rc = {0, 0, CLIENT_W, CLIENT_H};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&rc, style, FALSE);
  int ww = rc.right - rc.left;
  int wh = rc.bottom - rc.top;

  int sw = GetSystemMetrics(SM_CXSCREEN);
  int sh = GetSystemMetrics(SM_CYSCREEN);
  int x = (sw - ww) / 2;
  int y = (sh - wh) / 2;

  g_hwnd = CreateWindowExW(0, L"TextSyncQR",
                           L"\u5B9E\u65F6\u6587\u672C\u540C\u6B65",
                           style, x, y, ww, wh,
                           NULL, NULL, hInstance, NULL);
  if (g_hwnd == NULL) {
    mg_mgr_free(&s_mgr);
    WSACleanup();
    return 1;
  }

  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  MSG msg;
  while (g_running) {
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_running = 0;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (!g_running) break;
    mg_mgr_poll(&s_mgr, 10);
    Sleep(10);
  }

  mg_mgr_free(&s_mgr);
  WSACleanup();
  return 0;
}