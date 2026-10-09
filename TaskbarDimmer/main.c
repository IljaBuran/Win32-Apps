#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// this needs to be here because Microslop's incompetent and can't manage their own macros
#ifndef NTDDI_WIN10_GE
#define NTDDI_WIN10_GE 0x0A000010  
#endif
#include <Shellapi.h>

#define global_var    static
#define local_persist static
#define internal      static 

#define APP_WIN_CLASS_NAME TEXT("DimmerWindowClass")
#define TIMER_ID 1
#define TRANSITION_TIME_MS 1000
#define DIMMER_OPACITY 128

// globals
global_var HWND g_dimmerWindowHandle = NULL;
global_var HWND g_taskbarWindowHandle = NULL;
global_var RECT g_taskbarRect;
global_var BOOL g_isHovered = FALSE;
global_var BOOL g_shouldRecalculate = TRUE;
global_var BYTE g_currentOpacity = DIMMER_OPACITY;
global_var BOOL g_needsToGoOnTop = FALSE;
global_var ULONGLONG g_lastOpacityTick = 0;

global_var HWINEVENTHOOK g_foregroundHook = NULL;
global_var HWINEVENTHOOK g_locationHook = NULL;

global_var BOOL g_wasFullscreen = FALSE;

static void CheckFullscreenState(void)
{
    HWND foreground = GetForegroundWindow();

    BOOL isFullscreen = IsWindowFullscreen(foreground);

    if (g_wasFullscreen && !isFullscreen)
    {
        // The user has just returned from fullscreen.
        OutputDebugString(TEXT("LEFT FULLSCREEN\n"));

        // Re-show/reposition your taskbar dimmer here.
        // RestoreTaskbarDimmer();
    }

    g_wasFullscreen = isFullscreen;
}

static BOOL IsWindowFullscreen(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return FALSE;

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return FALSE;

    RECT windowRect;
    if (!GetWindowRect(hwnd, &windowRect))
        return FALSE;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi = {
        .cbSize = sizeof(mi)
    };

    if (!GetMonitorInfo(monitor, &mi))
        return FALSE;

    /*
        Compare against rcMonitor, NOT rcWork.
        rcWork excludes the taskbar.
    */
    return
        windowRect.left <= mi.rcMonitor.left &&
        windowRect.top <= mi.rcMonitor.top &&
        windowRect.right >= mi.rcMonitor.right &&
        windowRect.bottom >= mi.rcMonitor.bottom;
}

static void CALLBACK WinEventProc(
    HWINEVENTHOOK hook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD eventThread,
    DWORD eventTime)
{
    if (event == EVENT_SYSTEM_FOREGROUND)
    {
        CheckFullscreenState();
        return;
    }

    if (event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        // We only care about actual windows, not controls/carets/etc.
        if (idObject != OBJID_WINDOW)
            return;

        // Only care if the changed window is currently foreground.
        if (hwnd != GetForegroundWindow())
            return;

        CheckFullscreenState();
    }
}

// function definitions
internal void SetUp(HINSTANCE hInstance);
internal inline BOOL IsHovered(void);
internal LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
internal inline BYTE CalculateOpacity(void);

INT WINAPI wWinMain(
    HINSTANCE hInstance, 
    HINSTANCE hPrevInstance, 
    PWSTR pCmdLine, 
    INT nCmdShow
)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // this restricts usage of this application to Windows 10 (v1703) and beyond
    // but v1703 was released in 2017...
    // maybe i'll handle previous versions later, when Microslop forces me to use windows 7
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetUp(hInstance);
    ShowWindow(g_dimmerWindowHandle, SW_SHOW);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

