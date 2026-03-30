#pragma once

#include <QMainWindow>

class RenderWidget;
class QAction;

class MainWindow : public QMainWindow
{
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);

 private:
  RenderWidget *renderWidget_ = nullptr;
  QAction *selectBrlcadObjectAction_ = nullptr;
  void setupMenus();
  void updateBrlcadMenuState();
  void chooseAndLoadBrlcadObject(const QString &path, const QStringList &objects);
};
