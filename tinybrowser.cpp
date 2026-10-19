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
#include <vector>
#include <wrl.h>

#include "WebView2.h"


using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int  kWidth = 160;      // page size: what a loaded page needs to be readable
constexpr int  kHeight = 90;
// The bar has its own size: at 141 px wide the magnifier button lands exactly on top of the close
// button (measured: 0.0 px apart), and at 77 px tall the brand line gets the same room above and
// below it. Navigating switches the window back to the page size and showing the bar switches it
// back here, so the layout the user asked for is the one the start page always has.
constexpr int  kBarWidth = 141;
constexpr int  kBarHeight = 77;
constexpr int  kMinWidth = 60;   // the window can be resized down to this and no further
constexpr int  kMinHeight = 60;
constexpr int  kMargin = 24;   // gap to the right and bottom edges of the desktop
constexpr UINT_PTR kFitTimer = 1;      // one-shot: measure the page after it settles
constexpr UINT_PTR kResizeTimer = 2;   // one-shot: re-fit once the resize drag settles
constexpr UINT kResizeSettle = 180;    // ms of quiet before that happens
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
constexpr UINT kHotkeyHide = 5;       // Ctrl+Alt+C
// CTRL+ALT+C is app-wide, so the instance that owns it posts one of these to every copy instead of
// toggling each window on its own: all of them follow the same command on the same press.
constexpr UINT kCommandHideAll = 10;
constexpr UINT kCommandShowAll = 11;
constexpr UINT kCommandSettings = 12;  // a switch changed in another copy

// The two switches are kept in a file next to the exe so every copy starts with the same answers.
constexpr wchar_t kSettingsFile[] = L"tinybrowser.ini";
constexpr wchar_t kSettingsSection[] = L"options";

constexpr wchar_t kTitle[] = L"tinybrowser";
constexpr wchar_t kWindowClass[] = L"TinyBrowserWindow";
constexpr int kIconId = 1;  // resource id of the donut in tinybrowser.rc
constexpr wchar_t kSearchPrefix[] = L"https://www.google.com/search?q=";
constexpr wchar_t kRuntimeUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
constexpr wchar_t kBootstrapperName[] = L"MicrosoftEdgeWebview2Setup.exe";

HWND g_window = nullptr;
bool g_blockOverlap = true;   // option: a window never ends up on top of another one
bool g_pushOthers = false;    // option: the window being dragged moves its neighbours aside
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
// Resizing and moving the window are driven from HERE, never from the page. Over a scrollbar the
// engine hands the drag to the scrollbar and the page stops seeing the mouse entirely (measured:
// no movement arrived, and not even the hover cursor), so a page-side resize cannot cover that
// strip. A low level mouse hook sees the input before anything can take it -- the same trick the
// global shortcuts use -- and the page is left with nothing to do about window geometry.
std::wstring g_url;
std::wstring g_startTarget;  // URL given on the command line, if any
RECT g_placedRect = {};        // --place x y width height, when a duplicate asks for its spot
bool g_hasPlacedRect = false;
wchar_t g_resizeEdge[3] = {};  // edges being dragged ("", "n", "se", ...), empty when idle
POINT g_lastMove = {};         // pointer position of the previous step: a step is a step, so a
                               // window held back by a neighbour never stores up distance