LRESULT WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps = { 0 };
            HDC hdc = BeginPaint(hWnd, &ps);

            FillRect(hdc, &ps.rcPaint, GetStockObject(BLACK_BRUSH));

            EndPaint(hWnd, &ps);

            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == TIMER_ID)
            {
                BOOL currHover = IsHovered();

                if (currHover != g_isHovered)
                {
                    g_isHovered = currHover;
                    g_shouldRecalculate = TRUE;
                    g_lastOpacityTick = GetTickCount64();

                    if (!g_isHovered)
                        g_needsToGoOnTop = TRUE;
                }

                if (g_shouldRecalculate)
                {
                    BYTE oldOpacity = g_currentOpacity;

                    CalculateOpacity();

                    if (oldOpacity != g_currentOpacity)
                    {
                        SetLayeredWindowAttributes(
                            g_dimmerWindowHandle,
                            0,
                            g_currentOpacity,
                            LWA_ALPHA
                        );
                    }
                }

                if (g_needsToGoOnTop &&
                    g_popupMenuDepth == 0)
                {
                    SetWindowPos(
                        g_dimmerWindowHandle,
                        HWND_TOP,
                        0, 0, 0, 0,
                        SWP_NOSIZE |
                        SWP_NOMOVE |
                        SWP_NOACTIVATE |
                        SWP_NOSENDCHANGING |
                        SWP_NOOWNERZORDER
                    );

                    g_needsToGoOnTop = FALSE;
                }

                return 0;
            }

            break;
        }

        case WM_DESTROY:
        {
            if (g_menuHook != NULL)
            {
                UnhookWinEvent(g_menuHook);
                g_menuHook = NULL;
            }

            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// LERP
internal BYTE CalculateOpacity(void)
{
    ULONGLONG currentTickCount = GetTickCount64();

    if (g_lastOpacityTick == 0)
    {
        g_lastOpacityTick = currentTickCount;
        return g_currentOpacity;
    }

    ULONGLONG elapsed =
        currentTickCount - g_lastOpacityTick;

    g_lastOpacityTick = currentTickCount;

    BYTE goal =
        g_isHovered
        ? 0
        : DIMMER_OPACITY;

    if (g_currentOpacity == goal)
    {
        g_shouldRecalculate = FALSE;

        return g_currentOpacity;
    }

    UINT delta =
        (UINT)(
            ((ULONGLONG)DIMMER_OPACITY * elapsed)
            / TRANSITION_TIME_MS
        );

    if (delta == 0)
        delta = 1;

    if (goal > g_currentOpacity)
    {
        UINT opacity =
            (UINT)g_currentOpacity + delta;

        if (opacity > goal)
            opacity = goal;

        g_currentOpacity = (BYTE)opacity;
    }
    else
    {
        INT opacity =
            (INT)g_currentOpacity - (INT)delta;

        if (opacity < goal)
            opacity = goal;

        g_currentOpacity = (BYTE)opacity;
    }

    return g_currentOpacity;
}


void SetUp(HINSTANCE hInstance)
{
    g_taskbarWindowHandle = FindWindow(TEXT("Shell_TrayWnd"), NULL);

    if (g_taskbarWindowHandle == NULL)
    {
        ExitProcess(GetLastError());
    }

    HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    HWINEVENTHOOK locationHook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    WNDCLASSEX windowClass =
    {
        .cbSize = sizeof(WNDCLASSEX),
        .style = 0,
        .lpfnWndProc = WndProc,
        .cbClsExtra = 0,
        .cbWndExtra = 0,
        .hInstance = hInstance,
        .hIcon = NULL,
        .hCursor = NULL,
        .hbrBackground = NULL,
        .lpszMenuName = NULL,
        .lpszClassName = APP_WIN_CLASS_NAME,
        .hIconSm = NULL,
    };

    if (RegisterClassEx(&windowClass) == (ATOM)0)
    {
        ExitProcess(GetLastError());
    }

    APPBARDATA appBarData =
    {
        .cbSize = sizeof(appBarData)
    };

    if (!(BOOL)SHAppBarMessage(ABM_GETTASKBARPOS, &appBarData))
    {
        ExitProcess(GetLastError());
    }

    g_taskbarRect.left = appBarData.rc.left;
    g_taskbarRect.top = appBarData.rc.top;
    g_taskbarRect.right = appBarData.rc.right;
    g_taskbarRect.bottom = appBarData.rc.bottom;

    g_dimmerWindowHandle = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        APP_WIN_CLASS_NAME,
        NULL,
        WS_POPUP,
        g_taskbarRect.left,
        g_taskbarRect.top,
        g_taskbarRect.right - g_taskbarRect.left,
        g_taskbarRect.bottom - g_taskbarRect.top,
        g_taskbarWindowHandle,
        NULL,
        hInstance,
        NULL
    );

    if (g_dimmerWindowHandle == NULL)
    {
        ExitProcess(GetLastError());
    }

    SetLayeredWindowAttributes(
        g_dimmerWindowHandle,
        0,
        DIMMER_OPACITY,
        LWA_ALPHA
    );

    SetTimer(
        g_dimmerWindowHandle,
        TIMER_ID,
        50,
        NULL
    );
}

inline BOOL IsHovered(void)
{
    POINT cursorPt;

    if (!GetCursorPos(&cursorPt))
        return FALSE;

    return PtInRect(&g_taskbarRect, cursorPt);
}