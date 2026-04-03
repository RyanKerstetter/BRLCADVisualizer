#include "renderwidget.h"

#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_impl_opengl3.h"

using rkcommon::math::vec3f;

namespace {
struct BackendSnapshot
{
  QString lastError;
  QString currentRenderer;
  float boundsCenterX = 0.0f;
  float boundsCenterY = 0.0f;
  float boundsCenterZ = 0.0f;
  float boundsMaxExtent = 1.0f;
  float lastFrameTimeMs = 0.0f;
  float renderFps = 0.0f;
  uint64_t accumulatedFrames = 0;
  uint64_t watchdogCancelCount = 0;
  uint64_t aoAutoReductionCount = 0;
  int currentScale = 1;
  bool dynamicModeActive = false;
  bool backoffApplied = false;
  OsprayBackend::SettingsMode settingsMode =
      OsprayBackend::SettingsMode::Automatic;
  OsprayBackend::AutomaticPreset automaticPreset =
      OsprayBackend::AutomaticPreset::Balanced;
  float automaticTargetFrameTimeMs = 16.0f;
  bool automaticAccumulationEnabled = true;
  int customStartScale = 8;
  float customTargetFrameTimeMs = 16.0f;
  int customAoSamples = 1;
  int customPixelSamples = 1;
  bool customAccumulationEnabled = true;
  int customMaxAccumulationFrames = 0;
  bool customLowQualityWhileInteracting = true;
  bool customFullResAccumulationOnly = true;
  int customWatchdogTimeoutMs = 1500;
};
}

Q_DECLARE_METATYPE(BackendSnapshot)

class RenderBackendWorker : public QObject
{
  Q_OBJECT

 public:
  explicit RenderBackendWorker(QObject *parent = nullptr) : QObject(parent) {}

  void initialize(int w, int h)
  {
    backend_.init();
    backend_.resize(w, h);
    backendReady_ = true;
    emit stateChanged(snapshot());
  }

  void start()
  {
    if (renderTimer_)
      return;

    renderTimer_ = new QTimer(this);
    renderTimer_->setInterval(16);
    connect(renderTimer_, &QTimer::timeout, this, &RenderBackendWorker::renderTick);
    renderTimer_->start();
  }

  void stop()
  {
    if (renderTimer_)
      renderTimer_->stop();
  }

  void resizeBackend(int w, int h)
  {
    if (!backendReady_)
      return;
    backend_.resize(w, h);
    emit stateChanged(snapshot());
  }

  BackendSnapshot snapshot() const
  {
    const auto center = backend_.getBoundsCenter();

    BackendSnapshot state;
    state.lastError = QString::fromStdString(backend_.lastError());
    state.currentRenderer = QString::fromStdString(backend_.currentRenderer());
    state.boundsCenterX = center.x;
    state.boundsCenterY = center.y;
    state.boundsCenterZ = center.z;
    state.boundsMaxExtent = backend_.getBoundsMaxExtent();
    state.lastFrameTimeMs = backend_.lastFrameTimeMs();
    state.renderFps = backend_.renderFPS();
    state.accumulatedFrames = backend_.accumulatedFrames();
    state.watchdogCancelCount = backend_.watchdogCancelCount();
    state.aoAutoReductionCount = backend_.aoAutoReductionCount();
    state.currentScale = backend_.currentScale();
    state.dynamicModeActive = backend_.dynamicModeActive();
    state.backoffApplied = backend_.backoffApplied();
    state.settingsMode = backend_.settingsMode();
    state.automaticPreset = backend_.automaticPreset();
    state.automaticTargetFrameTimeMs = backend_.automaticTargetFrameTimeMs();
    state.automaticAccumulationEnabled =
        backend_.automaticAccumulationEnabled();
    state.customStartScale = backend_.customStartScale();
    state.customTargetFrameTimeMs = backend_.customTargetFrameTimeMs();
    state.customAoSamples = backend_.customAoSamples();
    state.customPixelSamples = backend_.customPixelSamples();
    state.customAccumulationEnabled = backend_.customAccumulationEnabled();
    state.customMaxAccumulationFrames = backend_.customMaxAccumulationFrames();
    state.customLowQualityWhileInteracting =
        backend_.customLowQualityWhileInteracting();
    state.customFullResAccumulationOnly =
        backend_.customFullResAccumulationOnly();
    state.customWatchdogTimeoutMs = backend_.customWatchdogTimeoutMs();
    return state;
  }

  void publishState()
  {
    emit stateChanged(snapshot());
  }

  bool loadObjFile(const QString &path)
  {
    const bool ok = backend_.loadObj(path.toStdString());
    publishState();
    return ok;
  }

  bool loadBrlcadFile(const QString &path, const QString &topObject)
  {
    const bool ok =
        backend_.loadBrlcad(path.toStdString(), topObject.toStdString());
    publishState();
    return ok;
  }

  QStringList listBrlcadObjects(const QString &path) const
  {
    QStringList out;
    const auto names = backend_.listBrlcadObjects(path.toStdString());
    for (const auto &name : names)
      out << QString::fromStdString(name);
    return out;
  }

  OsprayBackend backend_;

 signals:
  void frameReady(const QImage &image, const BackendSnapshot &snapshot);
  void stateChanged(const BackendSnapshot &snapshot);

 private slots:
  void renderTick()
  {
    if (!backendReady_)
      return;

    const bool updatedImage = backend_.advanceRender(0);
    if (!updatedImage)
      return;

    const uint32_t *px = backend_.pixels();
    const int w = backend_.width();
    const int h = backend_.height();
    if (!px || w <= 0 || h <= 0)
      return;

    QImage image(w, h, QImage::Format_RGBA8888);
    std::memcpy(image.bits(), px, size_t(w) * size_t(h) * 4);
    emit frameReady(image, snapshot());
  }

 private:
  bool backendReady_ = false;
  QTimer *renderTimer_ = nullptr;
};

template <typename F>
void invokeWorkerAsync(RenderBackendWorker *worker, F &&fn)
{
  QMetaObject::invokeMethod(
      worker,
      [worker, fn = std::forward<F>(fn)]() mutable { fn(*worker); },
      Qt::QueuedConnection);
}

template <typename F>
void invokeWorkerBlocking(RenderBackendWorker *worker, F &&fn)
{
  QMetaObject::invokeMethod(
      worker,
      [worker, fn = std::forward<F>(fn)]() mutable { fn(*worker); },
      Qt::BlockingQueuedConnection);
}

