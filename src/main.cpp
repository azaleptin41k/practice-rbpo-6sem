#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\RbpoPracTrayAppZad1SingleInstance";
constexpr wchar_t kWindowClass[] = L"RbpoTrayAppZad1MainWnd";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 1;

constexpr UINT_PTR kIdTrayOpen = 1001;
constexpr UINT_PTR kIdTrayExit = 1002;
constexpr UINT_PTR kIdFileExit = 1003;

UINT g_wmTaskbarCreated = 0;

void RemoveTrayIcon(HWND hwnd) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AddTrayIcon(HWND hwnd) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = kTrayCallbackMsg;
  nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(nid.szTip, L"RBPO Tray (zad-1)");
  Shell_NotifyIconW(NIM_ADD, &nid);
}

void ShowMainWindow(HWND hwnd) {
  ShowWindow(hwnd, SW_SHOW);
  ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
}

bool CommandLineHasHiddenFlag() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  bool hidden = false;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"/hidden") == 0 || _wcsicmp(argv[i], L"-hidden") == 0) {
      hidden = true;
      break;
    }
  }
  LocalFree(argv);
  return hidden;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == g_wmTaskbarCreated) {
    RemoveTrayIcon(hwnd);
    AddTrayIcon(hwnd);
    return 0;
  }

  switch (msg) {
    case WM_CREATE:
      AddTrayIcon(hwnd);
      return 0;

    case kTrayCallbackMsg:
      if (lParam == WM_LBUTTONUP) {
        ShowMainWindow(hwnd);
        return 0;
      }
      if (lParam == WM_RBUTTONUP) {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
          return 0;
        }
        AppendMenuW(menu, MF_STRING, kIdTrayOpen, L"Открыть");
        AppendMenuW(menu, MF_STRING, kIdTrayExit, L"Выход");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        return 0;
      }
      return 0;

    case WM_COMMAND: {
      const UINT_PTR id = LOWORD(wParam);
      if (id == kIdTrayOpen) {
        ShowMainWindow(hwnd);
        return 0;
      }
      if (id == kIdTrayExit || id == kIdFileExit) {
        RemoveTrayIcon(hwnd);
        DestroyWindow(hwnd);
        return 0;
      }
      return 0;
    }

    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      RemoveTrayIcon(hwnd);
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
  if (!mutex) {
    return 0;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClass;

  if (!RegisterClassExW(&wc)) {
    CloseHandle(mutex);
    return 0;
  }

  HWND hwnd = CreateWindowExW(
      0,
      kWindowClass,
      L"RBPO Tray — задание 1",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      480,
      320,
      nullptr,
      nullptr,
      hInstance,
      nullptr);

  if (!hwnd) {
    CloseHandle(mutex);
    return 0;
  }

  HMENU menuBar = CreateMenu();
  HMENU fileMenu = CreateMenu();
  AppendMenuW(fileMenu, MF_STRING, kIdFileExit, L"Выход");
  AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"Файл");
  SetMenu(hwnd, menuBar);

  const bool startHidden = CommandLineHasHiddenFlag();
  ShowWindow(hwnd, startHidden ? SW_HIDE : SW_SHOW);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CloseHandle(mutex);
  return static_cast<int>(msg.wParam);
}
