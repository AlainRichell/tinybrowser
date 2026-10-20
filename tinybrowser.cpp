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
#include <commdlg.h>
#include <d3d11.h>
#include <dcomp.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <strsafe.h>
#include <algorithm>
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
constexpr UINT kHotkeyPick = 6;       // Ctrl+Alt+A
constexpr UINT kHotkeyState = 7;      // Ctrl+Alt+G
// CTRL+ALT+C is app-wide, so the instance that owns it posts one of these to every copy instead of
// toggling each window on its own: all of them follow the same command on the same press.
constexpr UINT kCommandHideAll = 10;
constexpr UINT kCommandShowAll = 11;
constexpr UINT kCommandSettings = 12;  // a switch changed in another copy
constexpr UINT kPickTimer = 8;         // keeps the pipette's bulb on the colour under the tip
constexpr UINT kCommandPick = 13;      // the eyedropper was clicked
constexpr UINT kCommandPickCancel = 14;  // ... or given up


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
bool g_picking = false;       // eyedropper armed, waiting for the click
// The colours this window is keying, in the order they were picked. An empty list is the plain
// window; picking a colour that is already here takes it out, so one can go without clearing the rest.
std::vector<COLORREF> g_keyColours;
// Each colour costs a handful of filter primitives and the script travels to the page as one string:
// past this many the filter stops being cheap, and a page never needs more.
constexpr size_t kKeyColourMax = 12;
HCURSOR g_pickCursor = nullptr;
COLORREF g_pickPreview = CLR_INVALID;  // colour shown in the pipette's bulb
POINT g_pickAt = {};          // where the eyedropper was clicked
bool g_blockOverlap = true;   // option: a window never ends up on top of another one
bool g_pushOthers = false;    // option: the window being dragged moves its neighbours aside
// How far around the picked colour the key still reaches, in 8-bit levels per channel. 0 keys only
// the exact value; the eyedropper reads a screen pixel that may differ by a level or two from what
// the page painted, so some margin is what makes it usable at all.
int g_keyRange = 5;
constexpr int kKeyRangeMax = 40;
// The margin a .to brought with it belongs to that snapshot, not to the app: it stays in this window
// and never reaches the options file the other copies read. Moving the slider is what makes the
// margin the shared one again.
bool g_keyRangeLocal = false;
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
// Composition hosting: the engine draws into this DirectComposition tree instead of a child HWND.
// It is the only mode where a page can have per-pixel alpha, which is what lets a colour be made
// transparent to the desktop. The price is that pointer input has to be forwarded by hand.
ComPtr<ICoreWebView2CompositionController> g_composition;
ComPtr<IDCompositionDevice> g_dcompDevice;
ComPtr<IDCompositionTarget> g_dcompTarget;
ComPtr<IDCompositionVisual> g_dcompRoot;
HCURSOR g_pageCursor = nullptr;  // what the page wants under the pointer (links, text fields)
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
bool g_setupPanel = false;     // one-shot: the next bar comes up with the setup panel open
int g_fitPass = 0;     // measurements taken for the current page
int g_widestSeen = 0;  // widest layout seen for the current page, in CSS pixels
double g_bodyFont = 0.0;  // body font size of the page, in CSS pixels
// Zoom a .to brought in. While it is set the fit keeps its hands off the page: a window restored
// from a snapshot has to keep the zoom it was saved with, not the one its width would imply.
int g_stateZoom = 0;  // thousandths of the factor, 0 when the page is fitted as usual

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
  #settings{position:absolute;left:3px;right:3px;top:3px;bottom:3px;z-index:6;padding:3px 5px;
    background:#0c1219;border:1px solid #2b3644;border-radius:4px;font-size:10px;line-height:1.1;
    color:#9db2c6;overflow:hidden}
  #settings[hidden]{display:none}
  #settings .title{color:#7fb3d5;margin-bottom:2px}
  #settings label{display:flex;align-items:center;gap:4px;margin:0;cursor:pointer}
  #settings input[type=checkbox]{flex:0 0 auto;width:11px;height:11px;margin:0;
    accent-color:#4a90c2;cursor:pointer}
  #settings input[type=checkbox]:disabled{cursor:default}
  #settings .range{display:flex;align-items:center;justify-content:space-between;cursor:default}
  #settings input[type=range]{display:block;width:100%;height:10px;margin:0;padding:0;
    -webkit-appearance:none;appearance:none;background:transparent;cursor:pointer}
  #settings input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;
    background:#2b3644}
  #settings input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;
    width:9px;height:9px;margin-top:-2.5px;border-radius:50%;background:#4a90c2}
  #settings .value{flex:0 0 12px;text-align:right;color:#7fb3d5}
  #closepanel2{font-size:9px;color:#5b6b7d;margin-top:1px}
  #setup{position:absolute;left:3px;right:3px;top:3px;bottom:3px;z-index:7;padding:3px 5px;
    background:#0c1219;border:1px solid #2b3644;border-radius:4px;font-size:10px;line-height:1.1;
    color:#9db2c6;overflow:hidden}
  #setup[hidden]{display:none}
  #setup .title{color:#7fb3d5;margin-bottom:3px}
  #setup button{flex:none;width:100%;height:18px;margin-bottom:3px;font-size:10px}
  #setup label{display:flex;align-items:center;gap:4px;margin:0;cursor:pointer}
  #setup input[type=checkbox]{flex:0 0 auto;width:11px;height:11px;margin:0;
    accent-color:#4a90c2;cursor:pointer}
  #setup input[type=checkbox]:disabled{cursor:default}
  #setup .note{color:#5b6b7d;margin-top:3px}
</style></head><body>
<div id="grip" title="drag"></div>
<div class="row"><input id="url" type="text" spellcheck="false" autocomplete="off" placeholder="url o busqueda" value="%URL%"><button id="go" title="Go"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="6.8" cy="6.8" r="5"/><path d="M10.1 10.1 15 15"/></svg></button></div>
<div class="row"><button id="back" title="Back"%DISABLED%>&#8249;</button><button id="fwd" title="Forward"%DISABLED%>&#8250;</button><button id="setupbtn" title="Instalar tinybrowser"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M8 2v6.5"/><path d="M5 5.8 8 8.8 11 5.8"/><path d="M2.5 11.5v2h11v-2"/></svg></button><button id="options" title="Opciones"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M2 4h12M2 8h12M2 12h12"/><path d="M6 2v4M10 6v4M5 10v4"/></svg></button><button id="help" title="Comandos y atajos">?</button><button id="close" title="Close (Ctrl+Alt+X)">&#10005;</button></div>
<div id="brand">tinybrowser 2026</div>
<div id="commands" hidden>
<div><span class="k">Ctrl+Alt+S</span> barra</div>
<div><span class="k">Ctrl+Alt+D</span> duplicar</div>
<div><span class="k">Ctrl+Alt+G</span> guardar / cargar</div>
<div><span class="k">Ctrl+Alt+Z</span> atr&aacute;s</div>
<div><span class="k">Ctrl+Alt+C</span> ocultar</div>
<div><span class="k">Ctrl+Alt+A</span> gotero</div>
<div><span class="k">Ctrl+Alt+X</span> cerrar</div>
<div><span class="k">Alt+arrastrar</span> mover</div>
<div id="closepanel">clic para cerrar</div>
</div>
)HTML" LR"HTML(
<div id="settings" hidden>
<div class="title">Opciones</div>
<label><input type="checkbox" id="nooverlap"%CHECKED_OVERLAP%><span>No solapar ventanas</span></label>
<label><input type="checkbox" id="push"%CHECKED_PUSH%%PUSH_DISABLED%><span>Al arrastrar, empujar otras</span></label>
<label class="range"><span>Rango del color</span><span class="value" id="keyrangevalue">%KEY_RANGE%</span></label>
<input type="range" id="keyrange" min="0" max="40" step="1" value="%KEY_RANGE%">
</div>
<div id="setup"%SETUP_HIDDEN%>
<div class="title">Instalaci&oacute;n</div>
<button id="setupaction" data-cmd="%SETUP_COMMAND%">%SETUP_ACTION%</button>
<label><input type="checkbox" id="desktopicon"%DESKTOP_CHECKED%%DESKTOP_DISABLED%><span>Icono en el escritorio</span></label>
<div class="note">%SETUP_STATUS%</div>
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
  document.getElementById("close").onclick = function () { send("X"); };

  // Both panels cover the whole bar, so they fit at any window size, and only one is open at once.
  var commands = document.getElementById("commands");
  var settings = document.getElementById("settings");
  document.getElementById("help").onclick = function () {
    settings.hidden = true;
    setupPanel.hidden = true;
    commands.hidden = !commands.hidden;
  };
  commands.onclick = function () { commands.hidden = true; };
  document.getElementById("options").onclick = function () {
    commands.hidden = true;
    setupPanel.hidden = true;
    settings.hidden = !settings.hidden;
  };
  settings.onclick = function (e) {
    // Only the empty area closes it: a click on a switch or its label must reach the checkbox.
    if (e.target === settings) { settings.hidden = true; }
  };
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") {
      commands.hidden = true;
      settings.hidden = true;
      setupPanel.hidden = true;
    }
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
  // The range only travels on release: dragging would otherwise rewrite the settings file and
  // re-filter every window on each pixel of the slider.
  var keyRange = document.getElementById("keyrange");
  var keyRangeValue = document.getElementById("keyrangevalue");
  keyRange.oninput = function () { keyRangeValue.textContent = keyRange.value; };
  keyRange.onchange = function () { send("T" + keyRange.value); };

  // The setup panel: the host does the work (copy the exe, the PATH, the .to association) and rebuilds
  // the bar afterwards, so every label here shows the state the machine is really in.
  var setupPanel = document.getElementById("setup");
  document.getElementById("setupbtn").onclick = function () {
    commands.hidden = true;
    settings.hidden = true;
    setupPanel.hidden = !setupPanel.hidden;
  };
  setupPanel.onclick = function (e) {
    if (e.target === setupPanel) { setupPanel.hidden = true; }
  };
  var setupAction = document.getElementById("setupaction");
  setupAction.onclick = function () { send(setupAction.getAttribute("data-cmd")); };
  document.getElementById("desktopicon").onchange = function () {
    send("K" + (this.checked ? "1" : "0"));
  };

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
// Every occurrence: one token can be needed in more than one place (the range shows its value twice).
void ReplaceToken(std::wstring* html, const wchar_t* token, const wchar_t* value) {
  const size_t length = std::wcslen(token);
  const size_t grown = std::wcslen(value);
  for (size_t at = html->find(token); at != std::wstring::npos;
       at = html->find(token, at + grown)) {
    html->replace(at, length, value);
  }
}

