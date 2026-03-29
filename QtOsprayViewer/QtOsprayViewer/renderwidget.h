#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QWheelEvent>

#include <ospray/ospray_cpp/ext/rkcommon.h>
#include "imgui.h"
#include "ospraybackend.h"

class RenderWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
  Q_OBJECT

 public:
  enum class InputMode
  {
    Orbit,
    Fly
  };

  explicit RenderWidget(QWidget *parent = nullptr);
  ~RenderWidget() override;

  bool loadModel(const QString &path);
  bool loadBrlcadModel(const QString &path, const QString &topObject = QString());
  QString lastError() const;
  void resetView();
  void setInputMode(InputMode mode);

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
  void syncCameraToBackend();
  void renderOnce();

  static float clampf(float v, float lo, float hi);
  static rkcommon::math::vec3f normalizeVec(const rkcommon::math::vec3f &v);
  static rkcommon::math::vec3f crossVec(
      const rkcommon::math::vec3f &a, const rkcommon::math::vec3f &b);

  OsprayBackend backend_;
  QImage image_;
  QPoint lastMouse_;

  InputMode inputMode_ = InputMode::Orbit;

  // Orbit camera
  rkcommon::math::vec3f center_{0.f, 0.f, 1.5f};
  rkcommon::math::vec3f up_{0.f, 1.f, 0.f};

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
};
