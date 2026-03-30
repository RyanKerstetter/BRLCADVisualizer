#include <QApplication>
#include <cstdio>
#include <string>
#include <thread>

#include <ospray/ospray.h>

#include "mainwindow.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
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

int main(int argc, char *argv[])
{
#ifdef _WIN32
  installStderrFilter();
#endif

  int ac = argc;
  const char **av = const_cast<const char **>(argv);

  const OSPError err = ospInit(&ac, av);
  if (err != OSP_NO_ERROR) {
    fprintf(stderr, "IBRT: OSPRay initialization failed.\n");
    return 1;
  }

  OSPDevice device = ospNewDevice("cpu");
  if (!device) {
    fprintf(stderr, "IBRT: Failed to create OSPRay CPU device.\n");
    return 1;
  }

  ospSetCurrentDevice(device);
  ospDeviceSetErrorCallback(device, osprayErrorCallback, nullptr);
  ospDeviceSetStatusCallback(device, osprayStatusCallback, nullptr);
  ospCommit((OSPObject)device);

  if (ospLoadModule("cpu") != OSP_NO_ERROR) {
    fprintf(stderr, "IBRT: Failed to load OSPRay CPU module.\n");
    return 1;
  }
  if (ospLoadModule("brl_cad") != OSP_NO_ERROR) {
    fprintf(stderr, "IBRT: Failed to load BRL-CAD OSPRay module.\n");
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
