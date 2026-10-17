// ==WindhawkMod==
// @id              taskbar-win10-tray-spacing
// @name            Taskbar Tray Icon Spacing & 2 Rows (Windows 10 / ExplorerPatcher)
// @description     Customize icon spacing and force a compact 2-row grid (half top, half bottom) in the system tray on Windows 10 taskbar (including ExplorerPatcher)
// @version         1.6.0
// @author          Antigravity & Jakub
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -luxtheme
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Tray Icon Spacing & 2 Rows Grid (Windows 10 / ExplorerPatcher)

This mod arranges notification area (system tray) icons into **two compact rows
(half on the top, half on the bottom)** on the Windows 10 taskbar (including ExplorerPatcher
on Windows 11), with icons **pinned to the right edge** next to the system controls.

![Demonstration](https://raw.githubusercontent.com/jakubix30/mods-windhawk/refs/heads/main/demo_tray.png)

### Key Features:
- **2 Rows Balanced Grid (50% top / 50% bottom):** Perfectly balances visible icons into two rows!
- **Right-Aligned Attachment:** Keeps icons snug against the right controls (gear / clock), eliminating dead gaps.
- **Scroll Button (>) Suppressed:** Completely prevents the white overflow/scroll arrow from covering icons.
- **Centered Hover Highlights & Icons:** Icons and hover rectangles are vertically and horizontally centered with symmetrical margins.
- **Dynamic Tray Updates:** Adding or closing apps/icons never breaks the 2-row grid layout.
- **Adaptive Taskbar Height:** Supports standard (~40px) and double-height / expanded taskbars automatically.
- **Free Icon Reordering:** Full drag-and-drop support: you can reorder icons freely between and across rows.
- **Instant Live Updates:** Changing settings in Windhawk immediately recalculates layout on the taskbar UI thread.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- rows: 2
  $name: Tray icon rows (1 or 2)
  $description: >-
    Number of rows for the tray icons.
    1 = single horizontal row.
    2 = compact two-row grid (half top, half bottom).
- horizontalPadding: 2
  $name: Horizontal icon padding (px)
  $description: >-
    Horizontal padding on each side of tray icons in pixels.
    Default: 2. Lower = more compact, higher = more spaced out.
- applyToOverflow: false
  $name: Apply to overflow menu
  $description: >-
    Also apply custom padding to the hidden icons popup (chevron overflow window).
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>
#include <windhawk_utils.h>

#include <atomic>

constexpr UINT_PTR SUBCLASS_ID_TOOLBAR = 0x54524159; // 'TRAY'
constexpr UINT_PTR SUBCLASS_ID_PAGER   = 0x50414752; // 'PAGR'
constexpr UINT_PTR SUBCLASS_ID_NOTIFY  = 0x4E4F5446; // 'NOTF'

struct Settings {
    int rows;
    int horizontalPadding;
    bool applyToOverflow;
} g_settings;

std::atomic<bool> g_unloading{false};
std::atomic<int>  g_wrapButtonId{-1};

// Re-entrant guard: prevents RelayoutTray from triggering itself via
// WM_WINDOWPOSCHANGED -> SetWindowPos(pager) -> WM_WINDOWPOSCHANGED -> ...
// Use volatile LONG + Win32 Interlocked API for compatibility with
// Windhawk's clang SDK (older <atomic> headers may lack fetch_add/load).
volatile LONG g_layoutDepth = 0;

// Helpers wrapping Interlocked for readability
inline bool LayoutDepthTryEnter() {
    // Returns true if we successfully entered (depth was 0 before increment)
    return InterlockedIncrement(&g_layoutDepth) == 1;
}
inline void LayoutDepthLeave() {
    InterlockedDecrement(&g_layoutDepth);
}
inline bool LayoutDepthIsZero() {
    return InterlockedAdd(&g_layoutDepth, 0) == 0;
}

// Helper to run code synchronously on the taskbar's UI thread
using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    auto* param = reinterpret_cast<RUN_FROM_WINDOW_THREAD_PARAM*>(cwp->lParam);
                    param->proc(param->procParam);
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param{
        .proc = proc,
        .procParam = procParam,
    };
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, reinterpret_cast<LPARAM>(&param));

    UnhookWindowsHookEx(hook);
    return true;
}

// Queries the actual icon bitmap dimensions for the toolbar (ImageList or DPI-aware metrics)
void GetToolbarIconSize(HWND hToolbar, int& outIconW, int& outIconH) {
    outIconW = 16;
    outIconH = 16;

    // 1. Check ImageList attached to Toolbar
    HIMAGELIST himl = reinterpret_cast<HIMAGELIST>(SendMessage(hToolbar, TB_GETIMAGELIST, 0, 0));
    if (himl && ImageList_GetIconSize(himl, &outIconW, &outIconH) && outIconW > 0 && outIconH > 0) {
        return;
    }

    // 2. Check DPI-aware system metrics
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        using GetSystemMetricsForDpi_t = int(WINAPI*)(int, UINT);
        auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(
            GetProcAddress(user32, "GetDpiForWindow"));
        auto pGetMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpi_t>(
            GetProcAddress(user32, "GetSystemMetricsForDpi"));
        if (pGetDpiForWindow && pGetMetricsForDpi) {
            UINT dpi = pGetDpiForWindow(hToolbar);
            if (dpi == 0) {
                HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
                if (hTray) dpi = pGetDpiForWindow(hTray);
            }
            if (dpi > 0) {
                int cx = pGetMetricsForDpi(SM_CXSMICON, dpi);
                int cy = pGetMetricsForDpi(SM_CYSMICON, dpi);
                if (cx > 0 && cy > 0) {
                    outIconW = cx;
                    outIconH = cy;
                    return;
                }
            }
        }
    }

    // 3. Fallback to standard SM_CXSMICON / SM_CYSMICON
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx > 0 && cy > 0) {
        outIconW = cx;
        outIconH = cy;
    } else {
        outIconW = 16;
        outIconH = 16;
    }
}

