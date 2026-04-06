#pragma once

#include <QMainWindow>

class RenderWidget;
class QAction;
class RenderWorkerClient;
class QMenu;

class MainWindow : public QMainWindow
{
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);

 private:
 RenderWidget *renderWidget_ = nullptr;
  RenderWorkerClient *renderWorkerClient_ = nullptr;
  QAction *selectBrlcadObjectAction_ = nullptr;
  QAction *orbitModeAction_ = nullptr;
  QAction *flyModeAction_ = nullptr;
  void setupMenus();
  void updateBrlcadMenuState();
  void chooseAndLoadBrlcadObject(const QString &path, const QStringList &objects);
  QString demoModelsDir() const;
  void populateDemoModelsMenu(QMenu *menu);
  QString defaultDemoPath() const;
  void loadStartupDemo();
};
