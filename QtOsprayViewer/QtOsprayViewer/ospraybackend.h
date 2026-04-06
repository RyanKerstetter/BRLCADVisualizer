#pragma once

#include <cstdint>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <ospray/ospray_cpp.h>
#include <ospray/ospray_cpp/ext/rkcommon.h>

class OsprayBackend
{
 public:
  enum class SettingsMode
  {
    Automatic,
    Custom
  };

  enum class AutomaticPreset
  {
    Fast,
    Balanced,
    Quality
  };

  OsprayBackend() = default;

  void init();
  void resize(int w, int h);
  const std::string &currentRenderer() const;
  void setCamera(const rkcommon::math::vec3f &eye,
      const rkcommon::math::vec3f &center,
      const rkcommon::math::vec3f &up,
      float fovyDeg);

  void resetAccumulation();
  bool advanceRender(int timeBudgetMs = 2);
  const uint32_t *pixels() const;

  bool loadObj(const std::string &path);
  bool loadBrlcad(const std::string &path, const std::string &topObject = "");
  std::vector<std::string> listBrlcadObjects(const std::string &path) const;
  void loadTestMesh();

  rkcommon::math::vec3f getBoundsCenter() const;
  float getBoundsRadius() const;

  rkcommon::math::vec3f getBoundsMin() const;
  rkcommon::math::vec3f getBoundsMax() const;
  float getBoundsMaxExtent() const;

  void setRenderer(const std::string &type);
  void setAoSamples(int samples);
  void setPixelSamples(int samples);

  void setSettingsMode(SettingsMode mode);
  SettingsMode settingsMode() const;

  void setAutomaticPreset(AutomaticPreset preset);
  AutomaticPreset automaticPreset() const;
  void setAutomaticTargetFrameTimeMs(float ms);
  float automaticTargetFrameTimeMs() const;
  void setAutomaticAccumulationEnabled(bool enabled);
  bool automaticAccumulationEnabled() const;

  void setCustomStartScale(int scale);
  int customStartScale() const;
  void setCustomTargetFrameTimeMs(float ms);
  float customTargetFrameTimeMs() const;
  int customAoSamples() const;
  int customPixelSamples() const;
  void setCustomAccumulationEnabled(bool enabled);
  bool customAccumulationEnabled() const;
  void setCustomMaxAccumulationFrames(int frames);
  int customMaxAccumulationFrames() const;
  void setCustomLowQualityWhileInteracting(bool enabled);
  bool customLowQualityWhileInteracting() const;
  void setCustomFullResAccumulationOnly(bool enabled);
  bool customFullResAccumulationOnly() const;
  void setCustomWatchdogTimeoutMs(int ms);
  int customWatchdogTimeoutMs() const;
  void setInteracting(bool interacting);

  const std::string &lastError() const;
  uint64_t accumulatedFrames() const;
  uint64_t watchdogCancelCount() const;
  uint64_t aoAutoReductionCount() const;
  int currentScale() const;
  bool dynamicModeActive() const;
  bool backoffApplied() const;

  int width() const
  {
    return fbW_;
  }
  int height() const
  {
    return fbH_;
  }
    
  int& getAoSamples();

 

  float lastFrameTimeMs() const;
  float renderFPS() const;

 private:
  struct PendingCameraState
  {
    rkcommon::math::vec3f eye{0.f, 0.f, 1.f};
    rkcommon::math::vec3f center{0.f, 0.f, 0.f};
    rkcommon::math::vec3f up{0.f, 1.f, 0.f};
    float fovyDeg = 60.f;
  };

  enum class RenderPhase
  {
    Progressive,
    Accumulate
  };

  void setError(std::string message);
  void cancelInFlightFrame();
  void resetProgressiveState(bool clearDisplay = false);
  void updateCameraCrop(const rkcommon::math::vec2f &imageStart,
      const rkcommon::math::vec2f &imageEnd);
  bool startNextRenderWork();
  bool finishCompletedRender();
  void beginNextProgressivePass();
  void prepareTileFrameBuffer(int tileW, int tileH);
  void upsamplePassToDisplay();
  void applyAoBackoff(bool forcedByWatchdog);
  void applyPendingState();

  int fbW_ = 1;
  int fbH_ = 1;

  rkcommon::math::vec3f boundsMin_{0.f, 0.f, 0.f};
  rkcommon::math::vec3f boundsMax_{0.f, 0.f, 0.f};

  ospray::cpp::Renderer renderer_;
  ospray::cpp::Camera camera_;
  ospray::cpp::World world_;
  ospray::cpp::FrameBuffer passFb_;
  ospray::cpp::FrameBuffer accumFb_;
  ospray::cpp::Future currentFrame_;

  std::vector<uint32_t> displayPixels_;
  std::vector<uint32_t> passPixels_;
  std::string lastError_;
  float lastFrameTimeMs_ = 0.0f;
  std::string currentRenderer_ = "scivis";
  uint64_t accumulatedFrames_ = 0;
  static constexpr int kMaxSafeAoSamples = 32;
  static constexpr int kMaxSafePixelSamples = 64;

  RenderPhase renderPhase_ = RenderPhase::Progressive;
  bool frameInFlight_ = false;
  int currentScaleIndex_ = 0;
  int passScale_ = 1;
  int passW_ = 1;
  int passH_ = 1;
  int slowPassStreak_ = 0;
  int slowFrameStreak_ = 0;
  bool cameraDirty_ = false;
  int appliedAoSamples_ = -1;
  int appliedPixelSamples_ = -1;
  uint64_t watchdogCancelCount_ = 0;
  uint64_t aoAutoReductionCount_ = 0;
  std::chrono::steady_clock::time_point inFlightStart_;
  bool inFlightStartValid_ = false;
  static constexpr std::array<int, 5> kProgressiveScales{{16, 8, 4, 2, 1}};
  static constexpr int kDefaultWatchdogMs = 1500;
  static constexpr int kAoBackoffStreak = 3;

  bool watchdogTriggered_ = false;
  bool dynamicModeActive_ = false;
  bool backoffApplied_ = false;
  bool isInteracting_ = false;
  int aoBackoffSteps_ = 0;
  int progressiveFramesAtCurrentScale_ = 0;
  std::optional<PendingCameraState> pendingCameraState_;
  std::optional<std::string> pendingRendererType_;
  bool pendingResetAccumulation_ = false;
  int pendingResizeW_ = 1;
  int pendingResizeH_ = 1;
  bool pendingResize_ = false;

  SettingsMode settingsMode_ = SettingsMode::Automatic;
  AutomaticPreset automaticPreset_ = AutomaticPreset::Balanced;
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
  int customWatchdogTimeoutMs_ = kDefaultWatchdogMs;

  int sanitizeScale(int scale) const;
  int scaleToIndex(int scale) const;
  int startScaleForCurrentMode() const;
  float targetFrameTimeForCurrentMode() const;
  bool accumulationEnabledForCurrentMode() const;
  int maxAccumulationFramesForCurrentMode() const;
  int watchdogTimeoutForCurrentMode() const;
  int configuredAoSamplesForCurrentMode() const;
  int configuredPixelSamplesForCurrentMode() const;
  bool fullResAccumulationOnlyForCurrentMode() const;
  bool lowQualityWhileInteractingForCurrentMode() const;
  void applyRendererSamplingParams(int aoSamples, int pixelSamples);
};