// The setup panel: copy this exe into the user's folder, put that folder on the PATH and point the
// .to association at the copy. The bar asks about the installed copy on every redraw, so the readers
// are here next to it.
bool IsInstalled();
bool FileExists(const std::wstring& path);
std::wstring DesktopShortcutPath();
bool EnsureAssociation();
bool CreateDesktopShortcut();
void RemoveDesktopShortcut();
void RemoveAssociation();
void DoInstall();
void DoUninstall();

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
  wchar_t rangeValue[8] = {};
  wsprintfW(rangeValue, L"%d", g_keyRange);
  ReplaceToken(&html, L"%KEY_RANGE%", rangeValue);
  // Pushing without the rule above it makes no sense, so it is disabled in place, not hidden.
  ReplaceToken(&html, L"%PUSH_DISABLED%", g_blockOverlap ? L"" : L" disabled");
  // The setup panel speaks about the machine, not about this window, so every bar reads the same
  // answer: which button it offers, whether the copy is there and whether the icon exists.
  const bool installed = IsInstalled();
  ReplaceToken(&html, L"%SETUP_HIDDEN%", g_setupPanel ? L"" : L" hidden");
  g_setupPanel = false;
  ReplaceToken(&html, L"%SETUP_ACTION%", installed ? L"Desinstalar" : L"Instalar");
  ReplaceToken(&html, L"%SETUP_COMMAND%", installed ? L"U" : L"I");
  ReplaceToken(&html, L"%SETUP_STATUS%", installed ? L"instalado" : L"sin instalar");
  ReplaceToken(&html, L"%DESKTOP_CHECKED%", FileExists(DesktopShortcutPath()) ? L" checked" : L"");
  ReplaceToken(&html, L"%DESKTOP_DISABLED%", installed ? L"" : L" disabled");
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
  g_stateZoom = 0;  // a page that is opened is fitted; only a .to says otherwise
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
void StateChord();  // Ctrl+Alt+G: freezes a page into a .to file, or brings one back

// Windows ships no eyedropper cursor, and a crosshair does not say "pick a colour". The pipette is
// drawn here: a shaft with a round bulb at the end, black over a white halo so it reads on any
// background, and nothing anywhere else (fully transparent pixels). The tip has to sit on (0,0):
// a cursor built with CreateIconIndirect always has that hotspot, so a tip drawn anywhere else makes
// the pixel that gets picked land far from the pixel the user is aiming at.
HCURSOR CreatePickCursor(COLORREF preview, bool hasPreview) {
  const int size = 32;
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* pixels = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP colour = CreateDIBSection(screen, (BITMAPINFO*)&header, DIB_RGB_COLORS, &pixels,
                                    nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!colour || !pixels) {
    return nullptr;
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);  // all zero: use the colour bitmap

  // The pipette: tip at (0,0), shaft to (12,12), bulb at (17,17).
  const int shaftX0 = 0, shaftY0 = 0, shaftX1 = 12, shaftY1 = 12;
  const int bulbX = 17, bulbY = 17;
  const double shaftDx = (double)(shaftX1 - shaftX0);
  const double shaftDy = (double)(shaftY1 - shaftY0);
  const double shaftLen = shaftDx * shaftDx + shaftDy * shaftDy;
  auto distanceToShaft = [&](int x, int y) {
    const double t = ((x - shaftX0) * shaftDx + (y - shaftY0) * shaftDy) / shaftLen;
    const double clamped = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double px = shaftX0 + clamped * shaftDx;
    const double py = shaftY0 + clamped * shaftDy;
    const double dx = x - px;
    const double dy = y - py;
    return dx * dx + dy * dy;
  };
  auto distanceToBulb = [&](int x, int y) {
    const double dx = x - bulbX;
    const double dy = y - bulbY;
    return dx * dx + dy * dy;
  };

  DWORD* buffer = (DWORD*)pixels;
  // Two passes: the white halo first, the black body on top of it.
  for (int pass = 0; pass < 2; ++pass) {
    const double shaftRadius = pass == 0 ? 4.0 : 2.0;
    const double bulbRadius = pass == 0 ? 9.5 : 6.5;
    const DWORD paint = pass == 0 ? 0xFFFFFFFF : 0xFF000000;
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        if (x == 0 && y == 0) {
          continue;  // never paint the pixel that is being read
        }
        if (distanceToShaft(x, y) <= shaftRadius * shaftRadius ||
            distanceToBulb(x, y) <= bulbRadius * bulbRadius) {
          buffer[y * size + x] = paint;
        }
      }
    }
  }
  // The bulb shows the colour that is about to go: what the tip is pointing at.
  if (hasPreview) {
    const DWORD shown = 0xFF000000 | ((DWORD)GetRValue(preview) << 16) |
                        ((DWORD)GetGValue(preview) << 8) | GetBValue(preview);
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        if (distanceToBulb(x, y) <= 4.0 * 4.0) {
          buffer[y * size + x] = shown;
        }
      }
    }
  }

  ICONINFO info = {};
  info.fIcon = FALSE;
  info.hbmMask = mask;
  info.hbmColor = colour;
  HCURSOR cursor = (HCURSOR)CreateIconIndirect(&info);
  DeleteObject(colour);
  DeleteObject(mask);
  return cursor;
}

// The pixel under the tip, read from the desktop itself so it works over any window, not only ours.
COLORREF ScreenPixel(POINT point) {
  HDC screen = GetDC(nullptr);
  const COLORREF colour = GetPixel(screen, point.x, point.y);
  ReleaseDC(nullptr, screen);
  return colour;
}

// The pipette carries the colour it points at in its bulb, rebuilt only when that colour changes.
void RefreshPickPreview(POINT point) {
  const COLORREF colour = ScreenPixel(point);
  if (colour == CLR_INVALID || colour == g_pickPreview) {
    return;
  }
  HCURSOR next = CreatePickCursor(colour, true);
  if (!next) {
    return;
  }
  HCURSOR previous = g_pickCursor;
  g_pickCursor = next;
  g_pickPreview = colour;
  SetCursor(next);
  if (previous) {
    DestroyCursor(previous);
  }
}

