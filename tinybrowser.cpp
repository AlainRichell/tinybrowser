// tinybrowser: a 160x90 frameless, always-on-top browser window (Win32 + WebView2).
//
// The address bar IS the window's start page. It is rendered with NavigateToString
// and talks to the host through chrome.webview.postMessage using one-letter commands,
// so there is no local server, no temporary files and no scripting layer. WebView2
// requires every engine call on the UI thread; this file never touches it elsewhere.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <wrl.h>

#include "WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int  kWidth = 160;
constexpr int  kHeight = 90;
constexpr int  kMinWidth = 60;   // the window can be resized down to this and no further
constexpr int  kMinHeight = 60;
constexpr int  kMargin = 24;   // gap to the right and bottom edges of the desktop
constexpr UINT_PTR kFitTimer = 1;      // one-shot: measure the page after it settles
constexpr double kMinZoom = 0.10;
constexpr double kMaxZoom = 1.0;

// Crisp route: the page is drawn one to one into screen pixels, with grey antialiasing
// instead of ClearType fringes, and the zoom is picked so the site's font lands on a whole
// number of physical pixels. A fractional ppem is what leaves the rasterizer unable to fit
// stems to the pixel grid, and a 160 px wide window is exactly where that shows.
constexpr bool kCrispText = true;
constexpr wchar_t kCrispArgs[] = L"--disable-lcd-text --font-render-hinting=full";
// WebView2 reads extra browser arguments from this variable in the host process, which is
// cheaper and less fragile than handing it a custom ICoreWebView2EnvironmentOptions.
constexpr wchar_t kCrispArgsVar[] = L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
constexpr UINT kHotkeyBar = 1;        // Ctrl+Alt+S
constexpr UINT kHotkeyQuit = 2;       // Ctrl+Alt+X
constexpr UINT kHotkeyDuplicate = 3;  // Ctrl+Alt+D
constexpr UINT kHotkeyBack = 4;       // Ctrl+Alt+Z

constexpr wchar_t kTitle[] = L"tinybrowser";
constexpr wchar_t kWindowClass[] = L"TinyBrowserWindow";
constexpr int kIconId = 1;  // resource id of the donut in tinybrowser.rc
constexpr wchar_t kSearchPrefix[] = L"https://www.google.com/search?q=";
constexpr wchar_t kRuntimeUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
constexpr wchar_t kBootstrapperName[] = L"MicrosoftEdgeWebview2Setup.exe";

HWND g_window = nullptr;
HHOOK g_keyboardHook = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
// Resizing is driven from inside the page (see kPageScript): the window that really receives the
// mouse is the engine's render widget, and it belongs to the browser process, so no in-process
// subclass can ever see those clicks.
std::wstring g_url;
std::wstring g_startTarget;  // URL given on the command line, if any
RECT g_placedRect = {};        // --place x y width height, when a duplicate asks for its spot
bool g_hasPlacedRect = false;
wchar_t g_resizeEdge[3] = {};  // edges being dragged ("", "n", "se", ...), empty when idle
RECT g_resizeAnchor = {};      // window rectangle the drag started from
bool g_showingBar = false;     // the document on screen is our own bar, not a page
int g_fitPass = 0;     // measurements taken for the current page
int g_widestSeen = 0;  // widest layout seen for the current page, in CSS pixels
double g_bodyFont = 0.0;  // body font size of the page, in CSS pixels

void ResetFit();  // defined next to the fit helpers below

