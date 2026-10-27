#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601          /* Windows 7+：让 ws2tcpip.h 声明 inet_ntop */
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "mongoose.h"
#include "qrcodegen.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#include <ipifcons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "mongoose.h"
#include "qrcodegen.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")

#define DEFAULT_PORT 18900
#define PORT_TRIES   20

/* 共享文本大小上限（字节）—— 防误粘贴大文件撑爆内存 */
#define MAX_SHARED_TEXT (16u * 1024u * 1024u)   /* 16MB，可按需调整 */

/* 布局参数 */
#define PAD        10
#define GAP        10
#define URL_H      24
#define QR_SIZE    276
#define CLIENT_W   320
#define CLIENT_H   330

#define IDT_COPY_TIP 1

static char s_url[256] = "http://127.0.0.1:18900";
static struct mg_mgr s_mgr;
static int g_running = 1;
static int g_telemode = 0;

static HFONT g_font_url  = NULL;

static RECT g_rc_qr  = {0, 0, 0, 0};
static RECT g_rc_url = {0, 0, 0, 0};
static int  g_url_hot = 0;

/* ---- 自绘提示窗口 ---- */
static HWND    g_tip_hwnd = NULL;
static HFONT   g_font_tip = NULL;
static wchar_t g_tip_text[256] = L"";
static int     g_qr_hovered = 0;
static int     g_copy_tip_shown = 0;

/* ---- 服务端共享文本 ---- */
static char  *g_shared_text = NULL;
static size_t g_shared_text_len = 0;

/* ---- WebSocket 分片重组 ---- */
static char  *s_frag = NULL;
static size_t s_frag_len = 0;

/* ---- QR 编码缓存（WM_CREATE 编码一次）---- */
static uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
static int     g_qr_size = 0;

/* ---------- UTF-8 -> UTF-16 ---------- */
static void u2w(const char *src, wchar_t *dst, int dst_len) {
  if (dst_len > 0) dst[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_len);
}

/* ---------- 通用弹窗 ---------- */
static void show_msg_w(HWND owner, const char *title_utf8,
                       const char *text_utf8, UINT icon) {
  if (text_utf8 == NULL) return;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, text_utf8, -1, NULL, 0);
  if (wlen <= 0) return;
  wchar_t *wbuf = (wchar_t *) malloc((size_t) wlen * sizeof(wchar_t));
  if (wbuf == NULL) return;
  MultiByteToWideChar(CP_UTF8, 0, text_utf8, -1, wbuf, wlen);

  wchar_t wtitle[128];
  if (title_utf8 != NULL) {
    MultiByteToWideChar(CP_UTF8, 0, title_utf8, -1, wtitle, 128);
    wtitle[127] = L'\0';
  } else {
    wtitle[0] = L'\0';
  }

  MessageBoxW(owner, wbuf, wtitle, icon | MB_OK);
  free(wbuf);
}

static void show_help(void) {
  static const char *help_text =
    "\u7528\u6CD5\uFF1ATextSync.exe [\u9009\u9879]\r\n"
    "\r\n"
    "  --port=\u7AEF\u53E3\u53F7   \u6307\u5B9A\u76D1\u542C\u7AEF\u53E3"
    "\uFF08\u9ED8\u8BA4 18900\uFF0C\u51B2\u7A81\u65F6\u81EA\u52A8\u9012\u589E\uFF09\r\n"
    "  --telemode       \u542F\u52A8\u540E\u76F4\u63A5\u8FDB\u5165\u7535\u62A5\u6A21\u5F0F\r\n"
    "  --open           \u542F\u52A8\u540E\u7528\u9ED8\u8BA4\u6D4F\u89C8\u5668\u6253\u5F00\u94FE\u63A5\r\n"
    "  --help           \u663E\u793A\u6B64\u5E2E\u52A9\r\n"
    "\r\n"
    "\u793A\u4F8B\uFF1A\r\n"
    "  TextSync.exe --port=8080\r\n"
    "  TextSync.exe --telemode\r\n"
    "  TextSync.exe --open\r\n"
    "  TextSync.exe --port=8080 --telemode --open\r\n";

  show_msg_w(NULL,
             "\u5B9E\u65F6\u6587\u672C\u540C\u6B65 - \u5E2E\u52A9",
             help_text, MB_ICONINFORMATION);
}

