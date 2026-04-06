#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QStringList>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWheelEvent>

#include <atomic>
#include <functional>
#include <thread>

#include <ospray/ospray_cpp/ext/rkcommon.h>
#include "renderworkerclient.h"
#include "imgui.h"
#include "ospraybackend.h"
#include "interactioncontroller.h"

class RenderWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
  Q_OBJECT

 public:
  enum class InputMode
  {
    Orbit,
    Fly
  };

  enum class ManipulationTarget
  {
    View,
    Object
  };

  enum class UpAxis
  {
    Y,
    Z
  };

  ManipulationTarget manipulationTarget_ = ManipulationTarget::View;
  explicit RenderWidget(QWidget *parent = nullptr);
  ~RenderWidget() override;

  bool loadModel(const QString &path);
  bool loadBrlcadModel(const QString &path, const QString &topObject = QString());
  QStringList listBrlcadObjects(const QString &path) const;
  bool reloadBrlcadObject(const QString &topObject);
  QString lastError() const;
  void resetView();
  void setInputMode(InputMode mode);
  void setUpAxis(UpAxis axis);
  UpAxis upAxis() const;
  QString currentBrlcadPath() const;
  QString currentBrlcadObject() const;
  QStringList currentBrlcadObjects() const;
  bool hasBrlcadScene() const;
  void setRenderWorkerClient(RenderWorkerClient *client);
  void replayWorkerState();

  void setObjectTransform(const rkcommon::math::affine3f &xfm);
  rkcommon::math::affine3f objectTransform() const;
  rkcommon::math::affine3f objectTransform_{rkcommon::math::one};

 signals:
  void sceneLoadFinished(bool success, const QString &errorMessage);

  

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void keyReleaseEvent(QKeyEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void focusInEvent(QFocusEvent *e) override;
  void focusOutEvent(QFocusEvent *e) override;
 private:
  static float fitDistanceFromBounds(float maxExtent, float fovyDeg);
  void syncFlyFromOrbit();
  void syncOrbitFromFly();
  rkcommon::math::vec3f currentCameraPosition() const;
  rkcommon::math::vec3f currentCameraForward() const;
  rkcommon::math::vec3f currentCameraUp() const;
  bool projectWorldToScreen(const rkcommon::math::vec3f &worldPos, QPointF &screenPos) const;
  void drawRotationAxisOverlay(QPainter &p);
  rkcommon::math::vec3f worldUp() const;
  rkcommon::math::vec3f worldForwardReference() const;
  rkcommon::math::vec3f forwardFromAngles(float yaw, float pitch) const;
  void anglesFromForward(
      const rkcommon::math::vec3f &forward, float &yaw, float &pitch) const;
  rkcommon::math::vec3f projectOntoPlane(
      const rkcommon::math::vec3f &v, const rkcommon::math::vec3f &normal) const;
  rkcommon::math::vec3f orbitRight() const;
  void alignOrbitUpToReference();
  rkcommon::math::vec3f rotateAroundAxis(
      const rkcommon::math::vec3f &v, const rkcommon::math::vec3f &axis, float angle) const;
  void rotateOrbit(float yawDelta, float pitchDelta);
  float flyMoveFactor_ = 0.005f;
  void syncCameraToBackend();
  bool usingWorkerRenderPath() const;
  void resetAccumulationTargets();
  rkcommon::math::vec3f sceneBoundsCenter() const;
  float sceneBoundsMaxExtent() const;
  void renderOnce();
  void advanceRender();
  void startAsyncLoad(const std::function<void()> &loader, const QString &statusText);

  static float clampf(float v, float lo, float hi);
  static rkcommon::math::vec3f normalizeVec(const rkcommon::math::vec3f &v);
  static rkcommon::math::vec3f crossVec(
      const rkcommon::math::vec3f &a, const rkcommon::math::vec3f &b);

  void applyViewAction(const InteractionController::Result &result, const QPoint &delta);
  void applyObjectAction(const InteractionController::Result &result, const QPoint &delta);

  OsprayBackend backend_;
  QImage image_;
  QPoint lastMouse_;
  QTimer *renderTimer_ = nullptr;
  bool backendReady_ = false;
  int renderBudgetMs_ = 6;

  InputMode inputMode_ = InputMode::Orbit;
  UpAxis upAxis_ = UpAxis::Z;

  // Orbit camera
  rkcommon::math::vec3f center_{0.f, 0.f, 1.5f};
  rkcommon::math::vec3f orbitForward_{0.f, 1.f, 0.f};
  rkcommon::math::vec3f orbitUp_{0.f, 0.f, 1.f};

  float yaw_ = 0.3f;
  float pitch_ = 0.2f;
  float dist_ = 4.0f;
  float fovy_ = 60.0f;

  float orbitSpeed_ = 0.01f;
  float panSpeed_ = 0.0025f;
  float zoomFactor_ = 0.9f;

  // Fly camera
  rkcommon::math::vec3f flyPos_ = rkcommon::math::vec3f(0.f, 0.f, 5.f);
  float flyYaw_ = 0.f;
  float flyPitch_ = 0.f;

  bool imguiMouseDown_[3] = {false, false, false};
  float imguiMouseWheel_ = 0.0f;
  QPointF imguiMousePos_{0.0, 0.0};
  bool imguiHasFocus_ = false;
  bool imguiVisible_ = true;
  QString currentBrlcadPath_;
  QString currentBrlcadObject_;
  QStringList currentBrlcadObjects_;
  QString currentModelPath_;
  bool currentSceneIsObj_ = false;
  QString currentRenderer_ = QStringLiteral("scivis");
  RenderWorkerClient::RenderSettingsState workerSettings_;
  rkcommon::math::vec3f sceneBoundsMin_{-1.f, -1.f, -1.f};
  rkcommon::math::vec3f sceneBoundsMax_{1.f, 1.f, 1.f};
  QString lastError_;
  QString loadStatusText_;
  float workerLastFrameTimeMs_ = 0.0f;
  float workerRenderFPS_ = 0.0f;
  uint64_t workerAccumulatedFrames_ = 0;
  uint64_t workerWatchdogCancels_ = 0;
  uint64_t workerAoAutoReductions_ = 0;
  std::atomic<bool> sceneLoadInProgress_{false};
  std::thread sceneLoadThread_;
  RenderWorkerClient *renderWorkerClient_ = nullptr;
};