void RenderWidget::applyBackendSnapshot(const QString &lastError,
    const QString &currentRenderer,
    const vec3f &boundsCenter,
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
    int customWatchdogTimeoutMs)
{
  lastError_ = lastError;
  currentRenderer_ = currentRenderer;
  boundsCenter_ = boundsCenter;
  boundsMaxExtent_ = boundsMaxExtent;
  lastFrameTimeMs_ = lastFrameTimeMs;
  renderFps_ = renderFps;
  accumulatedFrames_ = accumulatedFrames;
  watchdogCancelCount_ = watchdogCancelCount;
  aoAutoReductionCount_ = aoAutoReductionCount;
  currentScale_ = currentScale;
  dynamicModeActive_ = dynamicModeActive;
  backoffApplied_ = backoffApplied;
  settingsMode_ = settingsMode;
  automaticPreset_ = automaticPreset;
  automaticTargetFrameTimeMs_ = automaticTargetFrameTimeMs;
  automaticAccumulationEnabled_ = automaticAccumulationEnabled;
  customStartScale_ = customStartScale;
  customTargetFrameTimeMs_ = customTargetFrameTimeMs;
  customAoSamples_ = customAoSamples;
  customPixelSamples_ = customPixelSamples;
  customAccumulationEnabled_ = customAccumulationEnabled;
  customMaxAccumulationFrames_ = customMaxAccumulationFrames;
  customLowQualityWhileInteracting_ = customLowQualityWhileInteracting;
  customFullResAccumulationOnly_ = customFullResAccumulationOnly;
  customWatchdogTimeoutMs_ = customWatchdogTimeoutMs;
}

RenderWidget::RenderWidget(QWidget *parent) : QOpenGLWidget(parent)
{
  setFocusPolicy(Qt::StrongFocus);

  qRegisterMetaType<BackendSnapshot>("BackendSnapshot");

  backendThread_ = new QThread(this);
  backendWorker_ = new RenderBackendWorker();
  backendWorker_->moveToThread(backendThread_);
  connect(backendThread_, &QThread::finished, backendWorker_, &QObject::deleteLater);
  connect(backendWorker_,
      &RenderBackendWorker::stateChanged,
      this,
      [this](const BackendSnapshot &snapshot) {
        applyBackendSnapshot(snapshot.lastError,
            snapshot.currentRenderer,
            vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
            snapshot.boundsMaxExtent,
            snapshot.lastFrameTimeMs,
            snapshot.renderFps,
            snapshot.accumulatedFrames,
            snapshot.watchdogCancelCount,
            snapshot.aoAutoReductionCount,
            snapshot.currentScale,
            snapshot.dynamicModeActive,
            snapshot.backoffApplied,
            snapshot.settingsMode,
            snapshot.automaticPreset,
            snapshot.automaticTargetFrameTimeMs,
            snapshot.automaticAccumulationEnabled,
            snapshot.customStartScale,
            snapshot.customTargetFrameTimeMs,
            snapshot.customAoSamples,
            snapshot.customPixelSamples,
            snapshot.customAccumulationEnabled,
            snapshot.customMaxAccumulationFrames,
            snapshot.customLowQualityWhileInteracting,
            snapshot.customFullResAccumulationOnly,
            snapshot.customWatchdogTimeoutMs);
      },
      Qt::QueuedConnection);
  connect(backendWorker_,
      &RenderBackendWorker::frameReady,
      this,
      [this](const QImage &image, const BackendSnapshot &snapshot) {
        applyBackendImage(image);
        applyBackendSnapshot(snapshot.lastError,
            snapshot.currentRenderer,
            vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
            snapshot.boundsMaxExtent,
            snapshot.lastFrameTimeMs,
            snapshot.renderFps,
            snapshot.accumulatedFrames,
            snapshot.watchdogCancelCount,
            snapshot.aoAutoReductionCount,
            snapshot.currentScale,
            snapshot.dynamicModeActive,
            snapshot.backoffApplied,
            snapshot.settingsMode,
            snapshot.automaticPreset,
            snapshot.automaticTargetFrameTimeMs,
            snapshot.automaticAccumulationEnabled,
            snapshot.customStartScale,
            snapshot.customTargetFrameTimeMs,
            snapshot.customAoSamples,
            snapshot.customPixelSamples,
            snapshot.customAccumulationEnabled,
            snapshot.customMaxAccumulationFrames,
            snapshot.customLowQualityWhileInteracting,
            snapshot.customFullResAccumulationOnly,
            snapshot.customWatchdogTimeoutMs);
      },
      Qt::QueuedConnection);
  backendThread_->start();

  cameraUpdateTimer_ = new QTimer(this);
  cameraUpdateTimer_->setSingleShot(true);
  cameraUpdateTimer_->setInterval(4);
  connect(cameraUpdateTimer_, &QTimer::timeout, this, &RenderWidget::flushQueuedCameraUpdate);

  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
}