static void show_port_fail(int port) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "\u65E0\u6CD5\u7ED1\u5B9A\u7AEF\u53E3"
           "\uFF08%d - %d \u5747\u88AB\u5360\u7528\uFF09",
           port, port + PORT_TRIES - 1);
  show_msg_w(NULL, "\u542F\u52A8\u5931\u8D25", buf, MB_ICONERROR);
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
    "#toolbar{flex:0 0 auto;display:flex;align-items:center;gap:6px;margin-bottom:6px;min-width:0}"
    "#toolbar button{padding:4px 9px;font-size:13px;cursor:pointer;"
    "border:1px solid #bbb;border-radius:5px;background:#fff;font-family:inherit;"
    "white-space:nowrap;flex:0 0 auto}"
    "#toolbar button:active{filter:brightness(.92)}"
    "#mode-btn.typewriter{background:#e8f0fe;border-color:#4a90e2;color:#1a5490}"
    "#mode-btn.typewriter:hover{background:#d0e1fd}"
    "#mode-btn.telegram{background:#fff3e0;border-color:#f5a623;color:#8a5a00}"
    "#mode-btn.telegram:hover{background:#ffe0b2}"
    "#toolbar button:not(#mode-btn):hover{background:#eef3fb;border-color:#4a90e2}"
    "#status{font-size:12px;color:#666;flex:0 1 auto;text-align:left;"
    "overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}"
    "#status.ok{color:#2a9d2a}#status.err{color:#c33}#status.warn{color:#d38b00}"
    "#mode-btn{margin-left:auto}"
    "textarea{flex:1 1 auto;width:100%;min-height:0;padding:12px;font-size:16px;"
    "border:1px solid #ccc;border-radius:8px;box-sizing:border-box;resize:none;"
    "font-family:inherit;outline:none;background:#fff}"
    "textarea:focus{border-color:#4a90e2;box-shadow:0 0 0 2px rgba(74,144,226,.15)}"
    "</style></head><body>"
    "<div id=\"toolbar\">"
    "<span id=\"status\">打字机模式 · 连接中...</span>"
    "<button id=\"send-btn\" type=\"button\" style=\"display:none\">发送</button>"
    "<button id=\"refresh-btn\" type=\"button\" style=\"display:none\">刷新</button>"
    "<button id=\"mode-btn\" type=\"button\" class=\"typewriter\">切换到电报模式</button>"
    "</div>"
    "<textarea id=\"text\" spellcheck=\"false\" autocapitalize=\"off\" "
    "autocomplete=\"off\" placeholder=\"在这里输入...\"></textarea>"
    "<script>"
    "(function(){"
    "var ta=document.getElementById('text');"
    "var statusEl=document.getElementById('status');"
    "var modeBtn=document.getElementById('mode-btn');"
    "var sendBtn=document.getElementById('send-btn');"
    "var refreshBtn=document.getElementById('refresh-btn');"
    "var mode='typewriter';"
    "var ws=null;"
    "var ignore=false;"
    "function setStatus(t,c){statusEl.textContent=t;statusEl.className=c||'';}"
    "function connectWs(){"
    "if(ws)return;"
    "setStatus('打字机模式 · 连接中...');"
    "ws=new WebSocket('ws://'+location.host+'/ws');"
    "ws.onopen=function(){setStatus('打字机模式 · 已连接','ok');};"
    "ws.onclose=function(){ws=null;if(mode==='typewriter'){"
    "setStatus('打字机模式 · 连接断开，重连中...','err');"
    "setTimeout(connectWs,2000);}};"
    "ws.onerror=function(){setStatus('打字机模式 · 连接错误','err');};"
    "ws.onmessage=function(e){"
    "if(mode!=='typewriter')return;"
    "if(e.data===ta.value)return;"
    "ignore=true;ta.value=e.data;ignore=false;"
    "};"
    "}"
    "function disconnectWs(){"
    "if(ws){var w=ws;ws=null;"
    "w.onopen=null;w.onclose=null;w.onerror=null;w.onmessage=null;"
    "try{w.close();}catch(e){}}"
    "}"
    "function doSend(){"
    "var xhr=new XMLHttpRequest();"
    "xhr.open('POST','/text',true);"
    "xhr.setRequestHeader('Content-Type','text/plain; charset=utf-8');"
    "xhr.onload=function(){"
    "if(xhr.status===200){setStatus('已发送 '+ta.value.length+' 字符','ok');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},1500);}"
    "else{setStatus('发送失败 ('+xhr.status+')','err');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},2000);}};"
    "xhr.onerror=function(){setStatus('发送失败','err');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},2000);};"
    "xhr.send(ta.value);"
    "}"
    "function doRefresh(){"
    "var xhr=new XMLHttpRequest();"
    "xhr.open('GET','/text?_='+Date.now(),true);"
    "xhr.onload=function(){"
    "if(xhr.status===200){"
    "if(xhr.responseText!==ta.value){"
    "ignore=true;ta.value=xhr.responseText;ignore=false;}"
    "setStatus('已刷新 '+ta.value.length+' 字符','ok');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},1500);"
    "}else{setStatus('刷新失败 ('+xhr.status+')','err');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},2000);}};"
    "xhr.onerror=function(){setStatus('刷新失败','err');"
    "setTimeout(function(){if(mode==='telegram')setStatus('电报模式','ok');},2000);};"
    "xhr.send();"
    "}"
    "function setMode(m){"
    "if(m==='typewriter'){"
    "mode='typewriter';"
    "modeBtn.className='typewriter';"
    "modeBtn.textContent='切换到电报模式';"
    "sendBtn.style.display='none';"
    "refreshBtn.style.display='none';"
    "setStatus('打字机模式 · 连接中...');"
    "connectWs();"
    "}else{"
    "mode='telegram';"
    "modeBtn.className='telegram';"
    "modeBtn.textContent='切换到打字机模式';"
    "sendBtn.style.display='';"
    "refreshBtn.style.display='';"
    "disconnectWs();"
    "setStatus('电报模式','ok');"
    "doRefresh();"
    "}"
    "}"
    "modeBtn.addEventListener('click',function(){"
    "setMode(mode==='typewriter'?'telegram':'typewriter');"
    "});"
    "sendBtn.addEventListener('click',doSend);"
    "refreshBtn.addEventListener('click',doRefresh);"
    "ta.addEventListener('input',function(){"
    "if(ignore)return;"
    "if(mode==='typewriter'){"
    "if(ws&&ws.readyState===WebSocket.OPEN){ws.send(ta.value);}"
    "}"
    "});"
    "ta.addEventListener('keydown',function(e){"
    "if(mode==='telegram'&&(e.ctrlKey||e.metaKey)&&e.key==='Enter'){"
    "e.preventDefault();doSend();}"
    "});"
    "if(mode==='telegram'){"
    "modeBtn.className='telegram';"
    "sendBtn.style.display='';"
    "refreshBtn.style.display='';"
    "modeBtn.textContent='切换到打字机模式';"
    "setStatus('电报模式','ok');"
    "doRefresh();"
    "}else{"
    "modeBtn.className='typewriter';"
    "connectWs();"
    "}"
    "})();"
    "</script></body></html>";

