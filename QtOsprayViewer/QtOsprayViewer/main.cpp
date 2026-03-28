#include <QApplication>
#include <cstdio>

#include <ospray/ospray.h>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
  fprintf(stderr, "main start\n");

  int ac = argc;
  const char **av = const_cast<const char **>(argv);

  OSPError err = ospInit(&ac, av);
  if (err != OSP_NO_ERROR) {
    fprintf(stderr, "ospInit failed\n");
    return 1;
  }

  fprintf(stderr, "ospInit ok\n");

  ospLoadModule("cpu");
  fprintf(stderr, "cpu module loaded\n");

  ospLoadModule("brl_cad");
  fprintf(stderr, "brl_cad module loaded\n");

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