RenderWidget::~RenderWidget()
{
  if (backendWorker_) {
    invokeWorkerBlocking(backendWorker_,
        [](RenderBackendWorker &worker) { worker.stop(); });
  }
  if (backendThread_) {
    backendThread_->quit();
    backendThread_->wait();
  }

  makeCurrent();
  if (displayTexture_ != 0) {
    glDeleteTextures(1, &displayTexture_);
    displayTexture_ = 0;
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui::DestroyContext();
  doneCurrent();
}

void RenderWidget::applyViewAction(
    const InteractionController::Result &result, const QPoint &delta)
{
  using Action = InteractionController::Action;
  using Axis = InteractionController::AxisConstraint;

  if (result.action == Action::None)
    return;

  vec3f forward = orbitForward_;
  vec3f right = orbitRight();
  vec3f upCam = orbitUp_;

  if (result.action == Action::Translate) {
    float sx = float(delta.x()) * panSpeed_ * dist_;
    float sy = float(delta.y()) * panSpeed_ * dist_;

    vec3f move(0.f, 0.f, 0.f);

    if (result.axis == Axis::Free) {
      move = vec3f(-right.x * sx + upCam.x * sy,
          -right.y * sx + upCam.y * sy,
          -right.z * sx + upCam.z * sy);
    } else {
      float axisDelta = float(delta.x() - delta.y()) * panSpeed_ * dist_;

      if (result.axis == Axis::X)
        move = vec3f(axisDelta, 0.f, 0.f);
      else if (result.axis == Axis::Y)
        move = vec3f(0.f, axisDelta, 0.f);
      else if (result.axis == Axis::Z)
        move = vec3f(0.f, 0.f, axisDelta);
    }

    center_ = vec3f(center_.x + move.x, center_.y + move.y, center_.z + move.z);
  }

  else if (result.action == Action::Rotate) {
    float dx = delta.x() * orbitSpeed_;
    float dy = delta.y() * orbitSpeed_;

    if (result.axis == Axis::Free) {
      rotateOrbit(dx, dy);
    } else if (result.axis == Axis::X) {
      const vec3f axis(1.f, 0.f, 0.f);
      orbitForward_ = normalizeVec(rotateAroundAxis(orbitForward_, axis, dy));
      orbitUp_ = normalizeVec(rotateAroundAxis(orbitUp_, axis, dy));
    } else if (result.axis == Axis::Y) {
      const vec3f axis(0.f, 1.f, 0.f);
      orbitForward_ = normalizeVec(rotateAroundAxis(orbitForward_, axis, dx));
      orbitUp_ = normalizeVec(rotateAroundAxis(orbitUp_, axis, dx));
    } else if (result.axis == Axis::Z) {
      const vec3f axis(0.f, 0.f, 1.f);
      orbitForward_ = normalizeVec(rotateAroundAxis(orbitForward_, axis, dx));
      orbitUp_ = normalizeVec(rotateAroundAxis(orbitUp_, axis, dx));
    }
  }

  else if (result.action == Action::Scale) {
    float amount = float(delta.y()) * 0.01f;

    float maxExtent = boundsMaxExtent_;
    if (maxExtent < 0.001f)
      maxExtent = 1.0f;

    float minDist = std::max(maxExtent * 1e-8f, 1e-8f);
    float maxDist = std::max(maxExtent * 100.0f, 10.0f);

    dist_ *= std::pow(1.05f, amount * 10.0f);
    dist_ = clampf(dist_, minDist, maxDist);
  }

  queueCameraUpdate(true);
}

float RenderWidget::fitDistanceFromBounds(float maxExtent, float fovyDeg)
{
  if (maxExtent < 0.001f)
    maxExtent = 1.0f;

  float halfAngle = 0.5f * fovyDeg * 3.14159265f / 180.0f;
  halfAngle = std::max(halfAngle, 0.05f);

  return (0.5f * maxExtent) / std::tan(halfAngle) * 1.3f;
}

void RenderWidget::syncFlyFromOrbit()
{
  vec3f forward = orbitForward_;
  vec3f eye(center_.x - dist_ * forward.x,
      center_.y - dist_ * forward.y,
      center_.z - dist_ * forward.z);

  flyPos_ = eye;
  anglesFromForward(forward, flyYaw_, flyPitch_);
}

void RenderWidget::syncOrbitFromFly()
{
  vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
  forward = normalizeVec(forward);

  vec3f eye = flyPos_;
  center_ = vec3f(eye.x + forward.x * dist_,
      eye.y + forward.y * dist_,
      eye.z + forward.z * dist_);

  orbitForward_ = forward;
  alignOrbitUpToReference();
  yaw_ = flyYaw_;
  pitch_ = flyPitch_;
}

float RenderWidget::clampf(float v, float lo, float hi)
{
  return std::max(lo, std::min(hi, v));
}

vec3f RenderWidget::normalizeVec(const vec3f &v)
{
  float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len <= 1e-8f)
    return vec3f(0.f, 0.f, 1.f);

  return vec3f(v.x / len, v.y / len, v.z / len);
}

vec3f RenderWidget::crossVec(const vec3f &a, const vec3f &b)
{
  return vec3f(
      a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

void RenderWidget::initializeGL()
{
  initializeOpenGLFunctions();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(width()), float(height()));

  ImGui_ImplOpenGL3_Init("#version 130");

  glGenTextures(1, &displayTexture_);
  glBindTexture(GL_TEXTURE_2D, displayTexture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  BackendSnapshot snapshot;
  invokeWorkerBlocking(backendWorker_, [this](RenderBackendWorker &worker) {
    worker.initialize(std::max(1, width()), std::max(1, height()));
    worker.start();
  });
  invokeWorkerBlocking(backendWorker_, [&snapshot](RenderBackendWorker &worker) {
    snapshot = worker.snapshot();
  });
  applyBackendSnapshot(snapshot.lastError,
      snapshot.currentRenderer,
      vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
      snapshot.boundsMaxExtent,
      snapshot.lastFrameTimeMs,
      snapshot.renderFps,
      snapshot.accumulatedFrames,
      snapshot.watchdogCancelCount,
      snapshot.aoAutoReductionCount,
      snapshot.currentScale,
      snapshot.dynamicModeActive,
      snapshot.backoffApplied,
      snapshot.settingsMode,
      snapshot.automaticPreset,
      snapshot.automaticTargetFrameTimeMs,
      snapshot.automaticAccumulationEnabled,
      snapshot.customStartScale,
      snapshot.customTargetFrameTimeMs,
      snapshot.customAoSamples,
      snapshot.customPixelSamples,
      snapshot.customAccumulationEnabled,
      snapshot.customMaxAccumulationFrames,
      snapshot.customLowQualityWhileInteracting,
      snapshot.customFullResAccumulationOnly,
      snapshot.customWatchdogTimeoutMs);
  backendReady_ = true;
  resetView();
}

void RenderWidget::resizeGL(int w, int h)
{
  ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));

  if (backendWorker_) {
    BackendSnapshot snapshot;
    invokeWorkerBlocking(
        backendWorker_, [w, h, &snapshot](RenderBackendWorker &worker) {
          worker.resizeBackend(std::max(1, w), std::max(1, h));
          snapshot = worker.snapshot();
        });
    applyBackendSnapshot(snapshot.lastError,
        snapshot.currentRenderer,
        vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
        snapshot.boundsMaxExtent,
        snapshot.lastFrameTimeMs,
        snapshot.renderFps,
        snapshot.accumulatedFrames,
        snapshot.watchdogCancelCount,
        snapshot.aoAutoReductionCount,
        snapshot.currentScale,
        snapshot.dynamicModeActive,
        snapshot.backoffApplied,
        snapshot.settingsMode,
        snapshot.automaticPreset,
        snapshot.automaticTargetFrameTimeMs,
        snapshot.automaticAccumulationEnabled,
        snapshot.customStartScale,
        snapshot.customTargetFrameTimeMs,
        snapshot.customAoSamples,
        snapshot.customPixelSamples,
        snapshot.customAccumulationEnabled,
        snapshot.customMaxAccumulationFrames,
        snapshot.customLowQualityWhileInteracting,
        snapshot.customFullResAccumulationOnly,
        snapshot.customWatchdogTimeoutMs);
  }
  resetView();
}

void RenderWidget::syncCameraToBackend()
{
  queueCameraUpdate(false);
}

void RenderWidget::renderOnce() {}

void RenderWidget::advanceRender() {}

void RenderWidget::queueCameraUpdate(bool resetAccumulation)
{
  if (!backendWorker_)
    return;

  pendingCameraReset_ = pendingCameraReset_ || resetAccumulation;

  if (interactionActive_) {
    cameraUpdateTimer_->start();
    return;
  }

  flushQueuedCameraUpdate();
}

