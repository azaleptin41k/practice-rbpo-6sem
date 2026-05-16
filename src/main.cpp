/**
 * РБПО — Tray Application (task 1.3)
 *
 * Win32 GUI client. Communicates with the RBPO Windows Service via RPC (ALPC).
 * Supports login, logout, product activation, and license status display.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <ctime>
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

// UI panes
enum class Pane { None = 0, Login, Activate, Licensed };

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE       g_hInstance       = nullptr;
static HWND            g_hWnd            = nullptr;
static NOTIFYICONDATAW g_nid             = {};
static UINT            WM_TASKBARCREATED = 0;
static HANDLE          g_hMutex          = nullptr;
static HFONT           g_hFont           = nullptr;

static Pane            g_currentPane     = Pane::None;
static bool            g_rpcBound        = false;

// login pane controls
static HWND h_loginEmail = nullptr, h_loginPass = nullptr,
            h_loginBtn = nullptr,   h_loginStatus = nullptr;
// main pane controls
static HWND h_userLbl = nullptr,   h_licLbl = nullptr,
            h_actKey  = nullptr,   h_actBtn = nullptr,
            h_actStatus = nullptr, h_avBtn = nullptr,
            h_logoutBtn = nullptr;
// AV scanning controls (licensed pane)
static HWND h_avDbLbl = nullptr, h_scanFileBtn = nullptr, h_scanDirBtn = nullptr;
static HWND h_scanAllDrivesBtn = nullptr;
static HWND h_schedPathEdit = nullptr, h_schedIntvEdit = nullptr;
static HWND h_schedSetBtn = nullptr, h_schedClearBtn = nullptr, h_schedResultsBtn = nullptr;
static HWND h_monPathEdit = nullptr, h_monAddBtn = nullptr, h_monRemoveBtn = nullptr;
static HWND h_monResultsBtn = nullptr;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void AddTrayIcon(HWND hWnd);
static void RemoveTrayIcon();
static void ShowTrayContextMenu(HWND hWnd);
static void ShowMainWindow();
static void HideMainWindow();

static bool BindRpc();
static void UnbindRpc();
static void RefreshUI();
static void DestroyAllPanes();
static void BuildLoginPane(HWND hWnd);
static void BuildMainPane(HWND hWnd, bool licensed,
                          const std::wstring& userText,
                          const std::wstring& licText);
static void DoLogin();
static void DoLogout();
static void DoActivate();
static void DoScanFile();
static void DoScanDirectory();
static void DoScanAllDrives();
static void DoSetSchedule();
static void DoClearSchedule();
static void DoGetScheduleResults();
static void DoAddMonitor();
static void DoRemoveMonitor();
static void DoGetMonitorResults();

// ---------------------------------------------------------------------------
// RPC memory allocation
// ---------------------------------------------------------------------------
void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
void  __RPC_USER midl_user_free(void* p)         { free(p); }

// ---------------------------------------------------------------------------
// Diagnostic log
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
// Service helpers (unchanged from task 1.2)
// ---------------------------------------------------------------------------
static bool IsServiceRunning()
{
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        Log("OpenSCManager failed, err=%lu", GetLastError());
        return false;
    }

    SC_HANDLE hSvc = OpenServiceW(hSCM, RBPO_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) {
        Log("OpenService(%ls) failed, err=%lu", RBPO_SERVICE_NAME, GetLastError());
        CloseServiceHandle(hSCM);
        return false;
    }

    SERVICE_STATUS status = {};
    QueryServiceStatus(hSvc, &status);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);

    if (status.dwCurrentState != SERVICE_RUNNING)
        Log("Service state=%lu (not RUNNING)", static_cast<unsigned long>(status.dwCurrentState));

    return status.dwCurrentState == SERVICE_RUNNING;
}

static bool StartServiceAndWait()
{
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        Log("OpenSCManager (start) failed, err=%lu", GetLastError());
        return false;
    }

    SC_HANDLE hSvc = OpenServiceW(hSCM, RBPO_SERVICE_NAME,
                                   SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        Log("OpenService (start) failed, err=%lu", GetLastError());
        CloseServiceHandle(hSCM);
        return false;
    }

    if (!StartServiceW(hSvc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            Log("StartServiceW failed, err=%lu", err);
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

    if (status.dwCurrentState != SERVICE_RUNNING)
        Log("Timeout waiting RUNNING, last state=%lu",
            static_cast<unsigned long>(status.dwCurrentState));

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
            if (pe.th32ProcessID == pid) { ppid = pe.th32ParentProcessID; break; }
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
            if (pe.th32ProcessID == pid) { ppid = pe.th32ParentProcessID; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    if (ppid == 0) { CloseHandle(hSnap); return false; }
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == ppid) { wcscpy_s(parentExe, pe.szExeFile); break; }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return (parentExe[0] && _wcsicmp(parentExe, RBPO_SERVICE_EXE_NAME) == 0);
}

// ---------------------------------------------------------------------------
// RPC binding helpers
// ---------------------------------------------------------------------------
static bool BindRpc()
{
    if (g_rpcBound) return true;
    RPC_WSTR sb = nullptr;
    RPC_STATUS s = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(RBPO_RPC_ENDPOINT)),
        nullptr, &sb);
    if (s != RPC_S_OK) return false;
    s = RpcBindingFromStringBindingW(sb, &hRBPOServiceBinding);
    RpcStringFreeW(&sb);
    if (s != RPC_S_OK) return false;
    g_rpcBound = true;
    return true;
}

static void UnbindRpc()
{
    if (!g_rpcBound) return;
    RpcBindingFree(&hRBPOServiceBinding);
    g_rpcBound = false;
}

static void StopServiceViaRpc()
{
    if (!BindRpc()) return;
    RpcTryExcept { RBPOService_Stop(); }
    RpcExcept(1) {} RpcEndExcept
    UnbindRpc();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int)
{
    Log("=== rbpo-app started (PID=%u) ===", GetCurrentProcessId());

    if (!IsServiceRunning()) {
        StartServiceAndWait();
    }

    g_hMutex = CreateMutexW(nullptr, TRUE, APP_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    g_hInstance = hInstance;
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName  = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) { CloseHandle(g_hMutex); return 1; }

    g_hWnd = CreateWindowExW(
        0, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 480,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd) { CloseHandle(g_hMutex); return 1; }

    g_hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    AddTrayIcon(g_hWnd);

    BindRpc();
    RefreshUI();

    SetTimer(g_hWnd, IDT_POLL_TIMER, 5000, nullptr);

    bool startSilent = pCmdLine && wcsstr(pCmdLine, L"--silent");
    if (!startSilent) ShowMainWindow();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(g_hWnd, IDT_POLL_TIMER);
    UnbindRpc();
    if (g_hFont) DeleteObject(g_hFont);
    RemoveTrayIcon();
    ReleaseMutex(g_hMutex);
    CloseHandle(g_hMutex);
    return static_cast<int>(msg.wParam);
}

// ---------------------------------------------------------------------------
// RPC operations (run on UI thread; sync — backend calls can block ~seconds)
// ---------------------------------------------------------------------------
static int RpcGetCurrentUser(bool& authenticated, std::wstring& email, std::wstring& name)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long auth = 0; long rc = RBPO_ERR_GENERIC;
    wchar_t* e = nullptr; wchar_t* n = nullptr;
    RpcTryExcept {
        rc = RBPO_GetCurrentUser(&auth, &e, &n);
    } RpcExcept(1) {
        return RBPO_ERR_NETWORK;
    } RpcEndExcept
    authenticated = (auth != 0);
    email = e ? e : L"";
    name  = n ? n : L"";
    if (e) midl_user_free(e);
    if (n) midl_user_free(n);
    return (int)rc;
}

static int RpcLogin(const std::wstring& email, const std::wstring& password,
                    std::wstring& errMsg)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    wchar_t* err = nullptr;
    RpcTryExcept {
        rc = RBPO_Login(email.c_str(), password.c_str(), &err);
    } RpcExcept(1) {
        return RBPO_ERR_NETWORK;
    } RpcEndExcept
    errMsg = err ? err : L"";
    if (err) midl_user_free(err);
    return (int)rc;
}

static int RpcLogout()
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    RpcTryExcept { rc = RBPO_Logout(); }
    RpcExcept(1) { return RBPO_ERR_NETWORK; } RpcEndExcept
    return (int)rc;
}

static int RpcLicenseStatus(bool& has, std::wstring& expIso,
                            bool& blocked, bool& expired, long long& secLeft)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long h = 0, b = 0; __int64 sl = 0; wchar_t* e = nullptr; long rc = RBPO_ERR_GENERIC;
    RpcTryExcept {
        rc = RBPO_GetLicenseStatus(&h, &e, &b, &sl);
    } RpcExcept(1) {
        return RBPO_ERR_NETWORK;
    } RpcEndExcept
    has     = (h != 0);
    blocked = (b == 1); // tri-state: 1=blocked, 2=expired, 0=neither
    expired = (b == 2);
    secLeft = (long long)sl;
    expIso = e ? e : L"";
    if (e) midl_user_free(e);
    return (int)rc;
}

static int RpcActivate(const std::wstring& key, std::wstring& errMsg)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* err = nullptr;
    RpcTryExcept {
        rc = RBPO_ActivateProduct(key.c_str(), &err);
    } RpcExcept(1) {
        return RBPO_ERR_NETWORK;
    } RpcEndExcept
    errMsg = err ? err : L"";
    if (err) midl_user_free(err);
    return (int)rc;
}

static int RpcAVPing(std::wstring& message)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* msg = nullptr;
    RpcTryExcept { rc = RBPO_AVPing(&msg); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    message = msg ? msg : L"(no message)";
    if (msg) midl_user_free(msg);
    return (int)rc;
}

static int RpcGetAvDbInfo(std::wstring& date, long& count)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* d = nullptr; long cnt = 0;
    RpcTryExcept { rc = RBPO_GetAvDbInfo(&d, &cnt); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    date  = d ? d : L"";
    count = cnt;
    if (d) midl_user_free(d);
    return (int)rc;
}

static int RpcScanFile(const std::wstring& path, bool& detected, std::wstring& threat)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; long det = 0; wchar_t* t = nullptr;
    RpcTryExcept { rc = RBPO_ScanFile(path.c_str(), &det, &t); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    detected = (det != 0);
    threat   = t ? t : L"";
    if (t) midl_user_free(t);
    return (int)rc;
}

static int RpcScanDirectory(const std::wstring& path, std::wstring& results)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* r = nullptr;
    RpcTryExcept { rc = RBPO_ScanDirectory(path.c_str(), &r); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    results = r ? r : L"";
    if (r) midl_user_free(r);
    return (int)rc;
}

static int RpcScanAllDrives(std::wstring& results)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* r = nullptr;
    RpcTryExcept { rc = RBPO_ScanAllDrives(&r); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    results = r ? r : L"";
    if (r) midl_user_free(r);
    return (int)rc;
}

static int RpcSetScanSchedule(const std::wstring& path, long interval)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    RpcTryExcept { rc = RBPO_SetScanSchedule(path.c_str(), interval); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    return (int)rc;
}

static int RpcClearScanSchedule()
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    RpcTryExcept { rc = RBPO_ClearScanSchedule(); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    return (int)rc;
}

static int RpcGetScheduleResults(std::wstring& results, int64_t& lastTime)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* r = nullptr; __int64 t = 0;
    RpcTryExcept { rc = RBPO_GetScheduleResults(&r, &t); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    results  = r ? r : L"";
    lastTime = (int64_t)t;
    if (r) midl_user_free(r);
    return (int)rc;
}

static int RpcAddMonitorDirectory(const std::wstring& path)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    RpcTryExcept { rc = RBPO_AddMonitorDirectory(path.c_str()); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    return (int)rc;
}

static int RpcRemoveMonitorDirectory(const std::wstring& path)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC;
    RpcTryExcept { rc = RBPO_RemoveMonitorDirectory(path.c_str()); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    return (int)rc;
}

static int RpcGetMonitorResults(std::wstring& results)
{
    if (!BindRpc()) return RBPO_ERR_NETWORK;
    long rc = RBPO_ERR_GENERIC; wchar_t* r = nullptr;
    RpcTryExcept { rc = RBPO_GetMonitorResults(&r); }
    RpcExcept(1)  { return RBPO_ERR_NETWORK; } RpcEndExcept
    results = r ? r : L"";
    if (r) midl_user_free(r);
    return (int)rc;
}

// ---------------------------------------------------------------------------
// UI construction helpers
// ---------------------------------------------------------------------------
static void SetCtrlFont(HWND h) {
    if (h && g_hFont) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void DestroyAllPanes()
{
    // Destroy every child window so untracked STATIC labels don't leave artifacts
    for (HWND ch = GetWindow(g_hWnd, GW_CHILD); ch;) {
        HWND nx = GetWindow(ch, GW_HWNDNEXT);
        DestroyWindow(ch);
        ch = nx;
    }
    h_loginEmail = h_loginPass = h_loginBtn = h_loginStatus = nullptr;
    h_userLbl = h_licLbl = h_actKey = h_actBtn = h_actStatus = h_avBtn = h_logoutBtn = nullptr;
    h_avDbLbl = h_scanFileBtn = h_scanDirBtn = nullptr;
    h_scanAllDrivesBtn = nullptr;
    h_schedPathEdit = h_schedIntvEdit = nullptr;
    h_schedSetBtn = h_schedClearBtn = h_schedResultsBtn = nullptr;
    h_monPathEdit = h_monAddBtn = h_monRemoveBtn = h_monResultsBtn = nullptr;
    g_currentPane = Pane::None;
}

static void BuildLoginPane(HWND hWnd)
{
    DestroyAllPanes();
    g_currentPane = Pane::Login;

    auto S = [&](const wchar_t* t, int x, int y, int w, int h) {
        HWND hw = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  x, y, w, h, hWnd, nullptr, g_hInstance, nullptr);
        SetCtrlFont(hw);
    };

    S(L"Вход в учётную запись", 20, 24, 520, 22);

    S(L"Email:", 60, 76, 80, 20);
    h_loginEmail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        150, 74, 330, 24, hWnd, (HMENU)IDC_LOGIN_EMAIL, g_hInstance, nullptr);
    SetCtrlFont(h_loginEmail);

    S(L"Пароль:", 60, 116, 80, 20);
    h_loginPass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
        150, 114, 330, 24, hWnd, (HMENU)IDC_LOGIN_PASS, g_hInstance, nullptr);
    SetCtrlFont(h_loginPass);

    h_loginBtn = CreateWindowExW(0, L"BUTTON", L"Войти",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        150, 156, 140, 30, hWnd, (HMENU)IDC_LOGIN_BUTTON, g_hInstance, nullptr);
    SetCtrlFont(h_loginBtn);

    h_loginStatus = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        60, 204, 450, 60, hWnd, (HMENU)IDC_LOGIN_STATUS, g_hInstance, nullptr);
    SetCtrlFont(h_loginStatus);

    SetFocus(h_loginEmail);
}

static void BuildMainPane(HWND hWnd, bool licensed,
                          const std::wstring& userText,
                          const std::wstring& licText)
{
    DestroyAllPanes();
    g_currentPane = licensed ? Pane::Licensed : Pane::Activate;

    auto S = [&](const wchar_t* t, int x, int y, int w, int h) {
        HWND hw = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  x, y, w, h, hWnd, nullptr, g_hInstance, nullptr);
        SetCtrlFont(hw);
    };

    h_userLbl = CreateWindowExW(0, L"STATIC", userText.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 20, 520, 22, hWnd, (HMENU)IDC_USER_LABEL, g_hInstance, nullptr);
    SetCtrlFont(h_userLbl);

    h_licLbl = CreateWindowExW(0, L"STATIC", licText.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 48, 520, 22, hWnd, (HMENU)IDC_LIC_LABEL, g_hInstance, nullptr);
    SetCtrlFont(h_licLbl);

    if (!licensed) {
        S(L"Введите код активации:", 20, 94, 520, 20);

        h_actKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 118, 360, 24, hWnd, (HMENU)IDC_ACT_KEY, g_hInstance, nullptr);
        SetCtrlFont(h_actKey);

        h_actBtn = CreateWindowExW(0, L"BUTTON", L"Активировать",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            390, 116, 140, 28, hWnd, (HMENU)IDC_ACT_BUTTON, g_hInstance, nullptr);
        SetCtrlFont(h_actBtn);

        h_actStatus = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 158, 510, 80, hWnd, (HMENU)IDC_ACT_STATUS, g_hInstance, nullptr);
        SetCtrlFont(h_actStatus);

        SetFocus(h_actKey);
    } else {
        h_avBtn = CreateWindowExW(0, L"BUTTON", L"Проверить AV",
            WS_CHILD | WS_VISIBLE,
            20, 96, 130, 28, hWnd, (HMENU)IDC_AV_BUTTON, g_hInstance, nullptr);
        SetCtrlFont(h_avBtn);

        h_scanFileBtn = CreateWindowExW(0, L"BUTTON", L"Скан файл",
            WS_CHILD | WS_VISIBLE,
            158, 96, 130, 28, hWnd, (HMENU)IDC_SCAN_FILE_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_scanFileBtn);

        h_scanDirBtn = CreateWindowExW(0, L"BUTTON", L"Скан папку",
            WS_CHILD | WS_VISIBLE,
            296, 96, 130, 28, hWnd, (HMENU)IDC_SCAN_DIR_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_scanDirBtn);

        h_avDbLbl = CreateWindowExW(0, L"STATIC", L"База: загрузка...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 134, 520, 18, hWnd, (HMENU)IDC_AV_DB_LABEL, g_hInstance, nullptr);
        SetCtrlFont(h_avDbLbl);

        h_scanAllDrivesBtn = CreateWindowExW(0, L"BUTTON", L"Скан все диски",
            WS_CHILD | WS_VISIBLE,
            20, 158, 170, 28, hWnd, (HMENU)IDC_SCAN_ALL_DRIVES, g_hInstance, nullptr);
        SetCtrlFont(h_scanAllDrivesBtn);

        S(L"Расписание сканирования:", 20, 196, 250, 18);

        h_schedPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 218, 230, 24, hWnd, (HMENU)IDC_SCHED_PATH_EDIT, g_hInstance, nullptr);
        SetCtrlFont(h_schedPathEdit);

        h_schedIntvEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"3600",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
            258, 218, 60, 24, hWnd, (HMENU)IDC_SCHED_INTV_EDIT, g_hInstance, nullptr);
        SetCtrlFont(h_schedIntvEdit);

        S(L"сек", 325, 222, 30, 18);

        h_schedSetBtn = CreateWindowExW(0, L"BUTTON", L"Установить",
            WS_CHILD | WS_VISIBLE,
            360, 216, 90, 28, hWnd, (HMENU)IDC_SCHED_SET_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_schedSetBtn);

        h_schedClearBtn = CreateWindowExW(0, L"BUTTON", L"Сбросить",
            WS_CHILD | WS_VISIBLE,
            458, 216, 80, 28, hWnd, (HMENU)IDC_SCHED_CLEAR_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_schedClearBtn);

        h_schedResultsBtn = CreateWindowExW(0, L"BUTTON", L"Результаты расписания",
            WS_CHILD | WS_VISIBLE,
            20, 250, 210, 28, hWnd, (HMENU)IDC_SCHED_RESULTS_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_schedResultsBtn);

        S(L"Мониторинг директорий:", 20, 290, 250, 18);

        h_monPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 312, 280, 24, hWnd, (HMENU)IDC_MON_PATH_EDIT, g_hInstance, nullptr);
        SetCtrlFont(h_monPathEdit);

        h_monAddBtn = CreateWindowExW(0, L"BUTTON", L"Добавить",
            WS_CHILD | WS_VISIBLE,
            308, 310, 90, 28, hWnd, (HMENU)IDC_MON_ADD_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_monAddBtn);

        h_monRemoveBtn = CreateWindowExW(0, L"BUTTON", L"Удалить",
            WS_CHILD | WS_VISIBLE,
            406, 310, 90, 28, hWnd, (HMENU)IDC_MON_REMOVE_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_monRemoveBtn);

        h_monResultsBtn = CreateWindowExW(0, L"BUTTON", L"Результаты мониторинга",
            WS_CHILD | WS_VISIBLE,
            20, 346, 230, 28, hWnd, (HMENU)IDC_MON_RESULTS_BTN, g_hInstance, nullptr);
        SetCtrlFont(h_monResultsBtn);

        std::wstring dbDate; long dbCount = 0;
        if (RpcGetAvDbInfo(dbDate, dbCount) == RBPO_OK) {
            std::wstring dbInfo = L"Антивирусная база: " + dbDate +
                                  L"  |  Записей: " + std::to_wstring(dbCount);
            SetWindowTextW(h_avDbLbl, dbInfo.c_str());
        }
    }

    h_logoutBtn = CreateWindowExW(0, L"BUTTON", L"Выйти из аккаунта",
        WS_CHILD | WS_VISIBLE,
        20, 390, 170, 28, hWnd, (HMENU)IDC_LOGOUT_BUTTON, g_hInstance, nullptr);
    SetCtrlFont(h_logoutBtn);
}

// ---------------------------------------------------------------------------
// State refresh — drives pane selection
// ---------------------------------------------------------------------------
static void RefreshUI()
{
    bool authed = false; std::wstring email, name;
    int rc = RpcGetCurrentUser(authed, email, name);
    if (rc != RBPO_OK) {
        if (g_currentPane != Pane::Login) BuildLoginPane(g_hWnd);
        if (h_loginStatus) SetWindowTextW(h_loginStatus, L"Нет связи со службой");
        return;
    }

    if (!authed) {
        if (g_currentPane != Pane::Login) BuildLoginPane(g_hWnd);
        return;
    }

    bool has = false; bool blocked = false; bool expired = false;
    std::wstring expIso; long long secLeft = 0;
    int rcL = RpcLicenseStatus(has, expIso, blocked, expired, secLeft);
    if (rcL != RBPO_OK) has = false;

    std::wstring userText = L"Пользователь: ";
    userText += name.empty() ? email : (name + L" (" + email + L")");

    bool activeLicense = has && !blocked && secLeft > 0;

    if (activeLicense) {
        std::wstring licText = L"Лицензия активна до: " + expIso;
        if (g_currentPane != Pane::Licensed) {
            BuildMainPane(g_hWnd, true, userText, licText);
        } else {
            SetWindowTextW(h_userLbl, userText.c_str());
            SetWindowTextW(h_licLbl,  licText.c_str());
        }
    } else {
        std::wstring licText;
        if (blocked && !expIso.empty())
            licText = L"Лицензия заблокирована (" + expIso + L")";
        else if (blocked)
            licText = L"Лицензия заблокирована";
        else if (expired && !expIso.empty())
            licText = L"Лицензия истекла (" + expIso + L")";
        else if (expired)
            licText = L"Лицензия истекла";
        else
            licText = L"Лицензия отсутствует";
        if (g_currentPane != Pane::Activate) {
            BuildMainPane(g_hWnd, false, userText, licText);
        } else {
            SetWindowTextW(h_userLbl, userText.c_str());
            SetWindowTextW(h_licLbl,  licText.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------
static std::wstring GetEditText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n > 0) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

static void DoLogin()
{
    if (!h_loginEmail || !h_loginPass) return;
    std::wstring email = GetEditText(h_loginEmail);
    std::wstring pass  = GetEditText(h_loginPass);
    if (email.empty() || pass.empty()) {
        SetWindowTextW(h_loginStatus, L"Введите email и пароль");
        return;
    }
    SetWindowTextW(h_loginStatus, L"Выполняется вход...");
    EnableWindow(h_loginBtn, FALSE);

    std::wstring err;
    int rc = RpcLogin(email, pass, err);

    EnableWindow(h_loginBtn, TRUE);
    if (rc != RBPO_OK) {
        std::wstring msg = L"Ошибка входа: " + (err.empty() ? L"unknown" : err);
        SetWindowTextW(h_loginStatus, msg.c_str());
        SetWindowTextW(h_loginPass, L"");
        SetFocus(h_loginPass);
        return;
    }
    SetWindowTextW(h_loginPass, L"");
    RefreshUI();
}

static void DoLogout()
{
    RpcLogout();
    RefreshUI();
}

static void DoActivate()
{
    if (!h_actKey) return;
    std::wstring key = GetEditText(h_actKey);
    if (key.empty()) {
        SetWindowTextW(h_actStatus, L"Введите код активации");
        return;
    }
    SetWindowTextW(h_actStatus, L"Активация...");
    EnableWindow(h_actBtn, FALSE);

    std::wstring err;
    int rc = RpcActivate(key, err);

    EnableWindow(h_actBtn, TRUE);
    if (rc != RBPO_OK) {
        std::wstring msg = L"Ошибка активации: " + (err.empty() ? L"unknown" : err);
        SetWindowTextW(h_actStatus, msg.c_str());
        SetWindowTextW(h_actKey, L"");
        SetFocus(h_actKey);
        return;
    }
    SetWindowTextW(h_actKey, L"");
    RefreshUI();
}

static void DoScanAllDrives()
{
    std::wstring results;
    int rc = RpcScanAllDrives(results);
    if (rc == RBPO_ERR_NO_LICENSE) {
        MessageBoxW(g_hWnd, L"Требуется активная лицензия",
                    L"Сканирование дисков", MB_OK | MB_ICONWARNING);
        return;
    }
    if (rc != RBPO_OK) {
        MessageBoxW(g_hWnd, L"Ошибка связи со службой",
                    L"Сканирование дисков", MB_OK | MB_ICONERROR);
        return;
    }
    bool clean = (results == L"No threats detected");
    MessageBoxW(g_hWnd, results.c_str(),
                clean ? L"Диски чисты" : L"Обнаружены угрозы",
                MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void DoSetSchedule()
{
    if (!h_schedPathEdit || !h_schedIntvEdit) return;
    std::wstring path    = GetEditText(h_schedPathEdit);
    std::wstring intvStr = GetEditText(h_schedIntvEdit);
    if (path.empty()) {
        MessageBoxW(g_hWnd, L"Введите путь для сканирования",
                    L"Расписание", MB_OK | MB_ICONWARNING);
        return;
    }
    long interval = intvStr.empty() ? 3600L : (long)_wtoi(intvStr.c_str());
    int rc = RpcSetScanSchedule(path, interval);
    if (rc == RBPO_ERR_NO_LICENSE) {
        MessageBoxW(g_hWnd, L"Требуется активная лицензия",
                    L"Расписание", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring msg = (rc == RBPO_OK)
        ? (L"Расписание установлено.\nПуть: " + path +
           L"\nИнтервал: " + std::to_wstring(interval) + L" сек")
        : L"Ошибка связи со службой";
    MessageBoxW(g_hWnd, msg.c_str(), L"Расписание", MB_OK | MB_ICONINFORMATION);
}

static void DoClearSchedule()
{
    int rc = RpcClearScanSchedule();
    MessageBoxW(g_hWnd,
                rc == RBPO_OK ? L"Расписание сброшено" : L"Ошибка связи со службой",
                L"Расписание", MB_OK | MB_ICONINFORMATION);
}

static void DoGetScheduleResults()
{
    std::wstring results; int64_t lastTime = 0;
    int rc = RpcGetScheduleResults(results, lastTime);
    if (rc != RBPO_OK) {
        MessageBoxW(g_hWnd, L"Ошибка связи со службой",
                    L"Расписание — результаты", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring msg;
    if (lastTime > 0) {
        time_t t = (time_t)lastTime;
        char tbuf[64] = {};
        struct tm tms = {};
        localtime_s(&tms, &t);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tms);
        wchar_t wtbuf[64] = {};
        MultiByteToWideChar(CP_ACP, 0, tbuf, -1, wtbuf, 64);
        msg = std::wstring(L"Последнее сканирование: ") + wtbuf + L"\n\n";
    } else {
        msg = L"Сканирование ещё не выполнялось.\n\n";
    }
    msg += results;
    bool clean = (results == L"No scan results yet" || results == L"No threats detected");
    MessageBoxW(g_hWnd, msg.c_str(),
                clean ? L"Угрозы не обнаружены" : L"Обнаружены угрозы",
                MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void DoAddMonitor()
{
    if (!h_monPathEdit) return;
    std::wstring path = GetEditText(h_monPathEdit);
    if (path.empty()) {
        wchar_t dirPath[MAX_PATH] = {};
        BROWSEINFOW bi = {};
        bi.hwndOwner = g_hWnd;
        bi.lpszTitle = L"Выберите директорию для мониторинга";
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return;
        SHGetPathFromIDListW(pidl, dirPath);
        CoTaskMemFree(pidl);
        path = dirPath;
        SetWindowTextW(h_monPathEdit, path.c_str());
    }
    int rc = RpcAddMonitorDirectory(path);
    if (rc == RBPO_ERR_NO_LICENSE) {
        MessageBoxW(g_hWnd, L"Требуется активная лицензия",
                    L"Мониторинг", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring msg = (rc == RBPO_OK)
        ? (L"Мониторинг запущен:\n" + path)
        : L"Ошибка связи со службой";
    MessageBoxW(g_hWnd, msg.c_str(), L"Мониторинг", MB_OK | MB_ICONINFORMATION);
}

static void DoRemoveMonitor()
{
    if (!h_monPathEdit) return;
    std::wstring path = GetEditText(h_monPathEdit);
    if (path.empty()) return;
    int rc = RpcRemoveMonitorDirectory(path);
    std::wstring msg = (rc == RBPO_OK)
        ? (L"Мониторинг остановлен:\n" + path)
        : L"Ошибка связи со службой";
    MessageBoxW(g_hWnd, msg.c_str(), L"Мониторинг", MB_OK | MB_ICONINFORMATION);
}

static void DoGetMonitorResults()
{
    std::wstring results;
    int rc = RpcGetMonitorResults(results);
    if (rc != RBPO_OK) {
        MessageBoxW(g_hWnd, L"Ошибка связи со службой",
                    L"Мониторинг — результаты", MB_OK | MB_ICONERROR);
        return;
    }
    bool clean = (results == L"No threats detected");
    MessageBoxW(g_hWnd, results.c_str(),
                clean ? L"Угрозы не обнаружены" : L"Обнаружены угрозы (мониторинг)",
                MB_OK | (clean ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void DoScanFile()
{
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hWnd;
    ofn.lpstrFile   = filePath;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Выберите файл для сканирования";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    bool detected = false;
    std::wstring threat;
    int rc = RpcScanFile(filePath, detected, threat);

    if (rc == RBPO_ERR_NO_LICENSE) {
        MessageBoxW(g_hWnd, L"Требуется активная лицензия",
                    L"Сканирование", MB_OK | MB_ICONWARNING);
        return;
    }
    if (rc != RBPO_OK) {
        MessageBoxW(g_hWnd, L"Ошибка связи со службой",
                    L"Сканирование", MB_OK | MB_ICONERROR);
        return;
    }

    if (detected) {
        std::wstring msg = L"Обнаружена угроза!\n\nФайл: ";
        msg += filePath;
        msg += L"\nУгроза: ";
        msg += threat;
        MessageBoxW(g_hWnd, msg.c_str(), L"Обнаружена угроза", MB_OK | MB_ICONWARNING);
    } else {
        std::wstring msg = L"Файл чист.\n\n";
        msg += filePath;
        MessageBoxW(g_hWnd, msg.c_str(), L"Сканирование завершено", MB_OK | MB_ICONINFORMATION);
    }
}

static void DoScanDirectory()
{
    // Use SHBrowseForFolder to pick a directory
    wchar_t dirPath[MAX_PATH] = {};

    BROWSEINFOW bi = {};
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = L"Выберите папку для сканирования";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    SHGetPathFromIDListW(pidl, dirPath);
    CoTaskMemFree(pidl);

    if (!dirPath[0]) return;

    std::wstring results;
    int rc = RpcScanDirectory(dirPath, results);

    if (rc == RBPO_ERR_NO_LICENSE) {
        MessageBoxW(g_hWnd, L"Требуется активная лицензия",
                    L"Сканирование", MB_OK | MB_ICONWARNING);
        return;
    }
    if (rc != RBPO_OK) {
        MessageBoxW(g_hWnd, L"Ошибка связи со службой",
                    L"Сканирование", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring title = (results == L"No threats detected")
        ? L"Угрозы не обнаружены"
        : L"Обнаружены угрозы";
    UINT icon = (results == L"No threats detected")
        ? MB_ICONINFORMATION : MB_ICONWARNING;

    std::wstring msg = L"Папка: ";
    msg += dirPath;
    msg += L"\n\nРезультаты:\n";
    msg += results;
    MessageBoxW(g_hWnd, msg.c_str(), title.c_str(), MB_OK | icon);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TASKBARCREATED) { AddTrayIcon(hWnd); return 0; }

    switch (message) {
    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP: ShowMainWindow(); break;
        case WM_RBUTTONUP: ShowTrayContextMenu(hWnd); break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_POLL_TIMER && IsWindowVisible(hWnd)) {
            HWND hFocus = GetFocus();
            bool userTyping = (hFocus == h_loginEmail || hFocus == h_loginPass
                               || hFocus == h_actKey);
            if (!userTyping) RefreshUI();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN:
            ShowMainWindow();
            break;
        case ID_TRAY_EXIT:
        case ID_FILE_EXIT:
            StopServiceViaRpc();
            DestroyWindow(hWnd);
            break;
        case ID_FILE_LOGOUT:
        case IDC_LOGOUT_BUTTON:
            DoLogout();
            break;
        case IDC_LOGIN_BUTTON:
            DoLogin();
            break;
        case IDC_ACT_BUTTON:
            DoActivate();
            break;
        case IDC_AV_BUTTON: {
            std::wstring m;
            int rc = RpcAVPing(m);
            std::wstring full = (rc == RBPO_OK)
                ? (L"AV: " + m)
                : (L"AV blocked: " + m);
            MessageBoxW(hWnd, full.c_str(), L"AV", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDC_SCAN_FILE_BTN:
            DoScanFile();
            break;
        case IDC_SCAN_DIR_BTN:
            DoScanDirectory();
            break;
        case IDC_SCAN_ALL_DRIVES:
            DoScanAllDrives();
            break;
        case IDC_SCHED_SET_BTN:
            DoSetSchedule();
            break;
        case IDC_SCHED_CLEAR_BTN:
            DoClearSchedule();
            break;
        case IDC_SCHED_RESULTS_BTN:
            DoGetScheduleResults();
            break;
        case IDC_MON_ADD_BTN:
            DoAddMonitor();
            break;
        case IDC_MON_REMOVE_BTN:
            DoRemoveMonitor();
            break;
        case IDC_MON_RESULTS_BTN:
            DoGetMonitorResults();
            break;
        }
        return 0;

    case WM_CLOSE:
        HideMainWindow();
        return 0;

    case WM_CREATE:
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Tray icon helpers
// ---------------------------------------------------------------------------
static void AddTrayIcon(HWND hWnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"РБПО Tray Application");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void ShowTrayContextMenu(HWND hWnd)
{
    POINT pt; GetCursorPos(&pt);
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
    RefreshUI();
}

static void HideMainWindow() { ShowWindow(g_hWnd, SW_HIDE); }
