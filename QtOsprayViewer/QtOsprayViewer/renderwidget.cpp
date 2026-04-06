#include "renderwidget.h"

#include "renderworkerclient.h"

#include <QMetaObject>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_impl_opengl3.h"

#include <QTimer>

using rkcommon::math::vec3f;

RenderWidget::RenderWidget(QWidget *parent) : QOpenGLWidget(parent)
{
  setFocusPolicy(Qt::StrongFocus);
  
  renderTimer_ = new QTimer(this);
  connect(renderTimer_, &QTimer::timeout, this, &RenderWidget::advanceRender);
  renderTimer_->start(16); // progressive render at ~60 Hz
  
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
}

RenderWidget::~RenderWidget()
{
  if (sceneLoadThread_.joinable())
    sceneLoadThread_.join();

  makeCurrent();
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

    float maxExtent = sceneBoundsMaxExtent();
    if (maxExtent < 0.001f)
      maxExtent = 1.0f;

    float minDist = std::max(maxExtent * 1e-8f, 1e-8f);
    float maxDist = std::max(maxExtent * 100.0f, 10.0f);

    dist_ *= std::pow(1.05f, amount * 10.0f);
    dist_ = clampf(dist_, minDist, maxDist);
  }

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
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

  backend_.init();
  backendReady_ = true;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(width()), float(height()));

  ImGui_ImplOpenGL3_Init("#version 130");
}

void RenderWidget::resizeGL(int w, int h)
{
  if (sceneLoadInProgress_.load()) {
    ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
    return;
  }

  if (usingWorkerRenderPath())
    renderWorkerClient_->resize(w, h);

  backend_.resize(w, h);
  ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));

  resetView();
}

void RenderWidget::syncCameraToBackend()
{
  if (inputMode_ == InputMode::Orbit) {
    vec3f forward = orbitForward_;
    vec3f eye(center_.x - dist_ * forward.x,
        center_.y - dist_ * forward.y,
        center_.z - dist_ * forward.z);

    backend_.setCamera(eye, center_, orbitUp_, fovy_);
    if (usingWorkerRenderPath())
      renderWorkerClient_->setCamera(eye, center_, orbitUp_, fovy_);
  } else {
    flyPitch_ = clampf(flyPitch_, -1.4f, 1.4f);

    vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);

    backend_.setCamera(flyPos_, flyPos_ + forward, worldUp(), fovy_);
    if (usingWorkerRenderPath())
      renderWorkerClient_->setCamera(flyPos_, flyPos_ + forward, worldUp(), fovy_);
  }
}

bool RenderWidget::usingWorkerRenderPath() const
{
  return renderWorkerClient_ && renderWorkerClient_->isConnected();
}

void RenderWidget::resetAccumulationTargets()
{
  backend_.resetAccumulation();
  if (usingWorkerRenderPath())
    renderWorkerClient_->resetAccumulation();
}

vec3f RenderWidget::sceneBoundsCenter() const
{
  if (usingWorkerRenderPath()) {
    return vec3f(0.5f * (sceneBoundsMin_.x + sceneBoundsMax_.x),
        0.5f * (sceneBoundsMin_.y + sceneBoundsMax_.y),
        0.5f * (sceneBoundsMin_.z + sceneBoundsMax_.z));
  }
  return backend_.getBoundsCenter();
}

float RenderWidget::sceneBoundsMaxExtent() const
{
  if (usingWorkerRenderPath()) {
    const float dx = sceneBoundsMax_.x - sceneBoundsMin_.x;
    const float dy = sceneBoundsMax_.y - sceneBoundsMin_.y;
    const float dz = sceneBoundsMax_.z - sceneBoundsMin_.z;
    return std::max(dx, std::max(dy, dz));
  }
  return backend_.getBoundsMaxExtent();
}