void RenderWidget::flushQueuedCameraUpdate()
{
  if (!backendWorker_)
    return;

  vec3f eye(0.f, 0.f, 0.f);
  vec3f center(0.f, 0.f, 0.f);
  vec3f up(0.f, 0.f, 1.f);
  float fovy = fovy_;

  if (inputMode_ == InputMode::Orbit) {
    const vec3f forward = orbitForward_;
    center = center_;
    eye = vec3f(center.x - dist_ * forward.x,
        center.y - dist_ * forward.y,
        center.z - dist_ * forward.z);
    up = orbitUp_;
  } else {
    flyPitch_ = clampf(flyPitch_, -1.4f, 1.4f);
    const vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
    eye = flyPos_;
    center = vec3f(flyPos_.x + forward.x,
        flyPos_.y + forward.y,
        flyPos_.z + forward.z);
    up = worldUp();
  }

  const bool resetAccumulation = pendingCameraReset_;
  pendingCameraReset_ = false;

  invokeWorkerAsync(backendWorker_,
      [eye, center, up, fovy, resetAccumulation](RenderBackendWorker &worker) {
        if (resetAccumulation)
          worker.backend_.resetAccumulation();
        worker.backend_.setCamera(eye, center, up, fovy);
      });
}

void RenderWidget::queueInteracting(bool interacting)
{
  if (!backendWorker_)
    return;

  interactionActive_ = interacting;
  if (!interactionActive_ && cameraUpdateTimer_->isActive())
    cameraUpdateTimer_->stop();

  invokeWorkerAsync(backendWorker_,
      [interacting](RenderBackendWorker &worker) {
        worker.backend_.setInteracting(interacting);
      });

  if (!interactionActive_ && pendingCameraReset_)
    flushQueuedCameraUpdate();
}

void RenderWidget::applyBackendImage(const QImage &image)
{
  image_ = image;
  imageSize_ = image.size();
  textureDirty_ = true;
  update();
}