// The engine's own background: opaque normally, transparent while a colour is keyed, because that
// is what the composition shows through the page's uncovered pixels.
void SetEngineBackground(bool transparent) {
  if (!g_controller) {
    return;
  }
  ICoreWebView2Controller2* controller2 = nullptr;
  if (SUCCEEDED(g_controller->QueryInterface(__uuidof(ICoreWebView2Controller2),
                                             reinterpret_cast<void**>(&controller2))) &&
      controller2) {
    // Alpha 0 is what the composition shows the desktop through; in windowed hosting it was ignored.
    const COREWEBVIEW2_COLOR background =
        transparent ? COREWEBVIEW2_COLOR{0, 0, 0, 0} : COREWEBVIEW2_COLOR{255, 15, 20, 27};
    controller2->put_DefaultBackgroundColor(background);
    controller2->Release();
  }
}

// A picked colour stops painting, and so does the page's own base: with composition hosting those
// pixels come out with alpha 0 and the desktop is what shows there. The key is one SVG filter with a
// mask per picked colour, multiplied together -- a pixel disappears when it matches ANY of them,
// which is exactly what multiplying gives (a 0 anywhere makes the whole product 0). Flat backgrounds
// are exact; a photo or a canvas of that colour is not reachable from CSS and stays as it is.
std::wstring BuildColourKeyScript() {
  std::wstring svg =
      L"<svg xmlns='http://www.w3.org/2000/svg'><filter id='k' color-interpolation-filters='sRGB'>";
  // The margin, in levels per channel, becomes the threshold of every mask: a pixel whose distance
  // to the picked colour adds up to less than this disappears along with it.
  const double threshold = g_keyRange * 3.0 / 255.0;
  // feFuncA is alpha = slope*alpha + intercept, so the threshold has to come back multiplied by the
  // slope: alpha reaches 0 exactly at the picked margin and is 1 a few levels past it.
  const double cutoff = -25.0 * threshold;
  wchar_t part[1024] = {};
  for (size_t i = 0; i < g_keyColours.size(); ++i) {
    const unsigned index = (unsigned)i;
    const double r = GetRValue(g_keyColours[i]) / 255.0;
    const double g = GetGValue(g_keyColours[i]) / 255.0;
    const double b = GetBValue(g_keyColours[i]) / 255.0;
    // Absolute difference per channel: one transfer keeps the positive side of source minus colour,
    // the other the positive side of colour minus source, and the screen blend adds the halves.
    // It reads no pixels, so it reaches animated GIFs, videos, canvas and images from another origin.
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feComponentTransfer in='SourceGraphic' result='hi%u'>"
                     L"<feFuncR type='linear' slope='1' intercept='%.6f'/>"
                     L"<feFuncG type='linear' slope='1' intercept='%.6f'/>"
                     L"<feFuncB type='linear' slope='1' intercept='%.6f'/>"
                     L"</feComponentTransfer>",
                     index, -r, -g, -b);
    svg += part;
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feComponentTransfer in='SourceGraphic' result='lo%u'>"
                     L"<feFuncR type='linear' slope='-1' intercept='%.6f'/>"
                     L"<feFuncG type='linear' slope='-1' intercept='%.6f'/>"
                     L"<feFuncB type='linear' slope='-1' intercept='%.6f'/>"
                     L"</feComponentTransfer>",
                     index, r, g, b);
    svg += part;
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feBlend in='hi%u' in2='lo%u' mode='screen' result='diff%u'/>", index, index,
                     index);
    svg += part;
    // The three channels added into the alpha of one value: the distance of that pixel to this
    // colour, on its own, before the threshold.
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feColorMatrix in='diff%u' type='matrix' "
                     L"values='1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  1 1 1 0 0' result='sum%u'/>",
                     index, index);
    svg += part;
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feComponentTransfer in='sum%u' result='mask%u'>"
                     L"<feFuncA type='linear' slope='25' intercept='%.6f'/></feComponentTransfer>",
                     index, index, cutoff);
    svg += part;
  }
  // One mask per colour, chained through feComposite's product term: alpha comes out as the product
  // of the two masks and its colour does not matter, because the last step takes the page's own
  // colours and only this alpha.
  std::wstring mask = L"mask0";
  for (size_t i = 1; i < g_keyColours.size(); ++i) {
    wchar_t next[16] = {};
    StringCchPrintfW(next, ARRAYSIZE(next), L"acc%u", (unsigned)i);
    StringCchPrintfW(part, ARRAYSIZE(part),
                     L"<feComposite in='%s' in2='mask%u' operator='arithmetic' "
                     L"k1='1' k2='0' k3='0' k4='0' result='%s'/>",
                     mask.c_str(), (unsigned)i, next);
    svg += part;
    mask = next;
  }
  StringCchPrintfW(part, ARRAYSIZE(part),
                   L"<feComposite in='SourceGraphic' in2='%s' operator='in'/></filter></svg>",
                   mask.c_str());
  svg += part;

  // The filter travels inside a data URL: a filter living in a page element is not resolved when
  // this script is injected from the host, and the page then renders as if it had no filter at all.
  // The SVG goes in as a double quoted JS string, so nothing in it may use double quotes.
  std::wstring script =
      L"(function(){"
      L"if(window.__tinyKeyRestore){window.__tinyKeyRestore();}"
      L"var svg=\"";
  script += svg;
  script +=
      L"\";"
      L"var keyUrl='url(\"data:image/svg+xml;charset=utf-8,'+encodeURIComponent(svg)+'#k\")';"
      // On the root, so the hole goes all the way down: every pixel of those colours anywhere in the
      // page disappears, whatever element it belonged to, and the page's own base goes too, so what
      // shows through is the desktop and not the backdrop of whatever sat behind the picture.
      // On the body, not on <html>: filtering the root brings the viewport's scrollbars back, because
      // they are painted outside the filtered subtree.
      L"var rootEl=document.body;"
      L"var previousFilter=rootEl?rootEl.style.filter:'';"
      L"if(rootEl){rootEl.style.setProperty('filter',keyUrl,'important');}"
      L"var style=document.createElement('style');style.id='__tiny_key_style';"
      L"style.textContent='html,body{background-color:transparent !important;"
      L"background-image:none !important;scrollbar-width:none !important}"
      L"::-webkit-scrollbar{display:none !important;width:0 !important;height:0 !important}';"
      L"document.documentElement.appendChild(style);"
      L"window.__tinyKeyRestore=function(){style.remove();"
      L"if(rootEl){rootEl.style.filter=previousFilter;}"
      L"window.__tinyKeyRestore=null;};"
      L"})()";
  return script;
}

void RunColourKey() {
  if (!g_webview || g_keyColours.empty()) {
    return;
  }
  const std::wstring script = BuildColourKeyScript();
  g_webview->ExecuteScript(script.c_str(), nullptr);
}

void ClearColourKey() {
  g_keyColours.clear();
  if (g_webview) {
    g_webview->ExecuteScript(
        L"if (window.__tinyKeyRestore) { window.__tinyKeyRestore(); }", nullptr);
  }
  SetEngineBackground(false);
}

// A second pickup adds to the key instead of replacing it, and picking a colour that is already
// keyed takes it out: that is the only way to drop one without clearing the others.
void ApplyColourKey(COLORREF colour) {
  const auto known = std::find(g_keyColours.begin(), g_keyColours.end(), colour);
  if (known != g_keyColours.end()) {
    g_keyColours.erase(known);
  } else if (g_keyColours.size() < kKeyColourMax) {
    g_keyColours.push_back(colour);
  }
  if (g_keyColours.empty()) {
    ClearColourKey();
    return;
  }
  SetEngineBackground(true);
  RunColourKey();
}

