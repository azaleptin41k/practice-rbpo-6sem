#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <string>
#include "resource.h"
#include "rbpo_rpc_h.h"
#include "rbpo_rpc_constants.h"

#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t APP_MUTEX_NAME[] = L"Local\\RBPO_TrayApp_SingleInstance";
static const wchar_t WINDOW_CLASS[]   = L"RBPOTrayAppClass";
static const wchar_t WINDOW_TITLE[]   = L"РБПО — Tray Application";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE       g_hInstance       = nullptr;
static HWND            g_hWnd            = nullptr;
static NOTIFYICONDATAW g_nid             = {};
static UINT            WM_TASKBARCREATED = 0;
static HANDLE          g_hMutex          = nullptr;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void AddTrayIcon(HWND hWnd);
static void RemoveTrayIcon();
static void ShowTrayContextMenu(HWND hWnd);
static void ShowMainWindow();
static void HideMainWindow();

// ---------------------------------------------------------------------------
// RPC memory allocation (required by the RPC runtime)
// ---------------------------------------------------------------------------
void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
void  __RPC_USER midl_user_free(void* p)         { free(p); }

// ---------------------------------------------------------------------------
// Diagnostic log — writes to rbpo-app.log next to the exe
// ---------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    static char logPath[MAX_PATH] = {};
    if (!logPath[0]) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string p(exePath);
        auto pos = p.find_last_of("\\/");
        if (pos != std::string::npos) p = p.substr(0, pos + 1);
        p += "rbpo-app.log";
        strncpy_s(logPath, p.c_str(), _TRUNCATE);
    }
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ---------------------------------------------------------------------------
// Service helpers
// ---------------------------------------------------------------------------

static bool IsServiceRunning()
{
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, RBPO_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hSCM); return false; }

    SERVICE_STATUS status = {};
    QueryServiceStatus(hSvc, &status);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);

    return status.dwCurrentState == SERVICE_RUNNING;
}

static bool StartServiceAndWait()
{
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, RBPO_SERVICE_NAME,
                                   SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hSCM); return false; }

    if (!StartServiceW(hSvc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(hSvc);
            CloseServiceHandle(hSCM);
            return false;
        }
    }

    SERVICE_STATUS status = {};
    for (int i = 0; i < 60; i++) {
        QueryServiceStatus(hSvc, &status);
        if (status.dwCurrentState == SERVICE_RUNNING) break;
        Sleep(500);
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return status.dwCurrentState == SERVICE_RUNNING;
}

static DWORD GetParentProcessId()
{
    DWORD pid  = GetCurrentProcessId();
    DWORD ppid = 0;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return ppid;
}

static bool IsParentService()
{
    DWORD pid  = GetCurrentProcessId();
    DWORD ppid = 0;
    wchar_t parentExe[MAX_PATH] = {};

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    if (ppid == 0) { CloseHandle(hSnap); return false; }

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == ppid) {
                wcscpy_s(parentExe, pe.szExeFile);
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);

    bool result = (parentExe[0] && _wcsicmp(parentExe, RBPO_SERVICE_EXE_NAME) == 0);
    Log("  IsParentService: ppid=%u, parentExe='%ls', expected='%ls', match=%d",
        ppid, parentExe, RBPO_SERVICE_EXE_NAME, result);
    return result;
}

static void StopServiceViaRpc()
{
    RPC_WSTR stringBinding = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(RBPO_RPC_ENDPOINT)),
        nullptr,
        &stringBinding);

    if (status != RPC_S_OK) return;

    status = RpcBindingFromStringBindingW(stringBinding, &hRBPOServiceBinding);
    RpcStringFreeW(&stringBinding);

    if (status != RPC_S_OK) return;

    RpcTryExcept {
        RBPOService_Stop();
    }
    RpcExcept(1) {
    }
    RpcEndExcept

    RpcBindingFree(&hRBPOServiceBinding);
}

// ===========================================================================
// Entry point
// ===========================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int)
{
    Log("=== rbpo-app started (PID=%u) ===", GetCurrentProcessId());

    bool svcRunning = IsServiceRunning();
    Log("IsServiceRunning = %d", svcRunning);
    if (!svcRunning) {
        bool started = StartServiceAndWait();
        Log("StartServiceAndWait = %d, exiting", started);
        return 0;
    }

    DWORD ppid = GetParentProcessId();
    bool parentIsSvc = IsParentService();
    Log("ParentPID=%u, IsParentService=%d", ppid, parentIsSvc);
    if (!parentIsSvc) {
        Log("Parent is not the service, exiting");
        return 0;
    }

    g_hMutex = CreateMutexW(nullptr, TRUE, APP_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    g_hInstance = hInstance;
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc  = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hIcon          = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName   = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName  = WINDOW_CLASS;
    wc.hIconSm        = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    if (!wc.hIcon)   wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        CloseHandle(g_hMutex);
        return 1;
    }

    g_hWnd = CreateWindowExW(
        0, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 400,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        CloseHandle(g_hMutex);
        return 1;
    }

    AddTrayIcon(g_hWnd);

    bool startSilent = pCmdLine && wcsstr(pCmdLine, L"--silent");
    if (!startSilent) {
        ShowMainWindow();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    ReleaseMutex(g_hMutex);
    CloseHandle(g_hMutex);

    return static_cast<int>(msg.wParam);
}

// ===========================================================================
// Window procedure
// ===========================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TASKBARCREATED) {
        AddTrayIcon(hWnd);
        return 0;
    }

    switch (message) {

    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            ShowMainWindow();
            break;
        case WM_RBUTTONUP:
            ShowTrayContextMenu(hWnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN:
            ShowMainWindow();
            break;
        case ID_TRAY_EXIT:
            StopServiceViaRpc();
            DestroyWindow(hWnd);
            break;
        case ID_FILE_EXIT:
            StopServiceViaRpc();
            DestroyWindow(hWnd);
            break;
        }
        return 0;

    case WM_CLOSE:
        HideMainWindow();
        return 0;

    case WM_CREATE: {
        HFONT hFont = CreateFontW(
            20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        HWND hLabel = CreateWindowExW(0, L"STATIC",
            L"Приложение работает в области уведомлений.\r\n"
            L"Закройте это окно — приложение продолжит работу в фоне.",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 0, 0,
            hWnd, nullptr, g_hInstance, nullptr);

        if (hFont && hLabel)
            SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)hLabel);
        return 0;
    }

    case WM_SIZE: {
        HWND hLabel = (HWND)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (hLabel) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            MoveWindow(hLabel, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ===========================================================================
// Tray icon helpers
// ===========================================================================
static void AddTrayIcon(HWND hWnd)
{
    g_nid.cbSize            = sizeof(g_nid);
    g_nid.hWnd              = hWnd;
    g_nid.uID               = 1;
    g_nid.uFlags            = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage  = WM_TRAYICON;
    g_nid.hIcon             = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"РБПО Tray Application");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Открыть");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

static void ShowMainWindow()
{
    ShowWindow(g_hWnd, SW_SHOW);
    ShowWindow(g_hWnd, SW_RESTORE);
    SetForegroundWindow(g_hWnd);
}

static void HideMainWindow()
{
    ShowWindow(g_hWnd, SW_HIDE);
}
