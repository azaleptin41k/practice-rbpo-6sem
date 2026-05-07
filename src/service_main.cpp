#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <rpc.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <cstdlib>

#include <vector>

#include "tray_config.h"
extern "C" {
#include "TrayRpc_h.h"
}

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "rpcrt4.lib")

#ifndef WTS_REMOTE_SESSION_LOGON
#define WTS_REMOTE_SESSION_LOGON 6
#endif

#ifndef RPC_IF_ALLOW_CALLBACKS_WITH_NULL_AUTH
#define RPC_IF_ALLOW_CALLBACKS_WITH_NULL_AUTH 0x80
#endif

static HANDLE g_stopEvent = nullptr;

extern "C" void* __RPC_USER midl_user_allocate(size_t size) {
  return std::malloc(size);
}

extern "C" void __RPC_USER midl_user_free(void* p) {
  std::free(p);
}

extern "C" void RequestStop(void) {
  if (g_stopEvent) {
    SetEvent(g_stopEvent);
  }
}

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
CRITICAL_SECTION g_childLock{};
std::vector<HANDLE> g_children;
wchar_t g_trayAppPath[MAX_PATH] = {};

void Report(DWORD state, DWORD win32Exit = 0, DWORD specific = 0, DWORD waitHint = 0) {
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32Exit;
  g_status.dwServiceSpecificExitCode = specific;
  g_status.dwWaitHint = waitHint;
  if (state == SERVICE_START_PENDING) {
    g_status.dwControlsAccepted = 0;
  } else if (state == SERVICE_RUNNING) {
    g_status.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
  } else {
    g_status.dwControlsAccepted = 0;
  }
  if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
    g_status.dwCheckPoint = 0;
  } else {
    ++g_status.dwCheckPoint;
  }
  SetServiceStatus(g_statusHandle, &g_status);
}

void BuildTrayAppPath() {
  DWORD n = GetModuleFileNameW(nullptr, g_trayAppPath, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    g_trayAppPath[0] = L'\0';
    return;
  }
  wchar_t* slash = wcsrchr(g_trayAppPath, L'\\');
  wchar_t* fname = slash ? slash + 1 : g_trayAppPath;
  wcscpy_s(fname, MAX_PATH - static_cast<DWORD>(fname - g_trayAppPath), L"rbpo-app.exe");
}

void RegisterChildProcess(HANDLE process) {
  EnterCriticalSection(&g_childLock);
  g_children.push_back(process);
  LeaveCriticalSection(&g_childLock);
}

void TerminateAllChildren() {
  EnterCriticalSection(&g_childLock);
  for (HANDLE h : g_children) {
    if (h) {
      TerminateProcess(h, 1);
      CloseHandle(h);
    }
  }
  g_children.clear();
  LeaveCriticalSection(&g_childLock);
}

bool LaunchTrayInSession(DWORD sessionId) {
  if (sessionId == 0) {
    return false;
  }
  if (!g_trayAppPath[0]) {
    return false;
  }

  HANDLE userToken = nullptr;
  if (!WTSQueryUserToken(sessionId, &userToken)) {
    return false;
  }

  HANDLE primary = nullptr;
  if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
    CloseHandle(userToken);
    return false;
  }
  CloseHandle(userToken);

  LPVOID env = nullptr;
  CreateEnvironmentBlock(&env, primary, FALSE);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

  wchar_t cmdLine[MAX_PATH + 32] = {};
  swprintf_s(cmdLine, L"\"%s\" --silent", g_trayAppPath);

  PROCESS_INFORMATION pi{};
  const DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;
  const BOOL ok = CreateProcessAsUserW(
      primary,
      g_trayAppPath,
      cmdLine,
      nullptr,
      nullptr,
      FALSE,
      creationFlags,
      env,
      nullptr,
      &si,
      &pi);

  if (env) {
    DestroyEnvironmentBlock(env);
  }
  CloseHandle(primary);

  if (!ok) {
    return false;
  }

  CloseHandle(pi.hThread);
  RegisterChildProcess(pi.hProcess);
  return true;
}

