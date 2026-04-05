#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <mutex>

#include <ospray/ospray_cpp/ext/rkcommon.h>

#ifdef _WIN32
#include <windows.h>
#endif

class QProcess;

class RenderWorkerClient : public QObject
{
  Q_OBJECT

 public:
  struct FrameResult
  {
    QImage image;
    float frameTimeMs = 0.0f;
    bool updated = false;
    float renderFPS = 0.0f;
    uint64_t accumulatedFrames = 0;
    uint64_t watchdogCancels = 0;
    uint64_t aoAutoReductions = 0;
    QString renderer;
  };

  struct SceneLoadResult
  {
    bool success = false;
    QString errorMessage;
    rkcommon::math::vec3f boundsMin{0.f, 0.f, 0.f};
    rkcommon::math::vec3f boundsMax{0.f, 0.f, 0.f};
  };

  struct RenderSettingsState
  {
    int settingsMode = 0;
    int automaticPreset = 1;
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

  explicit RenderWorkerClient(QObject *parent = nullptr);
  ~RenderWorkerClient() override;

  bool start(const QString &workerPath);
  void stop();
  bool isConnected() const;
  QString lastError() const;
  QStringList listBrlcadObjects(const QString &path);
  SceneLoadResult loadObj(const QString &path);
  SceneLoadResult loadBrlcad(const QString &path, const QString &objectName);
  bool resize(int width, int height);
  bool setCamera(const rkcommon::math::vec3f &eye,
      const rkcommon::math::vec3f &center,
      const rkcommon::math::vec3f &up,
      float fovyDeg);
  bool resetAccumulation();
  bool setRenderer(const QString &rendererType);
  bool setRenderSettings(const RenderSettingsState &settings);
  FrameResult requestFrame();
  bool restart();

 signals:
  void workerConnectionChanged(bool connected);

 private:
#ifdef _WIN32
  bool connectPipe();
  bool sendPing();
  bool sendRequest(uint32_t type, const QString &payload, QString *responsePayload);
  bool sendRequestBytes(uint32_t type,
      const std::string &payload,
      std::string *responsePayload);
  void closePipe();
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
#endif
  QProcess *process_ = nullptr;
  QString workerPath_;
  QString pipeName_;
  QString lastError_;
  uint64_t nextRequestId_ = 2;
  mutable std::mutex requestMutex_;
};