void RenderWidget::paintGL()
{
  glViewport(0, 0, width() * devicePixelRatioF(), height() * devicePixelRatioF());
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (displayTexture_ != 0 && !image_.isNull()) {
    glBindTexture(GL_TEXTURE_2D, displayTexture_);
    if (textureDirty_) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      if (textureSize_ == imageSize_) {
        glTexSubImage2D(GL_TEXTURE_2D,
            0,
            0,
            0,
            imageSize_.width(),
            imageSize_.height(),
            GL_BGRA,
            GL_UNSIGNED_BYTE,
            image_.constBits());
      } else {
        glTexImage2D(GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            image_.width(),
            image_.height(),
            0,
            GL_BGRA,
            GL_UNSIGNED_BYTE,
            image_.constBits());
        textureSize_ = imageSize_;
      }
      textureDirty_ = false;
    }

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.f, 1.f);
    glVertex2f(-1.f, -1.f);
    glTexCoord2f(1.f, 1.f);
    glVertex2f(1.f, -1.f);
    glTexCoord2f(0.f, 0.f);
    glVertex2f(-1.f, 1.f);
    glTexCoord2f(1.f, 0.f);
    glVertex2f(1.f, 1.f);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  ImGuiIO &io = ImGui::GetIO();

  // IMPORTANT: logical widget coordinates
  io.DisplaySize = ImVec2(float(width()), float(height()));

  // IMPORTANT: framebuffer scale for HiDPI
  const float dpr = float(devicePixelRatioF());
  io.DisplayFramebufferScale = ImVec2(dpr, dpr);

  io.DeltaTime = 1.0f / 60.0f;

  io.AddFocusEvent(imguiHasFocus_);
  io.AddMousePosEvent(float(imguiMousePos_.x()), float(imguiMousePos_.y()));
  io.AddMouseButtonEvent(0, imguiMouseDown_[0]);
  io.AddMouseButtonEvent(1, imguiMouseDown_[1]);
  io.AddMouseButtonEvent(2, imguiMouseDown_[2]);

  if (imguiMouseWheel_ != 0.0f) {
    io.AddMouseWheelEvent(0.0f, imguiMouseWheel_);
    imguiMouseWheel_ = 0.0f;
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();

    ImGui::Begin("Viewer");

  ImGui::Separator();
  ImGui::Text("Stats");

  ImGui::Text("UI FPS: %.1f", ImGui::GetIO().Framerate);
  ImGui::Text("Render time: %.2f ms", lastFrameTimeMs_);
  ImGui::Text("Render FPS: %.1f", renderFps_);
  ImGui::Text("Accumulated frames: %llu",
      static_cast<unsigned long long>(accumulatedFrames_));
  ImGui::Text("Watchdog cancels: %llu",
      static_cast<unsigned long long>(watchdogCancelCount_));
  ImGui::Text("AO auto-reductions: %llu",
      static_cast<unsigned long long>(aoAutoReductionCount_));
  ImGui::Text("Up Axis: %s", upAxis_ == UpAxis::Z ? "Z" : "Y");
  ImGui::Text("Resolution: %d x %d", width(), height());

  ImGui::Separator();
  ImGui::Text("Renderer");

  int rendererMode = 0;
  if (currentRenderer_ == "scivis")
    rendererMode = 1;
  else if (currentRenderer_ == "pathtracer")
    rendererMode = 2;
  else
    rendererMode = 0;

  if (ImGui::RadioButton("ao", rendererMode == 0)) {
    rendererMode = 0;
    currentRenderer_ = QStringLiteral("ao");
    invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
      worker.backend_.setRenderer("ao");
      worker.backend_.resetAccumulation();
      worker.publishState();
    });
    update();
  }
  if (ImGui::RadioButton("SciVis", rendererMode == 1)) {
    rendererMode = 1;
    currentRenderer_ = QStringLiteral("scivis");
    invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
      worker.backend_.setRenderer("scivis");
      worker.backend_.resetAccumulation();
      worker.publishState();
    });
    update();
  }
  if (ImGui::RadioButton("PathTracer", rendererMode == 2)) {
    rendererMode = 2;
    currentRenderer_ = QStringLiteral("pathtracer");
    invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
      worker.backend_.setRenderer("pathtracer");
      worker.backend_.resetAccumulation();
      worker.publishState();
    });
    update();
  }

  ImGui::Separator();
  ImGui::Text("Render Settings");

  bool settingsChanged = false;
  int settingsMode = settingsMode_ == OsprayBackend::SettingsMode::Automatic ? 0 : 1;
  if (ImGui::RadioButton("Automatic", settingsMode == 0)) {
    settingsMode_ = OsprayBackend::SettingsMode::Automatic;
    invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
      worker.backend_.setSettingsMode(OsprayBackend::SettingsMode::Automatic);
      worker.publishState();
    });
    settingsChanged = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Custom", settingsMode == 1)) {
    settingsMode_ = OsprayBackend::SettingsMode::Custom;
    invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
      worker.backend_.setSettingsMode(OsprayBackend::SettingsMode::Custom);
      worker.publishState();
    });
    settingsChanged = true;
  }

  if (settingsMode_ == OsprayBackend::SettingsMode::Automatic) {
    ImGui::SeparatorText("Automatic");

    int preset = 1;
    if (automaticPreset_ == OsprayBackend::AutomaticPreset::Fast)
      preset = 0;
    else if (automaticPreset_ == OsprayBackend::AutomaticPreset::Balanced)
      preset = 1;
    else
      preset = 2;
    const char *presetLabels[] = {"Fast", "Balanced", "Quality"};
    if (ImGui::Combo("Preset", &preset, presetLabels, IM_ARRAYSIZE(presetLabels))) {
      if (preset == 0)
        automaticPreset_ = OsprayBackend::AutomaticPreset::Fast;
      else if (preset == 1)
        automaticPreset_ = OsprayBackend::AutomaticPreset::Balanced;
      else
        automaticPreset_ = OsprayBackend::AutomaticPreset::Quality;
      const auto updatedPreset = automaticPreset_;
      invokeWorkerAsync(backendWorker_, [updatedPreset](RenderBackendWorker &worker) {
        worker.backend_.setAutomaticPreset(updatedPreset);
        worker.publishState();
      });
      settingsChanged = true;
    }

    float targetMs = automaticTargetFrameTimeMs_;
    if (ImGui::DragFloat("Target Frame Time (ms)", &targetMs, 0.1f, 2.0f, 1000.0f, "%.1f")) {
      automaticTargetFrameTimeMs_ = targetMs;
      invokeWorkerAsync(backendWorker_, [targetMs](RenderBackendWorker &worker) {
        worker.backend_.setAutomaticTargetFrameTimeMs(targetMs);
        worker.publishState();
      });
      settingsChanged = true;
    }

    bool accumEnabled = automaticAccumulationEnabled_;
    if (ImGui::Checkbox("Accumulation", &accumEnabled)) {
      automaticAccumulationEnabled_ = accumEnabled;
      invokeWorkerAsync(backendWorker_, [accumEnabled](RenderBackendWorker &worker) {
        worker.backend_.setAutomaticAccumulationEnabled(accumEnabled);
        worker.publishState();
      });
      settingsChanged = true;
    }

    if (ImGui::Button("Reset Render")) {
      invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
        worker.backend_.resetAccumulation();
      });
      settingsChanged = true;
    }
  } else {
    ImGui::SeparatorText("Custom");

    int startScale = customStartScale_;
    if (ImGui::SliderInt("Start Scale", &startScale, 1, 16)) {
      customStartScale_ = startScale;
      invokeWorkerAsync(backendWorker_, [startScale](RenderBackendWorker &worker) {
        worker.backend_.setCustomStartScale(startScale);
        worker.publishState();
      });
      settingsChanged = true;
    }

    float targetMs = customTargetFrameTimeMs_;
    if (ImGui::DragFloat("Target Frame Time (ms)", &targetMs, 0.1f, 2.0f, 1000.0f, "%.1f")) {
      customTargetFrameTimeMs_ = targetMs;
      invokeWorkerAsync(backendWorker_, [targetMs](RenderBackendWorker &worker) {
        worker.backend_.setCustomTargetFrameTimeMs(targetMs);
        worker.publishState();
      });
      settingsChanged = true;
    }

    int aoSamples = customAoSamples_;
    if (ImGui::SliderInt("AO Samples", &aoSamples, 0, 32)) {
      customAoSamples_ = aoSamples;
      invokeWorkerAsync(backendWorker_, [aoSamples](RenderBackendWorker &worker) {
        worker.backend_.setAoSamples(aoSamples);
        worker.publishState();
      });
      settingsChanged = true;
    }

    int pixelSamples = customPixelSamples_;
    if (ImGui::SliderInt("Pixel Samples", &pixelSamples, 1, 64)) {
      customPixelSamples_ = pixelSamples;
      invokeWorkerAsync(
          backendWorker_, [pixelSamples](RenderBackendWorker &worker) {
            worker.backend_.setPixelSamples(pixelSamples);
            worker.publishState();
          });
      settingsChanged = true;
    }

    bool accumEnabled = customAccumulationEnabled_;
    if (ImGui::Checkbox("Accumulation Enabled", &accumEnabled)) {
      customAccumulationEnabled_ = accumEnabled;
      invokeWorkerAsync(backendWorker_, [accumEnabled](RenderBackendWorker &worker) {
        worker.backend_.setCustomAccumulationEnabled(accumEnabled);
        worker.publishState();
      });
      settingsChanged = true;
    }

    int maxAccumFrames = customMaxAccumulationFrames_;
    if (ImGui::InputInt("Max Accumulation Frames", &maxAccumFrames)) {
      customMaxAccumulationFrames_ = maxAccumFrames;
      invokeWorkerAsync(
          backendWorker_, [maxAccumFrames](RenderBackendWorker &worker) {
            worker.backend_.setCustomMaxAccumulationFrames(maxAccumFrames);
            worker.publishState();
          });
      settingsChanged = true;
    }

    bool lowQualityInteract = customLowQualityWhileInteracting_;
    if (ImGui::Checkbox("Low Quality While Interacting", &lowQualityInteract)) {
      customLowQualityWhileInteracting_ = lowQualityInteract;
      invokeWorkerAsync(
          backendWorker_, [lowQualityInteract](RenderBackendWorker &worker) {
            worker.backend_.setCustomLowQualityWhileInteracting(lowQualityInteract);
            worker.publishState();
          });
      settingsChanged = true;
    }

    bool fullResAccumOnly = customFullResAccumulationOnly_;
    if (ImGui::Checkbox("Full-res Accumulation Only", &fullResAccumOnly)) {
      customFullResAccumulationOnly_ = fullResAccumOnly;
      invokeWorkerAsync(
          backendWorker_, [fullResAccumOnly](RenderBackendWorker &worker) {
            worker.backend_.setCustomFullResAccumulationOnly(fullResAccumOnly);
            worker.publishState();
          });
      settingsChanged = true;
    }

    int watchdogMs = customWatchdogTimeoutMs_;
    if (ImGui::InputInt("Watchdog Timeout (ms)", &watchdogMs)) {
      customWatchdogTimeoutMs_ = watchdogMs;
      invokeWorkerAsync(backendWorker_, [watchdogMs](RenderBackendWorker &worker) {
        worker.backend_.setCustomWatchdogTimeoutMs(watchdogMs);
        worker.publishState();
      });
      settingsChanged = true;
    }

    if (ImGui::Button("Reset Render")) {
      invokeWorkerAsync(backendWorker_, [](RenderBackendWorker &worker) {
        worker.backend_.resetAccumulation();
      });
      settingsChanged = true;
    }
  }

  ImGui::SeparatorText("Diagnostics");
  ImGui::Text("Current scale: %dx", currentScale_);
  ImGui::Text("Last render time: %.2f ms", lastFrameTimeMs_);
  ImGui::Text("Accumulation frames: %llu",
      static_cast<unsigned long long>(accumulatedFrames_));
  ImGui::Text("Dynamic mode active: %s",
      dynamicModeActive_ ? "Yes" : "No");
  ImGui::Text("Backoff applied: %s", backoffApplied_ ? "Yes" : "No");

  if (settingsChanged) {
    update();
  }

  if (ImGui::Button("Reset View")) {
    resetView();
  }

  static int mode = 0;
  if (ImGui::RadioButton("Orbit", mode == 0)) {
    mode = 0;
    setInputMode(InputMode::Orbit);
  }
  if (ImGui::RadioButton("Fly", mode == 1)) {
    mode = 1;
    setInputMode(InputMode::Fly);
  }

  ImGui::Separator();
  ImGui::Text("Controls");
  if (mode == 0) {
    ImGui::Text("Orbit: LMB rotate | RMB pan | wheel zoom");
    ImGui::Text("Shift + drag: Translate");
    ImGui::Text("Ctrl + drag: Rotate");
    ImGui::Text("Shift + Ctrl + drag: Scale");
    ImGui::Text("Alt + Left: X axis");
    ImGui::Text("Alt + Shift + Left: Y axis");
    ImGui::Text("Alt + Right: Z axis");
  }
  else {
    ImGui::Text("Fly: WASD move | LMB look | Tab toggle");
  }

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool RenderWidget::loadModel(const QString &path)
{
  bool ok = false;
  BackendSnapshot snapshot;
  invokeWorkerBlocking(backendWorker_, [&ok, &snapshot, path](RenderBackendWorker &worker) {
    ok = worker.loadObjFile(path);
    snapshot = worker.snapshot();
  });
  if (!ok)
    return false;
  applyBackendSnapshot(snapshot.lastError,
      snapshot.currentRenderer,
      vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
      snapshot.boundsMaxExtent,
      snapshot.lastFrameTimeMs,
      snapshot.renderFps,
      snapshot.accumulatedFrames,
      snapshot.watchdogCancelCount,
      snapshot.aoAutoReductionCount,
      snapshot.currentScale,
      snapshot.dynamicModeActive,
      snapshot.backoffApplied,
      snapshot.settingsMode,
      snapshot.automaticPreset,
      snapshot.automaticTargetFrameTimeMs,
      snapshot.automaticAccumulationEnabled,
      snapshot.customStartScale,
      snapshot.customTargetFrameTimeMs,
      snapshot.customAoSamples,
      snapshot.customPixelSamples,
      snapshot.customAccumulationEnabled,
      snapshot.customMaxAccumulationFrames,
      snapshot.customLowQualityWhileInteracting,
      snapshot.customFullResAccumulationOnly,
      snapshot.customWatchdogTimeoutMs);

  currentBrlcadPath_.clear();
  currentBrlcadObject_.clear();
  currentBrlcadObjects_.clear();
  resetView();
  return true;
}

