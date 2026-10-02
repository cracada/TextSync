#include "mongoose.h"
#include "qrcodegen.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAYICON 1
#define IDM_OPEN 1001
#define IDM_EXIT 1002
#define IDM_HELP 1003
#define IDM_QR   1004
#define DEFAULT_PORT 18900

static char s_listen_on[64] = "http://0.0.0.0:18900";
static NOTIFYICONDATAW nid;
static char s_url[256] = "http://127.0.0.1:18900";
static HWND g_hwnd = NULL;
static HWND g_qr_hwnd = NULL;
static struct mg_mgr s_mgr;
static int g_running = 1;
static int s_port = DEFAULT_PORT;

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

/* ---------- 获取本机局域网 IPv4 ---------- */
static void get_local_ip(char *buf, size_t len, int port) {
  ULONG outBufLen = 15000;
  PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(outBufLen);
  if (pAddresses == NULL) {
    snprintf(buf, len, "http://127.0.0.1:%d", port);
    return;
  }
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL,
                           pAddresses, &outBufLen) == NO_ERROR) {
    PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
    while (pCurr) {
      if (pCurr->OperStatus == IfOperStatusUp) {
        PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress;
        while (pUnicast) {
          if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
            struct sockaddr_in *sa =
                (struct sockaddr_in *) pUnicast->Address.lpSockaddr;
            const char *ip = inet_ntoa(sa->sin_addr);
            if (strcmp(ip, "127.0.0.1") != 0) {
              snprintf(buf, len, "http://%s:%d", ip, port);
              free(pAddresses);
              return;
            }
          }
          pUnicast = pUnicast->Next;
        }
      }
      pCurr = pCurr->Next;
    }
  }
  free(pAddresses);
  snprintf(buf, len, "http://127.0.0.1:%d", port);
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

/* ---------- 二维码窗口过程 ---------- */
static LRESULT CALLBACK QRWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
  switch (msg) {
    case WM_LBUTTONUP:
      ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
      return 0;
    case WM_RBUTTONUP:
      copy_text_to_clipboard(hwnd, s_url);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      int w = rc.right - rc.left;
      int h = rc.bottom - rc.top;

      HBRUSH hWhite = (HBRUSH) GetStockObject(WHITE_BRUSH);
      FillRect(hdc, &rc, hWhite);

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
        int side = (w < h ? w : h);
        int module_px = side / total;
        if (module_px < 1) module_px = 1;
        int qr_px = module_px * total;
        int ox = (w - qr_px) / 2 + quiet * module_px;
        int oy = (h - qr_px) / 2 + quiet * module_px;

        HBRUSH hBlack = (HBRUSH) GetStockObject(BLACK_BRUSH);
        for (int y = 0; y < size; y++) {
          for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
              RECT r;
              r.left   = ox + x * module_px;
              r.top    = oy + y * module_px;
              r.right  = ox + (x + 1) * module_px;
              r.bottom = oy + (y + 1) * module_px;
              FillRect(hdc, &r, hBlack);
            }
          }
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      g_qr_hwnd = NULL;
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

static void show_qr_window(HINSTANCE hInst) {
  if (g_qr_hwnd != NULL && IsWindow(g_qr_hwnd)) {
    SetForegroundWindow(g_qr_hwnd);
    return;
  }

  static BOOL s_registered = FALSE;
  if (!s_registered) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = QRWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.hbrBackground = (HBRUSH) GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"TextSyncQR";
    RegisterClassExW(&wc);
    s_registered = TRUE;
  }

  RECT rc = {0, 0, 300, 300};
  AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
  int ww = rc.right - rc.left;
  int wh = rc.bottom - rc.top;

  int sw = GetSystemMetrics(SM_CXSCREEN);
  int sh = GetSystemMetrics(SM_CYSCREEN);
  int x = (sw - ww) / 2;
  int y = (sh - wh) / 2;

  wchar_t wTitle[128];
  u2w("\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81: \xE5\xB7\xA6\xE9\x94\xAE\xE6\x89\x93\xE5\xBC\x80URL"
      "\xEF\xBC\x8C\xE5\x8F\xB3\xE9\x94\xAE\xE5\xA4\x8D\xE5\x88\xB6URL",
      wTitle, 128); /* "二维码: 左键打开URL，右键复制URL" */

  g_qr_hwnd = CreateWindowExW(
      0, L"TextSyncQR", wTitle,
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
      x, y, ww, wh,
      NULL, NULL, hInst, NULL);

  if (g_qr_hwnd != NULL) {
    ShowWindow(g_qr_hwnd, SW_SHOW);
    UpdateWindow(g_qr_hwnd);
    SetForegroundWindow(g_qr_hwnd);
  }
}