/* ---------- 共享文本（返回 1 成功，0 失败/超限）---------- */
static int set_shared_text(const char *text, size_t len) {
  /* C9：三重防御 */
  if (text == NULL && len > 0) return 0;          /* 空指针 + 非零长度 */
  if (len == (size_t) -1)      return 0;          /* len+1 会溢出 */
  if (len > MAX_SHARED_TEXT)   return 0;          /* 超上限 */

  char *p = (char *) realloc(g_shared_text, len + 1);
  if (p == NULL) return 0;

  g_shared_text = p;
  if (len > 0) memcpy(g_shared_text, text, len);
  g_shared_text[len] = '\0';
  g_shared_text_len = len;
  return 1;
}

/* ---------- WebSocket 分片 ---------- */
static void frag_reset(void) {
  free(s_frag);
  s_frag = NULL;
  s_frag_len = 0;
}

/* 返回 1 成功；0 表示超限或 OOM，并已 frag_reset */
static int frag_append(const char *buf, size_t len) {
  if (len == 0) return 1;

  /* 先防 len 本身超限，再防累加溢出 */
  if (len > MAX_SHARED_TEXT || s_frag_len > MAX_SHARED_TEXT - len) {
    frag_reset();
    return 0;
  }

  char *p = (char *) realloc(s_frag, s_frag_len + len);
  if (p == NULL) {
    frag_reset();
    return 0;
  }
  s_frag = p;
  memcpy(s_frag + s_frag_len, buf, len);
  s_frag_len += len;
  return 1;
}