void LaunchForAllUserSessions() {
  PWTS_SESSION_INFO sessions = nullptr;
  DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    return;
  }
  for (DWORD i = 0; i < count; ++i) {
    const DWORD sid = sessions[i].SessionId;
    if (sid == 0) {
      continue;
    }
    if (sessions[i].State != WTSActive && sessions[i].State != WTSConnected) {
      continue;
    }
    LaunchTrayInSession(sid);
  }
  WTSFreeMemory(sessions);
}

DWORD WINAPI RpcListenThread(LPVOID) {
  (void)RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
  return 0;
}

DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD eventType, LPVOID eventData, LPVOID) {
  if (control == SERVICE_CONTROL_INTERROGATE) {
    SetServiceStatus(g_statusHandle, &g_status);
    return NO_ERROR;
  }
  if (control == SERVICE_CONTROL_SESSIONCHANGE && eventData) {
    const auto* sn = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
    if (sn->cbSize != 0 && sn->cbSize != sizeof(WTSSESSION_NOTIFICATION)) {
      return NO_ERROR;
    }
    if (eventType == WTS_SESSION_LOGON || eventType == WTS_REMOTE_SESSION_LOGON) {
      LaunchTrayInSession(sn->dwSessionId);
    }
    return NO_ERROR;
  }
  return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
  g_statusHandle = RegisterServiceCtrlHandlerExW(TRAY_SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
  if (!g_statusHandle) {
    return;
  }

  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwServiceSpecificExitCode = 0;
  g_status.dwWin32ExitCode = 0;
  g_status.dwCheckPoint = 0;

  InitializeCriticalSection(&g_childLock);
  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stopEvent) {
    Report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
    DeleteCriticalSection(&g_childLock);
    return;
  }

  Report(SERVICE_START_PENDING, 0, 0, 5000);
  BuildTrayAppPath();

  RPC_STATUS rpcSt = RpcServerUseProtseqEpW(
      reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
      RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
      reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(TRAY_RPC_ENDPOINT)),
      nullptr);
  if (rpcSt != RPC_S_OK) {
    Report(SERVICE_STOPPED, rpcSt);
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_childLock);
    return;
  }

  rpcSt = RpcServerRegisterIf2(
      TrayRpc_v1_0_s_ifspec,
      nullptr,
      nullptr,
      RPC_IF_ALLOW_CALLBACKS_WITH_NULL_AUTH,
      RPC_C_LISTEN_MAX_CALLS_DEFAULT,
      static_cast<unsigned>(-1),
      nullptr);
  if (rpcSt != RPC_S_OK) {
    Report(SERVICE_STOPPED, rpcSt);
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_childLock);
    return;
  }

  Report(SERVICE_START_PENDING, 0, 0, 5000);

  HANDLE rpcThread = CreateThread(nullptr, 0, RpcListenThread, nullptr, 0, nullptr);
  if (!rpcThread) {
    RpcServerUnregisterIf(TrayRpc_v1_0_s_ifspec, nullptr, 0);
    Report(SERVICE_STOPPED, GetLastError());
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_childLock);
    return;
  }

  LaunchForAllUserSessions();

  Report(SERVICE_RUNNING);

  WaitForSingleObject(g_stopEvent, INFINITE);

  RpcMgmtStopServerListening(nullptr);
  RpcMgmtWaitServerListen();
  WaitForSingleObject(rpcThread, INFINITE);
  CloseHandle(rpcThread);

  RpcServerUnregisterIf(TrayRpc_v1_0_s_ifspec, nullptr, 0);

  TerminateAllChildren();

  CloseHandle(g_stopEvent);
  g_stopEvent = nullptr;
  DeleteCriticalSection(&g_childLock);

  Report(SERVICE_STOPPED);
}

}  // namespace

int wmain(int /*argc*/, wchar_t* /*argv*/[]) {
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(TRAY_SERVICE_NAME), ServiceMain},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    return static_cast<int>(GetLastError());
  }
  return 0;
}