// Returns the slot dimensions for a tray icon button.
// btnW = iconW + 2*horizontalPadding on each side.
// btnH = taskbarH / rows (fills full row height, pixel-perfect hot-tracking).
void GetButtonDimensions(HWND hToolbar, int& outWidth, int& outHeight) {
    int iconW = 16, iconH = 16;
    GetToolbarIconSize(hToolbar, iconW, iconH);

    int cxPad = g_settings.horizontalPadding;
    outWidth  = iconW + 2 * cxPad;
    if (outWidth < iconW) outWidth = iconW;

    // Always use Shell_TrayWnd (the full taskbar window) as source of truth for
    // taskbar height.  TrayNotifyWnd is only the right sub-section of the bar and
    // its height can be temporarily wrong during Explorer/ExplorerPatcher startup
    // (e.g. it spans the full screen before ep_taskbar finishes layout), which
    // would cause oversized buttons until the first WM_SIZE fires.
    int taskbarH = 40;
    HWND hTray = GetAncestor(hToolbar, GA_ROOT);  // Shell_TrayWnd or Shell_SecondaryTrayWnd
    if (!hTray) hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rcTray{};
    if (hTray && GetClientRect(hTray, &rcTray) && (rcTray.bottom - rcTray.top) > 8) {
        taskbarH = rcTray.bottom - rcTray.top;
    }

    int divisor = (g_settings.rows > 1) ? g_settings.rows : 1;
    outHeight   = taskbarH / divisor;
    if (outHeight < iconH) outHeight = iconH;
}

int GetVisibleButtonCount(HWND hToolbar) {
    int count = static_cast<int>(SendMessage(hToolbar, TB_BUTTONCOUNT, 0, 0));
    int visible = 0;
    for (int i = 0; i < count; i++) {
        TBBUTTON btn{};
        if (SendMessage(hToolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn))) {
            if (!(btn.fsState & TBSTATE_HIDDEN)) {
                visible++;
            }
        }
    }
    return visible;
}

int GetTrayRightAnchor(HWND hTrayNotify) {
    RECT rcNotify{};
    GetClientRect(hTrayNotify, &rcNotify);
    int rightAnchor = rcNotify.right;

    // Check ControlCenterButton first (action center / gear icon)
    HWND hCC = FindWindowEx(hTrayNotify, nullptr, L"ControlCenterButton", nullptr);
    if (hCC && IsWindowVisible(hCC)) {
        RECT rcCC{};
        GetWindowRect(hCC, &rcCC);
        POINT pt = {rcCC.left, rcCC.top};
        ScreenToClient(hTrayNotify, &pt);
        if (pt.x > 0 && pt.x < rightAnchor) {
            return pt.x;
        }
    }

    // Fallback: TrayInputIndicatorWClass (language / IME button)
    HWND hInput = FindWindowEx(hTrayNotify, nullptr, L"TrayInputIndicatorWClass", nullptr);
    if (hInput && IsWindowVisible(hInput)) {
        RECT rcInput{};
        GetWindowRect(hInput, &rcInput);
        POINT pt = {rcInput.left, rcInput.top};
        ScreenToClient(hTrayNotify, &pt);
        if (pt.x > 0 && pt.x < rightAnchor) {
            return pt.x;
        }
    }

    // Fallback: TrayClockWClass (clock)
    HWND hClock = FindWindowEx(hTrayNotify, nullptr, L"TrayClockWClass", nullptr);
    if (hClock && IsWindowVisible(hClock)) {
        RECT rcClock{};
        GetWindowRect(hClock, &rcClock);
        POINT pt = {rcClock.left, rcClock.top};
        ScreenToClient(hTrayNotify, &pt);
        if (pt.x > 0 && pt.x < rightAnchor) {
            return pt.x;
        }
    }

    return rightAnchor;
}

// Compute the expected pager geometry given visible icon count.
// Returns false if there are no visible icons (caller should skip layout).
bool ComputePagerGeometry(HWND hToolbar, HWND hNotify,
                          int& outPagerX, int& outPagerW,
                          int& outToolbarH, int& outTaskbarH) {
    int visibleCount = GetVisibleButtonCount(hToolbar);
    if (visibleCount <= 0) return false;

    int btnW, btnH;
    GetButtonDimensions(hToolbar, btnW, btnH);
    int cols      = (visibleCount + 1) / 2;
    int expectedW = cols * btnW;
    int expectedH = 2 * btnH; // == taskbarH when rows==2

    // Use Shell_TrayWnd (full taskbar) as the authoritative height source.
    // GetButtonDimensions already does the same, so btnH == taskbarH/rows.
    outTaskbarH = btnH * g_settings.rows;
    // Clamp to actual Shell_TrayWnd height as sanity check.
    HWND hTray = GetAncestor(hToolbar, GA_ROOT);
    if (!hTray) hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rcTray{};
    if (hTray && GetClientRect(hTray, &rcTray) && (rcTray.bottom - rcTray.top) > 8) {
        outTaskbarH = rcTray.bottom - rcTray.top;
    }

    int rightAnchor = GetTrayRightAnchor(hNotify);
    outPagerX       = rightAnchor - expectedW;
    if (outPagerX < 0) outPagerX = 0;
    outPagerW       = expectedW;
    outToolbarH     = expectedH;
    return true;
}

// Forward declarations
void ApplyToolbarSettings(HWND hToolbar, bool isOverflow);
void FindAndConfigureAllTrayToolbars();
void RestoreAllTrayToolbars();