void StopPicking() {
  if (!g_picking) {
    return;
  }
  g_picking = false;
  KillTimer(g_window, kPickTimer);
  ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

// Ctrl+Alt+A: armed -> stop picking, keying -> clear every colour, otherwise arm the eyedropper.
void TogglePicking() {
  if (!g_window) {
    return;
  }
  if (g_picking) {
    StopPicking();
    return;
  }
  if (!g_keyColours.empty()) {
    ClearColourKey();
    return;
  }
  g_picking = true;
  POINT tip = {};
  GetCursorPos(&tip);
  g_pickPreview = CLR_INVALID;
  RefreshPickPreview(tip);
  if (!g_pickCursor) {
    g_pickCursor = CreatePickCursor(0, false);
  }
  SetCapture(g_window);
  SetCursor(g_pickCursor);
  // The bulb follows the pointer on a timer: the low-level hook only sees real device moves, and a
  // cursor moved by something else would leave the preview showing an old colour.
  SetTimer(g_window, kPickTimer, 40, nullptr);
}
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
  } else if (id == kHotkeyState) {
    StateChord();
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
    if (!g_keyColours.empty()) {
      RunColourKey();
    }
  } else if (id == kHotkeyPick) {
    TogglePicking();
  } else if (id == kCommandPick) {
    // The screen pixel under the click, read from the desktop so it works over any window. The
    // cursor steps out of the way first: its tip sits exactly on the pixel being read.
    ShowCursor(FALSE);
    const COLORREF picked = ScreenPixel(g_pickAt);
    ShowCursor(TRUE);
    if (picked != CLR_INVALID) {
      ApplyColourKey(picked);
    }
    // Still armed, and still holding the capture: the next click takes another colour. Leaving is
    // Escape or the right button, and it keeps everything picked so far.
    if (g_picking && g_pickCursor) {
      SetCursor(g_pickCursor);
    }
  } else if (id == kCommandPickCancel) {
    StopPicking();
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
constexpr UINT kHotkeyKeyA = L'A';
constexpr UINT kHotkeyKeyG = L'G';

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
    {kHotkeyKeyA, kHotkeyPick},
    {kHotkeyKeyG, kHotkeyState},
};
constexpr int kHotkeyCount = 7;

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
  // A window that opened a snapshot keeps the margin that .to brought: the file answers for the
  // app, and this window already has its own answer.
  if (!g_keyRangeLocal) {
    const int range = GetPrivateProfileIntW(kSettingsSection, L"KeyRange", 5, path);
    g_keyRange = range < 0 ? 0 : (range > kKeyRangeMax ? kKeyRangeMax : range);
  }
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
  wsprintfW(value, L"%d", g_keyRange);
  WritePrivateProfileStringW(kSettingsSection, L"KeyRange", value, path);
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

void SetKeyRange(int value) {
  const int clamped = value < 0 ? 0 : (value > kKeyRangeMax ? kKeyRangeMax : value);
  if (g_keyRange == clamped && !g_keyRangeLocal) {
    return;
  }
  g_keyRange = clamped;
  g_keyRangeLocal = false;  // the slider is the app-wide control, always
  SaveSettings();
  ShareSettings();
  if (!g_keyColours.empty()) {
    RunColourKey();  // this window re-filters with the new margin straight away
  }
}

