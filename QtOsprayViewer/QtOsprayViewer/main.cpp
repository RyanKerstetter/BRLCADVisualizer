#include <windows.h>
#include <QApplication>

#include <ospray/ospray.h>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
  MessageBoxA(nullptr, "main start", "debug", MB_OK);

  int ac = argc;
  const char **av = const_cast<const char **>(argv);

  OSPError err = ospInit(&ac, av);
  if (err != OSP_NO_ERROR) {
    MessageBoxA(nullptr, "ospInit failed", "OSPRay", MB_OK | MB_ICONERROR);
    return 1;
  }

  MessageBoxA(nullptr, "ospInit ok", "debug", MB_OK);

  ospLoadModule("cpu");
  MessageBoxA(nullptr, "cpu module loaded", "debug", MB_OK);

 
  int rc = 0;
  {
    QApplication a(argc, argv);
    MessageBoxA(nullptr, "QApplication created", "debug", MB_OK);

    MainWindow w;
    MessageBoxA(nullptr, "MainWindow created", "debug", MB_OK);

    w.show();
    MessageBoxA(nullptr, "MainWindow shown", "debug", MB_OK);

    rc = a.exec();
  }    
      

  MessageBoxA(nullptr, "event loop ended", "debug", MB_OK);

  ospShutdown();
  return rc;
}