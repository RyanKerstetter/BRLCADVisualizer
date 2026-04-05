#include "renderworkerclient.h"

#include "worker_ipc.h"

#include <QCoreApplication>
#include <QProcess>
#include <QThread>

#include <cstring>
#include <string>

RenderWorkerClient::RenderWorkerClient(QObject *parent) : QObject(parent)
{
  process_ = new QProcess(this);
}

RenderWorkerClient::~RenderWorkerClient()
{
  stop();
}

bool RenderWorkerClient::start(const QString &workerPath)
{
#ifndef _WIN32
  Q_UNUSED(workerPath);
  lastError_ = QStringLiteral("Render worker is currently implemented for Windows only.");
  return false;
#else
  workerPath_ = workerPath;
  stop();

  pipeName_ =
      QString::fromStdString(ibrt::ipc::makePipeName(QCoreApplication::applicationPid()));
  process_->start(workerPath, {QStringLiteral("--pipe"), pipeName_});
  if (!process_->waitForStarted(5000)) {
    lastError_ = QStringLiteral("Failed to start render worker process.");
    return false;
  }

  if (!connectPipe()) {
    process_->kill();
    process_->waitForFinished(2000);
    return false;
  }

  if (!sendPing()) {
    stop();
    return false;
  }

  emit workerConnectionChanged(true);
  return true;
#endif
}

bool RenderWorkerClient::restart()
{
  if (workerPath_.isEmpty()) {
    lastError_ = QStringLiteral("Render worker path is not configured.");
    return false;
  }
  return start(workerPath_);
}

void RenderWorkerClient::stop()
{
#ifdef _WIN32
  if (pipe_ != INVALID_HANDLE_VALUE) {
    ibrt::ipc::writeMessage(
        pipe_, {ibrt::ipc::MessageType::Shutdown, 0, std::string()});
  }
  closePipe();
#endif

  if (process_->state() != QProcess::NotRunning) {
    process_->terminate();
    if (!process_->waitForFinished(2000))
      process_->kill();
  }

  emit workerConnectionChanged(false);
}

bool RenderWorkerClient::isConnected() const
{
#ifdef _WIN32
  return pipe_ != INVALID_HANDLE_VALUE;
#else
  return false;
#endif
}

QString RenderWorkerClient::lastError() const
{
  return lastError_;
}

QStringList RenderWorkerClient::listBrlcadObjects(const QString &path)
{
#ifndef _WIN32
  Q_UNUSED(path);
  return {};
#else
  QString payload;
  if (!sendRequest(static_cast<uint32_t>(ibrt::ipc::MessageType::ListBrlcadObjects),
          path,
          &payload)) {
    return {};
  }

  if (payload.isEmpty())
    return {};

  return payload.split('\n', Qt::SkipEmptyParts);
#endif
}

