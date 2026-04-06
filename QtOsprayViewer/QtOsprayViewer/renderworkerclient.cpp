#include "renderworkerclient.h"

#include "worker_ipc.h"

#include <QCoreApplication>
#include <QProcess>
#include <QThread>

#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
namespace {
void lowerWorkerPriority(qint64 processId)
{
  if (processId <= 0)
    return;

  HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
      FALSE,
      static_cast<DWORD>(processId));
  if (!process)
    return;

  SetPriorityClass(process, BELOW_NORMAL_PRIORITY_CLASS);
  CloseHandle(process);
}
} // namespace
#endif

RenderWorkerClient::RenderWorkerClient(QObject *parent) : QObject(parent)
{
  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(process_, &QProcess::readyReadStandardError, this, [this]() {
    appendProcessOutput(process_->readAllStandardError());
  });
  connect(process_, &QProcess::readyReadStandardOutput, this, [this]() {
    appendProcessOutput(process_->readAllStandardOutput());
  });
  connect(process_,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString reason = exitStatus == QProcess::CrashExit ? QStringLiteral("crashed")
                                                                 : QStringLiteral("exited");
        lastError_ = buildDisconnectedError(
            QStringLiteral("Render worker %1 with code %2.").arg(reason).arg(exitCode));
      });
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

#ifdef _WIN32
  lowerWorkerPriority(process_->processId());
#endif

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

QString RenderWorkerClient::messageTypeLabel(uint32_t type) const
{
  switch (static_cast<ibrt::ipc::MessageType>(type)) {
  case ibrt::ipc::MessageType::Ping:
    return QStringLiteral("ping");
  case ibrt::ipc::MessageType::Pong:
    return QStringLiteral("pong");
  case ibrt::ipc::MessageType::Shutdown:
    return QStringLiteral("shutdown");
  case ibrt::ipc::MessageType::ListBrlcadObjects:
    return QStringLiteral("list BRL-CAD objects");
  case ibrt::ipc::MessageType::BrlcadObjectList:
    return QStringLiteral("BRL-CAD object list");
  case ibrt::ipc::MessageType::LoadObj:
    return QStringLiteral("load OBJ");
  case ibrt::ipc::MessageType::LoadBrlcad:
    return QStringLiteral("load BRL-CAD");
  case ibrt::ipc::MessageType::LoadResult:
    return QStringLiteral("load result");
  case ibrt::ipc::MessageType::Error:
    return QStringLiteral("error");
  case ibrt::ipc::MessageType::Resize:
    return QStringLiteral("resize");
  case ibrt::ipc::MessageType::SetCamera:
    return QStringLiteral("set camera");
  case ibrt::ipc::MessageType::ResetAccumulation:
    return QStringLiteral("reset accumulation");
  case ibrt::ipc::MessageType::RequestFrame:
    return QStringLiteral("request frame");
  case ibrt::ipc::MessageType::FrameData:
    return QStringLiteral("frame data");
  case ibrt::ipc::MessageType::SetRenderer:
    return QStringLiteral("set renderer");
  case ibrt::ipc::MessageType::SetRenderSettings:
    return QStringLiteral("set render settings");
  case ibrt::ipc::MessageType::SetInteracting:
    return QStringLiteral("set interacting");
  case ibrt::ipc::MessageType::SetBrlcadColorEnabled:
    return QStringLiteral("set BRL-CAD color enabled");
  }
  return QStringLiteral("unknown request");
}

void RenderWorkerClient::appendProcessOutput(const QByteArray &data)
{
  if (data.isEmpty())
    return;

  workerOutputTail_ += QString::fromLocal8Bit(data);
  constexpr int maxChars = 4000;
  if (workerOutputTail_.size() > maxChars)
    workerOutputTail_.remove(0, workerOutputTail_.size() - maxChars);
}