// Commands the bar page sends over chrome.webview.postMessage:
// 'G'+url navigate, 'B' back, 'F' forward, 'R' reload, 'X' close, 'S' show the bar,
// 'M'+dx+','+dy move the window by that many pixels (dragging).
constexpr wchar_t kBarTemplate[] = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  :root{color-scheme:dark}
  *{box-sizing:border-box;margin:0;padding:0;font-family:"Segoe UI",sans-serif}
  html,body{height:100%;background:#0f141b;color:#d7e1ea;overflow:hidden}
  body{display:flex;flex-direction:column;gap:3px;padding:3px;user-select:none;cursor:default}
  #grip{height:8px;border-radius:3px;background:repeating-linear-gradient(90deg,#2b3644 0 6px,transparent 6px 12px);cursor:move}
  .row{display:flex;gap:3px;align-items:center}
  input{flex:1;min-width:0;height:20px;padding:0 4px;font-size:11px;color:#eaf2f8;background:#060a0f;border:1px solid #2b3644;border-radius:4px}
  input:focus{outline:none;border-color:#4a90c2}
  button{flex:0 0 20px;height:20px;font-size:11px;line-height:1;cursor:pointer;color:#9db2c6;background:#1a2330;border:1px solid #2b3644;border-radius:4px}
  button:hover{color:#fff;background:#253141}
  button:disabled{color:#46586a;background:#141c26;border-color:#1e2833;cursor:default}
  button:disabled:hover{color:#46586a;background:#141c26}
  kbd{color:#9db2c6;background:#1a2330;border-radius:3px;padding:0 3px;font:inherit}
  #brand{position:absolute;right:5px;bottom:2px;font-size:9px;letter-spacing:.3px;color:#33455a}
  #commands{position:absolute;left:3px;right:3px;top:3px;bottom:3px;z-index:5;padding:4px 6px;
    background:#0c1219;border:1px solid #2b3644;border-radius:4px;font-size:10px;line-height:1.25;
    color:#9db2c6;overflow:auto}
  #commands[hidden]{display:none}
  #commands .k{display:inline-block;min-width:58px;color:#7fb3d5}
  #closepanel{font-size:9px;color:#5b6b7d;margin-top:1px}
</style></head><body>
<div id="grip" title="drag"></div>
<div class="row"><input id="url" type="text" spellcheck="false" autocomplete="off" placeholder="url o busqueda" value="%URL%"><button id="go" title="Go">&#9166;</button></div>
<div class="row"><button id="back" title="Back"%DISABLED%>&#8249;</button><button id="fwd" title="Forward"%DISABLED%>&#8250;</button><button id="reload" title="Reload"%DISABLED%>&#8635;</button><button id="dup" title="Duplicate in another window (Ctrl+Alt+D)"%DISABLED%>&#10697;</button><button id="help" title="Comandos y atajos">?</button><button id="close" title="Close (Ctrl+Alt+X)">&#10005;</button></div>
<div id="brand">tinybrowser 2026</div>
<div id="commands" hidden>
<div><span class="k">Ctrl+Alt+S</span> barra</div>
<div><span class="k">Ctrl+Alt+D</span> duplicar</div>
<div><span class="k">Ctrl+Alt+Z</span> atr&aacute;s</div>
<div><span class="k">Ctrl+Alt+X</span> cerrar</div>
<div><span class="k">Alt+arrastrar</span> mover</div>
<div id="closepanel">clic para cerrar</div>
</div>
<script>
(function () {
  function send(message) { try { window.chrome.webview.postMessage(message); } catch (e) { } }
  function submit() {
    var value = document.getElementById("url").value.trim();
    if (value) { send("G" + value); }
  }
  document.getElementById("go").onclick = submit;
  document.getElementById("url").onkeydown = function (e) {
    if (e.key === "Enter") { e.preventDefault(); submit(); }
  };
  document.getElementById("back").onclick = function () { send("B"); };
  document.getElementById("fwd").onclick = function () { send("F"); };
  document.getElementById("reload").onclick = function () { send("R"); };
  document.getElementById("dup").onclick = function () { send("C"); };
  document.getElementById("close").onclick = function () { send("X"); };

  // The command list lives in a panel over the whole bar, so it fits at any window size.
  var commands = document.getElementById("commands");
  document.getElementById("help").onclick = function () { send("H"); commands.hidden = !commands.hidden; };
  commands.onclick = function () { commands.hidden = true; };
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") { commands.hidden = true; }
  }, true);

  // The engine swallows mouse input before it reaches the host window, so dragging has to
  // start inside the page: the grip reports pixel deltas and the host moves itself.
  function startDrag(element) {
    element.addEventListener("mousedown", function (e) {
      if (e.button !== 0) { return; }
      e.preventDefault();
      var lastX = e.screenX;
      var lastY = e.screenY;
      function move(ev) {
        send("M" + (ev.screenX - lastX) + "," + (ev.screenY - lastY));
        lastX = ev.screenX;
        lastY = ev.screenY;
      }
      function stop() {
        document.removeEventListener("mousemove", move, true);
        document.removeEventListener("mouseup", stop, true);
      }
      document.addEventListener("mousemove", move, true);
      document.addEventListener("mouseup", stop, true);
    });
  }
  startDrag(document.getElementById("grip"));

  var input = document.getElementById("url");
  input.focus();
  input.select();
})();
</script></body></html>
)HTML";

// Document-start script for every page: keep the bar reachable, stop target=_blank from
// asking for a popup window the host does not have, and move the window on Alt+drag (the
// host never sees mouse input that the engine already consumed).
constexpr wchar_t kPageScript[] = LR"JS((function () {
  function post(message) {
    try { window.chrome.webview.postMessage(message); } catch (err) { }
  }
  document.addEventListener("click", function (e) {
    var link = e.target && e.target.closest ? e.target.closest("a") : null;
    if (link && (link.target || "").toLowerCase() === "_blank") { link.target = "_self"; }
  }, true);
  document.addEventListener("keydown", function (e) {
    if (e.ctrlKey && e.altKey && (e.key === "s" || e.key === "S")) {
      e.preventDefault();
      post("S");
    }
  }, true);
  document.addEventListener("mousedown", function (e) {
    if (!e.altKey || e.button !== 0) { return; }
    e.preventDefault();
    e.stopPropagation();
    var lastX = e.screenX;
    var lastY = e.screenY;
    function move(ev) {
      post("M" + (ev.screenX - lastX) + "," + (ev.screenY - lastY));
      lastX = ev.screenX;
      lastY = ev.screenY;
    }
    function stop() {
      document.removeEventListener("mousemove", move, true);
      document.removeEventListener("mouseup", stop, true);
    }
    document.addEventListener("mousemove", move, true);
    document.addEventListener("mouseup", stop, true);
  }, true);

  // The window has no border and the engine's input window belongs to another process, so the
  // edges are found by coordinates: near an edge the pointer changes, and pressing there starts
  // a resize session. Only the edge and the screen deltas are sent; the host owns the geometry.
  var EDGE = 4;
  function windowSize() {
    var size = window.__tinybrowserSize;
    return (size && size.length === 2) ? size : [window.innerWidth, window.innerHeight];
  }
  function edgeAt(clientX, clientY) {
    var size = windowSize();
    var x = clientX * size[0] / window.innerWidth;
    var y = clientY * size[1] / window.innerHeight;
    var vertical = y < EDGE ? "n" : (y >= size[1] - EDGE ? "s" : "");
    var horizontal = x < EDGE ? "w" : (x >= size[0] - EDGE ? "e" : "");
    // Vertical first: the corners come out as nw/ne/sw/se, the names the cursor rules use.
    return vertical + horizontal;
  }
  // The cursor has to beat whatever the page sets on the element under the pointer, so it goes
  // through a class on <html> plus an !important rule: an inherited cursor loses to any element
  // that declares its own, which is why the edge only reacted where the page did not.
  var cursorStyle = null;
  function ensureCursorStyle() {
    if (cursorStyle || !document.head) { return; }
    cursorStyle = document.createElement("style");
    cursorStyle.textContent =
      "html.tb-ns,html.tb-ns *{cursor:ns-resize !important}" +
      "html.tb-ew,html.tb-ew *{cursor:ew-resize !important}" +
      "html.tb-nwse,html.tb-nwse *{cursor:nwse-resize !important}" +
      "html.tb-nesw,html.tb-nesw *{cursor:nesw-resize !important}";
    document.head.appendChild(cursorStyle);
  }
  function setEdgeCursor(edge) {
    var root = document.documentElement;
    var rest = root.className.replace(/\btb-(ns|ew|nwse|nesw)\b/g, "").replace(/\s+/g, " ").trim();
    if (!edge) {
      root.className = rest;
      return;
    }
    ensureCursorStyle();
    var shape = (edge === "n" || edge === "s") ? "tb-ns" : (edge === "w" || edge === "e") ? "tb-ew"
              : (edge === "nw" || edge === "se") ? "tb-nwse" : "tb-nesw";
    root.className = (rest ? rest + " " : "") + shape;
  }
  document.addEventListener("mousemove", function (e) {
    setEdgeCursor(edgeAt(e.clientX, e.clientY));
  }, true);
  document.addEventListener("mouseleave", function () { setEdgeCursor(""); }, true);
  document.addEventListener("mousedown", function (e) {
    if (e.button !== 0 || e.altKey) { return; }
    var edge = edgeAt(e.clientX, e.clientY);
    if (!edge) { return; }
    e.preventDefault();
    e.stopPropagation();
    var startX = e.screenX, startY = e.screenY;
    post("D" + edge);
    function move(ev) {
      post("Z" + (ev.screenX - startX) + "," + (ev.screenY - startY));
    }
    function stop() {
      document.removeEventListener("mousemove", move, true);
      document.removeEventListener("mouseup", stop, true);
      post("U");
    }
    document.addEventListener("mousemove", move, true);
    document.addEventListener("mouseup", stop, true);
  }, true);
})();)JS";

std::wstring Trim(const std::wstring& text) {
  const size_t first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return std::wstring();
  }
  return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::wstring EncodeQuery(const std::wstring& text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
  std::string utf8(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], size, nullptr, nullptr);

  const wchar_t* hex = L"0123456789ABCDEF";
  std::wstring encoded;
  for (unsigned char byte : utf8) {
    if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
      encoded += (wchar_t)byte;
    } else {
      encoded += L'%';
      encoded += hex[byte >> 4];
      encoded += hex[byte & 0x0F];
    }
  }
  return encoded;
}