/* ---------- Mongoose 回调 ---------- */
static void broadcast(struct mg_mgr *mgr, const char *msg, size_t len,
                      struct mg_connection *exclude) {
  struct mg_connection *c;
  for (c = mgr->conns; c != NULL; c = c->next) {
    if (c->is_websocket && c != exclude && !c->is_closing) {
      mg_ws_send(c, msg, len, WEBSOCKET_OP_TEXT);
    }
  }
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
      mg_ws_upgrade(c, hm, NULL);

    } else if (mg_match(hm->uri, mg_str("/text"), NULL)) {
      if (mg_match(hm->method, mg_str("POST"), NULL)) {
        if (hm->body.len > MAX_SHARED_TEXT) {
          /* 超限：413，不修改共享文本 */
          mg_http_reply(c, 413,
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n",
                        "Payload Too Large");
        } else if (!set_shared_text(hm->body.buf, hm->body.len)) {
          mg_http_reply(c, 500,
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n",
                        "Error");
        } else {
          broadcast(c->mgr, hm->body.buf, hm->body.len, NULL);
          mg_http_reply(c, 200,
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n",
                        "OK");
        }
      } else {
        if (g_shared_text != NULL) {
          mg_http_reply(c, 200,
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\n",
                        "%.*s", (int) g_shared_text_len, g_shared_text);
        } else {
          mg_http_reply(c, 200,
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\n",
                        "");
        }
      }

    } else if (mg_match(hm->uri, mg_str("/"), NULL)) {
      const char *marker = "var mode='typewriter';";
      const char *found  = strstr(s_html, marker);
      if (found == NULL) {
        mg_http_reply(c, 200, "Content-Type: text/html\r\n", "%s", s_html);
      } else {
        const char *repl = g_telemode
                               ? "var mode='telegram';"
                               : "var mode='typewriter';";
        mg_http_reply(c, 200, "Content-Type: text/html\r\n",
                      "%.*s%s%s",
                      (int) (found - s_html), s_html,
                      repl,
                      found + strlen(marker));
      }

    } else if (mg_match(hm->uri, mg_str("/favicon.ico"), NULL)) {
      mg_http_reply(c, 204, "", "");
    } else {
      mg_http_reply(c, 302, "Location: /\r\n", "");
    }

  } else if (ev == MG_EV_WS_OPEN) {
    if (g_shared_text != NULL && g_shared_text_len > 0) {
      mg_ws_send(c, g_shared_text, g_shared_text_len, WEBSOCKET_OP_TEXT);
    }

  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    int op  = wm->flags & 0x0f;
    int fin = (wm->flags & 0x80) != 0;

    if (op == WEBSOCKET_OP_TEXT) {
      frag_reset();
      if (!frag_append(wm->data.buf, wm->data.len)) return;
    } else if (op == 0) {                 /* continuation */
      if (s_frag == NULL) return;         /* 孤立 CONT（或已在超限时被 reset） */
      if (!frag_append(wm->data.buf, wm->data.len)) return;
    } else {
      return;                             /* binary / control 忽略 */
    }

    if (!fin) return;

    /* s_frag_len 已 ≤ MAX_SHARED_TEXT，set 仍双保险 */
    if (!set_shared_text(s_frag, s_frag_len)) {
      frag_reset();
      return;
    }
    broadcast(c->mgr, s_frag, s_frag_len, c);
    frag_reset();
  }
}

/* ---------- 尝试监听，冲突时自动递增端口 ---------- */
static int try_listen(int start_port, int max_tries) {
  for (int i = 0; i < max_tries; i++) {
    int port = start_port + i;
    if (port < 1 || port > 65535) break;

    char addr[64];
    snprintf(addr, sizeof(addr), "http://0.0.0.0:%d", port);

    struct mg_connection *c = mg_http_listen(&s_mgr, addr, fn, NULL);
    if (c != NULL) {
      return port;
    }
  }
  return 0;
}