// The same value, but this window only: no file, no other copy. No re-filter either: both callers
// open a page right after, and the filter is applied again when that page finishes loading.
void UseStateKeyRange(int value) {
  const int clamped = value < 0 ? 0 : (value > kKeyRangeMax ? kKeyRangeMax : value);
  g_keyRange = clamped;
  g_keyRangeLocal = true;
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
    if (g_picking && pressed && key->vkCode == VK_ESCAPE) {
      PostMessageW(g_window, kHotkeyMessage, kCommandPickCancel, 0);
      return 1;
    }

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
        if (chord.id == kHotkeyPick) {
          // Arming belongs to one window: the one in front takes it, and once armed it can still be
          // cancelled from anywhere, because with the mouse captured another app may be in front.
          if (g_picking || HotkeysApplyHere()) {
            PostMessageW(g_window, kHotkeyMessage, kHotkeyPick, 0);
            return 1;
          }
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
    if (g_picking) {
      // The window holds the capture, so every click on the desktop arrives here: the press is the
      // colour, the right button (or Escape) gives up, and nothing reaches the app below.
      if (wparam == WM_MOUSEMOVE) {
        RefreshPickPreview(point);
        SetCursor(g_pickCursor);
        return CallNextHookEx(nullptr, code, wparam, lparam);
      }
      if (wparam == WM_LBUTTONDOWN) {
        g_pickAt = point;
        PostMessageW(g_window, kHotkeyMessage, kCommandPick, 0);
        return 1;
      }
      if (wparam == WM_RBUTTONDOWN) {
        PostMessageW(g_window, kHotkeyMessage, kCommandPickCancel, 0);
        return 1;
      }
    }
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
    case L'C': DuplicateInNewInstance(); break;
    // From the options panel: the switches themselves, and the note that they changed.
    case L'N': SetBlockOverlap(message.size() > 1 && message[1] == L'1'); break;
    case L'P': SetPushOthers(message.size() > 1 && message[1] == L'1'); break;
    case L'T': SetKeyRange(std::wcstol(message.c_str() + 1, nullptr, 10)); break;
    // From the setup panel: install, uninstall and the desktop icon. The bar is rebuilt so the panel
    // comes back showing what the machine now holds.
    case L'I': DoInstall(); g_setupPanel = true; ShowBar(); break;
    case L'U': DoUninstall(); g_setupPanel = true; ShowBar(); break;
    case L'K':
      if (message.size() > 1 && message[1] == L'1') {
        CreateDesktopShortcut();
      } else {
        RemoveDesktopShortcut();
      }
      g_setupPanel = true;
      ShowBar();
      break;
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
  if (g_stateZoom > 0) {
    // Restored from a .to: the zoom in the file is the whole point, so the page is not measured.
    // It holds while the window is resized too, and the fit takes over again the next time an
    // address is opened.
    if (g_controller) {
      g_controller->put_ZoomFactor(g_stateZoom / 1000.0);
    }
    return;
  }
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

// A .to file ("tiny object") is one window frozen in a file: the page it shows, the rectangle it
// occupies, the zoom it is drawn at and the colours it made transparent. Plain key=value text,
// written with the same profile calls as the options file, so there is no parser here and it can be
// read in Notepad.
constexpr wchar_t kStateSection[] = L"tiny";
constexpr wchar_t kStateFileExtension[] = L".to";
constexpr wchar_t kStateProgId[] = L"tinybrowser.to";

struct TinyState {
  std::wstring url;
  RECT rect = {};
  bool hasRect = false;
  std::vector<COLORREF> keyColours;
  int keyRange = -1;  // -1: the file carries none, so the option stays as it is
  int zoom = 0;       // thousandths of the factor, 0 when none was saved
};

bool IsStatePath(const std::wstring& text) {
  const size_t length = std::wcslen(kStateFileExtension);
  if (text.size() <= length) {
    return false;
  }
  return lstrcmpiW(text.c_str() + text.size() - length, kStateFileExtension) == 0;
}

// A snapshot may come from a bigger monitor, or from a spot this desktop no longer has: the
// rectangle is pulled back inside the desktop instead of opening a window nothing could reach.
RECT KeepOnDesktop(const RECT& wanted) {
  RECT desktop = {};
  DesktopBounds(&desktop);
  int width = wanted.right - wanted.left;
  int height = wanted.bottom - wanted.top;
  const int desktopWidth = desktop.right - desktop.left;
  const int desktopHeight = desktop.bottom - desktop.top;
  if (width > desktopWidth) width = desktopWidth;
  if (height > desktopHeight) height = desktopHeight;
  if (width < kMinWidth) width = kMinWidth;
  if (height < kMinHeight) height = kMinHeight;
  int left = wanted.left;
  int top = wanted.top;
  if (left + width > desktop.right) left = desktop.right - width;
  if (top + height > desktop.bottom) top = desktop.bottom - height;
  if (left < desktop.left) left = desktop.left;
  if (top < desktop.top) top = desktop.top;
  const RECT clamped = {left, top, left + width, top + height};
  return clamped;
}

// KeyColours is a comma separated list of the same decimal numbers KeyColour has always carried.
void ParseColourList(const wchar_t* text, std::vector<COLORREF>* colours) {
  colours->clear();
  const wchar_t* at = text;
  while (*at && colours->size() < kKeyColourMax) {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(at, &end, 10);
    if (end == at) {
      break;  // nothing that looks like a number: the rest of the line is not ours
    }
    colours->push_back((COLORREF)value);
    at = end;
    while (*at == L',' || *at == L' ') {
      ++at;
    }
  }
}

bool ReadState(const std::wstring& path, TinyState* state) {
  if (!state || path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  wchar_t url[2048] = {};
  if (!GetPrivateProfileStringW(kStateSection, L"Url", L"", url, ARRAYSIZE(url), path.c_str())) {
    return false;  // no [tiny] section: not one of ours
  }
  state->url = url;
  const int left = (int)GetPrivateProfileIntW(kStateSection, L"Left", 0, path.c_str());
  const int top = (int)GetPrivateProfileIntW(kStateSection, L"Top", 0, path.c_str());
  const int width = (int)GetPrivateProfileIntW(kStateSection, L"Width", 0, path.c_str());
  const int height = (int)GetPrivateProfileIntW(kStateSection, L"Height", 0, path.c_str());
  state->hasRect = width >= kMinWidth && height >= kMinHeight;
  if (state->hasRect) {
    state->rect.left = left;
    state->rect.top = top;
    state->rect.right = left + width;
    state->rect.bottom = top + height;
  }
  wchar_t colours[512] = {};
  GetPrivateProfileStringW(kStateSection, L"KeyColours", L"", colours, ARRAYSIZE(colours),
                           path.c_str());
  ParseColourList(colours, &state->keyColours);
  if (state->keyColours.empty() &&
      GetPrivateProfileIntW(kStateSection, L"Keyed", 0, path.c_str()) != 0) {
    // Saved before the list existed: one colour, and nothing else.
    state->keyColours.push_back(
        (COLORREF)GetPrivateProfileIntW(kStateSection, L"KeyColour", 0, path.c_str()));
  }
  const int range =
      (int)GetPrivateProfileIntW(kStateSection, L"KeyRange", kKeyRangeMax + 1, path.c_str());
  state->keyRange = range <= kKeyRangeMax ? range : -1;
  // Wider than the fit's own limits on purpose: Ctrl+wheel can zoom a page past 100 %, and a state
  // that cannot remember that would come back looking nothing like it was saved.
  const int zoom = (int)GetPrivateProfileIntW(kStateSection, L"Zoom", 0, path.c_str());
  if (zoom >= 50 && zoom <= 5000) {
    state->zoom = zoom;
  }
  return !state->url.empty();
}

// The file is written whole: a key the snapshot does not carry cannot survive from an older save.
bool WriteState(const std::wstring& path, const TinyState& state) {
  if (path.empty()) {
    return false;
  }
  DeleteFileW(path.c_str());
  wchar_t value[64] = {};
  WritePrivateProfileStringW(kStateSection, L"Version", L"1", path.c_str());
  WritePrivateProfileStringW(kStateSection, L"Url", state.url.c_str(), path.c_str());
  if (state.hasRect) {
    wsprintfW(value, L"%d", (int)state.rect.left);
    WritePrivateProfileStringW(kStateSection, L"Left", value, path.c_str());
    wsprintfW(value, L"%d", (int)state.rect.top);
    WritePrivateProfileStringW(kStateSection, L"Top", value, path.c_str());
    wsprintfW(value, L"%d", (int)(state.rect.right - state.rect.left));
    WritePrivateProfileStringW(kStateSection, L"Width", value, path.c_str());
    wsprintfW(value, L"%d", (int)(state.rect.bottom - state.rect.top));
    WritePrivateProfileStringW(kStateSection, L"Height", value, path.c_str());
  }
  if (state.zoom > 0) {
    wsprintfW(value, L"%d", state.zoom);
    WritePrivateProfileStringW(kStateSection, L"Zoom", value, path.c_str());
  }
  wsprintfW(value, L"%d", state.keyColours.empty() ? 0 : 1);
  WritePrivateProfileStringW(kStateSection, L"Keyed", value, path.c_str());
  if (!state.keyColours.empty()) {
    std::wstring list;
    for (COLORREF colour : state.keyColours) {
      if (!list.empty()) {
        list += L',';
      }
      wchar_t number[16] = {};
      wsprintfW(number, L"%u", (unsigned)colour);
      list += number;
    }
    WritePrivateProfileStringW(kStateSection, L"KeyColours", list.c_str(), path.c_str());
    // The first one again under the old name: an older build still opens the file keying something.
    wsprintfW(value, L"%u", (unsigned)state.keyColours.front());
    WritePrivateProfileStringW(kStateSection, L"KeyColour", value, path.c_str());
  }
  wsprintfW(value, L"%d", state.keyRange);
  WritePrivateProfileStringW(kStateSection, L"KeyRange", value, path.c_str());
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The folder the last .to came from, kept for this run only: which file is in use is the file's
// own business, and the options file is not the place to remember it.
std::wstring g_stateFolder;

void RememberStateFolder(const std::wstring& path) {
  const size_t slash = path.find_last_of(static_cast<wchar_t>(92));
  if (slash != std::wstring::npos) {
    g_stateFolder = path.substr(0, slash);
  }
}

// The suggested name is the site's own, so a folder of them reads as the sites it holds.
std::wstring NameFromUrl(const std::wstring& url) {
  size_t start = url.find(L"://");
  start = start == std::wstring::npos ? 0 : start + 3;
  const size_t end = url.find_first_of(L"/?#", start);
  std::wstring name =
      url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
  if (name.compare(0, 4, L"www.") == 0) {
    name = name.substr(4);
  }
  for (size_t i = 0; i < name.size(); ++i) {
    const wchar_t ch = name[i];
    // What Windows refuses in a file name, the path separator included.
    if (ch < 32 || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' ||
        ch == L'>' || ch == L'|' || ch == L'/' || ch == static_cast<wchar_t>(92)) {
      name[i] = L'_';
    }
  }
  if (name.empty()) {
    name = L"tiny";
  }
  name += kStateFileExtension;
  return name;
}

bool AskStateToSave(wchar_t* path, DWORD size, const std::wstring& suggested,
                    const std::wstring& folder) {
  lstrcpynW(path, suggested.c_str(), (int)size);
  OPENFILENAMEW dialog = {};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = g_window;
  dialog.lpstrFilter = L"tiny object (*.to)\0*.to\0Todos los archivos\0*.*\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = size;
  dialog.lpstrDefExt = L"to";
  dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  // The file dialog is an ordinary window and this one is always on top: it steps down while the
  // dialog is open, or it would sit over the very fields being typed in.
  SetWindowPos(g_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  const BOOL picked = GetSaveFileNameW(&dialog);
  SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  return picked == TRUE;
}

bool AskStateToOpen(wchar_t* path, DWORD size, const std::wstring& folder) {
  path[0] = 0;
  OPENFILENAMEW dialog = {};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = g_window;
  dialog.lpstrFilter = L"tiny object (*.to)\0*.to\0Todos los archivos\0*.*\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = size;
  dialog.lpstrDefExt = L"to";
  dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  // The file dialog is an ordinary window and this one is always on top: it steps down while the
  // dialog is open, or it would sit over the very fields being typed in.
  SetWindowPos(g_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  const BOOL picked = GetOpenFileNameW(&dialog);
  SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  return picked == TRUE;
}

// Installing is copying this exe into the user's own folder, putting that folder on the PATH so the
// command exists, and pointing the .to association at the copy. Every write goes to HKCU or to the
// user's profile, so none of it needs admin rights and none of it touches other accounts.
std::wstring CurrentExe() {
  wchar_t path[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    return std::wstring();
  }
  return path;
}

// Under the vendor folder, and in a folder of its own: %LOCALAPPDATA%\tinybrowser is the WebView2
// profile, which is logins and cookies, not the program.
std::wstring InstallFolder() {
  wchar_t base[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
    return std::wstring();
  }
  return std::wstring(base) + L"\\Unresolute\\tinybrowser";
}

// Named tiny.exe on purpose: Win+R looks for an executable on the PATH, and this is the name the
// command is meant to have.
std::wstring InstalledExe() {
  const std::wstring folder = InstallFolder();
  return folder.empty() ? std::wstring() : folder + L"\\tiny.exe";
}

bool FileExists(const std::wstring& path) {
  return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsInstalled() { return FileExists(InstalledExe()); }

std::wstring AssociationExe() { return IsInstalled() ? InstalledExe() : CurrentExe(); }

std::wstring DesktopShortcutPath() {
  wchar_t folder[MAX_PATH] = {};
  if (FAILED(
          SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, folder))) {
    return std::wstring();
  }
  return std::wstring(folder) + L"\\tiny.lnk";
}

// Same folder written two ways: case does not matter on Windows, nor does a trailing separator.
bool SamePath(const std::wstring& left, const std::wstring& right) {
  std::wstring a = left;
  std::wstring b = right;
  while (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
  while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
  return !a.empty() && lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

// The user's own PATH, exactly as the shell reads it. The type travels along because a REG_EXPAND_SZ
// has to stay one: writing it back as REG_SZ would expand away any %VAR% the user put in there.
bool ReadUserPath(std::wstring* value, DWORD* type) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD bytes = 0;
  DWORD kind = REG_SZ;
  if (RegQueryValueExW(key, L"Path", nullptr, &kind, nullptr, &bytes) != ERROR_SUCCESS) {
    RegCloseKey(key);
    return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, 0);
  const LSTATUS status = RegQueryValueExW(key, L"Path", nullptr, &kind,
                                          reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  *value = buffer.data();
  *type = kind == REG_EXPAND_SZ ? REG_EXPAND_SZ : REG_SZ;
  return true;
}

bool WriteUserPath(const std::wstring& value, DWORD type) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status =
      RegSetValueExW(key, L"Path", 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                     (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  // Win+R and every shell started afterwards read the variable when they start: they only see the new
  // value once Windows is told that the environment changed.
  DWORD_PTR ignored = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG,
                      1000, &ignored);
  return true;
}

// The PATH text with exactly one folder added or removed, every other entry left as it was. Empty
// entries go: they mean "the current directory" on the PATH, which is nothing anyone wants, and the
// rebuild is where they tend to pile up.
std::wstring RebuildPath(const std::wstring& path, const std::wstring& entry, bool add) {
  std::wstring rebuilt;
  size_t at = 0;
  while (at <= path.size()) {
    const size_t separator = path.find(L';', at);
    const size_t length = separator == std::wstring::npos ? std::wstring::npos : separator - at;
    const std::wstring item = path.substr(at, length);
    if (!item.empty() && !SamePath(item, entry)) {
      if (!rebuilt.empty()) {
        rebuilt += L';';
      }
      rebuilt += item;
    }
    if (separator == std::wstring::npos) {
      break;
    }
    at = separator + 1;
  }
  if (add) {
    if (!rebuilt.empty()) {
      rebuilt += L';';
    }
    rebuilt += entry;
  }
  return rebuilt;
}

bool SetPathEntry(const std::wstring& entry, bool add) {
  std::wstring path;
  DWORD type = REG_SZ;
  ReadUserPath(&path, &type);
  const std::wstring rebuilt = RebuildPath(path, entry, add);
  if (rebuilt == path) {
    return true;
  }
  return WriteUserPath(rebuilt, type);
}

bool CreateDesktopShortcut() {
  const std::wstring target = InstalledExe();
  const std::wstring path = DesktopShortcutPath();
  if (!FileExists(target) || path.empty()) {
    return false;
  }
  ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
      !link) {
    return false;
  }
  link->SetPath(target.c_str());
  link->SetWorkingDirectory(InstallFolder().c_str());
  link->SetDescription(L"tinybrowser");
  link->SetIconLocation(target.c_str(), 0);
  ComPtr<IPersistFile> file;
  if (FAILED(link.As(&file)) || !file) {
    return false;
  }
  return SUCCEEDED(file->Save(path.c_str(), TRUE));
}

void RemoveDesktopShortcut() {
  const std::wstring path = DesktopShortcutPath();
  if (!path.empty()) {
    DeleteFileW(path.c_str());
  }
}

// The copy itself; running from the installed exe means there is nothing to copy.
bool CopyIntoInstallFolder() {
  const std::wstring folder = InstallFolder();
  const std::wstring target = InstalledExe();
  const std::wstring source = CurrentExe();
  if (folder.empty() || target.empty() || source.empty()) {
    return false;
  }
  if (SamePath(source, target)) {
    return true;
  }
  const int created = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
  if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && !FileExists(folder)) {
    return false;
  }
  return CopyFileW(source.c_str(), target.c_str(), FALSE) != 0;
}

void DoInstall() {
  if (!CopyIntoInstallFolder()) {
    MessageBoxW(nullptr, L"No pude copiar el tiny.exe a tu carpeta de usuario.", kTitle,
                MB_OK | MB_ICONERROR);
    return;
  }
  SetPathEntry(InstallFolder(), true);
  EnsureAssociation();  // now it points at the copy, which is what a double click should open
  if (FileExists(DesktopShortcutPath())) {
    CreateDesktopShortcut();  // a shortcut already on the desktop follows the copy, not the old path
  }
}

// Uninstalling takes away everything installing put there. The WebView2 profile stays: it is the
// logins and cookies of every page you opened, not part of the program.
void DoUninstall() {
  SetPathEntry(InstallFolder(), false);
  RemoveDesktopShortcut();
  RemoveAssociation();
  const std::wstring target = InstalledExe();
  if (SamePath(CurrentExe(), target)) {
    // Windows keeps the image of a running program open, but it does let the file be moved: parking it
    // in the temp folder is what lets the install folder go away while this window is still up.
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp)) {
      wchar_t name[64] = {};
      wsprintfW(name, L"tiny-uninstalled-%u.exe", (unsigned)GetCurrentProcessId());
      MoveFileW(target.c_str(), (std::wstring(temp) + name).c_str());
    }
  } else {
    DeleteFileW(target.c_str());  // the copy is not the program that is running
  }
  // The folder only goes away when it is empty, which is the check that catches a copy still in use.
  const std::wstring folder = InstallFolder();
  if (FileExists(folder) && !RemoveDirectoryW(folder.c_str())) {
    wchar_t text[MAX_PATH + 128] = {};
    wsprintfW(text, L"No pude borrar la carpeta instalada.\n\nQuedo en:\n%s", folder.c_str());
    MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONWARNING);
    return;
  }
  // And the vendor folder goes too once nothing else lives in it: an uninstall leaves no trail.
  const size_t slash = folder.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    RemoveDirectoryW(folder.substr(0, slash).c_str());
  }
}

// Only what is ours: a .to that belongs to another program is left alone, exactly as when taking it.
void RemoveAssociation() {
  const std::wstring classes = L"Software\\Classes\\";
  HKEY key = nullptr;
  wchar_t owner[128] = {};
  DWORD bytes = sizeof(owner);
  if (RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L".to").c_str(), 0, KEY_READ, &key) ==
      ERROR_SUCCESS) {
    if (RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(owner), &bytes) ==
            ERROR_SUCCESS &&
        std::wcscmp(owner, kStateProgId) == 0) {
      RegDeleteKeyW(HKEY_CURRENT_USER, (classes + L".to").c_str());
    }
    RegCloseKey(key);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, (classes + kStateProgId).c_str());
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Whether the extension is still free. Nothing there means it is; another program's progid means it
// is not ours to take.
bool ExtensionIsFree(HKEY root, const wchar_t* subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return true;
  }
  wchar_t owner[128] = {};
  DWORD bytes = sizeof(owner);
  const LSTATUS status =
      RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(owner), &bytes);
  RegCloseKey(key);
  return status != ERROR_SUCCESS || std::wcscmp(owner, kStateProgId) == 0;
}

