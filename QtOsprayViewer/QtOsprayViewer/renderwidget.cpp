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
  
  auto* timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, QOverload<>::of(&RenderWidget::update));
  timer->start(16); // ~60 FPS
  
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

    vec3f eye(center_.x + dist_ * std::cos(pitch_) * std::sin(yaw_),
        center_.y + dist_ * std::sin(pitch_),
        center_.z + dist_ * std::cos(pitch_) * std::cos(yaw_));

    backend_.setCamera(eye, center_, up_, fovy_);
  } else {
    flyPitch_ = clampf(flyPitch_, -1.4f, 1.4f);

    vec3f forward(std::cos(flyPitch_) * std::sin(flyYaw_),
        std::sin(flyPitch_),
        std::cos(flyPitch_) * std::cos(flyYaw_));

    backend_.setCamera(flyPos_, flyPos_ + forward, up_, fovy_);
  }
}

void RenderWidget::renderOnce()
{
  const uint32_t *px = backend_.render();
  const int w = backend_.width();
  const int h = backend_.height();

  if (w <= 0 || h <= 0)
    return;

  if (image_.width() != w || image_.height() != h)
    image_ = QImage(w, h, QImage::Format_RGBA8888);

  std::memcpy(image_.bits(), px, size_t(w) * size_t(h) * 4);
  update();
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
  ImGui::Text("OSPRay Qt Viewer");
  ImGui::Text("FPS: %.1f", io.Framerate);
  ImGui::Text("Resolution: %d x %d", width(), height());

  ImGui::Separator();
  ImGui::Text("Renderer");

  static int rendererMode = 0;

  if (ImGui::RadioButton("ao", rendererMode == 0)) {
    rendererMode = 0;
    backend_.setRenderer("ao");
  }
  if (ImGui::RadioButton("SciVis", rendererMode == 1)) {
    rendererMode = 1;
    backend_.setRenderer("scivis");
  }
  if (ImGui::RadioButton("PathTracer", rendererMode == 2)) {
    rendererMode = 2;
    backend_.setRenderer("pathtracer");
  }

  ImGui::Separator();
  ImGui::Text("Lighting");

  if (ImGui::SliderInt("AO Samples", &backend_.getAoSamples(), 0, 8)) {
    backend_.setAoSamples(backend_.getAoSamples());
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
  if (mode == 0)
    ImGui::Text("Orbit: LMB rotate | RMB pan | wheel zoom");
  else
    ImGui::Text("Fly: WASD move | LMB look | Tab toggle");

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool RenderWidget::loadModel(const QString &path)
{
  if (!backend_.loadObj(path.toStdString()))
    return false;

  resetView();
  return true;
}

bool RenderWidget::loadBrlcadModel(const QString &path, const QString &topObject)
{
  if (!backend_.loadBrlcad(path.toStdString(), topObject.toStdString()))
    return false;

  resetView();
  return true;
}

void RenderWidget::resetView()
{
  center_ = backend_.getBoundsCenter();

  float radius = backend_.getBoundsRadius();
  if (radius < 0.001f)
    radius = 1.0f;

  up_ = vec3f(0.f, 1.f, 0.f);

  yaw_ = 0.3f;
  pitch_ = 0.2f;
  dist_ = radius * 3.0f;
  fovy_ = 60.0f;

  flyPos_ = vec3f(center_.x, center_.y, center_.z - dist_);
  flyYaw_ = 0.f;
  flyPitch_ = 0.f;

  backend_.resetAccumulation();
  syncCameraToBackend();
  renderOnce();
  update();
}

void RenderWidget::setInputMode(InputMode mode)
{
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

  if (inputMode_ == InputMode::Orbit) {
    if (e->buttons() & Qt::LeftButton) {
      yaw_ += d.x() * orbitSpeed_;
      pitch_ += d.y() * orbitSpeed_;

      backend_.resetAccumulation();
      syncCameraToBackend();
      renderOnce();
    }

    if (e->buttons() & Qt::RightButton) {
      pitch_ = clampf(pitch_, -1.4f, 1.4f);

      vec3f forward = normalizeVec(vec3f(std::cos(pitch_) * std::sin(yaw_),
          std::sin(pitch_),
          std::cos(pitch_) * std::cos(yaw_)));
      vec3f right = normalizeVec(crossVec(forward, up_));
      vec3f upCam = normalizeVec(crossVec(right, forward));

      float sx = float(d.x()) * panSpeed_ * dist_;
      float sy = float(d.y()) * panSpeed_ * dist_;

      center_ = vec3f(center_.x - right.x * sx + upCam.x * sy,
          center_.y - right.y * sx + upCam.y * sy,
          center_.z - right.z * sx + upCam.z * sy);

      backend_.resetAccumulation();
      syncCameraToBackend();
      renderOnce();
    }
  } else {
    if (e->buttons() & Qt::LeftButton) {
      flyYaw_ += d.x() * orbitSpeed_;
      flyPitch_ += d.y() * orbitSpeed_;

      backend_.resetAccumulation();
      syncCameraToBackend();
      renderOnce();
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
    dist_ *= std::pow(zoomFactor_, steps);
    dist_ = clampf(dist_, 0.5f, 50.f);
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

  vec3f forward = normalizeVec(vec3f(std::cos(flyPitch_) * std::sin(flyYaw_),
      std::sin(flyPitch_),
      std::cos(flyPitch_) * std::cos(flyYaw_)));
  vec3f right = normalizeVec(crossVec(forward, up_));

  float step = 0.2f;

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
    flyPos_ = vec3f(flyPos_.x, flyPos_.y - step, flyPos_.z);
  if (e->key() == Qt::Key_E)
    flyPos_ = vec3f(flyPos_.x, flyPos_.y + step, flyPos_.z);

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