#include<QApplication>
#include <cstdio>

#include <ospray/ospray.h>
#include "mainwindow.h"

#include <windows.h>

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
  fprintf(stderr, "OSPRAY STATUS: %s\n", message ? message : "(null)");
  fflush(stderr);
}

int main(int argc, char *argv[])
{
  fprintf(stdout, "main start\n");

  int ac = argc;
  const char **av = const_cast<const char **>(argv);

  // ✅ STEP 1: initialize OSPRay
  OSPError err = ospInit(&ac, av);
  if (err != OSP_NO_ERROR) {
    fprintf(stderr, "ospInit failed\n");
    return 1;
  }
  fprintf(stderr, "ospInit ok\n");

  // ✅ STEP 2: create device
  OSPDevice device = ospNewDevice("cpu");
  if (!device) {
    fprintf(stderr, "ospNewDevice(cpu) failed\n");
    return 1;
  }

  // ✅ STEP 3: set device
  ospSetCurrentDevice(device);
  ospDeviceSetErrorCallback(device, osprayErrorCallback, nullptr);
  ospDeviceSetStatusCallback(device, osprayStatusCallback, nullptr);
  ospCommit((OSPObject)device);
  fprintf(stderr, "device created + committed\n");

  // ✅ STEP 4: load modules
  OSPError cpuErr = ospLoadModule("cpu");
  fprintf(stderr, "ospLoadModule(cpu) -> %d\n", (int)cpuErr);

  OSPError brlErr = ospLoadModule("brl_cad");
  fprintf(stderr, "ospLoadModule(brl_cad) -> %d\n", (int)brlErr);

  OSPGeometry brlcadProbe = ospNewGeometry("brlcad");
  fprintf(stderr, "probe ospNewGeometry(brlcad) -> %p\n", (void *)brlcadProbe);
  if (brlcadProbe)
    ospRelease(brlcadProbe);

  int rc = 0;
  {
    QApplication a(argc, argv);
    fprintf(stderr, "QApplication created\n");

    MainWindow w;
    fprintf(stderr, "MainWindow created\n");

    w.show();
    fprintf(stderr, "MainWindow shown\n");

    rc = a.exec();
  }

  fprintf(stderr, "event loop ended\n");

  ospShutdown();
  return rc;
}