// Bar text to URL: scheme passthrough, http for local hosts, https when the first
// segment looks like a host, otherwise a web search.
std::wstring Normalize(const std::wstring& raw) {
  const std::wstring text = Trim(raw);
  if (text.empty()) {
    return text;
  }
  if (text.find(L"://") != std::wstring::npos || StartsWith(text, L"about:") || StartsWith(text, L"view-source:")) {
    return text;
  }

  const std::wstring host = text.substr(0, text.find(L'/'));
  const bool localHost = StartsWith(host, L"localhost") || StartsWith(host, L"127.0.0.1");
  const bool ipLike = !host.empty() && std::iswdigit(host[0]) && host.find(L'.') != std::wstring::npos;
  if (localHost || ipLike) {
    return L"http://" + text;
  }
  if (host.find(L'.') != std::wstring::npos) {
    return L"https://" + text;
  }
  return std::wstring(kSearchPrefix) + EncodeQuery(text);
}

std::wstring EscapeAttribute(const std::wstring& text) {
  std::wstring escaped;
  for (wchar_t ch : text) {
    switch (ch) {
      case L'&': escaped += L"&amp;"; break;
      case L'<': escaped += L"&lt;"; break;
      case L'>': escaped += L"&gt;"; break;
      case L'"': escaped += L"&quot;"; break;
      default: escaped += ch;
    }
  }
  return escaped;
}

