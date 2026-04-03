#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QStringList>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QWheelEvent>

#include <ospray/ospray_cpp/ext/rkcommon.h>
#include "imgui.h"
#include "ospraybackend.h"
#include "interactioncontroller.h"

class QThread;
class QTimer;
class RenderBackendWorker;

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

  void setObjectTransform(const rkcommon::math::affine3f &xfm);
  rkcommon::math::affine3f objectTransform() const;
  rkcommon::math::affine3f objectTransform_{rkcommon::math::one};

  

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
  void renderOnce();
  void advanceRender();

  static float clampf(float v, float lo, float hi);
  static rkcommon::math::vec3f normalizeVec(const rkcommon::math::vec3f &v);
  static rkcommon::math::vec3f crossVec(
      const rkcommon::math::vec3f &a, const rkcommon::math::vec3f &b);

  void applyViewAction(const InteractionController::Result &result, const QPoint &delta);
  void applyObjectAction(const InteractionController::Result &result, const QPoint &delta);
  void queueCameraUpdate(bool resetAccumulation);
  void flushQueuedCameraUpdate();
  void queueInteracting(bool interacting);
  void applyBackendSnapshot(const QString &lastError,
      const QString &currentRenderer,
      const rkcommon::math::vec3f &boundsCenter,
      float boundsMaxExtent,
      float lastFrameTimeMs,
      float renderFps,
      uint64_t accumulatedFrames,
      uint64_t watchdogCancelCount,
      uint64_t aoAutoReductionCount,
      int currentScale,
      bool dynamicModeActive,
      bool backoffApplied,
      OsprayBackend::SettingsMode settingsMode,
      OsprayBackend::AutomaticPreset automaticPreset,
      float automaticTargetFrameTimeMs,
      bool automaticAccumulationEnabled,
      int customStartScale,
      float customTargetFrameTimeMs,
      int customAoSamples,
      int customPixelSamples,
      bool customAccumulationEnabled,
      int customMaxAccumulationFrames,
      bool customLowQualityWhileInteracting,
      bool customFullResAccumulationOnly,
      int customWatchdogTimeoutMs);
  void applyBackendImage(const QImage &image);

  QImage image_;
  QSize imageSize_;
  QSize textureSize_;
  QPoint lastMouse_;
  bool backendReady_ = false;
  QThread *backendThread_ = nullptr;
  RenderBackendWorker *backendWorker_ = nullptr;
  QTimer *cameraUpdateTimer_ = nullptr;
  bool pendingCameraReset_ = false;
  bool interactionActive_ = false;
  QString lastError_;
  QString currentRenderer_ = QStringLiteral("scivis");
  QStringList currentBrlcadObjects_;
  rkcommon::math::vec3f boundsCenter_{0.f, 0.f, 0.f};
  float boundsMaxExtent_ = 1.0f;
  float lastFrameTimeMs_ = 0.0f;
  float renderFps_ = 0.0f;
  uint64_t accumulatedFrames_ = 0;
  uint64_t watchdogCancelCount_ = 0;
  uint64_t aoAutoReductionCount_ = 0;
  int currentScale_ = 1;
  bool dynamicModeActive_ = false;
  bool backoffApplied_ = false;
  OsprayBackend::SettingsMode settingsMode_ =
      OsprayBackend::SettingsMode::Automatic;
  OsprayBackend::AutomaticPreset automaticPreset_ =
      OsprayBackend::AutomaticPreset::Balanced;
  float automaticTargetFrameTimeMs_ = 16.0f;
  bool automaticAccumulationEnabled_ = true;
  int customStartScale_ = 8;
  float customTargetFrameTimeMs_ = 16.0f;
  int customAoSamples_ = 1;
  int customPixelSamples_ = 1;
  bool customAccumulationEnabled_ = true;
  int customMaxAccumulationFrames_ = 0;
  bool customLowQualityWhileInteracting_ = true;
  bool customFullResAccumulationOnly_ = true;
  int customWatchdogTimeoutMs_ = 1500;
  GLuint displayTexture_ = 0;
  bool textureDirty_ = false;

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
  QString currentBrlcadPath_;
  QString currentBrlcadObject_;
};