bool RenderWidget::loadBrlcadModel(const QString &path, const QString &topObject)
{
  bool ok = false;
  BackendSnapshot snapshot;
  invokeWorkerBlocking(
      backendWorker_, [&ok, &snapshot, path, topObject](RenderBackendWorker &worker) {
        ok = worker.loadBrlcadFile(path, topObject);
        snapshot = worker.snapshot();
      });
  if (!ok)
    return false;
  applyBackendSnapshot(snapshot.lastError,
      snapshot.currentRenderer,
      vec3f(snapshot.boundsCenterX, snapshot.boundsCenterY, snapshot.boundsCenterZ),
      snapshot.boundsMaxExtent,
      snapshot.lastFrameTimeMs,
      snapshot.renderFps,
      snapshot.accumulatedFrames,
      snapshot.watchdogCancelCount,
      snapshot.aoAutoReductionCount,
      snapshot.currentScale,
      snapshot.dynamicModeActive,
      snapshot.backoffApplied,
      snapshot.settingsMode,
      snapshot.automaticPreset,
      snapshot.automaticTargetFrameTimeMs,
      snapshot.automaticAccumulationEnabled,
      snapshot.customStartScale,
      snapshot.customTargetFrameTimeMs,
      snapshot.customAoSamples,
      snapshot.customPixelSamples,
      snapshot.customAccumulationEnabled,
      snapshot.customMaxAccumulationFrames,
      snapshot.customLowQualityWhileInteracting,
      snapshot.customFullResAccumulationOnly,
      snapshot.customWatchdogTimeoutMs);

  currentBrlcadPath_ = path;
  currentBrlcadObject_ = topObject.trimmed().isEmpty() ? QStringLiteral("all")
                                                       : topObject.trimmed();
  currentBrlcadObjects_ = listBrlcadObjects(path);
  resetView();
  return true;
}

QStringList RenderWidget::listBrlcadObjects(const QString &path) const
{
  QStringList out;
  try {
    invokeWorkerBlocking(
        backendWorker_, [&out, path](RenderBackendWorker &worker) {
          out = worker.listBrlcadObjects(path);
        });
  } catch (...) {
  }
  return out;
}

bool RenderWidget::reloadBrlcadObject(const QString &topObject)
{
  if (currentBrlcadPath_.isEmpty())
    return false;
  return loadBrlcadModel(currentBrlcadPath_, topObject);
}

QString RenderWidget::lastError() const
{
  return lastError_;
}

void RenderWidget::resetView()
{
  center_ = boundsCenter_;

  float maxExtent = boundsMaxExtent_;
  if (maxExtent < 0.001f)
    maxExtent = 1.0f;

  yaw_ = 0.3f;
  pitch_ = 0.2f;
  fovy_ = 60.0f;
  orbitForward_ = forwardFromAngles(yaw_, pitch_);
  alignOrbitUpToReference();

  dist_ = fitDistanceFromBounds(maxExtent, fovy_);

  flyYaw_ = yaw_;
  flyPitch_ = pitch_;
  syncFlyFromOrbit();

  queueCameraUpdate(true);
  update();
}

void RenderWidget::setInputMode(InputMode mode)
{
  if (mode == inputMode_)
    return;

  if (mode == InputMode::Fly && inputMode_ == InputMode::Orbit) {
    syncFlyFromOrbit();
  } else if (mode == InputMode::Orbit && inputMode_ == InputMode::Fly) {
    syncOrbitFromFly();
  }

  inputMode_ = mode;

  queueCameraUpdate(true);
  update();
}