/* ---------- 托盘图标 ---------- */
static void create_tray_icon(HWND hwnd) {
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = ID_TRAYICON;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(101));

  wchar_t wlabel[64];
  wchar_t wurl[256];
  u2w("\xE6\x96\x87\xE6\x9C\xAC\xE5\x90\x8C\xE6\xAD\xA5", wlabel, 64); /* "文本同步" */
  u2w(s_url, wurl, 256);

  _snwprintf(nid.szTip, 128, L"%ls - %ls", wlabel, wurl);
  nid.szTip[127] = L'\0';

  Shell_NotifyIconW(NIM_ADD, &nid);
}

static void show_tray_menu(HWND hwnd) {
  POINT pt;
  GetCursorPos(&pt);

  wchar_t wOpen[32], wQR[32], wHelp[32], wExit[32];
  u2w("\xE6\x89\x93\xE5\xBC\x80", wOpen, 32);                        /* "打开" */
  u2w("\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81", wQR, 32);              /* "二维码" */
  u2w("\xE5\xB8\xAE\xE5\x8A\xA9", wHelp, 32);                        /* "帮助" */
  u2w("\xE9\x80\x80\xE5\x87\xBA", wExit, 32);                        /* "退出" */

  HMENU hMenu = CreatePopupMenu();
  AppendMenuW(hMenu, MF_STRING, IDM_OPEN, wOpen);
  AppendMenuW(hMenu, MF_STRING, IDM_QR, wQR);
  AppendMenuW(hMenu, MF_STRING, IDM_HELP, wHelp);
  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_STRING, IDM_EXIT, wExit);

  SetForegroundWindow(hwnd);
  TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
  DestroyMenu(hMenu);
}

/* ---------- 主窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  switch (msg) {
    case WM_TRAYICON:
      if (lParam == WM_LBUTTONUP) {
        ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
      } else if (lParam == WM_RBUTTONUP) {
        show_tray_menu(hwnd);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wParam) == IDM_EXIT) {
        DestroyWindow(hwnd);
      } else if (LOWORD(wParam) == IDM_OPEN) {
        ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
      } else if (LOWORD(wParam) == IDM_QR) {
        show_qr_window(GetModuleHandleW(NULL));
      } else if (LOWORD(wParam) == IDM_HELP) {
        wchar_t wTitle[32], wMsg[512];
        u2w("\xE5\xB8\xAE\xE5\x8A\xA9", wTitle, 32);
        u2w("\xE9\xBB\x98\xE8\xAE\xA4\xE7\xAB\xAF\xE5\x8F\xA3\xE4\xB8\xBA 18900\xEF\xBC\x8C"
            "\xE5\x90\xAF\xE5\x8A\xA8\xE6\x97\xB6\xE5\x8F\xAF\xE4\xBB\xA5\xE7\x9B\xB4\xE6\x8E\xA5"
            "\xE8\xB7\x9F\xE9\x9A\x8F\xE7\xAB\xAF\xE5\x8F\xA3\xE5\x8F\xB7\xE6\x9D\xA5"
            "\xE6\x8C\x87\xE5\xAE\x9A\xE6\x9C\x8D\xE5\x8A\xA1\xE7\xAB\xAF\xE5\x8F\xA3",
            wMsg, 512);
        MessageBoxW(hwnd, wMsg, wTitle, MB_OK | MB_ICONINFORMATION);
      }
      break;
    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &nid);
      g_running = 0;
      PostQuitMessage(0);
      break;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
  return 0;
}

/* ---------- 解析命令行端口 ---------- */
static int parse_port(const char *cmdline) {
  if (cmdline == NULL) return DEFAULT_PORT;
  while (*cmdline == ' ' || *cmdline == '\t' || *cmdline == '"') cmdline++;
  if (*cmdline == '\0') return DEFAULT_PORT;
  int p = atoi(cmdline);
  if (p > 0 && p < 65536) return p;
  return DEFAULT_PORT;
}

/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  (void) hPrevInstance;
  (void) nCmdShow;

  s_port = parse_port(lpCmdLine);
  snprintf(s_listen_on, sizeof(s_listen_on), "http://0.0.0.0:%d", s_port);
  get_local_ip(s_url, sizeof(s_url), s_port);

  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"TextSyncTray";
  RegisterClassExW(&wc);

  g_hwnd = CreateWindowExW(0, L"TextSyncTray", L"TextSync", 0, 0, 0, 0, 0,
                           NULL, NULL, hInstance, NULL);
  if (g_hwnd == NULL) return 1;

  create_tray_icon(g_hwnd);

  mg_mgr_init(&s_mgr);
  mg_log_set(MG_LL_NONE);
  mg_http_listen(&s_mgr, s_listen_on, fn, NULL);

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
  return 0;
}