void RenderWidget::renderOnce()
{
  if (!backendReady_ || sceneLoadInProgress_.load())
    return;

  if (usingWorkerRenderPath()) {
    const auto frame = renderWorkerClient_->requestFrame();
    if (frame.image.isNull())
      return;

    image_ = frame.image;
    workerLastFrameTimeMs_ = frame.frameTimeMs;
    workerRenderFPS_ = frame.renderFPS;
    workerAccumulatedFrames_ = frame.accumulatedFrames;
    workerWatchdogCancels_ = frame.watchdogCancels;
    workerAoAutoReductions_ = frame.aoAutoReductions;
    if (!frame.renderer.isEmpty())
      currentRenderer_ = frame.renderer;
    update();

    const float frameMs = frame.frameTimeMs;
    if (frameMs < 10.0f)
      renderBudgetMs_ = std::min(10, renderBudgetMs_ + 1);
    else if (frameMs > 22.0f)
      renderBudgetMs_ = std::max(3, renderBudgetMs_ - 1);
    return;
  }

  const bool updatedImage = backend_.advanceRender(renderBudgetMs_);
  const uint32_t *px = backend_.pixels();
  const int w = backend_.width();
  const int h = backend_.height();

  if (!px || w <= 0 || h <= 0)
    return;

  if (image_.width() != w || image_.height() != h)
    image_ = QImage(w, h, QImage::Format_RGBA8888);

  if (updatedImage) {
    std::memcpy(image_.bits(), px, size_t(w) * size_t(h) * 4);
    update();

    const float frameMs = backend_.lastFrameTimeMs();
    if (frameMs < 10.0f)
      renderBudgetMs_ = std::min(10, renderBudgetMs_ + 1);
    else if (frameMs > 22.0f)
      renderBudgetMs_ = std::max(3, renderBudgetMs_ - 1);
  }
}

void RenderWidget::advanceRender()
{
  if (!backendReady_ || !isVisible() || sceneLoadInProgress_.load())
    return;

  renderOnce();
}