/* ---------- 兜底方案：枚举网卡 ---------- */
static int get_ip_by_adapters(char *out, size_t out_len) {
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;
  ULONG outBufLen = 15000;
  PIP_ADAPTER_ADDRESSES pAddresses = NULL;
  DWORD rc = ERROR_BUFFER_OVERFLOW;

  for (int tries = 0; tries < 3; tries++) {
    free(pAddresses);
    pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(outBufLen);
    if (pAddresses == NULL) return 0;
    rc = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &outBufLen);
    if (rc != ERROR_BUFFER_OVERFLOW) break;
  }

  int found = 0;
  if (rc == NO_ERROR && pAddresses != NULL) {
    PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
    while (pCurr && !found) {
      if (pCurr->OperStatus == IfOperStatusUp &&
          pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
        PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress;
        while (pUnicast) {
          if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
            struct sockaddr_in *sa =
                (struct sockaddr_in *) pUnicast->Address.lpSockaddr;
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) != NULL) {
              if (strcmp(ip, "127.0.0.1") != 0 && strcmp(ip, "0.0.0.0") != 0) {
                strncpy(out, ip, out_len - 1);
                out[out_len - 1] = '\0';
                found = 1;
                break;
              }
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
  snprintf(buf, len, "http://127.0.0.1:%d", port);

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

      HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 225));
      FillRect(hdc, &rc, hBrush);
      DeleteObject(hBrush);

      HPEN hPen = CreatePen(PS_SOLID, 1, RGB(118, 118, 118));
      HPEN oldPen = (HPEN) SelectObject(hdc, hPen);
      HBRUSH oldBrush = (HBRUSH) SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, 0, 0, rc.right, rc.bottom);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBrush);
      DeleteObject(hPen);

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

