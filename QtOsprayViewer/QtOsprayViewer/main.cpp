#include <QApplication>
#include <QMessageBox>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <ospray/ospray.h>

#include "mainwindow.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#endif

#ifdef _WIN32
static bool shouldSuppressStderrLine(const std::string &line)
{
  return line.find(
             "getCurveEstimateOfV(): estimate of 'v' given a trim curve and 'u' did not converge")
      != std::string::npos;
}

static void installStderrFilter()
{
  HANDLE originalStderr = GetStdHandle(STD_ERROR_HANDLE);
  if (!originalStderr || originalStderr == INVALID_HANDLE_VALUE)
    return;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    return;

  const int stderrFd = _fileno(stderr);
  if (stderrFd < 0) {
    CloseHandle(readPipe);
    CloseHandle(writePipe);
    return;
  }

  const int pipeFd = _open_osfhandle(reinterpret_cast<intptr_t>(writePipe), _O_TEXT);
  if (pipeFd < 0) {
    CloseHandle(readPipe);
    CloseHandle(writePipe);
    return;
  }

  if (_dup2(pipeFd, stderrFd) != 0) {
    _close(pipeFd);
    CloseHandle(readPipe);
    return;
  }

  setvbuf(stderr, nullptr, _IONBF, 0);
  SetStdHandle(STD_ERROR_HANDLE, writePipe);
  _close(pipeFd);

  std::thread([readPipe, originalStderr]() {
    char buffer[1024];
    std::string pending;

    auto flushLine = [&](const std::string &line) {
      if (shouldSuppressStderrLine(line))
        return;

      DWORD written = 0;
      WriteFile(
          originalStderr, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    };

    for (;;) {
      DWORD bytesRead = 0;
      if (!ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr)
          || bytesRead == 0) {
        break;
      }

      pending.append(buffer, buffer + bytesRead);

      size_t newline = 0;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline + 1);
        pending.erase(0, newline + 1);
        flushLine(line);
      }
    }

    if (!pending.empty())
      flushLine(pending);

    CloseHandle(readPipe);
  }).detach();
}

static LONG WINAPI crashDumpExceptionFilter(EXCEPTION_POINTERS *exceptionInfo)
{
  char exePath[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);

  char dirPath[MAX_PATH] = {};
  std::strncpy(dirPath, exePath, MAX_PATH - 1);
  for (int i = int(std::strlen(dirPath)) - 1; i >= 0; --i) {
    if (dirPath[i] == '\\' || dirPath[i] == '/') {
      dirPath[i] = '\0';
      break;
    }
  }

  SYSTEMTIME st{};
  GetLocalTime(&st);

  char dumpPath[MAX_PATH] = {};
  std::snprintf(dumpPath,
      MAX_PATH,
      "%s\\IBRT_crash_%04u%02u%02u_%02u%02u%02u.dmp",
      dirPath,
      st.wYear,
      st.wMonth,
      st.wDay,
      st.wHour,
      st.wMinute,
      st.wSecond);

  HANDLE hFile = CreateFileA(
      dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
    dumpInfo.ThreadId = GetCurrentThreadId();
    dumpInfo.ExceptionPointers = exceptionInfo;
    dumpInfo.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo
            | MiniDumpWithIndirectlyReferencedMemory),
        &dumpInfo,
        nullptr,
        nullptr);
    CloseHandle(hFile);
  }

  return EXCEPTION_EXECUTE_HANDLER;
}

static void installCrashDumpHandler()
{
  SetUnhandledExceptionFilter(crashDumpExceptionFilter);
}
#endif

static void osprayErrorCallback(void *, OSPError error, const char *message)
{
  fprintf(stderr,
      "OSPRAY ERROR %d: %s\n",
      (int)error,
      message ? message : "(null)");
  fflush(stderr);
}

static void osprayStatusCallback(void *, const char *message)
{
  (void)message;
}

static void showFatalStartupError(const char *message)
{
#ifdef _WIN32
  MessageBoxA(nullptr, message, "IBRT Startup Error", MB_ICONERROR | MB_OK);
#else
  fprintf(stderr, "%s\n", message ? message : "Unknown startup error.");
#endif
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
  installStderrFilter();
  installCrashDumpHandler();
#endif

  int ac = argc;
  const char **av = const_cast<const char **>(argv);

  const OSPError err = ospInit(&ac, av);
  if (err != OSP_NO_ERROR) {
    fprintf(stderr, "IBRT: OSPRay initialization failed.\n");
    showFatalStartupError("IBRT: OSPRay initialization failed.");
    return 1;
  }

  OSPDevice device = ospNewDevice("cpu");
  if (!device) {
    fprintf(stderr, "IBRT: Failed to create OSPRay CPU device.\n");
    showFatalStartupError("IBRT: Failed to create OSPRay CPU device.");
    return 1;
  }

  ospSetCurrentDevice(device);
  ospDeviceSetErrorCallback(device, osprayErrorCallback, nullptr);
  ospDeviceSetStatusCallback(device, osprayStatusCallback, nullptr);
  ospCommit((OSPObject)device);

  if (ospLoadModule("cpu") != OSP_NO_ERROR) {
    fprintf(stderr, "IBRT: Failed to load OSPRay CPU module.\n");
    showFatalStartupError("IBRT: Failed to load OSPRay CPU module.");
    return 1;
  }

  int rc = 0;
  {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    rc = a.exec();
  }

  ospShutdown();
  return rc;
}