void RenderWidget::paintGL()
{
  glViewport(0, 0, width() * devicePixelRatioF(), height() * devicePixelRatioF());
  glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // Draw OSPRay image with Qt
  QPainter p(this);
  if (!image_.isNull()) {
    QImage img = image_.rgbSwapped().mirrored(false, true);
    p.drawImage(rect(), img);
  }
  p.end();

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

  if (!imguiVisible_) {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return;
  }

  ImGui::Begin("Interactive BRL-CAD Raytracer");

  if (sceneLoadInProgress_.load()) {
    ImGui::Separator();
    ImGui::Text("Status");
    ImGui::TextWrapped("%s", loadStatusText_.toStdString().c_str());
    ImGui::Text("Renderer work is paused until the scene load completes.");
    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return;
  }

  ImGui::Separator();
  ImGui::Text("Stats");

  ImGui::Text("UI FPS: %.1f", ImGui::GetIO().Framerate);
  if (usingWorkerRenderPath()) {
    ImGui::Text("Render path: Worker");
    ImGui::Text("Render time: %.2f ms", workerLastFrameTimeMs_);
    ImGui::Text("Render FPS: %.1f", workerRenderFPS_);
    ImGui::Text("Accumulated frames: %llu",
        static_cast<unsigned long long>(workerAccumulatedFrames_));
    ImGui::Text("Watchdog cancels: %llu",
        static_cast<unsigned long long>(workerWatchdogCancels_));
    ImGui::Text("AO auto-reductions: %llu",
        static_cast<unsigned long long>(workerAoAutoReductions_));
  } else {
    ImGui::Text("Render time: %.2f ms", backend_.lastFrameTimeMs());
    ImGui::Text("Render FPS: %.1f", backend_.renderFPS());
    ImGui::Text("Accumulated frames: %llu",
        static_cast<unsigned long long>(backend_.accumulatedFrames()));
    ImGui::Text("Watchdog cancels: %llu",
        static_cast<unsigned long long>(backend_.watchdogCancelCount()));
    ImGui::Text("AO auto-reductions: %llu",
        static_cast<unsigned long long>(backend_.aoAutoReductionCount()));
  }
  ImGui::Text("Up Axis: %s", upAxis_ == UpAxis::Z ? "Z" : "Y");
  ImGui::Text("Resolution: %d x %d", width(), height());

  ImGui::Separator();
  ImGui::Text("Renderer");

  int rendererMode = 0;
  const std::string rendererName =
      usingWorkerRenderPath() ? currentRenderer_.toStdString() : backend_.currentRenderer();
  if (rendererName == "scivis")
    rendererMode = 1;
  else if (rendererName == "pathtracer")
    rendererMode = 2;
  else
    rendererMode = 0;

  if (ImGui::RadioButton("ao", rendererMode == 0)) {
    rendererMode = 0;
    currentRenderer_ = QStringLiteral("ao");
    if (usingWorkerRenderPath())
      renderWorkerClient_->setRenderer(currentRenderer_);
    else
      backend_.setRenderer("ao");
    resetAccumulationTargets();
    renderOnce();
    update();
  }
  if (ImGui::RadioButton("SciVis", rendererMode == 1)) {
    rendererMode = 1;
    currentRenderer_ = QStringLiteral("scivis");
    if (usingWorkerRenderPath())
      renderWorkerClient_->setRenderer(currentRenderer_);
    else
      backend_.setRenderer("scivis");
    resetAccumulationTargets();
    renderOnce();
    update();
  }
  if (ImGui::RadioButton("PathTracer", rendererMode == 2)) {
    rendererMode = 2;
    currentRenderer_ = QStringLiteral("pathtracer");
    if (usingWorkerRenderPath())
      renderWorkerClient_->setRenderer(currentRenderer_);
    else
      backend_.setRenderer("pathtracer");
    resetAccumulationTargets();
    renderOnce();
    update();
  }

  ImGui::Separator();
  ImGui::Text("Render Settings");
  bool settingsChanged = false;
  int settingsMode = usingWorkerRenderPath()
      ? workerSettings_.settingsMode
      : (backend_.settingsMode() == OsprayBackend::SettingsMode::Automatic ? 0 : 1);
  if (ImGui::RadioButton("Automatic", settingsMode == 0)) {
    if (usingWorkerRenderPath())
      workerSettings_.settingsMode = 0;
    else
      backend_.setSettingsMode(OsprayBackend::SettingsMode::Automatic);
    settingsChanged = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Custom", settingsMode == 1)) {
    if (usingWorkerRenderPath())
      workerSettings_.settingsMode = 1;
    else
      backend_.setSettingsMode(OsprayBackend::SettingsMode::Custom);
    settingsChanged = true;
  }

  if (settingsMode == 0) {
    ImGui::SeparatorText("Automatic");

    int preset = usingWorkerRenderPath() ? workerSettings_.automaticPreset : 1;
    if (!usingWorkerRenderPath()) {
      if (backend_.automaticPreset() == OsprayBackend::AutomaticPreset::Fast)
        preset = 0;
      else if (backend_.automaticPreset() == OsprayBackend::AutomaticPreset::Balanced)
        preset = 1;
      else
        preset = 2;
    }
    const char *presetLabels[] = {"Fast", "Balanced", "Quality"};
    if (ImGui::Combo("Preset", &preset, presetLabels, IM_ARRAYSIZE(presetLabels))) {
      if (usingWorkerRenderPath())
        workerSettings_.automaticPreset = preset;
      else
        if (preset == 0)
          backend_.setAutomaticPreset(OsprayBackend::AutomaticPreset::Fast);
        else if (preset == 1)
          backend_.setAutomaticPreset(OsprayBackend::AutomaticPreset::Balanced);
        else
          backend_.setAutomaticPreset(OsprayBackend::AutomaticPreset::Quality);
      settingsChanged = true;
    }

    float targetMs = usingWorkerRenderPath() ? workerSettings_.automaticTargetFrameTimeMs
                                             : backend_.automaticTargetFrameTimeMs();
    if (ImGui::DragFloat("Target Frame Time (ms)", &targetMs, 0.1f, 2.0f, 1000.0f, "%.1f")) {
      if (usingWorkerRenderPath())
        workerSettings_.automaticTargetFrameTimeMs = targetMs;
      else
        backend_.setAutomaticTargetFrameTimeMs(targetMs);
      settingsChanged = true;
    }

    bool accumEnabled = usingWorkerRenderPath() ? workerSettings_.automaticAccumulationEnabled
                                                : backend_.automaticAccumulationEnabled();
    if (ImGui::Checkbox("Accumulation", &accumEnabled)) {
      if (usingWorkerRenderPath())
        workerSettings_.automaticAccumulationEnabled = accumEnabled;
      else
        backend_.setAutomaticAccumulationEnabled(accumEnabled);
      settingsChanged = true;
    }

    if (ImGui::Button("Reset Render")) {
      resetAccumulationTargets();
      settingsChanged = true;
    }
  } else {
    ImGui::SeparatorText("Custom");

    int startScale = usingWorkerRenderPath() ? workerSettings_.customStartScale
                                             : backend_.customStartScale();
    if (ImGui::SliderInt("Start Scale", &startScale, 1, 16)) {
      if (usingWorkerRenderPath())
        workerSettings_.customStartScale = startScale;
      else
        backend_.setCustomStartScale(startScale);
      settingsChanged = true;
    }

    float targetMs = usingWorkerRenderPath() ? workerSettings_.customTargetFrameTimeMs
                                             : backend_.customTargetFrameTimeMs();
    if (ImGui::DragFloat("Target Frame Time (ms)", &targetMs, 0.1f, 2.0f, 1000.0f, "%.1f")) {
      if (usingWorkerRenderPath())
        workerSettings_.customTargetFrameTimeMs = targetMs;
      else
        backend_.setCustomTargetFrameTimeMs(targetMs);
      settingsChanged = true;
    }

    int aoSamples = usingWorkerRenderPath() ? workerSettings_.customAoSamples
                                            : backend_.customAoSamples();
    if (ImGui::SliderInt("AO Samples", &aoSamples, 0, 32)) {
      if (usingWorkerRenderPath())
        workerSettings_.customAoSamples = aoSamples;
      else
        backend_.setAoSamples(aoSamples);
      settingsChanged = true;
    }

    int pixelSamples = usingWorkerRenderPath() ? workerSettings_.customPixelSamples
                                               : backend_.customPixelSamples();
    if (ImGui::SliderInt("Pixel Samples", &pixelSamples, 1, 64)) {
      if (usingWorkerRenderPath())
        workerSettings_.customPixelSamples = pixelSamples;
      else
        backend_.setPixelSamples(pixelSamples);
      settingsChanged = true;
    }

    bool accumEnabled = usingWorkerRenderPath() ? workerSettings_.customAccumulationEnabled
                                                : backend_.customAccumulationEnabled();
    if (ImGui::Checkbox("Accumulation Enabled", &accumEnabled)) {
      if (usingWorkerRenderPath())
        workerSettings_.customAccumulationEnabled = accumEnabled;
      else
        backend_.setCustomAccumulationEnabled(accumEnabled);
      settingsChanged = true;
    }

    int maxAccumFrames = usingWorkerRenderPath() ? workerSettings_.customMaxAccumulationFrames
                                                 : backend_.customMaxAccumulationFrames();
    if (ImGui::InputInt("Max Accumulation Frames", &maxAccumFrames)) {
      if (usingWorkerRenderPath())
        workerSettings_.customMaxAccumulationFrames = maxAccumFrames;
      else
        backend_.setCustomMaxAccumulationFrames(maxAccumFrames);
      settingsChanged = true;
    }

    bool lowQualityInteract = usingWorkerRenderPath()
        ? workerSettings_.customLowQualityWhileInteracting
        : backend_.customLowQualityWhileInteracting();
    if (ImGui::Checkbox("Low Quality While Interacting", &lowQualityInteract)) {
      if (usingWorkerRenderPath())
        workerSettings_.customLowQualityWhileInteracting = lowQualityInteract;
      else
        backend_.setCustomLowQualityWhileInteracting(lowQualityInteract);
      settingsChanged = true;
    }

    bool fullResAccumOnly = usingWorkerRenderPath()
        ? workerSettings_.customFullResAccumulationOnly
        : backend_.customFullResAccumulationOnly();
    if (ImGui::Checkbox("Full-res Accumulation Only", &fullResAccumOnly)) {
      if (usingWorkerRenderPath())
        workerSettings_.customFullResAccumulationOnly = fullResAccumOnly;
      else
        backend_.setCustomFullResAccumulationOnly(fullResAccumOnly);
      settingsChanged = true;
    }

    int watchdogMs = usingWorkerRenderPath() ? workerSettings_.customWatchdogTimeoutMs
                                             : backend_.customWatchdogTimeoutMs();
    if (ImGui::InputInt("Watchdog Timeout (ms)", &watchdogMs)) {
      if (usingWorkerRenderPath())
        workerSettings_.customWatchdogTimeoutMs = watchdogMs;
      else
        backend_.setCustomWatchdogTimeoutMs(watchdogMs);
      settingsChanged = true;
    }

    if (ImGui::Button("Reset Render")) {
      resetAccumulationTargets();
      settingsChanged = true;
    }
  }

  ImGui::SeparatorText("Diagnostics");
  if (!usingWorkerRenderPath()) {
    ImGui::Text("Current scale: %dx", backend_.currentScale());
    ImGui::Text("Last render time: %.2f ms", backend_.lastFrameTimeMs());
    ImGui::Text("Accumulation frames: %llu",
        static_cast<unsigned long long>(backend_.accumulatedFrames()));
    ImGui::Text("Dynamic mode active: %s",
        backend_.dynamicModeActive() ? "Yes" : "No");
    ImGui::Text("Backoff applied: %s", backend_.backoffApplied() ? "Yes" : "No");
  } else {
    ImGui::Text("Current renderer: %s", currentRenderer_.toStdString().c_str());
  }

  if (settingsChanged) {
    if (usingWorkerRenderPath())
      renderWorkerClient_->setRenderSettings(workerSettings_);
    renderOnce();
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
    ImGui::Text("G: Toggle overlay");
  }
  else {
    ImGui::Text("Fly: WASD move | LMB look | Tab toggle");
    ImGui::Text("G: Toggle overlay");
  }

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool RenderWidget::loadModel(const QString &path)
{
  if (sceneLoadInProgress_.load()) {
    lastError_ = QStringLiteral("A scene load is already in progress.");
    return false;
  }

  startAsyncLoad(
      [this, path]() {
        if (usingWorkerRenderPath()) {
          const auto result = renderWorkerClient_->loadObj(path);
          QMetaObject::invokeMethod(this, [this, result, path]() {
            sceneLoadInProgress_.store(false);
            lastError_ = result.errorMessage;
            if (result.success) {
              sceneBoundsMin_ = result.boundsMin;
              sceneBoundsMax_ = result.boundsMax;
              currentSceneIsObj_ = true;
              currentModelPath_ = path;
              currentBrlcadPath_.clear();
              currentBrlcadObject_.clear();
              currentBrlcadObjects_.clear();
              resetView();
            } else {
              update();
            }
            emit sceneLoadFinished(result.success, result.errorMessage);
          }, Qt::QueuedConnection);
          return;
        }

        const bool ok = backend_.loadObj(path.toStdString());
        const QString error =
            ok ? QString() : QString::fromStdString(backend_.lastError());

        QMetaObject::invokeMethod(this, [this, ok, error, path]() {
          sceneLoadInProgress_.store(false);
          lastError_ = error;
          if (ok) {
            currentSceneIsObj_ = true;
            currentModelPath_ = path;
            backend_.resize(width(), height());
            currentBrlcadPath_.clear();
            currentBrlcadObject_.clear();
            currentBrlcadObjects_.clear();
            resetView();
          } else {
            update();
          }
          emit sceneLoadFinished(ok, error);
        }, Qt::QueuedConnection);
      },
      QStringLiteral("Loading OBJ scene..."));
  return true;
}

bool RenderWidget::loadBrlcadModel(const QString &path, const QString &topObject)
{
  if (sceneLoadInProgress_.load()) {
    lastError_ = QStringLiteral("A scene load is already in progress.");
    return false;
  }

  const QString resolvedObject =
      topObject.trimmed().isEmpty() ? QStringLiteral("all") : topObject.trimmed();
  const QStringList availableObjects = listBrlcadObjects(path);

  startAsyncLoad(
      [this, path, resolvedObject, availableObjects]() {
        if (usingWorkerRenderPath()) {
          const auto result = renderWorkerClient_->loadBrlcad(path, resolvedObject);
          QMetaObject::invokeMethod(this,
              [this, result, path, resolvedObject, availableObjects]() {
            sceneLoadInProgress_.store(false);
            lastError_ = result.errorMessage;
            if (result.success) {
              sceneBoundsMin_ = result.boundsMin;
              sceneBoundsMax_ = result.boundsMax;
              currentSceneIsObj_ = false;
              currentModelPath_.clear();
              currentBrlcadPath_ = path;
              currentBrlcadObject_ = resolvedObject;
              currentBrlcadObjects_ = availableObjects;
              resetView();
            } else {
              update();
            }
            emit sceneLoadFinished(result.success, result.errorMessage);
          },
              Qt::QueuedConnection);
          return;
        }

        const bool ok =
            backend_.loadBrlcad(path.toStdString(), resolvedObject.toStdString());
        const QString error =
            ok ? QString() : QString::fromStdString(backend_.lastError());

        QMetaObject::invokeMethod(this,
            [this, ok, error, path, resolvedObject, availableObjects]() {
              sceneLoadInProgress_.store(false);
              lastError_ = error;
              if (ok) {
                currentSceneIsObj_ = false;
                currentModelPath_.clear();
                backend_.resize(width(), height());
                currentBrlcadPath_ = path;
                currentBrlcadObject_ = resolvedObject;
                currentBrlcadObjects_ = availableObjects;
                resetView();
              } else {
                update();
              }
              emit sceneLoadFinished(ok, error);
            },
            Qt::QueuedConnection);
      },
      QStringLiteral("Loading BRL-CAD scene..."));
  return true;
}

QStringList RenderWidget::listBrlcadObjects(const QString &path) const
{
  if (renderWorkerClient_ && renderWorkerClient_->isConnected())
    return renderWorkerClient_->listBrlcadObjects(path);

  QStringList out;
  try {
    const auto names = backend_.listBrlcadObjects(path.toStdString());
    for (const auto &name : names)
      out << QString::fromStdString(name);
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
  if (sceneLoadInProgress_.load())
    return;

  center_ = sceneBoundsCenter();

  float maxExtent = sceneBoundsMaxExtent();
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

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
  update();
}

void RenderWidget::setInputMode(InputMode mode)
{
  if (sceneLoadInProgress_.load()) {
    inputMode_ = mode;
    update();
    return;
  }

  if (mode == inputMode_)
    return;

  if (mode == InputMode::Fly && inputMode_ == InputMode::Orbit) {
    syncFlyFromOrbit();
  } else if (mode == InputMode::Orbit && inputMode_ == InputMode::Fly) {
    syncOrbitFromFly();
  }

  inputMode_ = mode;

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
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

  if (sceneLoadInProgress_.load())
    return;

  // let camera only react if ImGui doesn't want mouse
  if (ImGui::GetIO().WantCaptureMouse)
    return;

  backend_.setInteracting(true);
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
  if (sceneLoadInProgress_.load()) {
    update();
    return;
  }

  if (!(imguiMouseDown_[0] || imguiMouseDown_[1] || imguiMouseDown_[2]))
    backend_.setInteracting(false);
  update();
}

void RenderWidget::mouseMoveEvent(QMouseEvent *e)
{
  imguiMousePos_ = e->position();
  update();

  if (sceneLoadInProgress_.load())
    return;

  if (ImGui::GetIO().WantCaptureMouse)
    return;

  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(float(e->position().x()), float(e->position().y()));

  update();

  if (io.WantCaptureMouse)
    return;

  backend_.setInteracting(e->buttons() != Qt::NoButton);

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

      resetAccumulationTargets();
      syncCameraToBackend();
      renderOnce();
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

      resetAccumulationTargets();
      syncCameraToBackend();
      renderOnce();
      return;
    }

  } else {
    if (e->buttons() & Qt::LeftButton) {
      flyYaw_ += d.x() * orbitSpeed_;
      flyPitch_ += d.y() * orbitSpeed_;

      resetAccumulationTargets();
      syncCameraToBackend();
      renderOnce();
      return;
    }
  }
}

