#include "renderwidget.h"

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

  pitch_ = clampf(pitch_, -1.4f, 1.4f);

  vec3f forward = forwardFromAngles(yaw_, pitch_);
  vec3f right = normalizeVec(crossVec(forward, worldUp()));
  vec3f upCam = normalizeVec(crossVec(right, forward));

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
      yaw_ += dx;
      pitch_ += dy;
    } else if (result.axis == Axis::X) {
      pitch_ += dy;
    } else if (result.axis == Axis::Y) {
      yaw_ += dx;
    } else if (result.axis == Axis::Z) {
      yaw_ += dx;
    }

    pitch_ = clampf(pitch_, -1.4f, 1.4f);
  }

  else if (result.action == Action::Scale) {
    float amount = float(delta.y()) * 0.01f;

    float maxExtent = backend_.getBoundsMaxExtent();
    if (maxExtent < 0.001f)
      maxExtent = 1.0f;

    float minDist = std::max(maxExtent * 1e-8f, 1e-8f);
    float maxDist = std::max(maxExtent * 100.0f, 10.0f);

    dist_ *= std::pow(1.05f, amount * 10.0f);
    dist_ = clampf(dist_, minDist, maxDist);
  }

  backend_.resetAccumulation();
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
  vec3f forward = forwardFromAngles(yaw_, pitch_);
  vec3f eye(center_.x - dist_ * forward.x,
      center_.y - dist_ * forward.y,
      center_.z - dist_ * forward.z);

  flyPos_ = eye;
  flyYaw_ = yaw_;
  flyPitch_ = pitch_;
}

void RenderWidget::syncOrbitFromFly()
{
  vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
  forward = normalizeVec(forward);

  vec3f eye = flyPos_;
  center_ = vec3f(eye.x + forward.x * dist_,
      eye.y + forward.y * dist_,
      eye.z + forward.z * dist_);

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
  backend_.resize(w, h);
  ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));

  resetView();
}

void RenderWidget::syncCameraToBackend()
{
  if (inputMode_ == InputMode::Orbit) {
    pitch_ = clampf(pitch_, -1.4f, 1.4f);

    vec3f forward = forwardFromAngles(yaw_, pitch_);
    vec3f eye(center_.x - dist_ * forward.x,
        center_.y - dist_ * forward.y,
        center_.z - dist_ * forward.z);

    backend_.setCamera(eye, center_, worldUp(), fovy_);
  } else {
    flyPitch_ = clampf(flyPitch_, -1.4f, 1.4f);

    vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);

    backend_.setCamera(flyPos_, flyPos_ + forward, worldUp(), fovy_);
  }
}

void RenderWidget::renderOnce()
{
  if (!backendReady_)
    return;

  const uint32_t *px = backend_.render();
  const int w = backend_.width();
  const int h = backend_.height();

  if (!px || w <= 0 || h <= 0)
    return;

  if (image_.width() != w || image_.height() != h)
    image_ = QImage(w, h, QImage::Format_RGBA8888);

  std::memcpy(image_.bits(), px, size_t(w) * size_t(h) * 4);
  update();
}

