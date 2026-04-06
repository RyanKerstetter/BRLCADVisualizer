#include "mainwindow.h"
#include "renderwidget.h"
#include "renderworkerclient.h"

#include <algorithm>
#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QTimer>
#include <QStatusBar>

#include <QLabel>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
  renderWidget_ = new RenderWidget(this);
  renderWorkerClient_ = new RenderWorkerClient(this);
  renderWidget_->setRenderWorkerClient(renderWorkerClient_);
  setCentralWidget(renderWidget_);
  resize(1200, 800);
  setWindowTitle("Interactive BRL-CAD Raytracer");

  connect(renderWidget_,
      &RenderWidget::sceneLoadFinished,
      this,
      [this](bool success, const QString &errorMessage) {
        if (!success) {
          QMessageBox::warning(this,
              "Load Failed",
              errorMessage.isEmpty() ? "Scene load failed." : errorMessage);
        }
        updateBrlcadMenuState();
      });

  connect(renderWorkerClient_,
      &RenderWorkerClient::workerConnectionChanged,
      this,
      [this](bool connected) {
        if (connected) {
          statusBar()->showMessage(QStringLiteral("Render worker connected."));
          renderWidget_->replayWorkerState();
          return;
        }

        statusBar()->showMessage(QStringLiteral("Render worker disconnected. Restarting..."));
        QTimer::singleShot(500, this, [this]() {
          if (renderWorkerClient_->isConnected())
            return;
          if (renderWorkerClient_->restart()) {
            statusBar()->showMessage(QStringLiteral("Render worker reconnected."));
            renderWidget_->replayWorkerState();
          } else {
            statusBar()->showMessage(
                QStringLiteral("Render worker restart failed: %1")
                    .arg(renderWorkerClient_->lastError()));
          }
        });
      });

  const QString workerPath =
      QCoreApplication::applicationDirPath() + QStringLiteral("/IBRTRenderWorker.exe");
  if (!renderWorkerClient_->start(workerPath)) {
    statusBar()->showMessage(
        QStringLiteral("Render worker unavailable: %1").arg(renderWorkerClient_->lastError()));
  } else {
    statusBar()->showMessage(QStringLiteral("Render worker connected."));
  }

  setupMenus();
  QTimer::singleShot(0, this, [this]() { loadStartupDemo(); });
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
  QMenu *brlcadMenu = menuBar()->addMenu("&Select Model");
  QMenu *demoMenu = fileMenu->addMenu("&Demo Models");

  QAction *orbitModeAction = new QAction("Orbit Mode", this);
  orbitModeAction->setCheckable(true);
  orbitModeAction->setChecked(true);

  QAction *flyModeAction = new QAction("Fly Mode", this);
  flyModeAction->setCheckable(true);

  QActionGroup *upAxisGroup = new QActionGroup(this);
  upAxisGroup->setExclusive(true);

  QAction *yUpAction = new QAction("Y-Up", this);
  yUpAction->setCheckable(true);
  upAxisGroup->addAction(yUpAction);

  QAction *zUpAction = new QAction("Z-Up", this);
  zUpAction->setCheckable(true);
  zUpAction->setChecked(true);
  upAxisGroup->addAction(zUpAction);

  viewMenu->addAction(orbitModeAction);
  viewMenu->addAction(flyModeAction);
  viewMenu->addSeparator();
  viewMenu->addAction(yUpAction);
  viewMenu->addAction(zUpAction);

  selectBrlcadObjectAction_ = new QAction("Select Object...", this);
  brlcadMenu->addAction(selectBrlcadObjectAction_);
  updateBrlcadMenuState();
  populateDemoModelsMenu(demoMenu);

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
      chooseAndLoadBrlcadObject(path, renderWidget_->listBrlcadObjects(path));
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

  connect(yUpAction, &QAction::triggered, this, [this, yUpAction, zUpAction]() {
    yUpAction->setChecked(true);
    zUpAction->setChecked(false);
    renderWidget_->setUpAxis(RenderWidget::UpAxis::Y);
  });

  connect(zUpAction, &QAction::triggered, this, [this, yUpAction, zUpAction]() {
    zUpAction->setChecked(true);
    yUpAction->setChecked(false);
    renderWidget_->setUpAxis(RenderWidget::UpAxis::Z);
  });

  connect(selectBrlcadObjectAction_, &QAction::triggered, this, [this]() {
    chooseAndLoadBrlcadObject(
        renderWidget_->currentBrlcadPath(), renderWidget_->currentBrlcadObjects());
  });
}