bool g_dragActive = false;     // a hook-driven drag is in progress
bool g_dragMoves = false;      // ... and it moves the window instead of resizing it (Alt)
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
  #url{flex:1;min-width:0;height:20px;padding:0 4px;font-size:11px;color:#eaf2f8;background:#060a0f;border:1px solid #2b3644;border-radius:4px}
  #url:focus{outline:none;border-color:#4a90c2}
  button{flex:0 0 20px;height:20px;font-size:11px;line-height:1;cursor:pointer;color:#9db2c6;background:#1a2330;border:1px solid #2b3644;border-radius:4px}
  button:hover{color:#fff;background:#253141}
  #go{display:inline-flex;align-items:center;justify-content:center}
  button:disabled{color:#46586a;background:#141c26;border-color:#1e2833;cursor:default}
  button:disabled:hover{color:#46586a;background:#141c26}
  kbd{color:#9db2c6;background:#1a2330;border-radius:3px;padding:0 3px;font:inherit}
  #brand{position:absolute;right:5px;bottom:4px;font-size:9px;letter-spacing:.3px;color:#33455a}
  #commands{position:absolute;left:3px;right:3px;top:3px;bottom:3px;z-index:5;padding:4px 6px;
    background:#0c1219;border:1px solid #2b3644;border-radius:4px;font-size:10px;line-height:1.25;
    color:#9db2c6;overflow:auto}
  #commands[hidden]{display:none}
  #commands .k{display:inline-block;min-width:58px;color:#7fb3d5}
  #closepanel{font-size:9px;color:#5b6b7d;margin-top:1px}
  #options{display:inline-flex;align-items:center;justify-content:center}
  #settings{position:absolute;left:3px;right:3px;top:3px;bottom:3px;z-index:6;padding:4px 6px;
    background:#0c1219;border:1px solid #2b3644;border-radius:4px;font-size:10px;line-height:1.3;
    color:#9db2c6;overflow:auto}
  #settings[hidden]{display:none}
  #settings .title{color:#7fb3d5;margin-bottom:2px}
  #settings label{display:flex;align-items:center;gap:4px;cursor:pointer}
  #settings input{flex:0 0 auto;width:11px;height:11px;margin:0;accent-color:#4a90c2;cursor:pointer}
  #settings input:disabled{cursor:default}
  #closepanel2{font-size:9px;color:#5b6b7d;margin-top:1px}
</style></head><body>
<div id="grip" title="drag"></div>
<div class="row"><input id="url" type="text" spellcheck="false" autocomplete="off" placeholder="url o busqueda" value="%URL%"><button id="go" title="Go"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="6.8" cy="6.8" r="5"/><path d="M10.1 10.1 15 15"/></svg></button></div>
<div class="row"><button id="back" title="Back"%DISABLED%>&#8249;</button><button id="fwd" title="Forward"%DISABLED%>&#8250;</button><button id="reload" title="Reload"%DISABLED%>&#8635;</button><button id="options" title="Opciones"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M2 4h12M2 8h12M2 12h12"/><path d="M6 2v4M10 6v4M5 10v4"/></svg></button><button id="help" title="Comandos y atajos">?</button><button id="close" title="Close (Ctrl+Alt+X)">&#10005;</button></div>
<div id="brand">tinybrowser 2026</div>
<div id="commands" hidden>
<div><span class="k">Ctrl+Alt+S</span> barra</div>
<div><span class="k">Ctrl+Alt+D</span> duplicar</div>
<div><span class="k">Ctrl+Alt+Z</span> atr&aacute;s</div>
<div><span class="k">Ctrl+Alt+C</span> ocultar</div>
<div><span class="k">Ctrl+Alt+X</span> cerrar</div>
<div><span class="k">Alt+arrastrar</span> mover</div>
<div id="closepanel">clic para cerrar</div>
</div>
<div id="settings" hidden>
<div class="title">Opciones</div>
<label><input type="checkbox" id="nooverlap"%CHECKED_OVERLAP%><span>No solapar ventanas</span></label>
<label><input type="checkbox" id="push"%CHECKED_PUSH%%PUSH_DISABLED%><span>Al arrastrar, empujar otras</span></label>
<div id="closepanel2">clic para cerrar</div>
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
  document.getElementById("close").onclick = function () { send("X"); };

  // Both panels cover the whole bar, so they fit at any window size, and only one is open at once.
  var commands = document.getElementById("commands");
  var settings = document.getElementById("settings");
  document.getElementById("help").onclick = function () {
    settings.hidden = true;
    commands.hidden = !commands.hidden;
  };
  commands.onclick = function () { commands.hidden = true; };
  document.getElementById("options").onclick = function () {
    commands.hidden = true;
    settings.hidden = !settings.hidden;
  };
  settings.onclick = function (e) {
    // Only the empty area closes it: a click on a switch or its label must reach the checkbox.
    if (e.target === settings) { settings.hidden = true; }
  };
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") { commands.hidden = true; settings.hidden = true; }
  }, true);

  // The switches belong to the host: the page only reports the new value. Pushing needs the rule
  // above it to mean anything, so it stays disabled in its place while that one is off.
  var noOverlap = document.getElementById("nooverlap");
  var pushOthers = document.getElementById("push");
  noOverlap.onchange = function () {
    send("N" + (noOverlap.checked ? "1" : "0"));
    pushOthers.disabled = !noOverlap.checked;
  };
  pushOthers.onchange = function () { send("P" + (pushOthers.checked ? "1" : "0")); };

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
// Document-start script for every page: keep the bar reachable, and stop target=_blank from
// asking for a popup window the host does not have. Window geometry is the HOST's job, not the
// page's: over a scrollbar the engine stops sending the page any mouse at all, so a page-side
// resize could never cover that strip (see the mouse hook above).
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
  // The cursor on the window's edges belongs to the page, because CSS is the one thing the engine
  // obeys: the host cannot set it (the engine's own cursor handling wins), which was measured. The
  // host still owns the drag itself -- over a scrollbar the page never sees the mouse.
  var cursorStyle = null;
  function ensureCursorStyle() {
    if (cursorStyle || !document.head) { return; }
    cursorStyle = document.createElement("style");
    // The scrollbar is drawn by the engine itself, so its cursor can only be chosen here: over one
    // of those strips the page is never told where the pointer is, and the resize cursor used to
    // vanish exactly there.
    cursorStyle.textContent =
      "html.tb-ns,html.tb-ns *,html.tb-ns::-webkit-scrollbar,html.tb-ns ::-webkit-scrollbar{cursor:ns-resize !important}" +
      "html.tb-ew,html.tb-ew *,html.tb-ew::-webkit-scrollbar,html.tb-ew ::-webkit-scrollbar{cursor:ew-resize !important}" +
      "html.tb-nwse,html.tb-nwse *,html.tb-nwse::-webkit-scrollbar,html.tb-nwse ::-webkit-scrollbar{cursor:nwse-resize !important}" +
      "html.tb-nesw,html.tb-nesw *,html.tb-nesw::-webkit-scrollbar,html.tb-nesw ::-webkit-scrollbar{cursor:nesw-resize !important}";
    document.head.appendChild(cursorStyle);
  }
  function setEdgeClass(edge) {
    var root = document.documentElement;
    var rest = root.className.replace(/\btb-(ns|ew|nwse|nesw)\b/g, "").replace(/\s+/g, " ").trim();
    if (!edge) { root.className = rest; return; }
    ensureCursorStyle();
    var shape = (edge === "n" || edge === "s") ? "tb-ns" : (edge === "w" || edge === "e") ? "tb-ew"
              : (edge === "nw" || edge === "se") ? "tb-nwse" : "tb-nesw";
    root.className = (rest ? rest + " " : "") + shape;
  }
  // Only the host decides this. It sees the mouse everywhere -- including over a scrollbar, which the
  // page is never told about -- and it knows where the corners reach; the page's own version of that
  // sum could disagree and fight it.
  window.__tinybrowserSetEdge = setEdgeClass;
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

void ResizeIfCanonical(int width, int height);  // defined with the placement helpers below
void PublishEdgeCursor(int edges);  // tells the page which edge the pointer is on

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

// Single-use placeholders in the bar page: the value goes in once and the token disappears.
void ReplaceToken(std::wstring* html, const wchar_t* token, const wchar_t* value) {
  const size_t at = html->find(token);
  if (at == std::wstring::npos) {
    return;
  }
  html->replace(at, std::wcslen(token), value);
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
  ReplaceToken(&html, L"%CHECKED_OVERLAP%", g_blockOverlap ? L" checked" : L"");
  ReplaceToken(&html, L"%CHECKED_PUSH%", g_pushOthers ? L" checked" : L"");
  // Pushing without the rule above it makes no sense, so it is disabled in place, not hidden.
  ReplaceToken(&html, L"%PUSH_DISABLED%", g_blockOverlap ? L"" : L" disabled");
  g_showingBar = true;
  ResetFit();
  ResizeIfCanonical(kBarWidth, kBarHeight);
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
  ResizeIfCanonical(kWidth, kHeight);  // a page needs the room
  g_webview->Navigate(url.c_str());
}

void RunInPage(const wchar_t* script) {
  if (g_webview) {
    g_webview->ExecuteScript(script, nullptr);
  }
}

void DuplicateInNewInstance();  // defined with the placement helpers below
void LoadSettings();            // defined with the other instance-wide helpers below

// Ctrl+Alt+C is the only way in and out of hiding: a hidden window leaves the taskbar and the
// z-order, so nothing else on screen could bring it back.
void ToggleAllInstances();  // defined with the app-wide chord below

// Hiding and showing are commands rather than toggles: every copy of the app obeys the same one on
// the same press, so two of them can never drift into opposite states.
void HideInstance() {
  if (g_window && IsWindowVisible(g_window)) {
    ShowWindow(g_window, SW_HIDE);
  }
}

// Only one copy comes back holding the keyboard (`focus`); several windows activating at once
// would fight over the foreground and leave none of them ready to type.
void ShowInstance(bool focus) {
  if (!g_window) {
    return;
  }
  ShowWindow(g_window, SW_SHOWNA);
  SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (g_controller) {
    // The engine was painting a window nobody could see: handing it the bounds again is what makes
    // it draw on the way back instead of at the first resize.
    RECT bounds = {};
    GetClientRect(g_window, &bounds);
    g_controller->put_Bounds(bounds);
    g_controller->put_IsVisible(TRUE);
  }
  if (!focus) {
    return;
  }
  if (!SetForegroundWindow(g_window)) {
    // Windows only hands the foreground to the process that owns the current input, and the chord
    // was typed while another app had it. Borrowing that thread's input state is what gets the
    // window back in front, ready to type, instead of showing up behind the keyboard focus.
    const HWND front = GetForegroundWindow();
    const DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
    const DWORD ourThread = GetCurrentThreadId();
    if (frontThread && frontThread != ourThread && AttachThreadInput(frontThread, ourThread, TRUE)) {
      SetForegroundWindow(g_window);
      AttachThreadInput(frontThread, ourThread, FALSE);
    }
  }
  SetFocus(g_window);
  if (g_controller) {
    g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
}

void HandleHotkey(UINT id, LPARAM data) {
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
  } else if (id == kHotkeyHide) {
    ToggleAllInstances();
  } else if (id == kCommandHideAll) {
    HideInstance();
  } else if (id == kCommandShowAll) {
    ShowInstance(data != 0);
  } else if (id == kCommandSettings) {
    LoadSettings();  // another copy changed a switch: take its answer from the file
  }
}

// Global shortcuts through a low-level keyboard hook: RegisterHotKey registered fine but
// never fired for this window, and the hook is what every global-shortcut tool uses.
// Virtual-key codes for letters are the uppercase character values: 'S' = 0x53, 'X' = 0x58,
// 'D' = 0x44, 'Z' = 0x5A, 'C' = 0x43. Registering the lowercase values is accepted but never
// matches.
constexpr UINT kHotkeyMessage = WM_APP + 1;
constexpr UINT kHotkeyKeyS = L'S';
constexpr UINT kHotkeyKeyX = L'X';
constexpr UINT kHotkeyKeyD = L'D';
constexpr UINT kHotkeyKeyZ = L'Z';
constexpr UINT kHotkeyKeyC = L'C';

struct HotkeyChord {
  UINT key;
  UINT id;
};
constexpr HotkeyChord kHotkeys[] = {
    {kHotkeyKeyS, kHotkeyBar},
    {kHotkeyKeyX, kHotkeyQuit},
    {kHotkeyKeyD, kHotkeyDuplicate},
    {kHotkeyKeyZ, kHotkeyBack},
    {kHotkeyKeyC, kHotkeyHide},
};
constexpr int kHotkeyCount = 5;

// Only one copy of the app drives Ctrl+Alt+C. The named object is the arbiter: whoever creates it
// owns the chord, and a copy started later takes it over once the previous owner has exited.
constexpr wchar_t kChordOwner[] = L"TinyBrowserChordOwner";
HANDLE g_chordOwner = nullptr;

bool WeOwnTheChord() {
  if (g_chordOwner) {
    return true;
  }
  HANDLE created = CreateMutexW(nullptr, FALSE, kChordOwner);
  if (!created) {
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(created);
    return false;
  }
  g_chordOwner = created;
  return true;
}

// Every open copy, hidden ones included -- a hidden window is still a window, and it is exactly
// the one that has to come back. The visibility read comes from the same pass, before anything is
// touched, so the decision does not change halfway through hiding or showing them.
//
// No fixed size on purpose: with a ceiling the copies past it simply vanished from the layout, and
// the duplicate search went on to place windows on top of the ones it could not see.
std::vector<HWND> g_instances;
bool g_anyInstanceVisible = false;

// Ceiling for the drag/push snapshot only, which lives in fixed arrays. Past this many copies a drag
// stops respecting the ones it did not read; a screen this size cannot really hold that many.
constexpr int kMaxInstances = 64;

void CollectInstances() {
  g_instances.clear();
  g_anyInstanceVisible = false;
  EnumWindows(
      [](HWND window, LPARAM) -> BOOL {
        wchar_t name[64] = {};
        if (GetClassNameW(window, name, 64) <= 0 || std::wcscmp(name, kWindowClass) != 0) {
          return TRUE;
        }
        if (IsWindowVisible(window)) {
          g_anyInstanceVisible = true;
        }
        g_instances.push_back(window);
        return TRUE;
      },
      0);
}

// Ctrl+Alt+C acts on the whole app: one press hides every copy, the next brings every copy back.
// The direction comes from what is on screen at that moment -- any copy visible means hide, none
// visible means show -- so a mixed state repairs itself instead of each window flipping its own.
void ToggleAllInstances() {
  if (!g_window) {
    return;
  }
  CollectInstances();
  const UINT command = g_anyInstanceVisible ? kCommandHideAll : kCommandShowAll;
  for (HWND peer : g_instances) {
    const LPARAM focus = (command == kCommandShowAll && peer == g_window) ? 1 : 0;
    PostMessageW(peer, kHotkeyMessage, command, focus);
  }
}

// The file sits beside the exe. Writing it can fail (a read-only folder) and that is fine: the
// switches then simply last for the session.
void SettingsPath(wchar_t* path) {
  path[0] = 0;
  wchar_t exe[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
    return;
  }
  wchar_t* slash = std::wcsrchr(exe, L'\\');
  if (slash) {
    *(slash + 1) = 0;
  } else {
    exe[0] = 0;
  }
  wsprintfW(path, L"%s%s", exe, kSettingsFile);
}

void LoadSettings() {
  wchar_t path[MAX_PATH] = {};
  SettingsPath(path);
  if (!path[0]) {
    return;
  }
  g_blockOverlap = GetPrivateProfileIntW(kSettingsSection, L"NoOverlap", 1, path) != 0;
  g_pushOthers = GetPrivateProfileIntW(kSettingsSection, L"PushOthers", 0, path) != 0;
}

void SaveSettings() {
  wchar_t path[MAX_PATH] = {};
  SettingsPath(path);
  if (!path[0]) {
    return;
  }
  wchar_t value[8] = {};
  wsprintfW(value, L"%d", g_blockOverlap ? 1 : 0);
  WritePrivateProfileStringW(kSettingsSection, L"NoOverlap", value, path);
  wsprintfW(value, L"%d", g_pushOthers ? 1 : 0);
  WritePrivateProfileStringW(kSettingsSection, L"PushOthers", value, path);
}

// A switch is app-wide: the other copies pick it up from the file instead of keeping their own.
void ShareSettings() {
  CollectInstances();
  for (HWND peer : g_instances) {
    if (peer != g_window) {
      PostMessageW(peer, kHotkeyMessage, kCommandSettings, 0);
    }
  }
}

void SetBlockOverlap(bool on) {
  if (g_blockOverlap == on) {
    return;
  }
  g_blockOverlap = on;
  SaveSettings();
  ShareSettings();
}

void SetPushOthers(bool on) {
  if (g_pushOthers == on) {
    return;
  }
  g_pushOthers = on;
  SaveSettings();
  ShareSettings();
}

bool g_ctrlDown = false;
bool g_altDown = false;
// Auto-repeat guard per chord. It lives here, not inside the hook, because a window that starts with
// the keys already held has to be told about them (see SeedHotkeyState).
bool g_hotkeyDown[kHotkeyCount] = {};

// A window born while Ctrl+Alt (and maybe the letter) are already held never sees those keydowns:
// without this, the copy opened by the first Ctrl+Alt+D answers nothing until the user lets go and
// presses the chord again.
void SeedHotkeyState() {
  g_ctrlDown = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
  g_altDown = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
              (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
  for (int i = 0; i < kHotkeyCount; ++i) {
    g_hotkeyDown[i] = (GetAsyncKeyState((int)kHotkeys[i].key) & 0x8000) != 0;
  }
}

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
      if (!pressed) {
        g_hotkeyDown[i] = false;
      } else if (!g_hotkeyDown[i]) {  // ignore auto-repeat
        g_hotkeyDown[i] = true;
        if (!HotkeyChordPressed(chord.key)) {
          break;
        }
        if (chord.id == kHotkeyHide) {
          // App-wide, so it no longer depends on which copy is in front. The hook chain stops at
          // the first hook that swallows the key, so a copy that does not own the chord must let it
          // through: swallowing it here would hide the press from the owner and nothing would
          // happen at all.
          if (WeOwnTheChord()) {
            PostMessageW(g_window, kHotkeyMessage, kHotkeyHide, 0);
            return 1;
          }
          break;
        }
        // The per-window chords keep their rule, and a hidden window stays deaf to them: acting on
        // something nobody can see reads as a shortcut that does nothing.
        if (IsWindowVisible(g_window) && HotkeysApplyHere()) {
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



// How close to the window's own edge counts as grabbing it, in physical pixels. Six rather than
// four: on a window this small the point to resize from was needlessly hard to find with a mouse.
constexpr int kEdge = 6;
// How far a corner reaches, per side. The straight edges are grabbed along the whole window, so a
// narrow band costs nothing there; the corners are the handle that gets used most and used to be a
// few pixels, where a small drift towards one of the sides switched the pointer to the axis shape.
constexpr int kCorner = 14;


bool InsideWindow(POINT point) {
  RECT rect = {};
  if (!g_window || !GetWindowRect(g_window, &rect)) {
    return false;
  }
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

int EdgesAt(POINT point) {
  RECT rect = {};
  if (!InsideWindow(point) || !GetWindowRect(g_window, &rect)) {
    return 0;
  }
  const int fromLeft = point.x - rect.left;
  const int fromRight = rect.right - point.x;
  const int fromTop = point.y - rect.top;
  const int fromBottom = rect.bottom - point.y;
  // Inside a corner the diagonal is kept for the whole corner square, taking the nearest side of
  // each axis, so drifting a little towards one edge does not lose it.
  const bool nearSideX = fromLeft < kCorner || fromRight < kCorner;
  const bool nearSideY = fromTop < kCorner || fromBottom < kCorner;
  if (nearSideX && nearSideY) {
    return (fromTop <= fromBottom ? 1 : 2) | (fromLeft <= fromRight ? 4 : 8);
  }
  int edges = 0;
  if (fromTop < kEdge) edges |= 1;        // north
  if (fromBottom <= kEdge) edges |= 2;    // south
  if (fromLeft < kEdge) edges |= 4;       // west
  if (fromRight <= kEdge) edges |= 8;     // east
  return edges;
}

void EdgeChars(int edges) {
  g_resizeEdge[0] = 0;
  g_resizeEdge[1] = 0;
  g_resizeEdge[2] = 0;
  int at = 0;
  if (edges & 1) g_resizeEdge[at++] = L'n';
  if (edges & 2) g_resizeEdge[at++] = L's';
  if (edges & 4) g_resizeEdge[at++] = L'w';
  if (edges & 8) g_resizeEdge[at++] = L'e';
}


// The whole desktop, taskbar and any second monitor included. The work area is what the *default*
// placement uses to open above the taskbar; a move or a resize is not limited by it, because this
// window is topmost and covering the taskbar is a placement the user asks for.
void DesktopBounds(RECT* desktop) {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  desktop->left = left;
  desktop->top = top;
  desktop->right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  desktop->bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// Air each window keeps on its own sides: two of them therefore end up 4 px apart, and touching is
// only what is left when the screen has no room for the gap.
constexpr int kWindowMargin = 2;
constexpr int kWindowGap = kWindowMargin * 2;

// True when the rectangles share more than an edge, on both axes.
bool Overlaps(const RECT& a, const RECT& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool InsideDesktop(const RECT& rect) {
  RECT desktop = {};
  DesktopBounds(&desktop);
  return rect.left >= desktop.left && rect.top >= desktop.top && rect.right <= desktop.right &&
         rect.bottom <= desktop.bottom;
}

// The other copies, read once when a gesture starts: while one window is being dragged the others do
// not move on their own, so asking Windows again on every pixel would be pure overhead.
struct Neighbour {
  HWND window;
  RECT rect;
  bool wasOverlapping;  // already on top of us before the option existed: that one is never fought
};

Neighbour g_neighbours[kMaxInstances] = {};
int g_neighbourCount = 0;

void SnapshotNeighbours() {
  g_neighbourCount = 0;
  RECT self = {};
  if (!GetWindowRect(g_window, &self)) {
    return;
  }
  CollectInstances();
  for (HWND peer : g_instances) {
    if (g_neighbourCount >= kMaxInstances) {
      break;
    }
    if (peer == g_window || !IsWindowVisible(peer)) {
      continue;
    }
    RECT rect = {};
    if (!GetWindowRect(peer, &rect)) {
      continue;
    }
    Neighbour& neighbour = g_neighbours[g_neighbourCount++];
    neighbour.window = peer;
    neighbour.rect = rect;
    neighbour.wasOverlapping = Overlaps(rect, self);
  }
}

// Slides along what it runs into instead of stopping dead: the axis that reaches a neighbour stops at
// the gap while the other one keeps going, which is what lets a diagonal drag glide along the
// neighbour. `from` is always a free position, so only what lies ahead of the movement matters.
RECT ConstrainMove(const RECT& from, const RECT& to) {
  const int width = from.right - from.left;
  const int height = from.bottom - from.top;
  RECT result = to;

  if (to.left > from.left) {  // moving right
    for (int i = 0; i < g_neighbourCount; ++i) {
      const Neighbour& n = g_neighbours[i];
      if (n.wasOverlapping || n.rect.left < from.right) continue;  // behind us: not in the way
      if (n.rect.top >= to.bottom || n.rect.bottom <= to.top) continue;  // another row
      const int limit = n.rect.left - kWindowGap;
      if (result.right > limit) {
        result.right = limit;
        result.left = limit - width;
      }
    }
    if (result.left < from.left) {  // the gap leaves no room to advance at all
      result.left = from.left;
      result.right = from.right;
    }
  } else if (to.left < from.left) {  // moving left
    for (int i = 0; i < g_neighbourCount; ++i) {
      const Neighbour& n = g_neighbours[i];
      if (n.wasOverlapping || n.rect.right > from.left) continue;
      if (n.rect.top >= to.bottom || n.rect.bottom <= to.top) continue;
      const int limit = n.rect.right + kWindowGap;
      if (result.left < limit) {
        result.left = limit;
        result.right = limit + width;
      }
    }
    if (result.right > from.right) {
      result.left = from.left;
      result.right = from.right;
    }
  }

  if (to.top > from.top) {  // moving down
    for (int i = 0; i < g_neighbourCount; ++i) {
      const Neighbour& n = g_neighbours[i];
      if (n.wasOverlapping || n.rect.top < from.bottom) continue;
      if (n.rect.right <= result.left || n.rect.left >= result.right) continue;  // another column
      const int limit = n.rect.top - kWindowGap;
      if (result.bottom > limit) {
        result.bottom = limit;
        result.top = limit - height;
      }
    }
    if (result.top < from.top) {
      result.top = from.top;
      result.bottom = from.bottom;
    }
  } else if (to.top < from.top) {  // moving up
    for (int i = 0; i < g_neighbourCount; ++i) {
      const Neighbour& n = g_neighbours[i];
      if (n.wasOverlapping || n.rect.bottom > from.top) continue;
      if (n.rect.right <= result.left || n.rect.left >= result.right) continue;
      const int limit = n.rect.bottom + kWindowGap;
      if (result.top < limit) {
        result.top = limit;
        result.bottom = limit + height;
      }
    }
    if (result.bottom > from.bottom) {
      result.top = from.top;
      result.bottom = from.bottom;
    }
  }

  // Whatever the clamps did, never land on a neighbour that was clear before the gesture.
  for (int i = 0; i < g_neighbourCount; ++i) {
    if (!g_neighbours[i].wasOverlapping && Overlaps(result, g_neighbours[i].rect)) {
      return from;
    }
  }
  return result;
}

// A resize only stops the edges being dragged; the opposite ones belong to the anchor and stay put.
RECT ConstrainResize(const RECT& from, const RECT& to) {
  RECT result = to;
  const bool east = std::wcschr(g_resizeEdge, L'e') != nullptr;
  const bool west = std::wcschr(g_resizeEdge, L'w') != nullptr;
  const bool south = std::wcschr(g_resizeEdge, L's') != nullptr;
  const bool north = std::wcschr(g_resizeEdge, L'n') != nullptr;
  for (int i = 0; i < g_neighbourCount; ++i) {
    const Neighbour& n = g_neighbours[i];
    if (n.wasOverlapping) continue;
    const bool sameRows = n.rect.top < result.bottom && result.top < n.rect.bottom;
    const bool sameColumns = n.rect.left < result.right && result.left < n.rect.right;
    if (east && sameRows && n.rect.left >= from.right) {
      const int limit = n.rect.left - kWindowGap;
      if (result.right > limit) result.right = limit;
    }
    if (west && sameRows && n.rect.right <= from.left) {
      const int limit = n.rect.right + kWindowGap;
      if (result.left < limit) result.left = limit;
    }
    if (south && sameColumns && n.rect.top >= from.bottom) {
      const int limit = n.rect.top - kWindowGap;
      if (result.bottom > limit) result.bottom = limit;
    }
    if (north && sameColumns && n.rect.bottom <= from.top) {
      const int limit = n.rect.bottom + kWindowGap;
      if (result.top < limit) result.top = limit;
    }
  }
  if (result.right - result.left < kMinWidth || result.bottom - result.top < kMinHeight) {
    return from;
  }
  return result;
}

// Which way a window that gets hit should go, and how far. The direction does not come from the
// mouse: it comes from where the two windows are on screen -- the pushed one leaves by the side it
// is already on -- and the axis is the one that needs the least travel, so a corner contact is a
// nudge instead of a shove. Both are right angles, never diagonals.
struct Escape {
  bool horizontal;
  bool forward;  // right or down
  int travel;
};

Escape ChooseEscape(const RECT& rect, const RECT& mover, int forcedAxis) {
  const int overlapX = (rect.right < mover.right ? rect.right : mover.right) -
                       (rect.left > mover.left ? rect.left : mover.left);
  const int overlapY = (rect.bottom < mover.bottom ? rect.bottom : mover.bottom) -
                       (rect.top > mover.top ? rect.top : mover.top);
  Escape escape = {};
  escape.horizontal = forcedAxis >= 0 ? forcedAxis == 1 : overlapX <= overlapY;
  const int rectCentreX = (rect.left + rect.right) / 2;
  const int rectCentreY = (rect.top + rect.bottom) / 2;
  if (escape.horizontal) {
    const int moverCentreX = (mover.left + mover.right) / 2;
    escape.forward = rectCentreX != moverCentreX
                         ? rectCentreX > moverCentreX
                         : mover.right - rect.left <= rect.right - mover.left;
    escape.travel = escape.forward ? mover.right + kWindowGap - rect.left
                                   : rect.right - (mover.left - kWindowGap);
  } else {
    const int moverCentreY = (mover.top + mover.bottom) / 2;
    escape.forward = rectCentreY != moverCentreY
                         ? rectCentreY > moverCentreY
                         : mover.bottom - rect.top <= rect.bottom - mover.top;
    escape.travel = escape.forward ? mover.bottom + kWindowGap - rect.top
                                   : rect.bottom - (mover.top - kWindowGap);
  }
  return escape;
}

RECT AfterEscape(const RECT& rect, const Escape& escape) {
  RECT moved = rect;
  const int shift = escape.forward ? escape.travel : -escape.travel;
  if (escape.horizontal) {
    moved.left += shift;
    moved.right += shift;
  } else {
    moved.top += shift;
    moved.bottom += shift;
  }
  return moved;
}

// Moves the neighbours out of the way of a rect that has to stay clear, following the collision
// graph: every window it moves becomes a mover in turn, and each contact is resolved by its own
// pair of positions. Nothing is written until the whole chain leaves the destination clear, so a
// push that cannot finish changes nothing and the caller simply stops.
bool PushNeighbours(const RECT& target) {
  int index[kMaxInstances] = {};
  RECT pending[kMaxInstances] = {};
  int count = 0;
  for (int i = 0; i < g_neighbourCount; ++i) {
    if (g_neighbours[i].wasOverlapping) continue;
    index[count] = i;
    pending[count] = g_neighbours[i].rect;
    ++count;
  }

  RECT movers[kMaxInstances + 1] = {};
  int owner[kMaxInstances + 1] = {};
  int moverCount = 1;
  movers[0] = target;
  owner[0] = -1;  // the window being dragged belongs to nobody

  for (int m = 0; m < moverCount; ++m) {
    for (int i = 0; i < count; ++i) {
      if (owner[m] == i) continue;
      if (!Overlaps(pending[i], movers[m])) continue;
      const Escape shortest = ChooseEscape(pending[i], movers[m], -1);
      RECT moved = AfterEscape(pending[i], shortest);
      if (!InsideDesktop(moved)) {
        // The shortest way out has no room here; the other axis may still have some.
        moved = AfterEscape(pending[i], ChooseEscape(pending[i], movers[m], shortest.horizontal ? 0 : 1));
        if (!InsideDesktop(moved)) {
          return false;
        }
      }
      pending[i] = moved;
      if (moverCount <= kMaxInstances) {
        movers[moverCount] = moved;
        owner[moverCount] = i;
        ++moverCount;
      }
    }
  }

  // The destination must end up clear of every one of them, and none of them may land on another.
  for (int i = 0; i < count; ++i) {
    if (Overlaps(target, pending[i])) {
      return false;
    }
    for (int j = i + 1; j < count; ++j) {
      if (Overlaps(pending[i], pending[j])) {
        return false;
      }
    }
  }

  for (int i = 0; i < count; ++i) {
    Neighbour& neighbour = g_neighbours[index[i]];
    if (SameRect(neighbour.rect, pending[i])) continue;
    neighbour.rect = pending[i];  // the snapshot follows, so the next step starts from the truth
    SetWindowPos(neighbour.window, nullptr, pending[i].left, pending[i].top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return true;
}

// Where a moved window really ends up: as asked when the rule is off, stopping at the gap when it is
// on, and shoving the neighbour aside when pushing is on and the screen leaves room for it.
RECT StepMove(const RECT& from, const RECT& to) {
  if (!g_blockOverlap) {
    return to;
  }
  const RECT allowed = ConstrainMove(from, to);
  if (SameRect(allowed, to) || !g_pushOthers) {
    return allowed;
  }
  return PushNeighbours(to) ? to : allowed;
}

RECT StepResize(const RECT& from, const RECT& to) {
  if (!g_blockOverlap) {
    return to;
  }
  const RECT allowed = ConstrainResize(from, to);
  if (SameRect(allowed, to) || !g_pushOthers) {
    return allowed;
  }
  return PushNeighbours(to) ? to : allowed;
}

// Keeps a moved or dragged rectangle inside the desktop: past the edge it is invisible, which read
// as "the resize does nothing", and a frameless window has no title bar to pull back.
void ClampToDesktop(int* left, int* top, int* right, int* bottom) {
  RECT desktop = {};
  DesktopBounds(&desktop);
  const int width = *right - *left;
  const int height = *bottom - *top;
  if (*right > desktop.right) {
    *right = desktop.right;
    *left = *right - width;
  }
  if (*bottom > desktop.bottom) {
    *bottom = desktop.bottom;
    *top = *bottom - height;
  }
  if (*left < desktop.left) {
    *left = desktop.left;
    *right = *left + width;
  }
  if (*top < desktop.top) {
    *top = desktop.top;
    *bottom = *top + height;
  }
}

void ResizeTo(POINT point) {
  RECT from = {};
  if (!GetWindowRect(g_window, &from)) {
    return;
  }
  const int dx = point.x - g_lastMove.x;
  const int dy = point.y - g_lastMove.y;
  g_lastMove = point;
  int left = from.left;
  int top = from.top;
  int right = from.right;
  int bottom = from.bottom;
  if (std::wcschr(g_resizeEdge, L'e')) right += dx;
  if (std::wcschr(g_resizeEdge, L'w')) left += dx;
  if (std::wcschr(g_resizeEdge, L's')) bottom += dy;
  if (std::wcschr(g_resizeEdge, L'n')) top += dy;
  // A dragged edge stops at the desktop edge and the opposite edge does not move: growing past the
  // screen showed no change on screen, which read as the resize doing nothing. The taskbar is not a
  // limit -- the window is topmost, so the strip over it is visible.
  RECT desktop = {};
  DesktopBounds(&desktop);
  if (std::wcschr(g_resizeEdge, L'e') && right > desktop.right) right = desktop.right;
  if (std::wcschr(g_resizeEdge, L's') && bottom > desktop.bottom) bottom = desktop.bottom;
  if (std::wcschr(g_resizeEdge, L'w') && left < desktop.left) left = desktop.left;
  if (std::wcschr(g_resizeEdge, L'n') && top < desktop.top) top = desktop.top;
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
  const RECT to = {left, top, left + width, top + height};
  const RECT target = StepResize(from, to);
  SetWindowPos(g_window, nullptr, target.left, target.top, target.right - target.left,
               target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

// One pointer delta per step. Anchoring the rectangle to the press would hand the whole distance
// travelled while the window was held back to a single step the moment it came free, and everything
// it pushed along the way jumped that far at once -- the "teleport" seen near a corner.
void MoveTo(POINT point) {
  RECT from = {};
  if (!GetWindowRect(g_window, &from)) {
    return;
  }
  const int stepX = point.x - g_lastMove.x;
  const int stepY = point.y - g_lastMove.y;
  g_lastMove = point;
  int left = from.left + stepX;
  int top = from.top + stepY;
  int right = left + (from.right - from.left);
  int bottom = top + (from.bottom - from.top);
  ClampToDesktop(&left, &top, &right, &bottom);
  const RECT to = {left, top, right, bottom};
  const RECT target = StepMove(from, to);
  SetWindowPos(g_window, nullptr, target.left, target.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The hook sees the mouse before the engine's scrollbars do, and swallowing an event here is what
// makes the strip behave like a window border instead of like a page: the press never reaches the
// scrollbar, and during a drag nothing else on the system reacts.
LRESULT CALLBACK LowLevelMouse(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_window) {
    // A hidden window must not eat anything: its rectangle is still there, so the press and hover
    // logic below would swallow clicks over a piece of desktop nobody can see, and a drag that was
    // in progress when it hid would keep moving it.
    if (!IsWindowVisible(g_window)) {
      g_dragActive = false;
      g_dragMoves = false;
      g_resizeEdge[0] = 0;
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    const POINT point = mouse->pt;
    if (wparam == WM_MOUSEMOVE) {
      if (g_dragActive) {
        if (g_dragMoves) {
          MoveTo(point);
        } else {
          ResizeTo(point);
        }
        // The move is passed on instead of being swallowed: swallowing it stopped the system from
        // moving the cursor at all, so the pointer froze at the press while the window chased
        // coordinates the user could not see. Only the press has to be swallowed (that is what
        // keeps the scrollbar from taking it); the moves belong to the system.
        return CallNextHookEx(nullptr, code, wparam, lparam);
      }
      // Hover moves are left alone so the page keeps its hover, but the edge is published to it:
      // over a scrollbar the page is never given the mouse, and that strip is exactly where the
      // resize cursor disappeared. Only on a change, so a move costs nothing.
      static int lastHoverEdge = 0;
      const int hover = EdgesAt(point);
      if (hover != lastHoverEdge) {
        lastHoverEdge = hover;
        PublishEdgeCursor(hover);
      }
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    if (wparam == WM_LBUTTONDOWN) {
      // Any press inside the window starts a gesture -- an edge, Alt anywhere, or the grip, which
      // the page reports as deltas -- so the other copies are read once, right here. With the rule
      // off there is nothing to compare against, so the enumeration is skipped entirely.
      if (g_blockOverlap && InsideWindow(point)) {
        SnapshotNeighbours();
      }
      const int edges = EdgesAt(point);
      // Alt moves the window from ANYWHERE in it -- that is what the README promises -- so with Alt
      // held the edges are not consulted at all. Treating an Alt press on an edge as a resize made
      // the window obey the wrong gesture and snap back when the size hit its minimum.
      // The keyboard is asked live: a key-up can be missed (a hook the system drops, a release that
      // never arrives) and a remembered flag would then stick, turning every press into a move and
      // leaving the window with no way to be resized.
      const bool altDown = GetAsyncKeyState(VK_LMENU) < 0 || GetAsyncKeyState(VK_RMENU) < 0;
      const bool moves = altDown && InsideWindow(point);
      if (!edges && !moves) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
      }
      g_lastMove = point;
      g_dragActive = true;
      g_dragMoves = moves;
      EdgeChars(moves ? 0 : edges);
      return 1;  // the scrollbar never sees this press
    }
    if (wparam == WM_LBUTTONUP && g_dragActive) {
      g_dragActive = false;
      g_dragMoves = false;
      g_resizeEdge[0] = 0;
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

// The bar's grip reports deltas from the page, so it lands here instead of in the hook.
void MoveBy(long dx, long dy) {
  if ((dx == 0 && dy == 0) || !g_window) {
    return;
  }
  RECT from = {};
  if (!GetWindowRect(g_window, &from)) {
    return;
  }
  int left = from.left + (int)dx;
  int top = from.top + (int)dy;
  int right = left + (from.right - from.left);
  int bottom = top + (from.bottom - from.top);
  ClampToDesktop(&left, &top, &right, &bottom);
  const RECT to = {left, top, right, bottom};
  const RECT target = StepMove(from, to);
  SetWindowPos(g_window, nullptr, target.left, target.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    // From the options panel: the switches themselves, and the note that they changed.
    case L'N': SetBlockOverlap(message.size() > 1 && message[1] == L'1'); break;
    case L'P': SetPushOthers(message.size() > 1 && message[1] == L'1'); break;
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
  RECT client = {};
  const int target = (g_window && GetClientRect(g_window, &client)) ? (int)(client.right - client.left)
                                                                   : kWidth;
  if (g_showingBar || g_widestSeen <= target || !g_controller) {
    return;
  }
  double zoom = (double)target / (double)g_widestSeen;
  int ppem = 0;
  int fontHundredths = 0;
  if (kCrispText && g_bodyFont > 1.0) {
    // The largest whole number of physical pixels the body font can occupy and still fit.
    fontHundredths = (int)(g_bodyFont * 100.0 + 0.5);
    ppem = (int)((double)target * g_bodyFont / (double)g_widestSeen);
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

// The page cannot measure the window on its own (the fitted layout is 746 CSS px wide inside 160
// physical), so it is told the size it is living in and uses that for the edge cursor.
// Tells the page which edge the pointer is on, so its CSS can show the resize cursor there. The
// page cannot work this out for itself over a scrollbar: the engine never gives it the mouse there.
void PublishEdgeCursor(int edges) {
  if (!g_webview || g_dragActive) {
    return;
  }
  const bool vertical = (edges & 1) || (edges & 2);
  const bool horizontal = (edges & 4) || (edges & 8);
  const wchar_t* name = L"";
  if (vertical && horizontal) {
    // The page names a diagonal by its two sides ("nw", "se"), not by the cursor they produce.
    const bool downRight = ((edges & 1) && (edges & 4)) || ((edges & 2) && (edges & 8));
    name = downRight ? L"nw" : L"ne";
  } else if (vertical) {
    name = (edges & 1) ? L"n" : L"s";
  } else if (horizontal) {
    name = (edges & 4) ? L"w" : L"e";
  }
  wchar_t script[96] = {};
  wsprintfW(script, L"window.__tinybrowserSetEdge&&window.__tinybrowserSetEdge('%ws');", name);
  g_webview->ExecuteScript(script, nullptr);
}


void ResetFit() {
  g_fitPass = 0;
  g_widestSeen = 0;
  g_bodyFont = 0.0;
}

void StartFit() {
  // The bar is laid out for the raw window: fitting it would scale its own controls along with
  // the name of the field, which is not what a browser chrome should do when it grows.
  if (g_showingBar) {
    return;
  }
  ResetFit();
  SetTimer(g_window, kFitTimer, kFitDelayFirst, nullptr);
}
void OnFitTimer() {
  FitPageWidth();
  if (++g_fitPass < kFitPasses) {
    SetTimer(g_window, kFitTimer, kFitDelayNext, nullptr);
  }
}

// Bottom right of the desktop, above the taskbar: the work area is what the shell leaves free.
void DefaultPlacement(int width, int height, int* x, int* y) {
  RECT work = {};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    work.right = GetSystemMetrics(SM_CXSCREEN);
    work.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  *x = work.right - width - kMargin;
  *y = work.bottom - height - kMargin;
}

// The window lives at one of two sizes: the bar's and the page's. A window the user resized by
// hand matches neither, and is left alone, so switching between the two never undoes a manual
// resize.
void ResizeIfCanonical(int width, int height) {
  if (!g_window) {
    return;
  }
  RECT rect = {};
  if (!GetWindowRect(g_window, &rect)) {
    return;
  }
  const int currentWidth = rect.right - rect.left;
  const int currentHeight = rect.bottom - rect.top;
  const bool canonical = (currentWidth == kBarWidth && currentHeight == kBarHeight) ||
                         (currentWidth == kWidth && currentHeight == kHeight);
  if (!canonical || (currentWidth == width && currentHeight == height)) {
    return;
  }
  // Keeps the switch on the desktop, not on the work area: a window the user placed over the
  // taskbar stays where he put it while it changes size.
  RECT desktop = {};
  DesktopBounds(&desktop);
  int left = rect.left;
  int top = rect.top;
  if (left + width > desktop.right) {
    left = desktop.right - width;
  }
  if (top + height > desktop.bottom) {
    top = desktop.bottom - height;
  }
  if (left < desktop.left) {
    left = desktop.left;
  }
  if (top < desktop.top) {
    top = desktop.top;
  }
  SetWindowPos(g_window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void FitAndPlace() {
  if (g_hasPlacedRect) {
    SetWindowPos(g_window, nullptr, g_placedRect.left, g_placedRect.top,
                 g_placedRect.right - g_placedRect.left, g_placedRect.bottom - g_placedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  // The window is created at the right size: this only re-states its corner.
  RECT rect = {};
  GetWindowRect(g_window, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  int x = 0;
  int y = 0;
  DefaultPlacement(width, height, &x, &y);
  SetWindowPos(g_window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Where the next duplicate goes. Each open copy reserves one window-sized "ghost" area on each of
// its four sides, plus the gap: an area that runs into another copy means that side has no room and
// is dropped,
// and when the areas of two different copies land on the same spot it means both have room to grow
// there -- the spot earns a point per extra area, so the hole in an L-shape wins over making a row
// longer. Ties go to the spot nearest the window being duplicated, then to the one that leaves the
// group closest to a whole rectangle, and the side order last, so the same layout always gives the
// same answer.
//
// Where the points run out (a plain row has no hole in it) distance alone would keep extending that
// row for ever, because every spot beside it is equally near: the rectangle test is what starts a
// second row instead. Simulated from one window, the nine duplicates that follow make 11, an L, a
// full 2x2, 2x3, 3x3 -- never a row of four.
struct Spot {
  RECT rect;
  int points;
  long long distance;  // squared gap to the window being duplicated, 0 when touching it
  long long balance;   // how far the group would be from a whole rectangle of windows
  long long growth;    // how much area the group would gain
  int side;            // 0 right, 1 down, 2 left, 3 up
};

constexpr int kSides = 4;

long long Area(const RECT& rect) {
  return (long long)(rect.right - rect.left) * (long long)(rect.bottom - rect.top);
}

// A duplicate is born on the screen, not under the taskbar: only the free area of the monitor the
// spot lands on counts as room.
RECT WorkAreaNear(const POINT& point) {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info)) {
    return info.rcWork;
  }
  RECT desktop = {};
  DesktopBounds(&desktop);
  return desktop;
}

long long GapTo(const RECT& a, const RECT& b) {
  const long long gapX = a.left > b.right ? a.left - b.right : (b.left > a.right ? b.left - a.right : 0);
  const long long gapY = a.top > b.bottom ? a.top - b.bottom : (b.top > a.bottom ? b.top - a.bottom : 0);
  return gapX * gapX + gapY * gapY;
}

// Points first (that is the hole), then the spot closest to the window being duplicated, then the
// one that leaves the group closest to a whole rectangle of windows, then the least added area,
// and the side order last so the same layout always gives the same window. Distance before balance
// matters: growing beside the window you duplicated is what the user asked for, and a row only
// keeps growing when no closer spot makes the group more of a rectangle.
bool PreferSpot(const Spot& a, const Spot& b) {
  if (a.points != b.points) return a.points > b.points;
  if (a.distance != b.distance) return a.distance < b.distance;
  if (a.balance != b.balance) return a.balance < b.balance;
  if (a.growth != b.growth) return a.growth < b.growth;
  return a.side < b.side;
}

// Walks every open copy, one candidate per side, and keeps the best one. `gap` is the air left
// around the new window; at 0 it may sit flush but never on top. False when the screen has no room
// left beside any copy.
bool FindDuplicateSpot(RECT* spot, int gap) {
  RECT source = {};
  if (!g_window || !GetWindowRect(g_window, &source)) {
    return false;
  }
  const int width = source.right - source.left;
  const int height = source.bottom - source.top;

  CollectInstances();
  std::vector<RECT> windows;
  windows.reserve(g_instances.size());
  RECT group = {};  // what the open copies cover between them
  for (HWND peer : g_instances) {
    RECT rect = {};
    if (!GetWindowRect(peer, &rect)) {
      continue;
    }
    windows.push_back(rect);
    if (windows.size() == 1) {
      group = rect;
      continue;
    }
    if (rect.left < group.left) group.left = rect.left;
    if (rect.top < group.top) group.top = rect.top;
    if (rect.right > group.right) group.right = rect.right;
    if (rect.bottom > group.bottom) group.bottom = rect.bottom;
  }
  if (windows.empty()) {
    return false;
  }

  const long long groupArea = Area(group);
  // One window plus its share of the gap is the grid cell the rectangle test counts in.
  const long long cellWidth = (long long)width + gap;
  const long long cellHeight = (long long)height + gap;
  std::vector<Spot> spots;
  spots.reserve(windows.size() * kSides);
  for (const RECT& peer : windows) {
    const RECT sides[kSides] = {
        {peer.right + gap, peer.top, peer.right + gap + width, peer.top + height},
        {peer.left, peer.bottom + gap, peer.left + width, peer.bottom + gap + height},
        {peer.left - gap - width, peer.top, peer.left - gap, peer.top + height},
        {peer.left, peer.top - gap - height, peer.left + width, peer.top - gap},
    };
    for (int side = 0; side < kSides; ++side) {
      const RECT& candidate = sides[side];
      const POINT middle = {(candidate.left + candidate.right) / 2,
                            (candidate.top + candidate.bottom) / 2};
      const RECT work = WorkAreaNear(middle);
      if (candidate.left < work.left || candidate.top < work.top ||
          candidate.right > work.right || candidate.bottom > work.bottom) {
        continue;  // off the screen: no space on that side
      }
      bool blocked = false;
      for (const RECT& other : windows) {
        // The reserve includes the gap on every side, so the new window never lands on another one
        // and never ends up closer than that either.
        const RECT reserved = {other.left - gap, other.top - gap, other.right + gap,
                               other.bottom + gap};
        if (Overlaps(candidate, reserved)) {
          blocked = true;  // the ghost area hit a window, so that side does not count
          break;
        }
      }
      if (blocked) {
        continue;
      }
      Spot entry = {};
      entry.rect = candidate;
      entry.side = side;
      RECT grown = {candidate.left < group.left ? candidate.left : group.left,
                    candidate.top < group.top ? candidate.top : group.top,
                    candidate.right > group.right ? candidate.right : group.right,
                    candidate.bottom > group.bottom ? candidate.bottom : group.bottom};
      entry.growth = Area(grown) - groupArea;
      const long long grownWidth = grown.right - grown.left + gap;
      const long long grownHeight = grown.bottom - grown.top + gap;
      // Zero when the bounding box is exactly N cells wide by M tall; the smaller the residue, the
      // closer the group is to a filled rectangle instead of a long row or column.
      entry.balance = grownWidth * cellHeight - grownHeight * cellWidth;
      if (entry.balance < 0) {
        entry.balance = -entry.balance;
      }
      entry.distance = GapTo(candidate, source);
      spots.push_back(entry);
    }
  }
  if (spots.empty()) {
    return false;
  }

  for (size_t i = 0; i < spots.size(); ++i) {
    for (size_t j = 0; j < spots.size(); ++j) {
      if (i != j && Overlaps(spots[i].rect, spots[j].rect)) {
        ++spots[i].points;
      }
    }
  }

  size_t best = 0;
  for (size_t i = 1; i < spots.size(); ++i) {
    if (PreferSpot(spots[i], spots[best])) {
      best = i;
    }
  }
  *spot = spots[best].rect;
  return true;
}

// A duplicate opens the same page as the window it came from, in the first gap the group layout
// leaves free and in that window's size.
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
  // The gap is air, not a rule: if the screen only allows a flush spot, that is still better than
  // nothing, and if no free spot exists at all nothing is opened -- a copy never covers another one.
  RECT spot = {};
  if (!FindDuplicateSpot(&spot, kWindowGap) && !FindDuplicateSpot(&spot, 0)) {
    return;
  }
  wchar_t arguments[900] = {};
  wsprintfW(arguments, L"--place %d %d %d %d \"%s\"", spot.left, spot.top,
            spot.right - spot.left, spot.bottom - spot.top, g_url.c_str());
  ShellExecuteW(nullptr, L"open", exe, arguments, nullptr, SW_SHOWNORMAL);
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
                      // The engine paints its own background before the content catches up, and by
                      // default that is white: growing the window showed a pale strip on the freshly
                      // exposed edge. Same colour as the window, so a resize looks even.
                      ICoreWebView2Controller2* controller2 = nullptr;
                      if (SUCCEEDED(g_controller->QueryInterface(__uuidof(ICoreWebView2Controller2),
                                                                 reinterpret_cast<void**>(&controller2))) &&
                          controller2) {
                        const COREWEBVIEW2_COLOR background = {255, 15, 20, 27};
                        controller2->put_DefaultBackgroundColor(background);
                        controller2->Release();
                      }
                      // The engine has to be told, because the window's own focus notice arrives
                      // before the controller exists: without this the address field is drawn
                      // focused but the keystrokes go nowhere until you click the window.
                      g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

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
        // Re-fitting on every step of a drag re-zooms and re-measures the page at each pixel, which
        // makes the resize crawl and re-rasterise constantly. Wait for the drag to settle instead:
        // until then the page keeps the zoom it already had.
        SetTimer(window, kResizeTimer, kResizeSettle, nullptr);
      }
      return 0;
    }
    case WM_TIMER:
      if (wparam == kFitTimer) {
        KillTimer(window, kFitTimer);
        OnFitTimer();
      } else if (wparam == kResizeTimer) {
        KillTimer(window, kResizeTimer);
        if (g_controller) {
          // No zoom reset here: the page is measured in CSS pixels, which do not depend on the
          // zoom factor, so a reset only made the content jump to 1:1 and back on every settle.
          StartFit();
        }
      }
      return 0;
    case kHotkeyMessage:
      HandleHotkey((UINT)wparam, lparam);
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
      if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
      }
      if (g_chordOwner) {
        CloseHandle(g_chordOwner);  // frees the name so another copy can take the chord over
        g_chordOwner = nullptr;
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
  LoadSettings();
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
  // The page is known from the command line, long before the engine exists. Remembering it here is
  // what lets Ctrl+Alt+D work in the first second of a new window instead of waiting for that page
  // to finish opening: the duplicate only needs to know which address to copy.
  if (!g_startTarget.empty()) {
    g_url = Normalize(g_startTarget);
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

  const bool startsOnPage = !g_startTarget.empty();
  int x = 0;
  int y = 0;
  int width = startsOnPage ? kWidth : kBarWidth;
  int height = startsOnPage ? kHeight : kBarHeight;
  if (g_hasPlacedRect) {
    x = g_placedRect.left;
    y = g_placedRect.top;
    width = g_placedRect.right - g_placedRect.left;
    height = g_placedRect.bottom - g_placedRect.top;
  } else {
    DefaultPlacement(width, height, &x, &y);
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

  // Both hooks have to be installed on the thread that runs the message loop.
  SeedHotkeyState();
  g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboard, GetModuleHandleW(nullptr), 0);
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, GetModuleHandleW(nullptr), 0);

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
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
  }

  CoUninitialize();
  return 0;
}