void RenderWidget::wheelEvent(QWheelEvent *e)
{
  imguiMouseWheel_ += e->angleDelta().y() / 120.0f;
  update();

  if (sceneLoadInProgress_.load())
    return;

  if (ImGui::GetIO().WantCaptureMouse)
    return;

  backend_.setInteracting(true);

  float steps = e->angleDelta().y() / 120.f;
  if (steps == 0.f)
    return;

  if (inputMode_ == InputMode::Orbit) {
    float maxExtent = sceneBoundsMaxExtent();
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

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
  backend_.setInteracting(false);
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

  if (sceneLoadInProgress_.load())
    return;

  if (e->key() == Qt::Key_G && !e->isAutoRepeat()) {
    imguiVisible_ = !imguiVisible_;
    update();
    return;
  }

  if (io.WantCaptureKeyboard)
    return;

  if (e->key() == Qt::Key_W || e->key() == Qt::Key_A || e->key() == Qt::Key_S
      || e->key() == Qt::Key_D || e->key() == Qt::Key_Q || e->key() == Qt::Key_E
      || e->key() == Qt::Key_Tab) {
    backend_.setInteracting(true);
  }

  if (e->key() == Qt::Key_Tab) {
    inputMode_ =
        (inputMode_ == InputMode::Orbit) ? InputMode::Fly : InputMode::Orbit;
    resetAccumulationTargets();
    syncCameraToBackend();
    renderOnce();
    return;
  }

  if (inputMode_ != InputMode::Fly)
    return;

  vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
  vec3f right = normalizeVec(crossVec(forward, worldUp()));

  float modelScale = sceneBoundsMaxExtent();
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

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
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

  if (sceneLoadInProgress_.load())
    return;

  if (io.WantCaptureKeyboard)
    return;

  if (e->key() == Qt::Key_W || e->key() == Qt::Key_A || e->key() == Qt::Key_S
      || e->key() == Qt::Key_D || e->key() == Qt::Key_Q || e->key() == Qt::Key_E
      || e->key() == Qt::Key_Tab) {
    backend_.setInteracting(false);
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
  if (!sceneLoadInProgress_.load())
    backend_.setInteracting(false);
  update();
}

void RenderWidget::setUpAxis(UpAxis axis)
{
  if (sceneLoadInProgress_.load()) {
    upAxis_ = axis;
    update();
    return;
  }

  if (axis == upAxis_)
    return;

  upAxis_ = axis;
  if (inputMode_ == InputMode::Orbit) {
    alignOrbitUpToReference();
    syncFlyFromOrbit();
  } else {
    syncOrbitFromFly();
  }

  resetAccumulationTargets();
  syncCameraToBackend();
  renderOnce();
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

void RenderWidget::setRenderWorkerClient(RenderWorkerClient *client)
{
  renderWorkerClient_ = client;
}

void RenderWidget::replayWorkerState()
{
  if (!usingWorkerRenderPath())
    return;

  renderWorkerClient_->resize(width(), height());
  renderWorkerClient_->setRenderer(currentRenderer_);
  renderWorkerClient_->setRenderSettings(workerSettings_);

  if (currentSceneIsObj_ && !currentModelPath_.isEmpty()) {
    const auto result = renderWorkerClient_->loadObj(currentModelPath_);
    if (result.success) {
      sceneBoundsMin_ = result.boundsMin;
      sceneBoundsMax_ = result.boundsMax;
    }
  } else if (!currentBrlcadPath_.isEmpty()) {
    const QString objectName =
        currentBrlcadObject_.trimmed().isEmpty() ? QStringLiteral("all") : currentBrlcadObject_;
    const auto result = renderWorkerClient_->loadBrlcad(currentBrlcadPath_, objectName);
    if (result.success) {
      sceneBoundsMin_ = result.boundsMin;
      sceneBoundsMax_ = result.boundsMax;
    }
  }

  syncCameraToBackend();
  resetAccumulationTargets();
  renderOnce();
}

void RenderWidget::startAsyncLoad(
    const std::function<void()> &loader, const QString &statusText)
{
  if (sceneLoadThread_.joinable())
    sceneLoadThread_.join();

  sceneLoadInProgress_.store(true);
  loadStatusText_ = statusText;
  lastError_.clear();
  update();

  sceneLoadThread_ = std::thread([loader]() { loader(); });
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