void RenderWidget::advanceRender()
{
  if (!backendReady_ || !isVisible())
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

    ImGui::Begin("Viewer");

  ImGui::Separator();
  ImGui::Text("Stats");

  ImGui::Text("UI FPS: %.1f", ImGui::GetIO().Framerate);
  ImGui::Text("Render time: %.2f ms", backend_.lastFrameTimeMs());
  ImGui::Text("Render FPS: %.1f", backend_.renderFPS());
  ImGui::Text("Accumulated frames: %llu",
      static_cast<unsigned long long>(backend_.accumulatedFrames()));
  ImGui::Text("Up Axis: %s", upAxis_ == UpAxis::Z ? "Z" : "Y");
  ImGui::Text("Resolution: %d x %d", width(), height());

  ImGui::Separator();
  ImGui::Text("Renderer");

  int rendererMode = 0;
  if (backend_.currentRenderer() == "scivis")
    rendererMode = 1;
  else if (backend_.currentRenderer() == "pathtracer")
    rendererMode = 2;
  else
    rendererMode = 0;

  if (ImGui::RadioButton("ao", rendererMode == 0)) {
    rendererMode = 0;
    backend_.setRenderer("ao");
    backend_.resetAccumulation();
    renderOnce();
    update();
  }
  if (ImGui::RadioButton("SciVis", rendererMode == 1)) {
    rendererMode = 1;
    backend_.setRenderer("scivis");
    backend_.resetAccumulation();
    renderOnce();
    update();
  }
  if (ImGui::RadioButton("PathTracer", rendererMode == 2)) {
    rendererMode = 2;
    backend_.setRenderer("pathtracer");
    backend_.resetAccumulation();
    renderOnce();
    update();
  }

  ImGui::Separator();
  ImGui::Text("Lighting");

  if (ImGui::SliderInt("AO Samples", &backend_.getAoSamples(), 0, 64)) {
    backend_.setAoSamples(backend_.getAoSamples());
    backend_.resetAccumulation();
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
  if (!backend_.loadObj(path.toStdString()))
    return false;

  currentBrlcadPath_.clear();
  currentBrlcadObject_.clear();
  currentBrlcadObjects_.clear();
  resetView();
  return true;
}

bool RenderWidget::loadBrlcadModel(const QString &path, const QString &topObject)
{
  if (!backend_.loadBrlcad(path.toStdString(), topObject.toStdString()))
    return false;

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
  return QString::fromStdString(backend_.lastError());
}

void RenderWidget::resetView()
{
  center_ = backend_.getBoundsCenter();

  float maxExtent = backend_.getBoundsMaxExtent();
  if (maxExtent < 0.001f)
    maxExtent = 1.0f;

  yaw_ = 0.3f;
  pitch_ = 0.2f;
  fovy_ = 60.0f;

  dist_ = fitDistanceFromBounds(maxExtent, fovy_);

  flyYaw_ = yaw_;
  flyPitch_ = pitch_;
  syncFlyFromOrbit();

  backend_.resetAccumulation();
  syncCameraToBackend();
  renderOnce();
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

  backend_.resetAccumulation();
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

  // let camera only react if ImGui doesn't want mouse
  if (ImGui::GetIO().WantCaptureMouse)
    return;

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
      yaw_ += d.x() * orbitSpeed_;
      pitch_ += d.y() * orbitSpeed_;

      backend_.resetAccumulation();
      syncCameraToBackend();
      renderOnce();
      return;
    }

    if (e->buttons() & Qt::RightButton) {
      pitch_ = clampf(pitch_, -1.4f, 1.4f);

      vec3f forward = forwardFromAngles(yaw_, pitch_);
      vec3f right = normalizeVec(crossVec(forward, worldUp()));
      vec3f upCam = normalizeVec(crossVec(right, forward));

      float sx = float(d.x()) * panSpeed_ * dist_;
      float sy = float(d.y()) * panSpeed_ * dist_;

      center_ = vec3f(center_.x - right.x * sx + upCam.x * sy,
          center_.y - right.y * sx + upCam.y * sy,
          center_.z - right.z * sx + upCam.z * sy);

      backend_.resetAccumulation();
      syncCameraToBackend();
      renderOnce();
      return;
    }

  } else {
    if (e->buttons() & Qt::LeftButton) {
      flyYaw_ += d.x() * orbitSpeed_;
      flyPitch_ += d.y() * orbitSpeed_;

      backend_.resetAccumulation();
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

  if (ImGui::GetIO().WantCaptureMouse)
    return;

  float steps = e->angleDelta().y() / 120.f;
  if (steps == 0.f)
    return;

  if (inputMode_ == InputMode::Orbit) {
    float maxExtent = backend_.getBoundsMaxExtent();
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

  backend_.resetAccumulation();
  syncCameraToBackend();
  renderOnce();
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

  if (e->key() == Qt::Key_Tab) {
    inputMode_ =
        (inputMode_ == InputMode::Orbit) ? InputMode::Fly : InputMode::Orbit;
    backend_.resetAccumulation();
    syncCameraToBackend();
    renderOnce();
    return;
  }

  if (inputMode_ != InputMode::Fly)
    return;

  vec3f forward = forwardFromAngles(flyYaw_, flyPitch_);
  vec3f right = normalizeVec(crossVec(forward, worldUp()));

  float modelScale = backend_.getBoundsMaxExtent();
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

  backend_.resetAccumulation();
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
  update();
}

void RenderWidget::setUpAxis(UpAxis axis)
{
  if (axis == upAxis_)
    return;

  upAxis_ = axis;
  if (inputMode_ == InputMode::Orbit) {
    syncFlyFromOrbit();
  } else {
    syncOrbitFromFly();
  }

  backend_.resetAccumulation();
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
