/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "platform.h"
#include "windows/mousehook.h"

#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>

#include "interface/platform.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/thread.h"

#define ID_MENU_OPEN_LOG 3000
#define ID_MENU_EXIT     3001

struct AppState
{
  LARGE_INTEGER perfFreq;
  HINSTANCE hInst;

  int     argc;
  char ** argv;

  char         executable[MAX_PATH + 1];
  HWND         messageWnd;
  HMENU        trayMenu;
};

static struct AppState app = {0};
HWND MessageHWND;

// undocumented API to adjust the system timer resolution (yes, its a nasty hack)
typedef NTSTATUS (__stdcall *ZwSetTimerResolution_t)(ULONG RequestedResolution, BOOLEAN Set, PULONG ActualResolution);
static ZwSetTimerResolution_t ZwSetTimerResolution = NULL;

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch(msg)
  {
    case WM_DESTROY:
      PostQuitMessage(0);
      break;

    case WM_CALL_FUNCTION:
    {
      struct MSG_CALL_FUNCTION * cf = (struct MSG_CALL_FUNCTION *)lParam;
      return cf->fn(cf->wParam, cf->lParam);
    }

    case WM_TRAYICON:
    {
      if (lParam == WM_RBUTTONDOWN)
      {
        POINT curPoint;
        GetCursorPos(&curPoint);
        SetForegroundWindow(hwnd);
        UINT clicked = TrackPopupMenu(
          app.trayMenu,
          TPM_RETURNCMD | TPM_NONOTIFY,
          curPoint.x,
          curPoint.y,
          0,
          hwnd,
          NULL
        );

             if (clicked == ID_MENU_EXIT    ) app_quit();
        else if (clicked == ID_MENU_OPEN_LOG)
        {
          const char * logFile = option_get_string("os", "logFile");
          if (strcmp(logFile, "stderr") == 0)
            DEBUG_INFO("Ignoring request to open the logFile, logging to stderr");
          else
            ShellExecute(NULL, NULL, logFile, NULL, NULL, SW_SHOWNORMAL);
        }
      }
      break;
    }

    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
  return 0;
}

static int appThread(void * opaque)
{
  // register our TrayIcon
  NOTIFYICONDATA iconData =
  {
    .cbSize           = sizeof(NOTIFYICONDATA),
    .hWnd             = app.messageWnd,
    .uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP,
    .uCallbackMessage = WM_TRAYICON,
    .szTip            = "Looking Glass (host)"
  };
  iconData.hIcon = LoadIcon(app.hInst, IDI_APPLICATION);
  Shell_NotifyIcon(NIM_ADD, &iconData);

  int result = app_main(app.argc, app.argv);

  Shell_NotifyIcon(NIM_DELETE, &iconData);
  mouseHook_remove();
  SendMessage(app.messageWnd, WM_DESTROY, 0, 0);
  return result;
}

LRESULT sendAppMessage(UINT Msg, WPARAM wParam, LPARAM lParam)
{
  return SendMessage(app.messageWnd, Msg, wParam, lParam);
}

static BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
  if (dwCtrlType == CTRL_C_EVENT)
  {
    SendMessage(app.messageWnd, WM_CLOSE, 0, 0);
    return TRUE;
  }

  return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  /* this is a bit of a hack but without this --help will produce no output in a windows command prompt */
  if (!IsDebuggerPresent() && AttachConsole(ATTACH_PARENT_PROCESS))
  {
    HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    int std_err_fd = _open_osfhandle((intptr_t)std_err, _O_TEXT);
    int std_out_fd = _open_osfhandle((intptr_t)std_out, _O_TEXT);

    if (std_err_fd > 0)
      *stderr = *_fdopen(std_err_fd, "w");

    if  (std_out_fd > 0)
      *stdout = *_fdopen(std_out_fd, "w");
  }

  int result = 0;
  app.hInst = hInstance;

  char tempPath[MAX_PATH+1];
  GetTempPathA(sizeof(tempPath), tempPath);
  int len = snprintf(NULL, 0, "%slooking-glass-host.txt", tempPath);
  char * logFilePath = malloc(len + 1);
  sprintf(logFilePath, "%slooking-glass-host.txt", tempPath);

  struct Option options[] =
  {
    {
      .module         = "os",
      .name           = "logFile",
      .description    = "The log file to write to",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = logFilePath
    },
    {0}
  };

  option_register(options);
  free(logFilePath);

  // convert the command line to the standard argc and argv
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &app.argc);
  app.argv = malloc(sizeof(char *) * app.argc);
  for(int i = 0; i < app.argc; ++i)
  {
    const size_t s = (wcslen(wargv[i])+1) * 2;
    app.argv[i] = malloc(s);
    wcstombs(app.argv[i], wargv[i], s);
  }
  LocalFree(wargv);

  GetModuleFileName(NULL, app.executable, sizeof(app.executable));

  // setup a handler for ctrl+c
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  // create a message window so that our message pump works
  WNDCLASSEX wx    = {};
  wx.cbSize        = sizeof(WNDCLASSEX);
  wx.lpfnWndProc   = DummyWndProc;
  wx.hInstance     = hInstance;
  wx.lpszClassName = "DUMMY_CLASS";
  wx.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
  wx.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
  wx.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wx.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
  if (!RegisterClassEx(&wx))
  {
    DEBUG_ERROR("Failed to register message window class");
    result = -1;
    goto finish;
  }
  app.messageWnd = CreateWindowEx(0, "DUMMY_CLASS", "DUMMY_NAME", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

  // set the global
  MessageHWND = app.messageWnd;

  app.trayMenu = CreatePopupMenu();
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_OPEN_LOG, "Open Log File");
  AppendMenu(app.trayMenu, MF_SEPARATOR, 0               , NULL           );
  AppendMenu(app.trayMenu, MF_STRING   , ID_MENU_EXIT    , "Exit"         );

  // create the application thread
  LGThread * thread;
  if (!lgCreateThread("appThread", appThread, NULL, &thread))
  {
    DEBUG_ERROR("Failed to create the main application thread");
    result = -1;
    goto finish;
  }

  while(true)
  {
    MSG  msg;
    BOOL bRet = GetMessage(&msg, NULL, 0, 0);
    if (bRet > 0)
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      continue;
    }
    else if (bRet < 0)
    {
      DEBUG_ERROR("Unknown error from GetMessage");
      result = -1;
      goto shutdown;
    }

    break;
  }

shutdown:
  DestroyMenu(app.trayMenu);
  app_quit();
  if (!lgJoinThread(thread, &result))
  {
    DEBUG_ERROR("Failed to join the main application thread");
    result = -1;
  }

finish:

  for(int i = 0; i < app.argc; ++i)
    free(app.argv[i]);
  free(app.argv);

  return result;
}

bool app_init()
{
  const char * logFile   = option_get_string("os", "logFile"  );

  // redirect stderr to a file
  if (logFile && strcmp(logFile, "stderr") != 0)
    freopen(logFile, "a", stderr);

  // always flush stderr
  setbuf(stderr, NULL);

  // Increase the timer resolution
  ZwSetTimerResolution = (ZwSetTimerResolution_t)GetProcAddress(GetModuleHandle("ntdll.dll"), "ZwSetTimerResolution");
  if (ZwSetTimerResolution)
  {
    ULONG actualResolution;
    ZwSetTimerResolution(1, true, &actualResolution);
    DEBUG_INFO("System timer resolution: %.2f ns", (float)actualResolution / 100.0f);
  }

  // get the performance frequency for spinlocks
  QueryPerformanceFrequency(&app.perfFreq);

  return true;
}

const char * os_getExecutable()
{
  return app.executable;
}

HWND os_getMessageWnd()
{
  return app.messageWnd;
}
