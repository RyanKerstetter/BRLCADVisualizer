#include "mainwindow.h"
#include "renderwidget.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>

#include <QLabel>


MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
  renderWidget_ = new RenderWidget(this);
  setCentralWidget(renderWidget_);
  resize(1200, 800);
  setWindowTitle("Qt OSPRay Viewer");

  setupMenus();
}

void MainWindow::setupMenus()
{
  QMenu *fileMenu = menuBar()->addMenu("&File");

  QAction *openAction = new QAction("&Open Model...", this);
  fileMenu->addAction(openAction);

  QAction *resetViewAction = new QAction("&Reset View", this);
  fileMenu->addAction(resetViewAction);

  fileMenu->addSeparator();

  QAction *exitAction = new QAction("E&xit", this);
  fileMenu->addAction(exitAction);

  QMenu *viewMenu = menuBar()->addMenu("&View");

  QAction *orbitModeAction = new QAction("Orbit Mode", this);
  orbitModeAction->setCheckable(true);
  orbitModeAction->setChecked(true);

  QAction *flyModeAction = new QAction("Fly Mode", this);
  flyModeAction->setCheckable(true);

  viewMenu->addAction(orbitModeAction);
  viewMenu->addAction(flyModeAction);

  connect(openAction, &QAction::triggered, this, [this]() {
    QString path = QFileDialog::getOpenFileName(this,
        "Open Model",
        QString(),
        "Model Files (*.obj *.g *.stl *.ply);;OBJ Files (*.obj);;BRL-CAD Files (*.g)");

    if (path.isEmpty())
      return;

    QFileInfo info(path);
    const QString ext = info.suffix().toLower();

    if (ext == "obj") {
      if (!renderWidget_->loadModel(path)) {
        const QString detail = renderWidget_->lastError();
        QMessageBox::warning(this,
            "Load Failed",
            detail.isEmpty() ? "Could not load OBJ file." : detail);
      }
      return;
    }

    if (ext == "g") {
      bool ok = false;
      QString obj = QInputDialog::getText(this,
          "Load BRL-CAD Object",
          "Enter object name to render\n(e.g. \"component\", \"all.g\", or a region name):",
          QLineEdit::Normal, QString(), &ok);
      if (!ok)
        return;
      if (!renderWidget_->loadBrlcadModel(path, obj)) {
        const QString detail = renderWidget_->lastError();
        QMessageBox::warning(this, "Load Failed",
            detail.isEmpty()
                ? "Could not load BRL-CAD .g file.\n"
                  "Check that the object name exists in the database."
                : detail);
      }
      return;
    }

    QMessageBox::information(this,
        "Not Yet Implemented",
        "That file type is not implemented yet. Start with OBJ for testing.");
  });

  connect(resetViewAction, &QAction::triggered, this, [this]() {
    renderWidget_->resetView();
  });

  connect(exitAction, &QAction::triggered, this, [this]() { close(); });

  connect(orbitModeAction,
      &QAction::triggered,
      this,
      [this, orbitModeAction, flyModeAction]() {
        orbitModeAction->setChecked(true);
        flyModeAction->setChecked(false);
        renderWidget_->setInputMode(RenderWidget::InputMode::Orbit);
      });

  connect(flyModeAction,
      &QAction::triggered,
      this,
      [this, orbitModeAction, flyModeAction]() {
        orbitModeAction->setChecked(false);
        flyModeAction->setChecked(true);
        renderWidget_->setInputMode(RenderWidget::InputMode::Fly);
      });
}