void RenderWidget::mousePressEvent(QMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
    imguiMouseDown_[0] = true;
  if (e->button() == Qt::RightButton)
    imguiMouseDown_[1] = true;
  if (e->button() == Qt::MiddleButton)
    imguiMouseDown_[2] = true;

  imguiMousePos_ = e->position();
  update();

  // let camera only react if ImGui doesn't want mouse
  if (ImGui::GetIO().WantCaptureMouse)
    return;

  queueInteracting(true);
  lastMouse_ = e->pos();
}

void RenderWidget::mouseReleaseEvent(QMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
    imguiMouseDown_[0] = false;
  if (e->button() == Qt::RightButton)
    imguiMouseDown_[1] = false;
  if (e->button() == Qt::MiddleButton)
    imguiMouseDown_[2] = false;

  imguiMousePos_ = e->position();
  if (!(imguiMouseDown_[0] || imguiMouseDown_[1] || imguiMouseDown_[2]))
    queueInteracting(false);
  update();
}

void RenderWidget::mouseMoveEvent(QMouseEvent *e)
{
  imguiMousePos_ = e->position();
  update();

  if (ImGui::GetIO().WantCaptureMouse)
    return;

  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(float(e->position().x()), float(e->position().y()));

  update();

  if (io.WantCaptureMouse)
    return;

  queueInteracting(e->buttons() != Qt::NoButton);

  const QPoint d = e->pos() - lastMouse_;
  lastMouse_ = e->pos();

  auto result = InteractionController::classify(e->buttons(), e->modifiers());

  if (inputMode_ == InputMode::Orbit) {
    if (result.action != InteractionController::Action::None) {
      applyViewAction(result, d);
      return;
    }

    // Fallback to original behavior when no modifiers are pressed
    if (e->buttons() & Qt::LeftButton) {
      rotateOrbit(d.x() * orbitSpeed_, d.y() * orbitSpeed_);

      queueCameraUpdate(true);
      return;
    }

    if (e->buttons() & Qt::RightButton) {
      vec3f right = orbitRight();
      vec3f upCam = orbitUp_;

      float sx = float(d.x()) * panSpeed_ * dist_;
      float sy = float(d.y()) * panSpeed_ * dist_;

      center_ = vec3f(center_.x - right.x * sx + upCam.x * sy,
          center_.y - right.y * sx + upCam.y * sy,
          center_.z - right.z * sx + upCam.z * sy);

      queueCameraUpdate(true);
      return;
    }

  } else {
    if (e->buttons() & Qt::LeftButton) {
      flyYaw_ += d.x() * orbitSpeed_;
      flyPitch_ += d.y() * orbitSpeed_;

      queueCameraUpdate(true);
      return;
    }
  }
}

void RenderWidget::wheelEvent(QWheelEvent *e)
{
  imguiMouseWheel_ += e->angleDelta().y() / 120.0f;
  update();

  if (ImGui::GetIO().WantCaptureMouse)
    return;

  queueInteracting(true);

  float steps = e->angleDelta().y() / 120.f;
  if (steps == 0.f)
    return;

  if (inputMode_ == InputMode::Orbit) {
    float maxExtent = boundsMaxExtent_;
    if (maxExtent < 0.001f)
      maxExtent = 1.0f;

    float minDist = std::max(maxExtent * 1e-8f, 1e-8f);
    float maxDist = std::max(maxExtent * 100.0f, 10.0f);

    dist_ *= std::pow(zoomFactor_, steps);
    dist_ = clampf(dist_, minDist, maxDist);
  } else {
    fovy_ *= std::pow(0.95f, steps);
    fovy_ = clampf(fovy_, 20.f, 90.f);
  }

  queueCameraUpdate(true);
  queueInteracting(false);
}

void RenderWidget::keyPressEvent(QKeyEvent *e)
{
  ImGuiIO &io = ImGui::GetIO();

  auto sendKey = [&](int qtKey, ImGuiKey imguiKey) {
    if (e->key() == qtKey)
      io.AddKeyEvent(imguiKey, true);
  };

  sendKey(Qt::Key_Tab, ImGuiKey_Tab);
  sendKey(Qt::Key_W, ImGuiKey_W);
  sendKey(Qt::Key_A, ImGuiKey_A);
  sendKey(Qt::Key_S, ImGuiKey_S);
  sendKey(Qt::Key_D, ImGuiKey_D);
  sendKey(Qt::Key_Q, ImGuiKey_Q);
  sendKey(Qt::Key_E, ImGuiKey_E);

  if (io.WantCaptureKeyboard)
    return;

  if (e->key() == Qt::Key_W || e->key() == Qt::Key_A || e->key() == Qt::Key_S
      || e->key() == Qt::Key_D || e->key() == Qt::Key_Q || e->key() == Qt::Key_E
      || e->key() == Qt::Key_Tab) {
    queueInteracting(true);
  }

  if (e->key() == Qt::Key_Tab) {
    inputMode_ =
        (inputMode_ == InputMode::Orbit) ? InputMode::Fly : InputMode::Orbit;
    queueCameraUpdate(true);
    return;
  }

  if (inputMode_ != InputMode::Fly)
    return;

  vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
  vec3f right = normalizeVec(crossVec(forward, worldUp()));

  float modelScale = boundsMaxExtent_;
  if (modelScale < 0.001f)
    modelScale = 1.0f;

  float step = modelScale * flyMoveFactor_;

  if (e->key() == Qt::Key_W)
    flyPos_ = vec3f(flyPos_.x + forward.x * step,
        flyPos_.y + forward.y * step,
        flyPos_.z + forward.z * step);
  if (e->key() == Qt::Key_S)
    flyPos_ = vec3f(flyPos_.x - forward.x * step,
        flyPos_.y - forward.y * step,
        flyPos_.z - forward.z * step);
  if (e->key() == Qt::Key_A)
    flyPos_ = vec3f(flyPos_.x - right.x * step,
        flyPos_.y - right.y * step,
        flyPos_.z - right.z * step);
  if (e->key() == Qt::Key_D)
    flyPos_ = vec3f(flyPos_.x + right.x * step,
        flyPos_.y + right.y * step,
        flyPos_.z + right.z * step);
  if (e->key() == Qt::Key_Q)
    flyPos_ = vec3f(flyPos_.x - worldUp().x * step,
        flyPos_.y - worldUp().y * step,
        flyPos_.z - worldUp().z * step);
  if (e->key() == Qt::Key_E)
    flyPos_ = vec3f(flyPos_.x + worldUp().x * step,
        flyPos_.y + worldUp().y * step,
        flyPos_.z + worldUp().z * step);

  queueCameraUpdate(true);
}