// Balances visible buttons 50/50 and sets TBSTATE_WRAP strictly on the middle visible button
int UpdateRowWrapping(HWND hToolbar) {
    int count = static_cast<int>(SendMessage(hToolbar, TB_BUTTONCOUNT, 0, 0));
    if (count <= 0) {
        g_wrapButtonId = -1;
        return -1;
    }

    // Temporarily reset target wrap ID so TB_SETSTATE doesn't re-inject wrap while clearing
    g_wrapButtonId = -1;

    // 1. Clear WRAP on all buttons
    for (int i = 0; i < count; i++) {
        TBBUTTON btn{};
        if (SendMessage(hToolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn))) {
            if (btn.fsState & TBSTATE_WRAP) {
                SendMessage(hToolbar, TB_SETSTATE, btn.idCommand,
                            MAKELPARAM(btn.fsState & ~TBSTATE_WRAP, 0));
            }
        }
    }

    if (g_settings.rows <= 1) {
        return -1;
    }

    // 2. Collect visible buttons
    int visibleCount = 0;
    int visibleIndices[64];
    int visibleIds[64];

    for (int i = 0; i < count && visibleCount < 64; i++) {
        TBBUTTON btn{};
        if (SendMessage(hToolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn))) {
            if (!(btn.fsState & TBSTATE_HIDDEN)) {
                visibleIndices[visibleCount] = i;
                visibleIds[visibleCount] = btn.idCommand;
                visibleCount++;
            }
        }
    }

    if (visibleCount <= 1) {
        return -1;
    }

    int half = (visibleCount + 1) / 2;
    int wrapIdx = half - 1;
    int wrapBtnId = visibleIds[wrapIdx];

    g_wrapButtonId = wrapBtnId;

    // 3. Set WRAP on that exact middle visible button
    LRESULT state = SendMessage(hToolbar, TB_GETSTATE, wrapBtnId, 0);
    SendMessage(hToolbar, TB_SETSTATE, wrapBtnId, MAKELPARAM(state | TBSTATE_WRAP, 0));

    return wrapBtnId;
}