RenderWorkerClient::SceneLoadResult RenderWorkerClient::loadObj(const QString &path)
{
  SceneLoadResult result;
#ifndef _WIN32
  Q_UNUSED(path);
  result.errorMessage = QStringLiteral("Render worker is currently implemented for Windows only.");
  lastError_ = result.errorMessage;
  return result;
#else
  std::string payload;
  if (!sendRequestBytes(
          static_cast<uint32_t>(ibrt::ipc::MessageType::LoadObj), path.toStdString(), &payload)) {
    result.errorMessage = lastError_;
    return result;
  }

  struct LoadResultPayload
  {
    uint32_t success;
    float boundsMin[3];
    float boundsMax[3];
    uint32_t errorSize;
  } header{};

  if (payload.size() < sizeof(header)) {
    result.errorMessage = QStringLiteral("Render worker returned an invalid load result.");
    lastError_ = result.errorMessage;
    return result;
  }

  std::memcpy(&header, payload.data(), sizeof(header));
  result.success = header.success != 0;
  result.boundsMin = rkcommon::math::vec3f(
      header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
  result.boundsMax = rkcommon::math::vec3f(
      header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
  if (header.errorSize > 0
      && payload.size() >= sizeof(header) + size_t(header.errorSize)) {
    result.errorMessage = QString::fromStdString(
        payload.substr(sizeof(header), size_t(header.errorSize)));
  }
  lastError_ = result.errorMessage;
  return result;
#endif
}

RenderWorkerClient::SceneLoadResult RenderWorkerClient::loadBrlcad(
    const QString &path, const QString &objectName)
{
  SceneLoadResult result;
#ifndef _WIN32
  Q_UNUSED(path);
  Q_UNUSED(objectName);
  result.errorMessage = QStringLiteral("Render worker is currently implemented for Windows only.");
  lastError_ = result.errorMessage;
  return result;
#else
  const std::string requestPayload =
      path.toStdString() + '\n' + objectName.toStdString();
  std::string payload;
  if (!sendRequestBytes(
          static_cast<uint32_t>(ibrt::ipc::MessageType::LoadBrlcad), requestPayload, &payload)) {
    result.errorMessage = lastError_;
    return result;
  }

  struct LoadResultPayload
  {
    uint32_t success;
    float boundsMin[3];
    float boundsMax[3];
    uint32_t errorSize;
  } header{};

  if (payload.size() < sizeof(header)) {
    result.errorMessage = QStringLiteral("Render worker returned an invalid load result.");
    lastError_ = result.errorMessage;
    return result;
  }

  std::memcpy(&header, payload.data(), sizeof(header));
  result.success = header.success != 0;
  result.boundsMin = rkcommon::math::vec3f(
      header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
  result.boundsMax = rkcommon::math::vec3f(
      header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
  if (header.errorSize > 0
      && payload.size() >= sizeof(header) + size_t(header.errorSize)) {
    result.errorMessage = QString::fromStdString(
        payload.substr(sizeof(header), size_t(header.errorSize)));
  }
  lastError_ = result.errorMessage;
  return result;
#endif
}

bool RenderWorkerClient::resize(int width, int height)
{
#ifndef _WIN32
  Q_UNUSED(width);
  Q_UNUSED(height);
  return false;
#else
  std::string payload(sizeof(int32_t) * 2, '\0');
  auto *values = reinterpret_cast<int32_t *>(payload.data());
  values[0] = width;
  values[1] = height;
  std::string response;
  return sendRequestBytes(
      static_cast<uint32_t>(ibrt::ipc::MessageType::Resize), payload, &response);
#endif
}

bool RenderWorkerClient::setCamera(const rkcommon::math::vec3f &eye,
    const rkcommon::math::vec3f &center,
    const rkcommon::math::vec3f &up,
    float fovyDeg)
{
#ifndef _WIN32
  Q_UNUSED(eye);
  Q_UNUSED(center);
  Q_UNUSED(up);
  Q_UNUSED(fovyDeg);
  return false;
#else
  struct CameraPayload
  {
    float eye[3];
    float center[3];
    float up[3];
    float fovyDeg;
  } payloadData{{eye.x, eye.y, eye.z},
      {center.x, center.y, center.z},
      {up.x, up.y, up.z},
      fovyDeg};

  const std::string payload(
      reinterpret_cast<const char *>(&payloadData), sizeof(payloadData));
  std::string response;
  return sendRequestBytes(
      static_cast<uint32_t>(ibrt::ipc::MessageType::SetCamera), payload, &response);
#endif
}

bool RenderWorkerClient::resetAccumulation()
{
#ifndef _WIN32
  return false;
#else
  std::string response;
  return sendRequestBytes(static_cast<uint32_t>(ibrt::ipc::MessageType::ResetAccumulation),
      std::string(),
      &response);
#endif
}

bool RenderWorkerClient::setRenderer(const QString &rendererType)
{
#ifndef _WIN32
  Q_UNUSED(rendererType);
  return false;
#else
  std::string response;
  return sendRequestBytes(
      static_cast<uint32_t>(ibrt::ipc::MessageType::SetRenderer),
      rendererType.toStdString(),
      &response);
#endif
}

bool RenderWorkerClient::setRenderSettings(const RenderSettingsState &settings)
{
#ifndef _WIN32
  Q_UNUSED(settings);
  return false;
#else
  struct SettingsPayload
  {
    int32_t settingsMode;
    int32_t automaticPreset;
    float automaticTargetFrameTimeMs;
    uint32_t automaticAccumulationEnabled;
    int32_t customStartScale;
    float customTargetFrameTimeMs;
    int32_t customAoSamples;
    int32_t customPixelSamples;
    uint32_t customAccumulationEnabled;
    int32_t customMaxAccumulationFrames;
    uint32_t customLowQualityWhileInteracting;
    uint32_t customFullResAccumulationOnly;
    int32_t customWatchdogTimeoutMs;
  } payload{settings.settingsMode,
      settings.automaticPreset,
      settings.automaticTargetFrameTimeMs,
      settings.automaticAccumulationEnabled ? 1u : 0u,
      settings.customStartScale,
      settings.customTargetFrameTimeMs,
      settings.customAoSamples,
      settings.customPixelSamples,
      settings.customAccumulationEnabled ? 1u : 0u,
      settings.customMaxAccumulationFrames,
      settings.customLowQualityWhileInteracting ? 1u : 0u,
      settings.customFullResAccumulationOnly ? 1u : 0u,
      settings.customWatchdogTimeoutMs};
  std::string response;
  return sendRequestBytes(static_cast<uint32_t>(ibrt::ipc::MessageType::SetRenderSettings),
      std::string(reinterpret_cast<const char *>(&payload), sizeof(payload)),
      &response);
#endif
}

RenderWorkerClient::FrameResult RenderWorkerClient::requestFrame()
{
  FrameResult result;
#ifndef _WIN32
  return result;
#else
  std::string payload;
  if (!sendRequestBytes(static_cast<uint32_t>(ibrt::ipc::MessageType::RequestFrame),
          std::string(),
          &payload)) {
    return result;
  }

  struct FrameHeader
  {
    uint32_t width;
    uint32_t height;
    float frameTimeMs;
    float renderFPS;
    uint32_t updated;
    uint64_t accumulatedFrames;
    uint64_t watchdogCancels;
    uint64_t aoAutoReductions;
    uint32_t rendererNameSize;
  };

  if (payload.size() < sizeof(FrameHeader))
    return result;

  FrameHeader header{};
  std::memcpy(&header, payload.data(), sizeof(header));
  const int pixelBytes = int(header.width) * int(header.height) * 4;
  if (payload.size() < sizeof(FrameHeader) + size_t(pixelBytes) || header.width == 0
      || header.height == 0) {
    return result;
  }

  QImage image(header.width, header.height, QImage::Format_RGBA8888);
  std::memcpy(image.bits(), payload.data() + sizeof(FrameHeader), size_t(pixelBytes));
  result.image = image;
  result.frameTimeMs = header.frameTimeMs;
  result.renderFPS = header.renderFPS;
  result.updated = header.updated != 0;
  result.accumulatedFrames = header.accumulatedFrames;
  result.watchdogCancels = header.watchdogCancels;
  result.aoAutoReductions = header.aoAutoReductions;
  const size_t pixelOffset = sizeof(FrameHeader);
  const size_t rendererOffset = pixelOffset + size_t(pixelBytes);
  if (header.rendererNameSize > 0
      && payload.size() >= rendererOffset + size_t(header.rendererNameSize)) {
    result.renderer = QString::fromStdString(
        payload.substr(rendererOffset, size_t(header.rendererNameSize)));
  }
  return result;
#endif
}

#ifdef _WIN32
bool RenderWorkerClient::connectPipe()
{
  for (int attempt = 0; attempt < 50; ++attempt) {
    pipe_ = CreateFileA(pipeName_.toStdString().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE)
      return true;

    QThread::msleep(100);
  }

  lastError_ = QStringLiteral("Failed to connect to render worker pipe.");
  pipe_ = INVALID_HANDLE_VALUE;
  return false;
}

bool RenderWorkerClient::sendPing()
{
  if (pipe_ == INVALID_HANDLE_VALUE)
    return false;

  QString responsePayload;
  return sendRequest(static_cast<uint32_t>(ibrt::ipc::MessageType::Ping), QString(), &responsePayload);
}

bool RenderWorkerClient::sendRequest(
    uint32_t type, const QString &payload, QString *responsePayload)
{
  std::string responseBytes;
  const bool ok = sendRequestBytes(type, payload.toStdString(), &responseBytes);
  if (responsePayload)
    *responsePayload = QString::fromStdString(responseBytes);
  return ok;
}

bool RenderWorkerClient::sendRequestBytes(
    uint32_t type, const std::string &payload, std::string *responsePayload)
{
  std::lock_guard<std::mutex> lock(requestMutex_);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    lastError_ = QStringLiteral("Render worker pipe is not connected.");
    return false;
  }

  const uint64_t requestId = nextRequestId_++;
  const ibrt::ipc::Message request{static_cast<ibrt::ipc::MessageType>(type),
      requestId,
      payload};
  if (!ibrt::ipc::writeMessage(pipe_, request)) {
    lastError_ = QStringLiteral("Failed to send request to render worker.");
    closePipe();
    emit workerConnectionChanged(false);
    return false;
  }

  ibrt::ipc::Message response;
  if (!ibrt::ipc::readMessage(pipe_, response)) {
    lastError_ = QStringLiteral("Failed to read response from render worker.");
    closePipe();
    emit workerConnectionChanged(false);
    return false;
  }

  if (response.requestId != requestId) {
    lastError_ = QStringLiteral("Render worker response ID mismatch.");
    return false;
  }

  if (response.type == ibrt::ipc::MessageType::Error) {
    lastError_ = QString::fromStdString(response.payload);
    if (responsePayload)
      *responsePayload = response.payload;
    return false;
  }

  if (type == static_cast<uint32_t>(ibrt::ipc::MessageType::Ping)
      && response.type != ibrt::ipc::MessageType::Pong) {
    lastError_ = QStringLiteral("Render worker did not respond to ping.");
    return false;
  }

  if (type == static_cast<uint32_t>(ibrt::ipc::MessageType::ListBrlcadObjects)
      && response.type != ibrt::ipc::MessageType::BrlcadObjectList) {
    lastError_ = QStringLiteral("Unexpected response to BRL-CAD object list request.");
    return false;
  }

  if ((type == static_cast<uint32_t>(ibrt::ipc::MessageType::LoadObj)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::LoadBrlcad))
      && response.type != ibrt::ipc::MessageType::LoadResult) {
    lastError_ = QStringLiteral("Unexpected response to scene load request.");
    return false;
  }

  if ((type == static_cast<uint32_t>(ibrt::ipc::MessageType::Resize)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetCamera)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::ResetAccumulation)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetRenderer)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetRenderSettings))
      && response.type != ibrt::ipc::MessageType::LoadResult) {
    lastError_ = QStringLiteral("Unexpected response to render control request.");
    return false;
  }

  if (type == static_cast<uint32_t>(ibrt::ipc::MessageType::RequestFrame)
      && response.type != ibrt::ipc::MessageType::FrameData) {
    lastError_ = QStringLiteral("Unexpected response to frame request.");
    return false;
  }

  if (responsePayload)
    *responsePayload = response.payload;
  return true;
}

void RenderWorkerClient::closePipe()
{
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}
#endif