void SetKeyDefault(HKEY root, const std::wstring& subkey, const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
}

// Double-clicking a .to in Explorer has to open it with this app, which takes a per-user
// association: no admin rights and nothing system-wide. An extension another program already owns
// is left alone instead of taken over, and the machine-wide view is checked as well because that is
// the one Explorer ends up obeying.
bool EnsureAssociation() {
  if (!ExtensionIsFree(HKEY_CURRENT_USER, L"Software\\Classes\\.to") ||
      !ExtensionIsFree(HKEY_CLASSES_ROOT, L".to")) {
    return false;
  }
  // The installed copy when there is one: a double click has to open that, not whichever copy of the
  // program happens to be running.
  const std::wstring executable = AssociationExe();
  if (executable.empty()) {
    return false;
  }
  const std::wstring classes = L"Software\\Classes\\";
  SetKeyDefault(HKEY_CURRENT_USER, classes + L".to", kStateProgId);
  SetKeyDefault(HKEY_CURRENT_USER, classes + kStateProgId, L"tiny object");
  // Resource id 2, the document icon: a .to has to look like a file, not like the browser that
  // opens it.
  SetKeyDefault(HKEY_CURRENT_USER, classes + kStateProgId + L"\\DefaultIcon",
                L"\"" + executable + L"\",-2");
  SetKeyDefault(HKEY_CURRENT_USER, classes + kStateProgId + L"\\shell\\open\\command",
                L"\"" + executable + L"\" \"%1\"");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return true;
}