void RenderWidget::keyReleaseEvent(QKeyEvent *e)
{
  ImGuiIO &io = ImGui::GetIO();

  auto sendKey = [&](int qtKey, ImGuiKey imguiKey) {
    if (e->key() == qtKey)
      io.AddKeyEvent(imguiKey, false);
  };

  sendKey(Qt::Key_Tab, ImGuiKey_Tab);
  sendKey(Qt::Key_W, ImGuiKey_W);
  sendKey(Qt::Key_A, ImGuiKey_A);
  sendKey(Qt::Key_S, ImGuiKey_S);
  sendKey(Qt::Key_D, ImGuiKey_D);
  sendKey(Qt::Key_Q, ImGuiKey_Q);
  sendKey(Qt::Key_E, ImGuiKey_E);

  if (io.WantCaptureKeyboard)
    return;

  if (e->key() == Qt::Key_W || e->key() == Qt::Key_A || e->key() == Qt::Key_S
      || e->key() == Qt::Key_D || e->key() == Qt::Key_Q || e->key() == Qt::Key_E
      || e->key() == Qt::Key_Tab) {
    queueInteracting(false);
  }
}

void RenderWidget::focusInEvent(QFocusEvent *e)
{
  Q_UNUSED(e);
  imguiHasFocus_ = true;
  update();
}

void RenderWidget::focusOutEvent(QFocusEvent *e)
{
  Q_UNUSED(e);
  imguiHasFocus_ = false;
  queueInteracting(false);
  update();
}

void RenderWidget::setUpAxis(UpAxis axis)
{
  if (axis == upAxis_)
    return;

  upAxis_ = axis;
  if (inputMode_ == InputMode::Orbit) {
    alignOrbitUpToReference();
    syncFlyFromOrbit();
  } else {
    syncOrbitFromFly();
  }

  queueCameraUpdate(true);
}

RenderWidget::UpAxis RenderWidget::upAxis() const
{
  return upAxis_;
}

QString RenderWidget::currentBrlcadPath() const
{
  return currentBrlcadPath_;
}

QString RenderWidget::currentBrlcadObject() const
{
  return currentBrlcadObject_;
}

QStringList RenderWidget::currentBrlcadObjects() const
{
  return currentBrlcadObjects_;
}

bool RenderWidget::hasBrlcadScene() const
{
  return !currentBrlcadPath_.isEmpty();
}

vec3f RenderWidget::worldUp() const
{
  return upAxis_ == UpAxis::Z ? vec3f(0.f, 0.f, 1.f) : vec3f(0.f, 1.f, 0.f);
}

vec3f RenderWidget::worldForwardReference() const
{
  return upAxis_ == UpAxis::Z ? vec3f(0.f, 1.f, 0.f) : vec3f(0.f, 0.f, 1.f);
}

vec3f RenderWidget::forwardFromAngles(float yaw, float pitch) const
{
  const vec3f up = worldUp();
  const vec3f forwardRef = worldForwardReference();
  const vec3f rightRef = normalizeVec(crossVec(forwardRef, up));

  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);

  const vec3f dir(rightRef.x * sy * cp + up.x * sp + forwardRef.x * cy * cp,
      rightRef.y * sy * cp + up.y * sp + forwardRef.y * cy * cp,
      rightRef.z * sy * cp + up.z * sp + forwardRef.z * cy * cp);
  return normalizeVec(dir);
}

#include "renderwidget.moc"

void RenderWidget::anglesFromForward(const vec3f &forward, float &yaw, float &pitch) const
{
  const vec3f dir = normalizeVec(forward);
  const vec3f up = worldUp();
  const vec3f forwardRef = worldForwardReference();
  const vec3f rightRef = normalizeVec(crossVec(forwardRef, up));

  const float upDot = std::clamp(
      dir.x * up.x + dir.y * up.y + dir.z * up.z, -1.f, 1.f);
  pitch = std::asin(upDot);

  const float fwdComp = dir.x * forwardRef.x + dir.y * forwardRef.y + dir.z * forwardRef.z;
  const float rightComp = dir.x * rightRef.x + dir.y * rightRef.y + dir.z * rightRef.z;
  yaw = std::atan2(rightComp, fwdComp);
}

vec3f RenderWidget::projectOntoPlane(const vec3f &v, const vec3f &normal) const
{
  const float dot = v.x * normal.x + v.y * normal.y + v.z * normal.z;
  return vec3f(v.x - normal.x * dot, v.y - normal.y * dot, v.z - normal.z * dot);
}

vec3f RenderWidget::orbitRight() const
{
  vec3f right = crossVec(orbitForward_, orbitUp_);
  right = normalizeVec(right);
  if (std::fabs(right.x) < 1e-6f && std::fabs(right.y) < 1e-6f
      && std::fabs(right.z) < 1e-6f) {
    right = normalizeVec(crossVec(orbitForward_, worldForwardReference()));
  }
  return right;
}

void RenderWidget::alignOrbitUpToReference()
{
  orbitForward_ = normalizeVec(orbitForward_);
  vec3f projectedUp = projectOntoPlane(worldUp(), orbitForward_);
  projectedUp = normalizeVec(projectedUp);
  if (std::fabs(projectedUp.x) < 1e-6f && std::fabs(projectedUp.y) < 1e-6f
      && std::fabs(projectedUp.z) < 1e-6f) {
    projectedUp = normalizeVec(projectOntoPlane(worldForwardReference(), orbitForward_));
  }
  orbitUp_ = normalizeVec(projectedUp);
}

vec3f RenderWidget::rotateAroundAxis(const vec3f &v, const vec3f &axis, float angle) const
{
  const vec3f n = normalizeVec(axis);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const vec3f cross = crossVec(n, v);
  const float dot = n.x * v.x + n.y * v.y + n.z * v.z;

  return vec3f(v.x * c + cross.x * s + n.x * dot * (1.f - c),
      v.y * c + cross.y * s + n.y * dot * (1.f - c),
      v.z * c + cross.z * s + n.z * dot * (1.f - c));
}

void RenderWidget::rotateOrbit(float yawDelta, float pitchDelta)
{
  orbitForward_ = normalizeVec(rotateAroundAxis(orbitForward_, orbitUp_, yawDelta));

  const vec3f right = orbitRight();
  orbitForward_ = normalizeVec(rotateAroundAxis(orbitForward_, right, pitchDelta));
  orbitUp_ = normalizeVec(rotateAroundAxis(orbitUp_, right, pitchDelta));

  const vec3f correctedRight = orbitRight();
  orbitUp_ = normalizeVec(crossVec(correctedRight, orbitForward_));
  anglesFromForward(orbitForward_, yaw_, pitch_);
}