std::wstring UserDataFolder() {
  wchar_t path[MAX_PATH] = {};
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::wstring();
  }
  std::wstring folder(path);
  folder += L"\\tinybrowser";
  CreateDirectoryW(folder.c_str(), nullptr);
  return folder;
}

// Ctrl+wheel zoom is a feature on a page and a nuisance on the bar: it would shrink the bar's
// own controls by hand, which is exactly what we just stopped the fit from doing.
void SetPageZoomControl(bool enabled) {
  if (!g_webview) {
    return;
  }
  ICoreWebView2Settings* settings = nullptr;
  if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
    settings->put_IsZoomControlEnabled(enabled ? TRUE : FALSE);
    settings->Release();
  }
}

void ShowBar() {
  if (!g_webview) {
    return;
  }
  std::wstring html(kBarTemplate);
  const size_t at = html.find(L"%URL%");
  if (at != std::wstring::npos) {
    html.replace(at, 5, EscapeAttribute(g_url));
  }
  // Nothing to go back to, reload or duplicate until a page has been loaded: those buttons say so
  // instead of doing nothing or, worse, reloading the bar itself into a blank window.
  const std::wstring disabled = g_url.empty() ? L" disabled" : L"";
  for (size_t slot = html.find(L"%DISABLED%"); slot != std::wstring::npos;
       slot = html.find(L"%DISABLED%", slot)) {
    html.replace(slot, 10, disabled);
    slot += disabled.size();  // every button has its own placeholder, not just the first
  }
  g_showingBar = true;
  ResetFit();
  if (g_controller) {
    g_controller->put_ZoomFactor(1.0);  // the bar page is sized for the raw window
  }
  SetPageZoomControl(false);
  g_webview->NavigateToString(html.c_str());
}

void Navigate(const std::wstring& target) {
  if (!g_webview) {
    return;
  }
  const std::wstring url = Normalize(target);
  if (url.empty()) {
    return;
  }
  g_url = url;
  g_showingBar = false;
  ResetFit();
  SetPageZoomControl(true);
  g_webview->Navigate(url.c_str());
}

void RunInPage(const wchar_t* script) {
  if (g_webview) {
    g_webview->ExecuteScript(script, nullptr);
  }
}

void DuplicateInNewInstance();  // defined with the placement helpers below

void HandleHotkey(UINT id) {
  if (id == kHotkeyBar) {
    ShowBar();
  } else if (id == kHotkeyQuit) {
    PostMessageW(g_window, WM_CLOSE, 0, 0);
  } else if (id == kHotkeyDuplicate) {
    DuplicateInNewInstance();
  } else if (id == kHotkeyBack) {
    if (!g_url.empty()) {
      RunInPage(L"history.back()");
    }
  }
}

// Global shortcuts through a low-level keyboard hook: RegisterHotKey registered fine but
// never fired for this window, and the hook is what every global-shortcut tool uses.
// Virtual-key codes for letters are the uppercase character values: 'S' = 0x53, 'X' = 0x58,
// 'D' = 0x44, 'Z' = 0x5A. Registering the lowercase values is accepted but never matches.
constexpr UINT kHotkeyMessage = WM_APP + 1;
constexpr UINT kHotkeyKeyS = L'S';
constexpr UINT kHotkeyKeyX = L'X';
constexpr UINT kHotkeyKeyD = L'D';
constexpr UINT kHotkeyKeyZ = L'Z';

struct HotkeyChord {
  UINT key;
  UINT id;
};
constexpr HotkeyChord kHotkeys[] = {
    {kHotkeyKeyS, kHotkeyBar},
    {kHotkeyKeyX, kHotkeyQuit},
    {kHotkeyKeyD, kHotkeyDuplicate},
    {kHotkeyKeyZ, kHotkeyBack},
};
constexpr int kHotkeyCount = 4;

bool g_ctrlDown = false;
bool g_altDown = false;

bool HotkeyChordPressed(UINT key) {
  // Locked here instead of through GetAsyncKeyState, which does not report the injected
  // modifier state reliably while the hook is running.
  if (!g_ctrlDown || !g_altDown) {
    return false;
  }
  for (const HotkeyChord& chord : kHotkeys) {
    if (chord.key == key) {
      return true;
    }
  }
  return false;
}

// With several windows on screen a shortcut has to mean the one you are using. With a single
// window it keeps working from anywhere, which is how it behaved before clones existed.
int g_instanceCount = 0;

bool HotkeysApplyHere() {
  const HWND front = GetForegroundWindow();
  if (front && (front == g_window || IsChild(g_window, front))) {
    return true;
  }
  g_instanceCount = 0;
  EnumWindows(
      [](HWND window, LPARAM) -> BOOL {
        wchar_t name[64] = {};
        if (GetClassNameW(window, name, 64) > 0 && std::wcscmp(name, kWindowClass) == 0) {
          if (++g_instanceCount > 1) {
            return FALSE;  // two is all we need to know
          }
        }
        return TRUE;
      },
      0);
  return g_instanceCount <= 1;
}

LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const bool pressed = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;

    switch (key->vkCode) {
      case VK_LCONTROL:
      case VK_RCONTROL:
      case VK_CONTROL:
        g_ctrlDown = pressed;
        break;
      case VK_LMENU:
      case VK_RMENU:
      case VK_MENU:
        g_altDown = pressed;
        break;
      default:
        break;
    }

    for (int i = 0; i < kHotkeyCount; ++i) {
      const HotkeyChord& chord = kHotkeys[i];
      if (key->vkCode != chord.key) {
        continue;
      }
      static bool down[kHotkeyCount] = {};
      if (!pressed) {
        down[i] = false;
      } else if (!down[i]) {  // ignore auto-repeat
        down[i] = true;
        if (HotkeyChordPressed(chord.key) && HotkeysApplyHere()) {
          // Only post from here: the hook must return fast or Windows drops it.
          PostMessageW(g_window, kHotkeyMessage, chord.id, 0);
          return 1;
        }
      }
      break;
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

void MoveBy(long dx, long dy) {
  if ((dx == 0 && dy == 0) || !g_window) {
    return;
  }
  RECT rect = {};
  if (!GetWindowRect(g_window, &rect)) {
    return;
  }
  SetWindowPos(g_window, nullptr, rect.left + (int)dx, rect.top + (int)dy, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void OnBarCommand(const std::wstring& message) {
  if (message.empty()) {
    return;
  }
  switch (message[0]) {
    case L'G': Navigate(message.substr(1)); break;
    case L'B': if (!g_url.empty()) RunInPage(L"history.back()"); break;
    case L'F': if (!g_url.empty()) RunInPage(L"history.forward()"); break;
    // While the bar is on screen the current document is the bar itself, so "reload" has to mean
    // the page we remember; reloading the bar would leave a blank window and no bar at all.
    case L'R': if (!g_url.empty()) Navigate(g_url); break;
    case L'C': DuplicateInNewInstance(); break;
    case L'X': PostMessageW(g_window, WM_CLOSE, 0, 0); break;
    case L'S': ShowBar(); break;
    case L'M': {
      const wchar_t* rest = message.c_str() + 1;
      wchar_t* end = nullptr;
      const long dx = std::wcstol(rest, &end, 10);
      const long dy = (end && *end == L',') ? std::wcstol(end + 1, nullptr, 10) : 0;
      MoveBy(dx, dy);
      break;
    }
    case L'D': {  // start of a resize: the edge being dragged, and the rect we start from
      const std::wstring edge = message.substr(1, 2);
      g_resizeEdge[0] = edge.empty() ? 0 : edge[0];
      g_resizeEdge[1] = edge.size() > 1 ? edge[1] : 0;
      g_resizeEdge[2] = 0;
      GetWindowRect(g_window, &g_resizeAnchor);
      break;
    }
    case L'U':  // end of the resize
      g_resizeEdge[0] = 0;
      break;
    case L'Z': {  // Z + dx,dy: how far the pointer moved since the drag started
      if (!g_resizeEdge[0]) {
        break;
      }
      const wchar_t* rest = message.c_str() + 1;
      wchar_t* end = nullptr;
      const long dx = std::wcstol(rest, &end, 10);
      const long dy = (end && *end == L',') ? std::wcstol(end + 1, nullptr, 10) : 0;
      int left = g_resizeAnchor.left;
      int top = g_resizeAnchor.top;
      int right = g_resizeAnchor.right;
      int bottom = g_resizeAnchor.bottom;
      if (std::wcschr(g_resizeEdge, L'e')) right += (int)dx;
      if (std::wcschr(g_resizeEdge, L'w')) left += (int)dx;
      if (std::wcschr(g_resizeEdge, L's')) bottom += (int)dy;
      if (std::wcschr(g_resizeEdge, L'n')) top += (int)dy;
      int width = right - left;
      int height = bottom - top;
      // At the minimum the dragged edge stops while the opposite one stays where it was.
      if (width < kMinWidth) {
        width = kMinWidth;
        if (std::wcschr(g_resizeEdge, L'w')) {
          left = right - width;
        }
      }
      if (height < kMinHeight) {
        height = kMinHeight;
        if (std::wcschr(g_resizeEdge, L'n')) {
          top = bottom - height;
        }
      }
      SetWindowPos(g_window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
      break;
    }
    default: break;
  }
}

// A 160x90 window shows the corner of a desktop-layout page, so every navigated page
// is scaled down until its whole width fits. scrollWidth is in CSS pixels, which do not
// depend on the current zoom, so measuring it settles after one pass; pages that widen
// as they load need a couple more, hence the repeated passes and the widest-seen value.
constexpr int kFitPasses = 3;
constexpr UINT kFitDelayFirst = 250;
constexpr UINT kFitDelayNext = 1200;

void ApplyFitZoom() {
  if (g_showingBar || g_widestSeen <= kWidth || !g_controller) {
    return;
  }
  double zoom = (double)kWidth / (double)g_widestSeen;
  int ppem = 0;
  int fontHundredths = 0;
  if (kCrispText && g_bodyFont > 1.0) {
    // The largest whole number of physical pixels the body font can occupy and still fit.
    fontHundredths = (int)(g_bodyFont * 100.0 + 0.5);
    ppem = (int)((double)kWidth * g_bodyFont / (double)g_widestSeen);
    if (ppem >= 1) {
      zoom = (double)ppem / g_bodyFont;
    } else {
      ppem = 0;
    }
  }
  zoom = zoom < kMinZoom ? kMinZoom : (zoom > kMaxZoom ? kMaxZoom : zoom);
  g_controller->put_ZoomFactor(zoom);
}

// Widest content that has to fit: the document scroll widths plus the right edge of every
// element that is not inside a horizontally scrollable box. Google's result column overflows
// a 160 px window while its own scrollWidth stays at the viewport width, and its wide filter
// strip lives in a scrolling container that must not force everything to shrink. Resolving the
// computed style of every ancestor is the expensive part, so an element that cannot beat the
// widest one seen so far is dropped by its rectangle first (measured on a 24k element page:
// 470 ms per pass down to 65 ms, same result).
void FitPageWidth() {
  if (!g_webview || g_showingBar) {
    return;
  }
  g_webview->ExecuteScript(
      L"(function () {"
      L" var d = document.documentElement, b = document.body;"
      L" var max = 0, all = document.querySelectorAll('*');"
      L" for (var i = 0; i < all.length; i++) {"
      L"   var el = all[i];"
      L"   var r = el.getBoundingClientRect();"
      L"   if (r.right <= max) { continue; }"  // cheaper than resolving styles for nothing
      L"   var blocked = false;"
      L"   for (var p = el.parentElement; p; p = p.parentElement) {"
      L"     var s = getComputedStyle(p);"
      L"     if ((s.overflowX === 'auto' || s.overflowX === 'scroll') && p.scrollWidth > p.clientWidth + 1) {"
      L"       blocked = true; break;"
      L"     }"
      L"   }"
      L"   if (blocked) { continue; }"
      L"   if (r.right > max) { max = r.right; }"
      L" }"
      L" return (d ? d.scrollWidth : 0) + ',' + (b ? b.scrollWidth : 0) + ',' + Math.round(max) + ','"
      L"   + (b ? Math.round((parseFloat(getComputedStyle(b).fontSize) || 0) * 100) : 0); })()",
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [](HRESULT result, LPCWSTR json) -> HRESULT {
            if (FAILED(result) || !json) {
              return S_OK;
            }
            // The callback wraps a returned string in quotes: "145,145,418"
            const wchar_t* start = *json == L'"' ? json + 1 : json;
            wchar_t* end = nullptr;
            const int scroll = (int)std::wcstol(start, &end, 10);
            const int body = (end && *end == L',') ? (int)std::wcstol(end + 1, &end, 10) : 0;
            const int right = (end && *end == L',') ? (int)std::wcstol(end + 1, &end, 10) : 0;
            const int fontHundredths = (end && *end == L',') ? (int)std::wcstol(end + 1, nullptr, 10) : 0;
            if (fontHundredths > 100) {  // the page states its body font in hundredths of a px
              g_bodyFont = fontHundredths / 100.0;
            }

            int width = scroll;
            if (body > width) {
              width = body;
            }
            if (right > width) {
              width = right;
            }
            if (width > g_widestSeen) {
              g_widestSeen = width;
              ApplyFitZoom();
            }
            return S_OK;
          })
          .Get());
}

void ResetFit() {
  g_fitPass = 0;
  g_widestSeen = 0;
  g_bodyFont = 0.0;
}

void PublishWindowSize();  // defined below, next to FitAndPlace

void StartFit() {
  // The bar is laid out for the raw window: fitting it would scale its own controls along with
  // the name of the field, which is not what a browser chrome should do when it grows.
  if (g_showingBar) {
    return;
  }
  ResetFit();
  PublishWindowSize();  // the page needs it: its own idea of the window is the layout width
  SetTimer(g_window, kFitTimer, kFitDelayFirst, nullptr);
}
void OnFitTimer() {
  FitPageWidth();
  if (++g_fitPass < kFitPasses) {
    SetTimer(g_window, kFitTimer, kFitDelayNext, nullptr);
  }
}

// Bottom right of the desktop, above the taskbar: the work area is what the shell leaves free.
void DefaultPlacement(int* x, int* y) {
  RECT work = {};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  *x = work.right - kWidth - kMargin;
  *y = work.bottom - kHeight - kMargin;
}

void FitAndPlace() {
  if (g_hasPlacedRect) {
    SetWindowPos(g_window, nullptr, g_placedRect.left, g_placedRect.top,
                 g_placedRect.right - g_placedRect.left, g_placedRect.bottom - g_placedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  int x = 0;
  int y = 0;
  DefaultPlacement(&x, &y);
  SetWindowPos(g_window, nullptr, x, y, kWidth, kHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

// A duplicate opens the same page beside this one: same size, shifted a bit so it is visible.
void DuplicateInNewInstance() {
  if (!g_window || g_url.empty()) {
    return;
  }
  wchar_t exe[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
    return;
  }
  RECT rect = {};
  if (!GetWindowRect(g_window, &rect)) {
    return;
  }
  RECT work = {};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work.left = 0;
    work.top = 0;
  }
  int x = rect.left - kMargin;
  int y = rect.top - kMargin;
  if (x < work.left) {
    x = work.left;
  }
  if (y < work.top) {
    y = work.top;
  }
  wchar_t arguments[900] = {};
  wsprintfW(arguments, L"--place %d %d %d %d \"%s\"", x, y, rect.right - rect.left,
            rect.bottom - rect.top, g_url.c_str());
  ShellExecuteW(nullptr, L"open", exe, arguments, nullptr, SW_SHOWNORMAL);
}

// The page cannot measure the window on its own (the fitted layout is 746 CSS px wide inside 160
// physical), so it is told the size it is living in, and the edge detection uses that.
void PublishWindowSize() {
  if (!g_webview) {
    return;
  }
  RECT client = {};
  GetClientRect(g_window, &client);
  wchar_t script[96] = {};
  wsprintfW(script, L"window.__tinybrowserSize=[%d,%d];", (int)(client.right - client.left),
            (int)(client.bottom - client.top));
  g_webview->ExecuteScript(script, nullptr);
}

void ReportMissingRuntime() {
  const std::wstring bootstrapper = std::wstring(L".\\") + kBootstrapperName;
  const bool hasBootstrapper = GetFileAttributesW(bootstrapper.c_str()) != INVALID_FILE_ATTRIBUTES;

  // The window is useless without the runtime, so ask before doing anything else.
  const int answer = MessageBoxW(
      nullptr,
      L"Falta el runtime de Microsoft Edge WebView2.\n\n"
      L"¿Quieres instalarlo ahora?",
      kTitle, MB_ICONWARNING | MB_YESNO);

  if (answer != IDYES) {
    return;
  }
  if (hasBootstrapper) {
    ShellExecuteW(nullptr, L"open", bootstrapper.c_str(), L"/silent", nullptr, SW_SHOWNORMAL);
  } else {
    ShellExecuteW(nullptr, L"open", kRuntimeUrl, nullptr, nullptr, SW_SHOWNORMAL);
  }
}

// A startup that fails has to say what failed and where: on the spot, since there is no log
// file to send anyone to.
void ReportFailure(const wchar_t* what, HRESULT result) {
  wchar_t text[256] = {};
  wsprintfW(text, L"%s fallo (0x%08X).\n\ntinybrowser no pudo iniciar el motor de WebView2.",
            what, (unsigned)result);
  MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONERROR);
}

void CreateWebView() {
  const std::wstring folder = UserDataFolder();
  if (kCrispText) {
    // Set before the engine starts: the browser process inherits it and reads the arguments.
    SetEnvironmentVariableW(kCrispArgsVar, kCrispArgs);
  }
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, folder.empty() ? nullptr : folder.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                ReportMissingRuntime();  // only this code means the runtime really is absent
              } else {
                ReportFailure(L"CreateCoreWebView2EnvironmentWithOptions", result);
              }
              PostMessageW(g_window, WM_CLOSE, 0, 0);
              return S_OK;
            }
            environment->CreateCoreWebView2Controller(
                g_window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(controllerResult) || !controller) {
                        ReportFailure(L"CreateCoreWebView2Controller", controllerResult);
                        PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                      }
                      g_controller = controller;
                      g_controller->get_CoreWebView2(&g_webview);

                      RECT bounds = {};
                      GetClientRect(g_window, &bounds);
                      g_controller->put_Bounds(bounds);
                      g_controller->put_IsVisible(TRUE);
                      // The engine has to be told, because the window's own focus notice arrives
                      // before the controller exists: without this the address field is drawn
                      // focused but the keystrokes go nowhere until you click the window.
                      g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                      PublishWindowSize();

                      EventRegistrationToken messageToken = {};
                      g_webview->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                LPWSTR raw = nullptr;
                                if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                  OnBarCommand(raw);
                                  CoTaskMemFree(raw);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &messageToken);

                      EventRegistrationToken blankToken = {};
                      g_webview->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                LPWSTR uri = nullptr;
                                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                  Navigate(uri);
                                  CoTaskMemFree(uri);
                                }
                                args->put_Handled(TRUE);
                                return S_OK;
                              })
                              .Get(),
                          &blankToken);

                      EventRegistrationToken navigatedToken = {};
                      g_webview->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                // What the page really is, taken from the engine instead of from
                                // our own calls, so a link click also updates it. The bar's own
                                // document is skipped: it would otherwise erase the page's URL.
                                if (!g_showingBar && sender) {
                                  LPWSTR source = nullptr;
                                  if (SUCCEEDED(sender->get_Source(&source)) && source) {
                                    g_url = source;
                                    CoTaskMemFree(source);
                                  }
                                }
                                // Pages keep widening as they load, so the fit is measured
                                // in a few passes instead of once.
                                StartFit();
                                return S_OK;
                              })
                              .Get(),
                          &navigatedToken);

                      g_webview->AddScriptToExecuteOnDocumentCreated(kPageScript, nullptr);
                      if (g_startTarget.empty()) {
                        ShowBar();
                      } else {
                        Navigate(g_startTarget);
                      }
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());

  if (FAILED(started)) {
    if (started == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
      ReportMissingRuntime();  // only this code means the runtime really is absent
    } else {
      ReportFailure(L"CreateCoreWebView2EnvironmentWithOptions", started);
    }
    PostMessageW(g_window, WM_CLOSE, 0, 0);
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE: {
      if (g_controller) {
        RECT bounds = {};
        GetClientRect(window, &bounds);
        g_controller->put_Bounds(bounds);
        // The page is fitted to the width, so a new width means a new fit: back to 1:1 and let
        // the measurement zoom in again only if the content is wider than the window now is.
        g_controller->put_ZoomFactor(1.0);
        StartFit();
        PublishWindowSize();
      }
      return 0;
    }
    case WM_TIMER:
      if (wparam == kFitTimer) {
        KillTimer(window, kFitTimer);
        OnFitTimer();
      }
      return 0;
    case kHotkeyMessage:
      HandleHotkey((UINT)wparam);
      return 0;
    case WM_SETFOCUS:
      if (g_controller) {
        g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
      return 0;
    case WM_DESTROY:
      g_webview.Reset();
      if (g_controller) {
        g_controller->Close();
        g_controller.Reset();
      }
      if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
      }
      PostQuitMessage(0);
      return 0;
  }
  // Anything not handled above, and WM_NCCALCSIZE when wparam is not TRUE, is default work.
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const std::wstring arguments = Trim(commandLine ? commandLine : L"");
  if (arguments.compare(0, 8, L"--place ") == 0) {
    // A duplicate asks for its own rectangle so the copy does not land on top of the original.
    const wchar_t* rest = arguments.c_str() + 8;
    wchar_t* end = nullptr;
    const long values[4] = {std::wcstol(rest, &end, 10),
                            end && *end == L' ' ? std::wcstol(end + 1, &end, 10) : 0,
                            end && *end == L' ' ? std::wcstol(end + 1, &end, 10) : 0,
                            end && *end == L' ' ? std::wcstol(end + 1, &end, 10) : 0};
    g_placedRect.left = (int)values[0];
    g_placedRect.top = (int)values[1];
    g_placedRect.right = (int)(values[0] + values[2]);
    g_placedRect.bottom = (int)(values[1] + values[3]);
    g_hasPlacedRect = values[2] > 0 && values[3] > 0;
    g_startTarget = Trim(end ? std::wstring(end) : std::wstring());
  } else {
    g_startTarget = arguments;
  }
  if (g_startTarget.size() >= 2 && g_startTarget.front() == L'"' && g_startTarget.back() == L'"') {
    g_startTarget = g_startTarget.substr(1, g_startTarget.size() - 2);
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    return 1;
  }

  const HICON iconBig = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  const HICON iconSmall = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kWindowClass;
  windowClass.hbrBackground = CreateSolidBrush(RGB(15, 20, 27));  // paints instantly, before the engine is up
  windowClass.hIcon = iconBig;
  windowClass.hIconSm = iconSmall;
  if (!RegisterClassExW(&windowClass)) {
    return 1;
  }

  int x = 0;
  int y = 0;
  int width = kWidth;
  int height = kHeight;
  if (g_hasPlacedRect) {
    x = g_placedRect.left;
    y = g_placedRect.top;
    width = g_placedRect.right - g_placedRect.left;
    height = g_placedRect.bottom - g_placedRect.top;
  } else {
    DefaultPlacement(&x, &y);
  }
  g_window = CreateWindowExW(
      WS_EX_TOPMOST, kWindowClass, kTitle, WS_POPUP,
      x, y, width, height,
      nullptr, nullptr, instance, nullptr);
  if (!g_window) {
    return 1;
  }

  // WM_SETICON as well as the class icons: the taskbar reads the message, the class icons
  // only cover the window itself.
  SendMessageW(g_window, WM_SETICON, ICON_BIG, (LPARAM)iconBig);
  SendMessageW(g_window, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);

  // The hook has to be installed on the thread that runs the message loop.
  g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboard, GetModuleHandleW(nullptr), 0);

  ShowWindow(g_window, showCommand);
  FitAndPlace();
  // Opened by hand, the app should be ready to type in: the window comes forward and keeps the
  // keyboard focus, and the engine is reminded of it once the controller exists.
  SetForegroundWindow(g_window);
  SetFocus(g_window);
  CreateWebView();

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (g_keyboardHook) {
    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = nullptr;
  }

  CoUninitialize();
  return 0;
}