// Recalculates exact dimensions, positions SysPager and Toolbar, and updates wrapping.
// Guarded by g_layoutDepth to prevent reentrancy loops.
void RelayoutTray(HWND hToolbar) {
    if (!IsWindow(hToolbar) || g_settings.rows <= 1) {
        return;
    }

    // Reentrancy guard: our SetWindowPos calls would otherwise trigger
    // WM_WINDOWPOSCHANGED -> RelayoutTray again infinitely.
    if (!LayoutDepthTryEnter()) {
        return;
    }

    HWND hPager  = GetParent(hToolbar);
    HWND hNotify = hPager ? GetParent(hPager) : nullptr;
    if (!hPager || !hNotify) {
        LayoutDepthLeave();
        return;
    }

    int iconW, iconH;
    GetToolbarIconSize(hToolbar, iconW, iconH);

    int btnW, btnH;
    GetButtonDimensions(hToolbar, btnW, btnH);
    int cxPad = (btnW >= iconW) ? ((btnW - iconW) / 2) : 0;
    int cyPad = (btnH >= iconH) ? ((btnH - iconH) / 2) : 0;

    // 1. Ensure WRAPABLE style
    DWORD curStyle = static_cast<DWORD>(SendMessage(hToolbar, TB_GETSTYLE, 0, 0));
    if (!(curStyle & TBSTYLE_WRAPABLE)) {
        SendMessage(hToolbar, TB_SETSTYLE, 0, curStyle | TBSTYLE_WRAPABLE);
    }

    // 2. Update row wrapping (50/50 balance) BEFORE button sizing
    UpdateRowWrapping(hToolbar);

    // 3. Set bitmap size and padding (DPI-aware and centered)
    SendMessage(hToolbar, 0x0420 /* TB_SETBITMAPSIZE */, 0, MAKELPARAM(iconW, iconH));
    SendMessage(hToolbar, TB_SETPADDING, 0, MAKELPARAM(cxPad, cyPad));

    // 4. Reset button size cache and apply button dimensions
    SendMessage(hToolbar, TB_SETBUTTONSIZE, 0, 0);
    SendMessage(hToolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(btnW, btnH));

    TBMETRICS metrics{};
    metrics.cbSize = sizeof(TBMETRICS);
    metrics.dwMask = TBMF_PAD | TBMF_BARPAD | TBMF_BUTTONSPACING;
    metrics.cxPad = cxPad;
    metrics.cyPad = cyPad;
    metrics.cxBarPad = 0;
    metrics.cyBarPad = 0;
    metrics.cxButtonSpacing = 0;
    metrics.cyButtonSpacing = 0;
    SendMessage(hToolbar, TB_SETMETRICS, 0, reinterpret_cast<LPARAM>(&metrics));

    int pagerX, pagerW, toolbarH, taskbarH;
    if (ComputePagerGeometry(hToolbar, hNotify,
                             pagerX, pagerW, toolbarH, taskbarH)) {
        // 5. Force SysPager to correct position/size.
        //    SWP_NOSENDCHANGING prevents ep_taskbar from seeing this as an
        //    external move and re-triggering its own layout.
        SetWindowPos(hPager, HWND_TOP,
                     pagerX, 0, pagerW, taskbarH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

        // 6. Position toolbar inside pager (fills full height, y=0 — no
        //    vertical offset, so comctl32 hot-tracking is pixel-perfect)
        SetWindowPos(hToolbar, nullptr,
                     0, 0, pagerW, toolbarH,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        // 7. Recalculate pager scroll state (should produce no arrows now)
        SendMessage(hPager, 0x1402 /* PGM_RECALCSIZE */, 0, 0);

        // 8. Explicitly zero-size any scroll-arrow Button children of SysPager
        HWND hScrollBtn = GetWindow(hPager, GW_CHILD);
        while (hScrollBtn) {
            WCHAR cls[32]{};
            GetClassNameW(hScrollBtn, cls, ARRAYSIZE(cls));
            if (_wcsicmp(cls, L"Button") == 0) {
                SetWindowPos(hScrollBtn, nullptr,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            }
            hScrollBtn = GetWindow(hScrollBtn, GW_HWNDNEXT);
        }

        RedrawWindow(hNotify, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    LayoutDepthLeave();
}

// Subclass for TrayNotifyWnd.
//
// WM_SIZE is the critical hook: ep_taskbar/ExplorerPatcher calls SetWindowPos
// on SysPager *inside* its own WM_SIZE handler. By calling DefSubclassProc
// first we let ep_taskbar finish, then we correct the pager position.
LRESULT CALLBACK TrayNotifySubclassProc(HWND hWnd,
                                        UINT uMsg,
                                        WPARAM wParam,
                                        LPARAM lParam,
                                        UINT_PTR uIdSubclass,
                                        DWORD_PTR dwRefData) {
    if (g_unloading) {
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
        // PGN_CALCSIZE: tell pager the exact child dimensions ─────────────────
        case WM_NOTIFY: {
            if (lParam != 0 && g_settings.rows > 1) {
                auto* hdr = reinterpret_cast<NMHDR*>(lParam);
                if (hdr->code == PGN_CALCSIZE) {
                    HWND hPager   = hdr->hwndFrom;
                    HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
                    if (hToolbar) {
                        int visibleCount = GetVisibleButtonCount(hToolbar);
                        if (visibleCount > 0) {
                            int btnW, btnH;
                            GetButtonDimensions(hToolbar, btnW, btnH);
                            int cols      = (visibleCount + 1) / 2;
                            int expectedW = cols * btnW;
                            int expectedH = 2 * btnH;

                            auto* pCalc = reinterpret_cast<NMPGCALCSIZE*>(lParam);
                            if (pCalc->dwFlag == PGF_CALCWIDTH) {
                                pCalc->iWidth = expectedW;
                            } else if (pCalc->dwFlag == PGF_CALCHEIGHT) {
                                pCalc->iHeight = expectedH;
                            }
                            return 0;
                        }
                    }
                }
            }
            break;
        }

        // WM_SIZE: ep_taskbar lays out SysPager here; we correct it after ─────
        case WM_SIZE: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (g_settings.rows > 1 && LayoutDepthIsZero()) {
                HWND hPager = FindWindowEx(hWnd, nullptr, L"SysPager", nullptr);
                if (hPager) {
                    HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
                    if (hToolbar) {
                        RelayoutTray(hToolbar);
                    }
                }
            }
            return res;
        }

        // WM_WINDOWPOSCHANGED: catch DPI / monitor topology changes ───────────
        case WM_WINDOWPOSCHANGED: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (g_settings.rows > 1 && LayoutDepthIsZero()) {
                HWND hPager = FindWindowEx(hWnd, nullptr, L"SysPager", nullptr);
                if (hPager) {
                    HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
                    if (hToolbar) {
                        RelayoutTray(hToolbar);
                    }
                }
            }
            return res;
        }

        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, TrayNotifySubclassProc, uIdSubclass);
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Subclass for SysPager (WC_PAGESCROLLER).
//
// WM_WINDOWPOSCHANGING: intercept every attempt by ep_taskbar to resize the
// pager and clamp it to the correct width.  Only active when g_layoutDepth==0
// (i.e. not during our own RelayoutTray SetWindowPos calls).
//
// PGM_GETBUTTONSTATE: return PGF_INVISIBLE so the pager never draws arrows.
//
// WM_WINDOWPOSCHANGED: hide scroll-arrow Button children (belt-and-suspenders).
LRESULT CALLBACK SysPagerSubclassProc(HWND hWnd,
                                      UINT uMsg,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      UINT_PTR uIdSubclass,
                                      DWORD_PTR dwRefData) {
    if (g_unloading) {
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
        // Suppress scroll arrows at pager-control level ──────────────────────
        case 0x140c /* PGM_GETBUTTONSTATE */: {
            if (g_settings.rows > 1) {
                // PGF_INVISIBLE (4) — pager hides the arrow button entirely
                return 4 /* PGF_INVISIBLE */;
            }
            break;
        }

        // Clamp every incoming resize to our computed width ──────────────────
        case WM_WINDOWPOSCHANGING: {
            if (g_settings.rows > 1 && LayoutDepthIsZero()) {
                HWND hToolbar = FindWindowEx(hWnd, nullptr, L"ToolbarWindow32", nullptr);
                HWND hNotify  = GetParent(hWnd);
                if (hToolbar && hNotify) {
                    int pagerX, pagerW, toolbarH, taskbarH;
                    if (ComputePagerGeometry(hToolbar, hNotify,
                                            pagerX, pagerW, toolbarH, taskbarH)) {
                        auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
                        pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
                        pos->x  = pagerX;
                        pos->y  = 0;
                        pos->cx = pagerW;
                        pos->cy = taskbarH;
                    }
                }
            }
            break;
        }

        // After any position commit, hide scroll-arrow child Button windows ──
        case WM_WINDOWPOSCHANGED: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (g_settings.rows > 1) {
                HWND hBtn = GetWindow(hWnd, GW_CHILD);
                while (hBtn) {
                    WCHAR cls[32]{};
                    GetClassNameW(hBtn, cls, ARRAYSIZE(cls));
                    if (_wcsicmp(cls, L"Button") == 0) {
                        RECT rcBtn{};
                        GetWindowRect(hBtn, &rcBtn);
                        int w = rcBtn.right  - rcBtn.left;
                        int h = rcBtn.bottom - rcBtn.top;
                        if (w > 0 || h > 0) {
                            SetWindowPos(hBtn, nullptr,
                                         0, 0, 0, 0,
                                         SWP_NOMOVE | SWP_NOZORDER |
                                         SWP_NOACTIVATE | SWP_NOSENDCHANGING);
                        }
                    }
                    hBtn = GetWindow(hBtn, GW_HWNDNEXT);
                }
                HWND hNotify = GetParent(hWnd);
                if (hNotify) {
                    RedrawWindow(hNotify, nullptr, nullptr,
                                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
                }
            }
            return res;
        }

        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, SysPagerSubclassProc, uIdSubclass);
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Subclass for ToolbarWindow32 to manage padding, sizing, wrapping, and ideal size
LRESULT CALLBACK TrayToolbarSubclassProc(HWND hWnd,
                                         UINT uMsg,
                                         WPARAM wParam,
                                         LPARAM lParam,
                                         UINT_PTR uIdSubclass,
                                         DWORD_PTR dwRefData) {
    bool isOverflow = (dwRefData != 0);

    if (g_unloading) {
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
        case 0x0463 /* TB_GETIDEALSIZE */: {
            if (!isOverflow && g_settings.rows > 1 && lParam != 0) {
                auto* pSize = reinterpret_cast<SIZE*>(lParam);
                int visibleCount = GetVisibleButtonCount(hWnd);
                if (visibleCount > 0) {
                    int btnW, btnH;
                    GetButtonDimensions(hWnd, btnW, btnH);
                    int cols      = (visibleCount + 1) / 2;
                    int expectedW = cols * btnW;
                    int expectedH = 2 * btnH;

                    pSize->cx = expectedW;
                    pSize->cy = expectedH;
                    return TRUE;
                }
            }
            break;
        }

        case 0x0427 /* TB_SETROWS */: {
            if (!isOverflow && g_settings.rows > 1) {
                wParam = MAKEWPARAM(2, FALSE);
                LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                RelayoutTray(hWnd);
                return res;
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case TB_SETPADDING: {
            if (!isOverflow || g_settings.applyToOverflow) {
                int iconW, iconH;
                GetToolbarIconSize(hWnd, iconW, iconH);
                int btnW, btnH;
                GetButtonDimensions(hWnd, btnW, btnH);
                int cx = (btnW >= iconW) ? ((btnW - iconW) / 2) : 0;
                int cy = (!isOverflow && g_settings.rows > 1)
                    ? ((btnH >= iconH) ? ((btnH - iconH) / 2) : 0)
                    : 2;
                lParam = MAKELPARAM(cx, cy);
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case 0x0420 /* TB_SETBITMAPSIZE */: {
            if (!isOverflow && g_settings.rows > 1) {
                int iconW, iconH;
                GetToolbarIconSize(hWnd, iconW, iconH);
                lParam = MAKELPARAM(iconW, iconH);
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case TB_SETBUTTONSIZE: {
            if (!isOverflow || g_settings.applyToOverflow) {
                int iconW, iconH;
                GetToolbarIconSize(hWnd, iconW, iconH);
                int btnW, btnH;
                GetButtonDimensions(hWnd, btnW, btnH);
                int cxPad = (btnW >= iconW) ? ((btnW - iconW) / 2) : 0;
                int cyPad = (!isOverflow && g_settings.rows > 1)
                    ? ((btnH >= iconH) ? ((btnH - iconH) / 2) : 0)
                    : 2;
                DefSubclassProc(hWnd, 0x0420 /* TB_SETBITMAPSIZE */, 0, MAKELPARAM(iconW, iconH));
                DefSubclassProc(hWnd, TB_SETPADDING, 0, MAKELPARAM(cxPad, cyPad));
                DefSubclassProc(hWnd, TB_SETBUTTONSIZE, 0, 0);
                lParam = MAKELPARAM(btnW, btnH);
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case TB_SETMETRICS: {
            if ((!isOverflow || g_settings.applyToOverflow) && lParam != 0) {
                auto* pMetrics = reinterpret_cast<TBMETRICS*>(lParam);
                int iconW, iconH;
                GetToolbarIconSize(hWnd, iconW, iconH);
                int btnW, btnH;
                GetButtonDimensions(hWnd, btnW, btnH);
                int cx = (btnW >= iconW) ? ((btnW - iconW) / 2) : 0;
                int cy = (!isOverflow && g_settings.rows > 1)
                    ? ((btnH >= iconH) ? ((btnH - iconH) / 2) : 0)
                    : 2;
                if (pMetrics->dwMask & TBMF_PAD) {
                    pMetrics->cxPad = cx;
                    pMetrics->cyPad = cy;
                }
                if (pMetrics->dwMask & TBMF_BARPAD) {
                    pMetrics->cxBarPad = 0;
                    pMetrics->cyBarPad = 0;
                }
                if (pMetrics->dwMask & TBMF_BUTTONSPACING) {
                    pMetrics->cxButtonSpacing = 0;
                    pMetrics->cyButtonSpacing = 0;
                }
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case TB_SETSTATE: {
            if (!isOverflow && g_settings.rows > 1) {
                int btnId      = static_cast<int>(wParam);
                int targetWrap = static_cast<int>(g_wrapButtonId);
                if (targetWrap > 0) {
                    if (btnId == targetWrap) {
                        lParam = MAKELPARAM(LOWORD(lParam) | TBSTATE_WRAP, HIWORD(lParam));
                    } else {
                        lParam = MAKELPARAM(LOWORD(lParam) & ~TBSTATE_WRAP, HIWORD(lParam));
                    }
                }
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case WM_LBUTTONUP: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (!isOverflow && g_settings.rows > 1) {
                RelayoutTray(hWnd);
            }
            return res;
        }

        case TB_AUTOSIZE: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (!isOverflow && g_settings.rows > 1) {
                RelayoutTray(hWnd);
            }
            return res;
        }

        case WM_WINDOWPOSCHANGING: {
            // Guard: only correct when we are not inside RelayoutTray's own SetWindowPos calls
            if (!isOverflow && g_settings.rows > 1 && LayoutDepthIsZero()) {
                int visibleCount = GetVisibleButtonCount(hWnd);
                if (visibleCount > 1) {
                    int btnW, btnH;
                    GetButtonDimensions(hWnd, btnW, btnH);
                    int cols      = (visibleCount + 1) / 2;
                    int expectedW = cols * btnW;
                    int expectedH = 2 * btnH;

                    auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
                    pos->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
                    pos->x  = 0;
                    pos->y  = 0;  // no vertical offset: toolbar fills full pager height
                    pos->cx = expectedW;
                    pos->cy = expectedH;
                }
            }
            break;
        }

        case TB_MOVEBUTTON:
        case TB_INSERTBUTTONA:
        case TB_INSERTBUTTONW:
        case TB_DELETEBUTTON:
        case TB_ADDBUTTONSA:
        case TB_ADDBUTTONSW: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (!isOverflow && g_settings.rows > 1) {
                RelayoutTray(hWnd);
            }
            return res;
        }

        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, TrayToolbarSubclassProc, uIdSubclass);
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void ApplyToolbarSettings(HWND hToolbar, bool isOverflow) {
    if (!IsWindow(hToolbar)) {
        return;
    }

    Wh_Log(L"ApplyToolbarSettings: toolbar=0x%p, isOverflow=%d, rows=%d",
           hToolbar, isOverflow, g_settings.rows);

    SetWindowSubclass(hToolbar, TrayToolbarSubclassProc, SUBCLASS_ID_TOOLBAR,
                      isOverflow ? 1 : 0);

    if (isOverflow && !g_settings.applyToOverflow) {
        return;
    }

    HWND hPager = GetParent(hToolbar);
    HWND hNotify = hPager ? GetParent(hPager) : nullptr;

    if (!isOverflow) {
        if (hPager) {
            SetWindowSubclass(hPager, SysPagerSubclassProc, SUBCLASS_ID_PAGER, 0);
        }
        if (hNotify) {
            SetWindowSubclass(hNotify, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);
        }
    }

    // 1. Style
    DWORD curStyle = static_cast<DWORD>(SendMessage(hToolbar, TB_GETSTYLE, 0, 0));
    if (g_settings.rows > 1 && !isOverflow) {
        if (!(curStyle & TBSTYLE_WRAPABLE)) {
            SendMessage(hToolbar, TB_SETSTYLE, 0, curStyle | TBSTYLE_WRAPABLE);
        }
    } else {
        if (curStyle & TBSTYLE_WRAPABLE) {
            SendMessage(hToolbar, TB_SETSTYLE, 0, curStyle & ~TBSTYLE_WRAPABLE);
        }
    }

    // 2. Padding & Bitmap size
    int iconW, iconH;
    GetToolbarIconSize(hToolbar, iconW, iconH);

    int btnWidth, btnHeight;
    GetButtonDimensions(hToolbar, btnWidth, btnHeight);
    int cxPad = (btnWidth >= iconW) ? ((btnWidth - iconW) / 2) : 0;
    int cyPad = (!isOverflow && g_settings.rows > 1)
        ? ((btnHeight >= iconH) ? ((btnHeight - iconH) / 2) : 0)
        : 2;
    SendMessage(hToolbar, TB_SETPADDING, 0, MAKELPARAM(cxPad, cyPad));
    SendMessage(hToolbar, 0x0420 /* TB_SETBITMAPSIZE */, 0, MAKELPARAM(iconW, iconH));

    // Reset comctl32 button cache before setting new dimensions
    SendMessage(hToolbar, TB_SETBUTTONSIZE, 0, 0);
    SendMessage(hToolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(btnWidth, btnHeight));

    TBMETRICS metrics{};
    metrics.cbSize = sizeof(TBMETRICS);
    metrics.dwMask = TBMF_PAD | TBMF_BARPAD | TBMF_BUTTONSPACING;
    metrics.cxPad = cxPad;
    metrics.cyPad = cyPad;
    metrics.cxBarPad = 0;
    metrics.cyBarPad = 0;
    metrics.cxButtonSpacing = 0;
    metrics.cyButtonSpacing = 0;
    SendMessage(hToolbar, TB_SETMETRICS, 0, reinterpret_cast<LPARAM>(&metrics));

    // 4. Update row wrapping and position
    if (!isOverflow && g_settings.rows > 1) {
        RelayoutTray(hToolbar);
    } else if (g_settings.rows <= 1) {
        UpdateRowWrapping(hToolbar);
        SetWindowPos(hToolbar, nullptr, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (hNotify) {
            RedrawWindow(hNotify, nullptr, nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
    }

    InvalidateRect(hToolbar, nullptr, TRUE);
    UpdateWindow(hToolbar);
}

void UnsubclassToolbar(HWND hToolbar) {
    if (!IsWindow(hToolbar)) {
        return;
    }

    RemoveWindowSubclass(hToolbar, TrayToolbarSubclassProc, SUBCLASS_ID_TOOLBAR);

    HWND hPager = GetParent(hToolbar);
    HWND hNotify = hPager ? GetParent(hPager) : nullptr;
    if (hPager) {
        RemoveWindowSubclass(hPager, SysPagerSubclassProc, SUBCLASS_ID_PAGER);
    }
    if (hNotify) {
        RemoveWindowSubclass(hNotify, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY);
    }

    DWORD curStyle = static_cast<DWORD>(SendMessage(hToolbar, TB_GETSTYLE, 0, 0));
    if (curStyle & TBSTYLE_WRAPABLE) {
        SendMessage(hToolbar, TB_SETSTYLE, 0, curStyle & ~TBSTYLE_WRAPABLE);
    }

    // Clear all wrap states
    int count = static_cast<int>(SendMessage(hToolbar, TB_BUTTONCOUNT, 0, 0));
    for (int i = 0; i < count; i++) {
        TBBUTTON btn{};
        if (SendMessage(hToolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn))) {
            if (btn.fsState & TBSTATE_WRAP) {
                SendMessage(hToolbar, TB_SETSTATE, btn.idCommand,
                            MAKELPARAM(btn.fsState & ~TBSTATE_WRAP, 0));
            }
        }
    }

    // Restore standard Explorer metrics & position
    int iconW = 16, iconH = 16;
    GetToolbarIconSize(hToolbar, iconW, iconH);
    SetWindowPos(hToolbar, nullptr, 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(hToolbar, 0x0420 /* TB_SETBITMAPSIZE */, 0, MAKELPARAM(iconW, iconH));
    SendMessage(hToolbar, TB_SETPADDING, 0, MAKELPARAM(0, 0));
    SendMessage(hToolbar, TB_SETBUTTONSIZE, 0, 0);
    SendMessage(hToolbar, TB_SETROWS, MAKEWPARAM(1, FALSE), 0);
    SendMessage(hToolbar, TB_AUTOSIZE, 0, 0);
    InvalidateRect(hToolbar, nullptr, TRUE);

    if (hNotify) {
        RedrawWindow(hNotify, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void ConfigureTrayInTaskbar(HWND hTaskbar) {
    if (!hTaskbar) {
        return;
    }

    HWND hTrayNotify = FindWindowEx(hTaskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!hTrayNotify) {
        return;
    }

    SetWindowSubclass(hTrayNotify, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);

    HWND hPager = FindWindowEx(hTrayNotify, nullptr, L"SysPager", nullptr);
    if (hPager) {
        SetWindowSubclass(hPager, SysPagerSubclassProc, SUBCLASS_ID_PAGER, 0);
        HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
        while (hToolbar) {
            ApplyToolbarSettings(hToolbar, false);
            hToolbar = FindWindowEx(hPager, hToolbar, L"ToolbarWindow32", nullptr);
        }
    }

    HWND hDirectToolbar = FindWindowEx(hTrayNotify, nullptr, L"ToolbarWindow32", nullptr);
    while (hDirectToolbar) {
        int count = static_cast<int>(SendMessage(hDirectToolbar, TB_BUTTONCOUNT, 0, 0));
        if (count > 0) {
            ApplyToolbarSettings(hDirectToolbar, false);
        }
        hDirectToolbar = FindWindowEx(hTrayNotify, hDirectToolbar, L"ToolbarWindow32", nullptr);
    }
}

void ConfigureOverflowWindow() {
    HWND hOverflow = FindWindow(L"NotifyIconOverflowWindow", nullptr);
    if (hOverflow) {
        HWND hPager = FindWindowEx(hOverflow, nullptr, L"SysPager", nullptr);
        HWND hParent = hPager ? hPager : hOverflow;

        HWND hToolbar = FindWindowEx(hParent, nullptr, L"ToolbarWindow32", nullptr);
        while (hToolbar) {
            ApplyToolbarSettings(hToolbar, true);
            hToolbar = FindWindowEx(hParent, hToolbar, L"ToolbarWindow32", nullptr);
        }
    }
}

void FindAndConfigureAllTrayToolbars() {
    Wh_Log(L"FindAndConfigureAllTrayToolbars running on UI thread");

    HWND hPrimaryTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hPrimaryTaskbar) {
        ConfigureTrayInTaskbar(hPrimaryTaskbar);
    }

    EnumWindows([](HWND hWnd, LPARAM) -> BOOL {
        WCHAR className[64];
        if (GetClassName(hWnd, className, ARRAYSIZE(className)) &&
            wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            ConfigureTrayInTaskbar(hWnd);
        }
        return TRUE;
    }, 0);

    ConfigureOverflowWindow();
}

void RestoreAllTrayToolbars() {
    Wh_Log(L"Restoring all tray toolbars");

    auto RestoreTrayInTaskbar = [](HWND hTaskbar) {
        HWND hTrayNotify = FindWindowEx(hTaskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (!hTrayNotify) return;

        RemoveWindowSubclass(hTrayNotify, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY);

        HWND hPager = FindWindowEx(hTrayNotify, nullptr, L"SysPager", nullptr);
        if (hPager) {
            HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
            while (hToolbar) {
                UnsubclassToolbar(hToolbar);
                hToolbar = FindWindowEx(hPager, hToolbar, L"ToolbarWindow32", nullptr);
            }
        }
    };

    HWND hPrimaryTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hPrimaryTaskbar) {
        RestoreTrayInTaskbar(hPrimaryTaskbar);
    }

    EnumWindows([](HWND hWnd, LPARAM) -> BOOL {
        WCHAR className[64];
        if (GetClassName(hWnd, className, ARRAYSIZE(className)) &&
            wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            HWND hTrayNotify = FindWindowEx(hWnd, nullptr, L"TrayNotifyWnd", nullptr);
            if (hTrayNotify) {
                RemoveWindowSubclass(hTrayNotify, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY);
                HWND hPager = FindWindowEx(hTrayNotify, nullptr, L"SysPager", nullptr);
                if (hPager) {
                    HWND hToolbar = FindWindowEx(hPager, nullptr, L"ToolbarWindow32", nullptr);
                    while (hToolbar) {
                        UnsubclassToolbar(hToolbar);
                        hToolbar = FindWindowEx(hPager, hToolbar, L"ToolbarWindow32", nullptr);
                    }
                }
            }
        }
        return TRUE;
    }, 0);

    HWND hOverflow = FindWindow(L"NotifyIconOverflowWindow", nullptr);
    if (hOverflow) {
        HWND hToolbar = FindWindowEx(hOverflow, nullptr, L"ToolbarWindow32", nullptr);
        while (hToolbar) {
            UnsubclassToolbar(hToolbar);
            hToolbar = FindWindowEx(hOverflow, hToolbar, L"ToolbarWindow32", nullptr);
        }
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hWnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                        dwStyle, X, Y, nWidth, nHeight,
                                        hWndParent, hMenu, hInstance, lpParam);
    if (!hWnd || g_unloading) {
        return hWnd;
    }

    if ((reinterpret_cast<ULONG_PTR>(lpClassName) & ~static_cast<ULONG_PTR>(0xFFFF)) != 0) {
        if (_wcsicmp(lpClassName, L"ToolbarWindow32") == 0) {
            if (hWndParent) {
                WCHAR parentClass[64]{};
                if (GetClassName(hWndParent, parentClass, ARRAYSIZE(parentClass))) {
                    if (_wcsicmp(parentClass, L"SysPager") == 0) {
                        HWND hGrandParent = GetParent(hWndParent);
                        WCHAR grandParentClass[64]{};
                        if (hGrandParent && GetClassName(hGrandParent, grandParentClass, ARRAYSIZE(grandParentClass))) {
                            if (_wcsicmp(grandParentClass, L"TrayNotifyWnd") == 0) {
                                Wh_Log(L"New tray toolbar created under SysPager: 0x%p", hWnd);
                                SetWindowSubclass(hGrandParent, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);
                                SetWindowSubclass(hWndParent, SysPagerSubclassProc, SUBCLASS_ID_PAGER, 0);
                                ApplyToolbarSettings(hWnd, false);
                            } else if (_wcsicmp(grandParentClass, L"NotifyIconOverflowWindow") == 0) {
                                Wh_Log(L"New overflow toolbar created under SysPager: 0x%p", hWnd);
                                ApplyToolbarSettings(hWnd, true);
                            }
                        }
                    } else if (_wcsicmp(parentClass, L"TrayNotifyWnd") == 0) {
                        Wh_Log(L"New tray toolbar created directly under TrayNotifyWnd: 0x%p", hWnd);
                        SetWindowSubclass(hWndParent, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);
                        ApplyToolbarSettings(hWnd, false);
                    } else if (_wcsicmp(parentClass, L"NotifyIconOverflowWindow") == 0) {
                        Wh_Log(L"New overflow toolbar created directly under NotifyIconOverflowWindow: 0x%p", hWnd);
                        ApplyToolbarSettings(hWnd, true);
                    }
                }
            }
        } else if (_wcsicmp(lpClassName, L"SysPager") == 0) {
            if (hWndParent) {
                WCHAR parentClass[64]{};
                if (GetClassName(hWndParent, parentClass, ARRAYSIZE(parentClass)) &&
                    _wcsicmp(parentClass, L"TrayNotifyWnd") == 0) {
                    Wh_Log(L"New SysPager created under TrayNotifyWnd: 0x%p", hWnd);
                    SetWindowSubclass(hWndParent, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);
                    SetWindowSubclass(hWnd, SysPagerSubclassProc, SUBCLASS_ID_PAGER, 0);
                }
            }
        } else if (_wcsicmp(lpClassName, L"TrayNotifyWnd") == 0) {
            Wh_Log(L"New TrayNotifyWnd created: 0x%p", hWnd);
            SetWindowSubclass(hWnd, TrayNotifySubclassProc, SUBCLASS_ID_NOTIFY, 0);
        }
    }

    return hWnd;
}

void LoadSettings() {
    g_settings.rows = Wh_GetIntSetting(L"rows");
    if (g_settings.rows <= 0) {
        g_settings.rows = 2; // Default to 2 rows
    }

    g_settings.horizontalPadding = Wh_GetIntSetting(L"horizontalPadding");
    if (g_settings.horizontalPadding < 0) {
        g_settings.horizontalPadding = 2; // Default 2px
    }

    g_settings.applyToOverflow = Wh_GetIntSetting(L"applyToOverflow") != 0;

    Wh_Log(L"Settings: rows=%d, horizontalPadding=%d, applyToOverflow=%d",
           g_settings.rows, g_settings.horizontalPadding, g_settings.applyToOverflow);
}

void ApplySettingsToAllTaskbars() {
    HWND hPrimaryTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hPrimaryTaskbar) {
        RunFromWindowThread(
            hPrimaryTaskbar,
            [](void*) {
                FindAndConfigureAllTrayToolbars();
            },
            nullptr);
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing Taskbar Tray Spacing & 2 Rows (Windows 10 / ExplorerPatcher) mod v1.6.2");

    LoadSettings();

    HMODULE user32 = GetModuleHandle(L"user32.dll");
    if (user32) {
        auto pCreateWindowExW = reinterpret_cast<CreateWindowExW_t>(
            GetProcAddress(user32, "CreateWindowExW"));
        if (pCreateWindowExW) {
            WindhawkUtils::SetFunctionHook(pCreateWindowExW,
                                           CreateWindowExW_Hook,
                                           &CreateWindowExW_Original);
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"Wh_ModAfterInit");
    ApplySettingsToAllTaskbars();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"Wh_ModBeforeUninit");
    g_unloading = true;
    RestoreAllTrayToolbars();
}

void Wh_ModUninit() {
    Wh_Log(L"Wh_ModUninit");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Wh_ModSettingsChanged");
    LoadSettings();
    ApplySettingsToAllTaskbars();
}
