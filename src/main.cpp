/**
 * РБПО — Tray Application (zad-2)
 *
 * Win32 tray + Windows service integration: RPC stop, parent checks.
 * GUI aligned with zad-1 (resources, --silent).
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdlib>

#include "resource.h"
#include "tray_config.h"

extern "C" {
#include "TrayRpc_h.h"
}

extern "C" void* __RPC_USER midl_user_allocate(size_t size) {
  return std::malloc(size);
}

extern "C" void __RPC_USER midl_user_free(void* p) {
  std::free(p);
}

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

// ---------------------------------------------------------------------------
// Constants (zad-1 UI)
// ---------------------------------------------------------------------------
static const wchar_t WINDOW_CLASS[] = L"RBPOTrayAppClass";
static const wchar_t WINDOW_TITLE[] = L"РБПО — Tray Application (zad-2)";

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

static DWORD GetParentProcessId();
static bool QueryServiceProcessId(DWORD* outPid);
static void BootstrapStartServiceIfStopped();
static void RequireParentIsTrayService();
static void StopServiceThroughRpc();

// ===========================================================================
// Service helpers (zad-2)
// ===========================================================================

static DWORD GetParentProcessId() {
  const DWORD self = GetCurrentProcessId();
  DWORD parent = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return 0;
  }
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == self) {
        parent = pe.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return parent;
}

static bool QueryServiceProcessId(DWORD* outPid) {
  *outPid = 0;
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, TRAY_SERVICE_NAME, SERVICE_QUERY_STATUS);
  CloseServiceHandle(scm);
  if (!svc) {
    return false;
  }
  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes = 0;
  const BOOL ok = QueryServiceStatusEx(
      svc,
      SC_STATUS_PROCESS_INFO,
      reinterpret_cast<LPBYTE>(&ssp),
      sizeof(ssp),
      &bytes);
  CloseServiceHandle(svc);
  if (!ok) {
    return false;
  }
  *outPid = ssp.dwProcessId;
  return true;
}

static void BootstrapStartServiceIfStopped() {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    ExitProcess(0);
  }
  SC_HANDLE svc = OpenServiceW(scm, TRAY_SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_START);
  CloseServiceHandle(scm);
  if (!svc) {
    ExitProcess(0);
  }

  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes = 0;
  if (!QueryServiceStatusEx(
          svc,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&ssp),
          sizeof(ssp),
          &bytes)) {
    CloseServiceHandle(svc);
    ExitProcess(0);
  }

  if (ssp.dwCurrentState == SERVICE_STOPPED) {
    if (!StartServiceW(svc, 0, nullptr)) {
      CloseServiceHandle(svc);
      ExitProcess(0);
    }
    const DWORD deadline = GetTickCount() + 120000;
    for (;;) {
      if (!QueryServiceStatusEx(
              svc,
              SC_STATUS_PROCESS_INFO,
              reinterpret_cast<LPBYTE>(&ssp),
              sizeof(ssp),
              &bytes)) {
        CloseServiceHandle(svc);
        ExitProcess(0);
      }
      if (ssp.dwCurrentState == SERVICE_RUNNING) {
        CloseServiceHandle(svc);
        ExitProcess(0);
      }
      if (GetTickCount() >= deadline) {
        CloseServiceHandle(svc);
        ExitProcess(0);
      }
      Sleep(200);
    }
  }

  CloseServiceHandle(svc);
}

static void RequireParentIsTrayService() {
  DWORD servicePid = 0;
  if (!QueryServiceProcessId(&servicePid) || servicePid == 0) {
    ExitProcess(0);
  }
  const DWORD parentPid = GetParentProcessId();
  if (parentPid == 0 || parentPid != servicePid) {
    ExitProcess(0);
  }
}

static void StopServiceThroughRpc() {
  RPC_WSTR stringBinding = nullptr;
  RpcStringBindingComposeW(
      nullptr,
      reinterpret_cast<RPC_WSTR>(L"ncalrpc"),
      nullptr,
      reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(TRAY_RPC_ENDPOINT)),
      nullptr,
      &stringBinding);

  handle_t binding = nullptr;
  if (stringBinding) {
    if (RpcBindingFromStringBindingW(stringBinding, &binding) != RPC_S_OK) {
      binding = nullptr;
    }
    RpcStringFreeW(&stringBinding);
  }

  if (!binding) {
    return;
  }

  RpcTryExcept {
    RequestStop(binding);
  }
  RpcExcept(1) {
  }
  RpcEndExcept

  RpcBindingFree(&binding);
}

// ===========================================================================
// Entry point
// ===========================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int)
{
    g_hMutex = CreateMutexW(nullptr, TRUE, TRAY_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    BootstrapStartServiceIfStopped();
    RequireParentIsTrayService();

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
        case ID_FILE_EXIT:
            RemoveTrayIcon();
            StopServiceThroughRpc();
            return 0;
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
    wcscpy_s(g_nid.szTip, L"РБПО Tray — zad-2");

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