void MainWindow::populateDemoModelsMenu(QMenu *menu)
{
  if (!menu)
    return;
  const QString dbDir = demoModelsDir();

  struct DemoModel
  {
    const char *fileName;
    const char *label;
  };

  const DemoModel models[] = {
      {"moss.g", "Moss"},
      {"havoc.g", "Havoc"},
  };

  bool addedAny = false;
  for (const DemoModel &model : models) {
    const QString path = QDir(dbDir).filePath(QString::fromLatin1(model.fileName));
    if (!QFileInfo::exists(path))
      continue;

    QAction *action = new QAction(QString::fromLatin1(model.label), this);
    connect(action, &QAction::triggered, this, [this, path]() {
      chooseAndLoadBrlcadObject(path, renderWidget_->listBrlcadObjects(path));
    });
    menu->addAction(action);
    addedAny = true;
  }

  if (!addedAny) {
    QAction *missingAction = new QAction("Demo Models Unavailable", this);
    missingAction->setEnabled(false);
    menu->addAction(missingAction);
  }
}

QString MainWindow::defaultDemoPath() const
{
  const QString dbDir = demoModelsDir();
  const QString mossPath = QDir(dbDir).filePath(QStringLiteral("moss.g"));
  if (QFileInfo::exists(mossPath))
    return mossPath;
  return QString();
}

QString MainWindow::demoModelsDir() const
{
  const QString localDir =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models"));
  if (QFileInfo::exists(QDir(localDir).filePath(QStringLiteral("moss.g"))))
    return localDir;

#ifdef BRLCAD_INSTALL_PREFIX
  return QDir(QStringLiteral(BRLCAD_INSTALL_PREFIX)).filePath(QStringLiteral("share/db"));
#else
  return QString();
#endif
}

void MainWindow::loadStartupDemo()
{
  if (!renderWidget_ || renderWidget_->hasBrlcadScene() || !renderWidget_->currentBrlcadPath().isEmpty())
    return;

  const QString path = defaultDemoPath();
  if (path.isEmpty())
    return;

  if (!renderWidget_->loadBrlcadModel(path, QStringLiteral("all.g"))) {
    statusBar()->showMessage(QStringLiteral("Startup demo load failed: %1")
                                 .arg(renderWidget_->lastError()),
        5000);
  }
}

void MainWindow::updateBrlcadMenuState()
{
  if (selectBrlcadObjectAction_)
    selectBrlcadObjectAction_->setEnabled(renderWidget_->hasBrlcadScene());
}

void MainWindow::chooseAndLoadBrlcadObject(
    const QString &path, const QStringList &objects)
{
  if (path.isEmpty())
    return;

  QStringList choices = objects;
  if (choices.isEmpty()) {
    QMessageBox::warning(this,
        "No Objects Found",
        "No selectable BRL-CAD objects were found in this .g file.");
    return;
  }

  bool ok = false;
  const QString current = renderWidget_->currentBrlcadObject();
  const qsizetype foundIndex = choices.indexOf(current);
  const int currentIndex = foundIndex >= 0 ? static_cast<int>(foundIndex) : 0;
  const QString obj = QInputDialog::getItem(this,
      "Load BRL-CAD Object",
      "Choose or enter object name to render:",
      choices,
      currentIndex,
      true,
      &ok);
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
}