// Everything the file holds, in the order the window can take it: the margin first (this window only,
// which is what the file was saved with), then the page, then the rectangle -- opening a page may
// resize the window on its own -- and the zoom last, because the fit applies it once the page
// finishes opening.
void ApplyState(const TinyState& state) {
  if (!g_window || !g_webview || state.url.empty()) {
    return;
  }
  if (state.keyRange >= 0) {
    UseStateKeyRange(state.keyRange);
  }
  g_keyColours = state.keyColours;
  SetEngineBackground(!g_keyColours.empty());
  Navigate(state.url);
  g_stateZoom = state.zoom;  // after Navigate, which clears it: this page keeps the zoom it had
  if (state.hasRect) {
    const RECT rect = KeepOnDesktop(state.rect);
    SetWindowPos(g_window, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

// Ctrl+Alt+G on a page: freeze this window into a file.
void SaveState() {
  if (!g_window || g_showingBar || g_url.empty()) {
    return;  // there is no page to photograph
  }
  TinyState state;
  state.url = g_url;
  RECT rect = {};
  if (GetWindowRect(g_window, &rect)) {
    state.rect = rect;
    state.hasRect = true;
  }
  state.keyColours = g_keyColours;
  state.keyRange = g_keyRange;
  if (g_controller) {
    double zoom = 1.0;
    if (SUCCEEDED(g_controller->get_ZoomFactor(&zoom))) {
      state.zoom = (int)(zoom * 1000.0 + 0.5);
    }
  }
  wchar_t path[MAX_PATH * 2] = {};
  if (!AskStateToSave(path, ARRAYSIZE(path), NameFromUrl(g_url), g_stateFolder)) {
    return;
  }
  if (!IsStatePath(path)) {
    StringCchCatW(path, ARRAYSIZE(path), kStateFileExtension);
  }
  if (WriteState(path, state)) {
    RememberStateFolder(path);
    EnsureAssociation();  // a file nothing can open is half a feature
  }
}

// One chord, two meanings: a page can be photographed, the bar has nothing to photograph, so there
// it asks which file to bring back -- the same files Explorer opens by double click.
void StateChord() {
  if (!g_window) {
    return;
  }
  if (!g_showingBar) {
    SaveState();
    return;
  }
  wchar_t picked[MAX_PATH * 2] = {};
  if (!AskStateToOpen(picked, ARRAYSIZE(picked), g_stateFolder)) {
    return;
  }
  TinyState state;
  if (ReadState(picked, &state)) {
    ApplyState(state);
    RememberStateFolder(picked);
  }
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

// A duplicate is the same window somewhere else: it opens the same page in the first gap the group
// layout leaves free, in that window's size, and carrying what that window was showing -- the colours
// it had keyed and, when the margin came from a .to, that margin too. The address goes last, quoted.
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
  wchar_t numbers[128] = {};
  wsprintfW(numbers, L"--place %d %d %d %d", spot.left, spot.top, spot.right - spot.left,
            spot.bottom - spot.top);
  std::wstring arguments(numbers);
  if (!g_keyColours.empty()) {
    arguments += L" --key ";
    for (size_t i = 0; i < g_keyColours.size(); ++i) {
      wchar_t colour[16] = {};
      wsprintfW(colour, i ? L",%u" : L"%u", (unsigned)g_keyColours[i]);
      arguments += colour;
    }
  }
  // Only when it is not the app-wide one: otherwise the copy reads the same number from the options
  // file, and marking it local would freeze it there for no reason.
  if (g_keyRangeLocal) {
    wchar_t range[16] = {};
    wsprintfW(range, L" --range %d", g_keyRange);
    arguments += range;
  }
  arguments += L" \"" + g_url + L"\"";
  ShellExecuteW(nullptr, L"open", exe, arguments.c_str(), nullptr, SW_SHOWNORMAL);
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

// The device first, and one message only if both drivers fail: a hardware attempt that falls back to
// software is not an error worth interrupting anyone for, and the pair of codes is what says why.
// E_OUTOFMEMORY is the answer of a machine that already holds dozens of copies of this engine: the
// driver has no room for one more device, and no retry makes room for it.
bool CreateD3DDevice(ComPtr<ID3D11Device>* device) {
  const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
  HRESULT results[2] = {};
  for (int i = 0; i < 2; ++i) {
    ComPtr<ID3D11Device> attempt;
    results[i] = D3D11CreateDevice(nullptr, drivers[i], nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &attempt, nullptr, nullptr);
    if (SUCCEEDED(results[i]) && attempt) {
      *device = attempt;
      return true;
    }
  }
  wchar_t text[512] = {};
  wsprintfW(text,
            L"tinybrowser no pudo iniciar el motor de WebView2.\n\n"
            L"Crear el dispositivo grafico fallo con la placa (0x%08X) y por software (0x%08X).\n\n%s",
            (unsigned)results[0], (unsigned)results[1],
            results[0] == E_OUTOFMEMORY || results[1] == E_OUTOFMEMORY
                ? L"La maquina se quedo sin memoria para el motor grafico: cierra algunas ventanas de "
                  L"tinybrowser y vuelve a intentar."
                : L"");
  MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONERROR);
  return false;
}

// Builds the tree the engine paints into. Every step reports its own failing call: a silent
// failure here is a window that shows nothing at all, which is impossible to diagnose from outside.
bool StartComposition() {
  ComPtr<ID3D11Device> d3dDevice;
  if (!CreateD3DDevice(&d3dDevice)) {
    return false;
  }
  HRESULT result = S_OK;
  ComPtr<IDXGIDevice> dxgiDevice;
  result = d3dDevice.As(&dxgiDevice);
  if (FAILED(result) || !dxgiDevice) {
    ReportFailure(L"IDXGIDevice", result);
    return false;
  }
  result = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g_dcompDevice));
  if (FAILED(result) || !g_dcompDevice) {
    ReportFailure(L"DCompositionCreateDevice", result);
    return false;
  }
  result = g_dcompDevice->CreateTargetForHwnd(g_window, TRUE, &g_dcompTarget);
  if (FAILED(result) || !g_dcompTarget) {
    ReportFailure(L"IDCompositionDevice::CreateTargetForHwnd", result);
    return false;
  }
  result = g_dcompDevice->CreateVisual(&g_dcompRoot);
  if (FAILED(result) || !g_dcompRoot) {
    ReportFailure(L"IDCompositionDevice::CreateVisual", result);
    return false;
  }
  result = g_dcompTarget->SetRoot(g_dcompRoot.Get());
  if (FAILED(result)) {
    ReportFailure(L"IDCompositionTarget::SetRoot", result);
    return false;
  }
  result = g_composition->put_RootVisualTarget(g_dcompRoot.Get());
  if (FAILED(result)) {
    ReportFailure(L"put_RootVisualTarget", result);
    return false;
  }
  result = g_dcompDevice->Commit();
  if (FAILED(result)) {
    ReportFailure(L"IDCompositionDevice::Commit", result);
    return false;
  }
  return true;
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
            ComPtr<ICoreWebView2Environment3> environment3;
            if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&environment3))) || !environment3) {
              ReportFailure(L"ICoreWebView2Environment3", E_NOINTERFACE);
              PostMessageW(g_window, WM_CLOSE, 0, 0);
              return S_OK;
            }
            environment3->CreateCoreWebView2CompositionController(
                g_window,
                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [](HRESULT controllerResult,
                       ICoreWebView2CompositionController* composition) -> HRESULT {
                      if (FAILED(controllerResult) || !composition) {
                        ReportFailure(L"CreateCoreWebView2CompositionController", controllerResult);
                        PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                      }
                      g_composition = composition;
                      // The composition controller also answers for the controller API -- bounds,
                      // visibility, zoom, focus -- so the rest of the file needs no changes.
                      if (FAILED(composition->QueryInterface(IID_PPV_ARGS(&g_controller))) ||
                          !g_controller) {
                        ReportFailure(L"ICoreWebView2Controller", (HRESULT)E_NOINTERFACE);
                        PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                      }
                      g_controller->get_CoreWebView2(&g_webview);

                      RECT bounds = {};
                      GetClientRect(g_window, &bounds);
                      g_controller->put_Bounds(bounds);
                      g_controller->put_IsVisible(TRUE);
                      // The tree the engine paints into. Everything visible in this window comes
                      // from here; the window itself has no GDI surface.
                      if (!StartComposition()) {
                        PostMessageW(g_window, WM_CLOSE, 0, 0);
                        return S_OK;
                      }
                      EventRegistrationToken cursorToken = {};
                      g_composition->add_CursorChanged(
                          Callback<ICoreWebView2CursorChangedEventHandler>(
                              [](ICoreWebView2CompositionController* sender,
                                 IUnknown*) -> HRESULT {
                                HCURSOR cursor = nullptr;
                                if (sender && SUCCEEDED(sender->get_Cursor(&cursor)) && cursor) {
                                  g_pageCursor = cursor;
                                }
                                return S_OK;
                              })
                              .Get(),
                          &cursorToken);
                      // The engine paints its own background before the content catches up, and by
                      // default that is white: growing the window showed a pale strip on the freshly
                      // exposed edge. Same colour as the window, so a resize looks even, and
                      // transparent when a .to brought a colour already keyed in.
                      SetEngineBackground(!g_keyColours.empty());
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
                                RunColourKey();  // a new document starts without the key
                                StartFit();
                                return S_OK;
                              })
                              .Get(),
                          &navigatedToken);

                      g_webview->AddScriptToExecuteOnDocumentCreated(kPageScript, nullptr);
                      if (g_startTarget.empty()) {
                        ShowBar();
                      } else {
                        const int stateZoom = g_stateZoom;  // Navigate drops it
                        Navigate(g_startTarget);
                        g_stateZoom = stateZoom;  // ... and a .to keeps its own
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

// Without a child window there is nobody else to deliver pointer input: every mouse message is
// translated into the engine's own vocabulary, in client coordinates.
void ForwardMouse(UINT message, WPARAM wparam, LPARAM lparam) {
  if (!g_composition) {
    return;
  }
  POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  COREWEBVIEW2_MOUSE_EVENT_KIND kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
  COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
  UINT32 data = 0;
  switch (message) {
    case WM_MOUSEMOVE:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
      break;
    case WM_LBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
      keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
      break;
    case WM_LBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
      break;
    case WM_LBUTTONDBLCLK:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
      keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
      break;
    case WM_RBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
      keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
      break;
    case WM_RBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
      break;
    case WM_MBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
      keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
      break;
    case WM_MBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
      break;
    case WM_MOUSEWHEEL: {
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
      data = (UINT32)(SHORT)HIWORD(wparam);
      POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(g_window, &screen);
      point = screen;
      break;
    }
    case WM_MOUSELEAVE:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
      point = {0, 0};
      break;
    default:
      return;
  }
  g_composition->SendMouseInput(kind, keys, data, point);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;  // nothing of our own to paint: the composition is the window
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
      if (wparam == kPickTimer) {
        POINT tip = {};
        if (GetCursorPos(&tip)) {
          RefreshPickPreview(tip);
        }
      } else if (wparam == kFitTimer) {
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
    case WM_SETCURSOR:
      if (g_picking && g_pickCursor) {
        SetCursor(g_pickCursor);
        return TRUE;
      }
      if (LOWORD(lparam) == HTCLIENT && g_pageCursor) {
        // No child window to ask any more: the engine publishes the cursor it wants and this is the
        // only place it can be shown, or links and text fields lose their pointer.
        SetCursor(g_pageCursor);
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSELEAVE: {
      static bool tracking = false;
      if (message == WM_MOUSEMOVE && !tracking) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
        tracking = TrackMouseEvent(&track);
      } else if (message == WM_MOUSELEAVE) {
        tracking = false;
      }
      if (message == WM_LBUTTONDOWN) {
        // A drag that starts in the page can end anywhere: the engine needs the moves outside the
        // window too, or a text selection stops at the border.
        SetCapture(window);
      } else if (message == WM_LBUTTONUP) {
        ReleaseCapture();
      }
      ForwardMouse(message, wparam, lparam);
      return 0;
    }
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
      if (g_pickCursor) {
        DestroyCursor(g_pickCursor);
        g_pickCursor = nullptr;
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
  // The association records the path of the exe that wrote it, so it is restated on every start:
  // after moving the exe, double-clicking one of those files still opens this build.
  EnsureAssociation();
  const std::wstring arguments = Trim(commandLine ? commandLine : L"");
  // Options in front of the address, all optional and only sent by a duplicate: --place x y width
  // height (the spot it was given), --key (the colours it had keyed, as the same decimal list the .to
  // files use) and --range (its margin, only when that one was not the app-wide value).
  std::wstring rest = arguments;
  while (StartsWith(rest, L"--")) {
    const size_t gap = rest.find(L' ');
    if (gap == std::wstring::npos) {
      break;  // an option with no value: whatever is left is the address
    }
    const std::wstring option = rest.substr(0, gap);
    rest = Trim(rest.substr(gap + 1));
    if (option == L"--place") {
      const wchar_t* at = rest.c_str();
      long values[4] = {};
      for (int i = 0; i < 4; ++i) {
        wchar_t* end = nullptr;
        values[i] = std::wcstol(at, &end, 10);
        if (end == at) {
          break;
        }
        at = end;
      }
      g_placedRect.left = (int)values[0];
      g_placedRect.top = (int)values[1];
      g_placedRect.right = (int)(values[0] + values[2]);
      g_placedRect.bottom = (int)(values[1] + values[3]);
      g_hasPlacedRect = values[2] > 0 && values[3] > 0;
      rest = Trim(rest.substr((size_t)(at - rest.c_str())));
    } else if (option == L"--key") {
      const size_t end = rest.find(L' ');
      const std::wstring list = end == std::wstring::npos ? rest : rest.substr(0, end);
      ParseColourList(list.c_str(), &g_keyColours);
      rest = end == std::wstring::npos ? std::wstring() : Trim(rest.substr(end + 1));
    } else if (option == L"--range") {
      const long value = std::wcstol(rest.c_str(), nullptr, 10);
      g_keyRange = (int)(value < 0 ? 0 : (value > kKeyRangeMax ? kKeyRangeMax : value));
      g_keyRangeLocal = true;  // the margin of the window that was duplicated, not this one's
      const size_t end = rest.find(L' ');
      rest = end == std::wstring::npos ? std::wstring() : Trim(rest.substr(end + 1));
    } else {
      break;
    }
  }
  g_startTarget = Trim(rest);
  if (g_startTarget.size() >= 2 && g_startTarget.front() == L'"' && g_startTarget.back() == L'"') {
    g_startTarget = g_startTarget.substr(1, g_startTarget.size() - 2);
  }
  // Explorer hands a double-clicked .to over as the only argument, and it is a snapshot rather
  // than an address: opening it is what the file is for.
  if (IsStatePath(g_startTarget)) {
    TinyState state;
    if (ReadState(g_startTarget, &state)) {
      const std::wstring statePath = g_startTarget;
      g_startTarget = state.url;
      if (state.hasRect) {
        g_placedRect = KeepOnDesktop(state.rect);
        g_hasPlacedRect = true;
      }
      g_stateZoom = state.zoom;
      g_keyColours = state.keyColours;
      if (state.keyRange >= 0) {
        UseStateKeyRange(state.keyRange);  // the snapshot's margin, not the app's
      }
      RememberStateFolder(statePath);
    } else {
      g_startTarget.clear();  // unreadable: the bar, not a search for a path
    }
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
  // No background brush: with composition hosting the window has no surface of its own, and a
  // brush here is exactly what would sit between the engine's transparent pixels and the desktop.
  windowClass.hbrBackground = nullptr;
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
  // No redirection bitmap: the window owns no GDI surface, so what shows is exactly the composition
  // tree, alpha included. Without this the page could never be see-through to the desktop.
  g_window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kWindowClass, kTitle, WS_POPUP,
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