static void show_tip_at(const wchar_t *text, int sx, int sy) {
  if (g_tip_hwnd == NULL) return;

  wcsncpy(g_tip_text, text, 255);
  g_tip_text[255] = L'\0';

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
  int H = rc.bottom;
  if (W <= 0) W = CLIENT_W;
  if (H <= 0) H = CLIENT_H;

  int max_w = W - 2 * PAD;
  int max_h = H - PAD - GAP - URL_H - PAD;
  int qs = QR_SIZE;
  if (qs > max_w) qs = max_w;
  if (qs > max_h) qs = max_h;
  if (qs < 40) qs = 40;

  g_rc_qr.left   = (W - qs) / 2;
  g_rc_qr.top    = PAD;
  g_rc_qr.right  = g_rc_qr.left + qs;
  g_rc_qr.bottom = g_rc_qr.top + qs;

  wchar_t wurl[256];
  u2w(s_url, wurl, 256);

  int cx = 0, cy = URL_H;
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

  int uy = g_rc_qr.bottom + GAP;
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
      g_font_url = create_font(17, FW_NORMAL, TRUE);
      create_tip_window(((LPCREATESTRUCTW) lParam)->hInstance);
      /* QR 编码一次缓存，WM_PAINT 不再重复编码 */
      {
        uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
        if (qrcodegen_encodeText(s_url, tmp, g_qr, qrcodegen_Ecc_MEDIUM,
                                 qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                 qrcodegen_Mask_AUTO, true)) {
          g_qr_size = qrcodegen_getSize(g_qr);
        } else {
          g_qr_size = 0;
        }
      }
      compute_layout(hwnd);
      return 0;

    case WM_SIZE:
      compute_layout(hwnd);
      return 0;

    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;

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

      if (g_qr_size > 0) {
        int size = g_qr_size;
        int quiet = 4;                     /* 规范推荐 4，提升扫码鲁棒性 */
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
            if (qrcodegen_getModule(g_qr, x, y)) {
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

      {
        wchar_t wurl[256];
        u2w(s_url, wurl, 256);
        HFONT oldFont = (HFONT) SelectObject(hdc, g_font_url);
        SetTextColor(hdc, g_url_hot ? RGB(214, 69, 65) : RGB(0, 102, 204));
        r.left = 0; r.right = rc.right;
        r.top = g_rc_url.top; r.bottom = g_rc_url.bottom;
        DrawTextW(hdc, wurl, -1, &r,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
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

      /* 只在进入 QR 区域的一次显示提示，避免跟随鼠标反复重绘 */
      if (in_qr) {
        if (!g_qr_hovered && !g_copy_tip_shown) {
          POINT sp = pt;
          ClientToScreen(hwnd, &sp);
          show_tip_at(L"\u5DE6\u952E\u6253\u5F00URL  \u53F3\u952E\u590D\u5236URL",
                      sp.x + 16, sp.y + 20);
        }
      } else {
        if (g_qr_hovered) {
          if (!g_copy_tip_shown) hide_tip_window();
        }
      }
      g_qr_hovered = in_qr;

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

    case WM_LBUTTONUP: {
      POINT pt;
      pt.x = GET_X_LPARAM(lParam);
      pt.y = GET_Y_LPARAM(lParam);
      if (PtInRect(&g_rc_url, pt) || PtInRect(&g_rc_qr, pt)) {
        ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
      }
      return 0;
    }

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
      g_running = 0;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------- 解析命令行参数 ---------- */
static int parse_args(const char *cmdline, int *port, int *telemode,
                      int *open_browser) {
  *port = DEFAULT_PORT;
  *telemode = 0;
  *open_browser = 0;
  if (cmdline == NULL) return 0;

  const char *p = cmdline;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (*p == '\0') break;

    if (strncmp(p, "--port=", 7) == 0) {
      const char *val = p + 7;
      if (*val == '"') val++;
      int v = atoi(val);
      if (v > 0 && v < 65536) *port = v;
      while (*p && *p != ' ' && *p != '\t' && *p != '"') p++;
    } else if (strncmp(p, "--telemode", 10) == 0 &&
               (p[10] == '\0' || p[10] == ' ' || p[10] == '\t' || p[10] == '"')) {
      *telemode = 1;
      p += 10;
    } else if (strncmp(p, "--open", 6) == 0 &&
               (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '"')) {
      *open_browser = 1;
      p += 6;
    } else if (strncmp(p, "--help", 6) == 0 &&
               (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '"')) {
      return 1;
    } else {
      while (*p && *p != ' ' && *p != '\t' && *p != '"') p++;
    }
  }
  return 0;
}

/* ---------- 启用 Per-Monitor DPI 感知（动态加载，兼容旧 SDK） ---------- */
static void enable_dpi_awareness(void) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != NULL) {
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
    SetProcessDpiAwarenessContextFn pSet =
        (SetProcessDpiAwarenessContextFn)(void *)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (pSet != NULL) {
      /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
      if (pSet((HANDLE) -4)) return;
    }
  }
  SetProcessDPIAware();
}

/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  (void) hPrevInstance;
  (void) nCmdShow;

  enable_dpi_awareness();

  int port = DEFAULT_PORT;
  int open_browser = 0;
  int show_help_flag = parse_args(lpCmdLine, &port, &g_telemode, &open_browser);

  if (show_help_flag) {
    show_help();
    return 0;
  }

  /* ---- 单实例判断 ---- */
  HANDLE hMutex = CreateMutexW(NULL, TRUE, L"TextSync_SingleInstance_Mutex");
  if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND hExisting = FindWindowW(L"TextSyncQR", NULL);
    if (hExisting != NULL) {
      ShowWindow(hExisting, SW_RESTORE);
      SetForegroundWindow(hExisting);
    }
    CloseHandle(hMutex);
    return 0;
  }

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    show_msg_w(NULL, "\u542F\u52A8\u5931\u8D25",
               "Winsock \u521D\u59CB\u5316\u5931\u8D25", MB_ICONERROR);
    if (hMutex != NULL) CloseHandle(hMutex);
    return 1;
  }

  mg_mgr_init(&s_mgr);
  mg_log_set(MG_LL_NONE);

  int actual_port = try_listen(port, PORT_TRIES);
  if (actual_port <= 0) {
    show_port_fail(port);
    mg_mgr_free(&s_mgr);
    WSACleanup();
    if (hMutex != NULL) CloseHandle(hMutex);
    return 1;
  }

  get_local_ip(s_url, sizeof(s_url), actual_port);

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
    if (hMutex != NULL) CloseHandle(hMutex);
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

  HWND hwnd = CreateWindowExW(0, L"TextSyncQR",
                              L"\u5B9E\u65F6\u6587\u672C\u540C\u6B65",
                              style, x, y, ww, wh,
                              NULL, NULL, hInstance, NULL);
  if (hwnd == NULL) {
    mg_mgr_free(&s_mgr);
    WSACleanup();
    if (hMutex != NULL) CloseHandle(hMutex);
    return 1;
  }

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);

  /* --open：窗口就绪后用默认浏览器打开链接 */
  if (open_browser) {
    ShellExecuteA(NULL, "open", s_url, NULL, NULL, SW_SHOWNORMAL);
  }

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
    mg_mgr_poll(&s_mgr, 1);
  }

  mg_mgr_free(&s_mgr);
  frag_reset();
  if (g_shared_text != NULL) free(g_shared_text);
  WSACleanup();
  if (hMutex != NULL) CloseHandle(hMutex);
  return 0;
}