QString RenderWorkerClient::buildDisconnectedError(const QString &contextMessage) const
{
  QString error = contextMessage;

  if (process_->state() == QProcess::NotRunning) {
    error += QStringLiteral(" Process state: not running.");
    if (process_->exitStatus() == QProcess::CrashExit) {
      error += QStringLiteral(" Exit status: crash.");
    } else {
      error += QStringLiteral(" Exit code: %1.").arg(process_->exitCode());
    }
  }

  const QString output = workerOutputTail_.trimmed();
  if (!output.isEmpty()) {
    QString tail = output;
    const int lastBreak = std::max(tail.lastIndexOf('\n'), tail.lastIndexOf('\r'));
    if (lastBreak >= 0)
      tail = tail.mid(lastBreak + 1).trimmed();
    if (tail.isEmpty())
      tail = output;
    error += QStringLiteral(" Worker output: %1").arg(tail);
  }

  return error;
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

bool RenderWorkerClient::setInteracting(bool interacting)
{
#ifndef _WIN32
  Q_UNUSED(interacting);
  return false;
#else
  const uint32_t payloadValue = interacting ? 1u : 0u;
  std::string response;
  return sendRequestBytes(static_cast<uint32_t>(ibrt::ipc::MessageType::SetInteracting),
      std::string(reinterpret_cast<const char *>(&payloadValue), sizeof(payloadValue)),
      &response);
#endif
}

bool RenderWorkerClient::setBrlcadColorEnabled(bool enabled)
{
#ifndef _WIN32
  Q_UNUSED(enabled);
  return false;
#else
  const uint32_t payloadValue = enabled ? 1u : 0u;
  std::string response;
  return sendRequestBytes(
      static_cast<uint32_t>(ibrt::ipc::MessageType::SetBrlcadColorEnabled),
      std::string(reinterpret_cast<const char *>(&payloadValue), sizeof(payloadValue)),
      &response);
#endif
}

RenderWorkerClient::FrameResult RenderWorkerClient::requestFrame()
{
  FrameResult result;
#ifndef _WIN32
  return result;
#else
  const auto requestStart = std::chrono::steady_clock::now();
  std::string payload;
  if (!sendRequestBytes(static_cast<uint32_t>(ibrt::ipc::MessageType::RequestFrame),
          std::string(),
          &payload)) {
    return result;
  }
  const auto requestEnd = std::chrono::steady_clock::now();
  result.requestRoundTripMs =
      std::chrono::duration<float, std::milli>(requestEnd - requestStart).count();

  struct FrameHeader
  {
    uint32_t width;
    uint32_t height;
    float frameTimeMs;
    float mapCopyTimeMs;
    float upsampleTimeMs;
    float renderFPS;
    int32_t currentScale;
    int32_t appliedAoSamples;
    int32_t appliedPixelSamples;
    uint32_t updated;
    uint32_t interacting;
    uint64_t accumulatedFrames;
    uint64_t watchdogCancels;
    uint64_t aoAutoReductions;
    uint64_t brlcadTraceCalls;
    uint64_t brlcadIntersectCalls;
    uint64_t brlcadRaysTested;
    float brlcadTraceTimeMs;
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

  const auto imageCopyStart = std::chrono::steady_clock::now();
  QImage image(header.width, header.height, QImage::Format_ARGB32);
  std::memcpy(image.bits(), payload.data() + sizeof(FrameHeader), size_t(pixelBytes));
  const auto imageCopyEnd = std::chrono::steady_clock::now();
  result.image = image;
  result.frameTimeMs = header.frameTimeMs;
  result.mapCopyTimeMs = header.mapCopyTimeMs;
  result.upsampleTimeMs = header.upsampleTimeMs;
  result.imageDecodeCopyMs =
      std::chrono::duration<float, std::milli>(imageCopyEnd - imageCopyStart).count();
  result.renderFPS = header.renderFPS;
  result.currentScale = header.currentScale;
  result.appliedAoSamples = header.appliedAoSamples;
  result.appliedPixelSamples = header.appliedPixelSamples;
  result.interacting = header.interacting != 0;
  result.brlcadTraceCalls = header.brlcadTraceCalls;
  result.brlcadIntersectCalls = header.brlcadIntersectCalls;
  result.brlcadRaysTested = header.brlcadRaysTested;
  result.brlcadTraceTimeMs = header.brlcadTraceTimeMs;
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
    lastError_ = buildDisconnectedError(
        QStringLiteral("Failed to send %1 request to render worker.")
            .arg(messageTypeLabel(type)));
    closePipe();
    emit workerConnectionChanged(false);
    return false;
  }

  ibrt::ipc::Message response;
  if (!ibrt::ipc::readMessage(pipe_, response)) {
    appendProcessOutput(process_->readAllStandardError());
    appendProcessOutput(process_->readAllStandardOutput());
    lastError_ = buildDisconnectedError(
        QStringLiteral("Failed to read %1 response from render worker.")
            .arg(messageTypeLabel(type)));
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
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetRenderSettings)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetInteracting)
          || type == static_cast<uint32_t>(ibrt::ipc::MessageType::SetBrlcadColorEnabled))
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
