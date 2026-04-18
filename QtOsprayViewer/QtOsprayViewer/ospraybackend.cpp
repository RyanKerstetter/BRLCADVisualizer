#include "ospraybackend.h"
#include <chrono>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <stdexcept>
#include <unordered_set>

#include <ospray/ospray.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

// BRL-CAD headers (C API, needs extern "C" guard)
extern "C" {
#include <brlcad/raytrace.h>
#include <brlcad/rt/search.h>
}

using rkcommon::math::vec3f;
using rkcommon::math::vec2f;
using rkcommon::math::vec3ui;
using rkcommon::math::vec4f;

namespace {
const vec3f kSunLightDirection(-0.3f, -1.0f, -0.2f);
const vec3f kFillLightDirection(0.65f, -0.45f, 0.55f);
const vec3f kRimLightDirection(-0.55f, -0.2f, 0.75f);

vec3f normalizeDirection(const vec3f &v)
{
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len <= 1e-6f)
    return vec3f(0.f, -1.f, 0.f);
  return vec3f(v.x / len, v.y / len, v.z / len);
}

std::string trimCopy(const std::string &value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};

  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool ensureBrlcadModuleLoaded(std::string &errorOut)
{
  static bool attempted = false;
  static bool loaded = false;
  static std::string loadError;

  if (!attempted) {
    attempted = true;
    loaded = (ospLoadModule("brl_cad") == OSP_NO_ERROR);
    if (!loaded) {
      loadError =
          "Failed to load BRL-CAD OSPRay module 'brl_cad'. Ensure "
          "'ospray_module_brl_cad.dll' is deployed in the IBRT runtime folder "
          "(set BRLCAD_OSPRAY_MODULE_DLL in CMake if needed).";
    }
  }

  if (!loaded)
    errorOut = loadError;

  return loaded;
}

std::vector<ospray::cpp::Light> makeDefaultLights(const std::string &rendererType)
{
  std::vector<ospray::cpp::Light> lights;

  if (rendererType == "pathtracer") {
    // Path tracing needs actual illumination from a light or environment.
    ospray::cpp::Light sunSky("sunSky");
    sunSky.setParam("direction", kSunLightDirection);
    sunSky.setParam("intensity", 0.08f);
    sunSky.setParam("albedo", 0.2f);
    sunSky.setParam("turbidity", 5.0f);
    sunSky.setParam("visible", true);
    sunSky.commit();
    lights.push_back(sunSky);

    ospray::cpp::Light distant("distant");
    distant.setParam("direction", kSunLightDirection);
    distant.setParam("intensity", 1.8f);
    distant.setParam("visible", true);
    distant.setParam("angularDiameter", 1.8f);
    distant.commit();
    lights.push_back(distant);
  } else if (rendererType == "scivis") {
    ospray::cpp::Light ambient("ambient");
    ambient.setParam("intensity", 0.18f);
    ambient.commit();
    lights.push_back(ambient);

    ospray::cpp::Light key("distant");
    key.setParam("direction", kSunLightDirection);
    key.setParam("intensity", 2.2f);
    key.setParam("angularDiameter", 2.4f);
    key.commit();
    lights.push_back(key);

    ospray::cpp::Light fill("distant");
    fill.setParam("direction", kFillLightDirection);
    fill.setParam("intensity", 0.65f);
    fill.setParam("angularDiameter", 12.0f);
    fill.commit();
    lights.push_back(fill);

    ospray::cpp::Light rim("distant");
    rim.setParam("direction", kRimLightDirection);
    rim.setParam("intensity", 0.18f);
    rim.setParam("angularDiameter", 6.0f);
    rim.commit();
    lights.push_back(rim);
  } else {
    ospray::cpp::Light ambient("ambient");
    ambient.setParam("intensity", 0.05f);
    ambient.commit();
    lights.push_back(ambient);
  }

  return lights;
}
}

void OsprayBackend::init()
{
  try {
    renderer_ = ospray::cpp::Renderer("scivis");
    currentRenderer_ = "scivis";
    applyRendererDefaults();
    appliedAoSamples_ = configuredAoSamplesForCurrentMode();
    appliedPixelSamples_ = configuredPixelSamplesForCurrentMode();

    camera_ = ospray::cpp::Camera("perspective");
    camera_.setParam("fovy", 60.f);
    updateCameraCrop(vec2f(0.f, 0.f), vec2f(1.f, 1.f));
    camera_.commit();
    cameraDirty_ = false;

    loadTestMesh();
  } catch (const std::exception &e) {
    setError(e.what());
  } catch (...) {
    setError("Unknown failure while initializing OSPRay backend.");
  }
}

void OsprayBackend::resize(int w, int h)
{
  if (frameInFlight_) {
    pendingResizeW_ = std::max(1, w);
    pendingResizeH_ = std::max(1, h);
    pendingResize_ = true;
    enqueueLatestRenderRequest("resize");
    return;
  }

  fbW_ = std::max(1, w);
  fbH_ = std::max(1, h);

  camera_.setParam("aspect", float(fbW_) / float(fbH_));
  cameraDirty_ = true;

  // Full-resolution accumulation buffer (used once progressive scale reaches 1x).
  accumFb_ = ospray::cpp::FrameBuffer(
      fbW_, fbH_, OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
  displayPixels_.assign(size_t(fbW_) * size_t(fbH_), 0u);
  resetProgressiveState(true);
  enqueueLatestRenderRequest("resize");
}

void OsprayBackend::setCamera(const vec3f &eye, const vec3f &center, const vec3f &up, float fovyDeg)
{
  ++cameraVersion_;
  if (frameInFlight_) {
    if (isInteracting_ && activeRenderRequest_
        && activeRenderRequest_->type != RenderRequestType::Preview) {
      cancelInFlightFrame("camera_move_preempt");
    }
  }

  if (frameInFlight_) {
    pendingCameraState_ = PendingCameraState{eye, center, up, fovyDeg};
    enqueueLatestRenderRequest("camera");
    return;
  }

  camera_.setParam("position", eye);
  camera_.setParam("direction", center - eye);
  camera_.setParam("up", up);
  camera_.setParam("fovy", fovyDeg);
  cameraDirty_ = true;
  enqueueLatestRenderRequest("camera");
}

void OsprayBackend::resetAccumulation()
{
  if (frameInFlight_) {
    pendingResetAccumulation_ = true;
    enqueueLatestRenderRequest("resetAccumulation");
    return;
  }

  resetProgressiveState(false);
  enqueueLatestRenderRequest("resetAccumulation");
}

const uint32_t *OsprayBackend::pixels() const
{
  return displayPixels_.empty() ? nullptr : displayPixels_.data();
}

bool OsprayBackend::advanceRender(int timeBudgetMs)
{
  try {
    (void)timeBudgetMs;

    if (frameInFlight_) {
      if (!currentFrame_.handle()) {
        frameInFlight_ = false;
        inFlightStartValid_ = false;
        return false;
      }

      if (!currentFrame_.isReady(OSP_FRAME_FINISHED))
        return false;

      const bool updatedImage = finishCompletedRender();
      applyPendingState();
      return updatedImage;
    }

    if (!renderer_.handle() || !camera_.handle() || !world_.handle()
        || fbW_ <= 0 || fbH_ <= 0
        || displayPixels_.empty()) {
      return false;
    }

    applyPendingState();

    const bool accumulationEnabled = accumulationEnabledForCurrentMode();
    const int maxAccumulationFrames = maxAccumulationFramesForCurrentMode();
    const bool fullResAccumOnly = fullResAccumulationOnlyForCurrentMode();
    const bool lowQualityOnInteract = lowQualityWhileInteractingForCurrentMode();
    const int configuredAo = configuredAoSamplesForCurrentMode();
    const int configuredPixel = configuredPixelSamplesForCurrentMode();
    const bool fixedPreviewMode = isInteracting_ && lowQualityOnInteract;
    const int interactionAo = fixedPreviewMode ? 0 : configuredAo;
    const int interactionPixel = fixedPreviewMode ? 1 : configuredPixel;
    dynamicModeActive_ = (settingsMode_ == SettingsMode::Automatic);
    if (fixedPreviewMode) {
      renderPhase_ = RenderPhase::Progressive;
      setProgressiveScale(16);
    }
    const int backoffAo = std::max(0, interactionAo - aoBackoffSteps_);
    const int effectiveAoSamples = (passScale_ > 1) ? 0 : backoffAo;
    const int effectivePixelSamples =
        (passScale_ > 1) ? 1 : std::max(1, interactionPixel);
    applyRendererSamplingParams(effectiveAoSamples, effectivePixelSamples);

    if (!fixedPreviewMode && passScale_ <= 1 && accumFb_.handle() && accumulationEnabled) {
      if (maxAccumulationFrames > 0
          && accumulatedFrames_ >= uint64_t(maxAccumulationFrames)) {
        return false;
      }
      renderPhase_ = RenderPhase::Accumulate;
    } else {
      renderPhase_ = RenderPhase::Progressive;
    }

    assert(!frameInFlight_);
    startNextRenderWork();
    return false;
  } catch (const std::exception &e) {
    frameInFlight_ = false;
    setError(e.what());
    return false;
  } catch (...) {
    frameInFlight_ = false;
    setError("Unknown failure while advancing progressive render.");
    return false;
  }
}

float OsprayBackend::lastFrameTimeMs() const
{
  return lastFrameTimeMs_;
}

float OsprayBackend::renderFPS() const
{
  if (lastFrameTimeMs_ <= 0.0001f)
    return 0.0f;
  return 1000.0f / lastFrameTimeMs_;
}

rkcommon::math::vec3f OsprayBackend::getBoundsMin() const
{
  return boundsMin_;
}

rkcommon::math::vec3f OsprayBackend::getBoundsMax() const
{
  return boundsMax_;
}

float OsprayBackend::getBoundsMaxExtent() const
{
  float dx = boundsMax_.x - boundsMin_.x;
  float dy = boundsMax_.y - boundsMin_.y;
  float dz = boundsMax_.z - boundsMin_.z;
  return std::max(dx, std::max(dy, dz));
}

rkcommon::math::vec3f OsprayBackend::getBoundsCenter() const
{
  return rkcommon::math::vec3f(0.5f * (boundsMin_.x + boundsMax_.x),
      0.5f * (boundsMin_.y + boundsMax_.y),
      0.5f * (boundsMin_.z + boundsMax_.z));
}

float OsprayBackend::getBoundsRadius() const
{
  float dx = boundsMax_.x - boundsMin_.x;
  float dy = boundsMax_.y - boundsMin_.y;
  float dz = boundsMax_.z - boundsMin_.z;

  float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
  return std::max(0.5f * diag, 0.001f);
}

void OsprayBackend::loadTestMesh()
{
  std::vector<vec3f> vertex = {vec3f(-1.0f, -1.0f, 3.0f),
      vec3f(-1.0f, 1.0f, 3.0f),
      vec3f(1.0f, -1.0f, 3.0f),
      vec3f(0.1f, 0.1f, 0.3f)};

  std::vector<vec4f> color = {vec4f(0.9f, 0.5f, 0.5f, 1.0f),
      vec4f(0.8f, 0.8f, 0.8f, 1.0f),
      vec4f(0.8f, 0.8f, 0.8f, 1.0f),
      vec4f(0.5f, 0.9f, 0.5f, 1.0f)};

  std::vector<vec3ui> index = {vec3ui(0, 1, 2), vec3ui(1, 2, 3)};

  boundsMin_ = vertex[0];
  boundsMax_ = vertex[0];
  for (const auto &v : vertex) {
    boundsMin_.x = std::min(boundsMin_.x, v.x);
    boundsMin_.y = std::min(boundsMin_.y, v.y);
    boundsMin_.z = std::min(boundsMin_.z, v.z);
    boundsMax_.x = std::max(boundsMax_.x, v.x);
    boundsMax_.y = std::max(boundsMax_.y, v.y);
    boundsMax_.z = std::max(boundsMax_.z, v.z);
  }

  ospray::cpp::Geometry mesh("mesh");
  mesh.setParam("vertex.position", ospray::cpp::CopiedData(vertex));
  mesh.setParam("vertex.color", ospray::cpp::CopiedData(color));
  mesh.setParam("index", ospray::cpp::CopiedData(index));
  mesh.commit();

  ospray::cpp::GeometricModel model(mesh);
  model.commit();

  ospray::cpp::Group group;
  group.setParam("geometry", ospray::cpp::CopiedData(model));
  group.commit();

  ospray::cpp::Instance instance(group);
  instance.commit();

  sceneInstances_ = {instance};
  world_ = ospray::cpp::World();
  applyWorldInstances();
  applyDefaultLights();
  world_.commit();

  resetAccumulation();
}

bool OsprayBackend::loadObj(const std::string &path)
{
  if (frameInFlight_) {
    cancelInFlightFrame();
  }

  lastError_.clear();
  try {
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;

    if (!reader.ParseFromFile(path, config)) {
      setError("Could not parse OBJ file: " + path);
      return false;
    }

    if (!reader.Error().empty()) {
      setError(reader.Error());
      return false;
    }

    const auto &attrib = reader.GetAttrib();
    const auto &shapes = reader.GetShapes();

    std::vector<vec3f> vertices;
    std::vector<vec4f> colors;
    std::vector<vec3ui> indices;

    for (size_t v = 0; v < attrib.vertices.size() / 3; ++v) {
      vertices.emplace_back(attrib.vertices[3 * v + 0],
          attrib.vertices[3 * v + 1],
          attrib.vertices[3 * v + 2]);
      colors.emplace_back(0.8f, 0.8f, 0.8f, 1.0f);
    }

    for (const auto &shape : shapes) {
      size_t indexOffset = 0;
      for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
        int fv = shape.mesh.num_face_vertices[f];
        if (fv != 3) {
          indexOffset += fv;
          continue;
        }

        const auto &i0 = shape.mesh.indices[indexOffset + 0];
        const auto &i1 = shape.mesh.indices[indexOffset + 1];
        const auto &i2 = shape.mesh.indices[indexOffset + 2];

        if (i0.vertex_index < 0 || i1.vertex_index < 0
            || i2.vertex_index < 0) {
          indexOffset += fv;
          continue;
        }

        indices.emplace_back(static_cast<unsigned>(i0.vertex_index),
            static_cast<unsigned>(i1.vertex_index),
            static_cast<unsigned>(i2.vertex_index));

        indexOffset += fv;
      }
    }

    if (vertices.empty() || indices.empty()) {
      setError("OBJ file did not contain any triangulated mesh data.");
      return false;
    }

    boundsMin_ = vertices[0];
    boundsMax_ = vertices[0];
    for (const auto &v : vertices) {
      boundsMin_.x = std::min(boundsMin_.x, v.x);
      boundsMin_.y = std::min(boundsMin_.y, v.y);
      boundsMin_.z = std::min(boundsMin_.z, v.z);
      boundsMax_.x = std::max(boundsMax_.x, v.x);
      boundsMax_.y = std::max(boundsMax_.y, v.y);
      boundsMax_.z = std::max(boundsMax_.z, v.z);
    }

    ospray::cpp::Geometry mesh("mesh");
    mesh.setParam("vertex.position", ospray::cpp::CopiedData(vertices));
    mesh.setParam("vertex.color", ospray::cpp::CopiedData(colors));
    mesh.setParam("index", ospray::cpp::CopiedData(indices));
    mesh.commit();

    ospray::cpp::GeometricModel model(mesh);
    model.commit();

    ospray::cpp::Group group;
    group.setParam("geometry", ospray::cpp::CopiedData(model));
    group.commit();

    ospray::cpp::Instance instance(group);
    instance.commit();

    sceneInstances_ = {instance};
    world_ = ospray::cpp::World();
    applyWorldInstances();
    applyDefaultLights();
    world_.commit();

    resetAccumulation();
    return true;
  } catch (const std::exception &e) {
    setError(e.what());
    return false;
  } catch (...) {
    setError("Unknown failure while loading OBJ.");
    return false;
  }
}


bool OsprayBackend::loadBrlcad(
    const std::string &path, const std::string &topObject)
{
  if (frameInFlight_) {
    cancelInFlightFrame();
  }

  lastError_.clear();
  try {
  std::string moduleError;
  if (!ensureBrlcadModuleLoaded(moduleError)) {
    setError(moduleError);
    return false;
  }

  fprintf(stderr, "loadBrlcad: START\n");
  fprintf(stderr, "loadBrlcad: path = %s\n", path.c_str());
  fprintf(stderr, "loadBrlcad: object = %s\n", topObject.c_str());

  // STEP 1: Create geometry
  fprintf(stderr, "STEP 1: Creating OSPRay brlcad geometry\n");

  OSPGeometry rawGeom = ospNewGeometry("brlcad");
  fprintf(stderr, "geom handle = %p\n", (void *)rawGeom);
  fflush(stderr);

  if (!rawGeom) {
    setError("OSPRay could not create geometry type 'brlcad'. "
             "The BRL-CAD module loaded, but the active device did not create the custom geometry.");
    fprintf(stderr, "ERROR: %s\n", lastError_.c_str());
    return false;
  }

  ospray::cpp::Geometry geom(rawGeom);

  fprintf(stderr, "STEP 2: Setting filename param\n");
  geom.setParam("filename", path);

  if (!topObject.empty()) {
    fprintf(stderr, "STEP 3: Setting objects param\n");
    geom.setParam("objects", topObject);
  }

  fprintf(stderr, "STEP 4: Committing geometry\n");
  geom.commit(); // 🔥 VERY LIKELY CRASH POINT
  fprintf(stderr, "STEP 4 DONE\n");

  // STEP 5: Bounds calculation
  fprintf(stderr, "STEP 5: Default bounds\n");
  boundsMin_ = vec3f(-1.f, -1.f, -1.f);
  boundsMax_ = vec3f(1.f, 1.f, 1.f);

  fprintf(stderr, "STEP 11: Creating GeometricModel\n");
  ospray::cpp::GeometricModel gmodel(geom);
  applyDefaultMaterial(gmodel);
  gmodel.commit();

  fprintf(stderr, "STEP 12: Creating Group\n");
  ospray::cpp::Group group;

  std::vector<ospray::cpp::GeometricModel> models = {gmodel};
  group.setParam("geometry", ospray::cpp::CopiedData(models));
  fprintf(stderr, "STEP 12: Creating Group - set param\n");
  group.commit();
  fprintf(stderr, "STEP 12: Creating Group - commit\n");


  fprintf(stderr, "STEP 13: Creating Instance\n");
  ospray::cpp::Instance instance(group);
  instance.commit();

  fprintf(stderr, "STEP 14: Creating World\n");
  sceneInstances_ = {instance};
  world_ = ospray::cpp::World();
  applyWorldInstances();

  fprintf(stderr, "STEP 15: Adding light\n");
  applyDefaultLights();

  fprintf(stderr, "STEP 16: Commit world\n");
  world_.commit();

  fprintf(stderr, "STEP 16B: Reading world bounds\n");
  const OSPBounds worldBounds = ospGetBounds(instance.handle());
  if (std::isfinite(worldBounds.lower[0]) && std::isfinite(worldBounds.lower[1])
      && std::isfinite(worldBounds.lower[2]) && std::isfinite(worldBounds.upper[0])
      && std::isfinite(worldBounds.upper[1]) && std::isfinite(worldBounds.upper[2])) {
    boundsMin_ =
        vec3f(worldBounds.lower[0], worldBounds.lower[1], worldBounds.lower[2]);
    boundsMax_ =
        vec3f(worldBounds.upper[0], worldBounds.upper[1], worldBounds.upper[2]);
    fprintf(stderr,
        "STEP 16B DONE: min=(%f,%f,%f) max=(%f,%f,%f)\n",
        boundsMin_.x,
        boundsMin_.y,
        boundsMin_.z,
        boundsMax_.x,
        boundsMax_.y,
        boundsMax_.z);
  } else {
    fprintf(stderr, "STEP 16B: world bounds unavailable, using defaults\n");
  }

  fprintf(stderr, "STEP 17: Reset accumulation\n");
  resetAccumulation();

  fprintf(stderr, "loadBrlcad: SUCCESS\n");

  return true;
  } catch (const std::exception &e) {
    setError(e.what());
    return false;
  } catch (...) {
    setError("Unknown failure while loading BRL-CAD geometry.");
    return false;
  }
}

void OsprayBackend::setRenderer(const std::string &type)
{
  if (frameInFlight_) {
    pendingRendererType_ = type;
    pendingResetAccumulation_ = true;
    enqueueLatestRenderRequest("renderer");
    return;
  }

  try {
    renderer_ = ospray::cpp::Renderer(type);
    currentRenderer_ = type;
    applyRendererDefaults();
    appliedAoSamples_ = configuredAoSamplesForCurrentMode();
    appliedPixelSamples_ = configuredPixelSamplesForCurrentMode();

    if (world_.handle()) {
      applyWorldInstances();
      applyDefaultLights();
      world_.commit();
    }

    resetAccumulation();
    enqueueLatestRenderRequest("renderer");
  } catch (const std::exception &e) {
    setError(e.what());
  } catch (...) {
    setError("Unknown failure while changing renderer.");
  }
}

const std::string &OsprayBackend::currentRenderer() const
{
  return currentRenderer_;
}

void OsprayBackend::setAoSamples(int samples)
{
  if (frameInFlight_) {
    setError("AO sample update ignored while render is in flight.");
    return;
  }

  const int clamped = std::clamp(samples, 0, kMaxSafeAoSamples);
  if (customAoSamples_ == clamped)
    return;

  customAoSamples_ = clamped;
  resetAccumulation();
}

void OsprayBackend::setPixelSamples(int samples)
{
  if (frameInFlight_) {
    setError("Pixel sample update ignored while render is in flight.");
    return;
  }

  const int clamped = std::clamp(samples, 1, kMaxSafePixelSamples);
  if (customPixelSamples_ == clamped)
    return;

  customPixelSamples_ = clamped;
  resetAccumulation();
}

void OsprayBackend::setSettingsMode(SettingsMode mode)
{
  if (settingsMode_ == mode)
    return;
  settingsMode_ = mode;
  resetAccumulation();
}

OsprayBackend::SettingsMode OsprayBackend::settingsMode() const
{
  return settingsMode_;
}

void OsprayBackend::setAutomaticPreset(AutomaticPreset preset)
{
  if (automaticPreset_ == preset)
    return;
  automaticPreset_ = preset;
  resetAccumulation();
}

OsprayBackend::AutomaticPreset OsprayBackend::automaticPreset() const
{
  return automaticPreset_;
}

void OsprayBackend::setAutomaticTargetFrameTimeMs(float ms)
{
  const float clamped = std::clamp(ms, 2.0f, 1000.0f);
  if (std::fabs(automaticTargetFrameTimeMs_ - clamped) < 0.001f)
    return;
  automaticTargetFrameTimeMs_ = clamped;
  resetAccumulation();
}

float OsprayBackend::automaticTargetFrameTimeMs() const
{
  return automaticTargetFrameTimeMs_;
}

void OsprayBackend::setAutomaticAccumulationEnabled(bool enabled)
{
  if (automaticAccumulationEnabled_ == enabled)
    return;
  automaticAccumulationEnabled_ = enabled;
  resetAccumulation();
}

bool OsprayBackend::automaticAccumulationEnabled() const
{
  return automaticAccumulationEnabled_;
}

void OsprayBackend::setCustomStartScale(int scale)
{
  const int sanitized = sanitizeScale(scale);
  if (customStartScale_ == sanitized)
    return;
  customStartScale_ = sanitized;
  resetAccumulation();
}

int OsprayBackend::customStartScale() const
{
  return customStartScale_;
}

void OsprayBackend::setCustomTargetFrameTimeMs(float ms)
{
  const float clamped = std::clamp(ms, 2.0f, 1000.0f);
  if (std::fabs(customTargetFrameTimeMs_ - clamped) < 0.001f)
    return;
  customTargetFrameTimeMs_ = clamped;
  resetAccumulation();
}

float OsprayBackend::customTargetFrameTimeMs() const
{
  return customTargetFrameTimeMs_;
}

int OsprayBackend::customAoSamples() const
{
  return customAoSamples_;
}

int OsprayBackend::customPixelSamples() const
{
  return customPixelSamples_;
}

void OsprayBackend::setCustomAccumulationEnabled(bool enabled)
{
  if (customAccumulationEnabled_ == enabled)
    return;
  customAccumulationEnabled_ = enabled;
  resetAccumulation();
}

bool OsprayBackend::customAccumulationEnabled() const
{
  return customAccumulationEnabled_;
}

void OsprayBackend::setCustomMaxAccumulationFrames(int frames)
{
  const int clamped = std::clamp(frames, 0, 1000000);
  if (customMaxAccumulationFrames_ == clamped)
    return;
  customMaxAccumulationFrames_ = clamped;
  resetAccumulation();
}

int OsprayBackend::customMaxAccumulationFrames() const
{
  return customMaxAccumulationFrames_;
}

void OsprayBackend::setCustomLowQualityWhileInteracting(bool enabled)
{
  if (customLowQualityWhileInteracting_ == enabled)
    return;
  customLowQualityWhileInteracting_ = enabled;
  resetAccumulation();
}

bool OsprayBackend::customLowQualityWhileInteracting() const
{
  return customLowQualityWhileInteracting_;
}

void OsprayBackend::setCustomFullResAccumulationOnly(bool enabled)
{
  if (customFullResAccumulationOnly_ == enabled)
    return;
  customFullResAccumulationOnly_ = enabled;
  resetAccumulation();
}

bool OsprayBackend::customFullResAccumulationOnly() const
{
  return customFullResAccumulationOnly_;
}

void OsprayBackend::setCustomWatchdogTimeoutMs(int ms)
{
  const int clamped = std::clamp(ms, 10, 60000);
  if (customWatchdogTimeoutMs_ == clamped)
    return;
  customWatchdogTimeoutMs_ = clamped;
  resetAccumulation();
}

int OsprayBackend::customWatchdogTimeoutMs() const
{
  return customWatchdogTimeoutMs_;
}

void OsprayBackend::setInteracting(bool interacting)
{
  if (isInteracting_ == interacting)
    return;
  isInteracting_ = interacting;
  enqueueLatestRenderRequest(interacting ? "interaction.begin" : "interaction.end");
  resetAccumulation();
}

int OsprayBackend::currentScale() const
{
  return passScale_;
}

bool OsprayBackend::dynamicModeActive() const
{
  return dynamicModeActive_;
}

bool OsprayBackend::backoffApplied() const
{
  return backoffApplied_;
}

void OsprayBackend::applyRendererSamplingParams(int aoSamples, int pixelSamples)
{
  const int clampedAo = std::clamp(aoSamples, 0, kMaxSafeAoSamples);
  const int clampedPixel = std::clamp(pixelSamples, 1, kMaxSafePixelSamples);
  if (clampedAo == appliedAoSamples_ && clampedPixel == appliedPixelSamples_)
    return;

  renderer_.setParam("aoSamples", clampedAo);
  renderer_.setParam("pixelSamples", clampedPixel);
  renderer_.commit();
  appliedAoSamples_ = clampedAo;
  appliedPixelSamples_ = clampedPixel;
}

int OsprayBackend::sanitizeScale(int scale) const
{
  int bestScale = kProgressiveScales.front();
  int bestDistance = std::abs(scale - bestScale);
  for (const int candidate : kProgressiveScales) {
    const int distance = std::abs(scale - candidate);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestScale = candidate;
    }
  }
  return bestScale;
}

int OsprayBackend::scaleToIndex(int scale) const
{
  const int sanitized = sanitizeScale(scale);
  for (size_t i = 0; i < kProgressiveScales.size(); ++i) {
    if (kProgressiveScales[i] == sanitized)
      return int(i);
  }
  return int(kProgressiveScales.size()) - 1;
}

int OsprayBackend::startScaleForCurrentMode() const
{
  if (settingsMode_ == SettingsMode::Custom)
    return customStartScale_;

  switch (automaticPreset_) {
  case AutomaticPreset::Fast:
    return 16;
  case AutomaticPreset::Balanced:
    return 8;
  case AutomaticPreset::Quality:
    return 4;
  }
  return 8;
}

float OsprayBackend::targetFrameTimeForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom) ? customTargetFrameTimeMs_
                                                 : automaticTargetFrameTimeMs_;
}

bool OsprayBackend::accumulationEnabledForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom) ? customAccumulationEnabled_
                                                 : automaticAccumulationEnabled_;
}

int OsprayBackend::maxAccumulationFramesForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom) ? customMaxAccumulationFrames_ : 0;
}

int OsprayBackend::watchdogTimeoutForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom) ? customWatchdogTimeoutMs_
                                                 : kDefaultWatchdogMs;
}

int OsprayBackend::configuredAoSamplesForCurrentMode() const
{
  if (settingsMode_ == SettingsMode::Custom)
    return customAoSamples_;

  switch (automaticPreset_) {
  case AutomaticPreset::Fast:
    return 0;
  case AutomaticPreset::Balanced:
    return 1;
  case AutomaticPreset::Quality:
    return 2;
  }
  return 1;
}

int OsprayBackend::configuredPixelSamplesForCurrentMode() const
{
  if (settingsMode_ == SettingsMode::Custom)
    return customPixelSamples_;

  switch (automaticPreset_) {
  case AutomaticPreset::Fast:
    return 1;
  case AutomaticPreset::Balanced:
    return 1;
  case AutomaticPreset::Quality:
    return 2;
  }
  return 1;
}

bool OsprayBackend::fullResAccumulationOnlyForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom) ? customFullResAccumulationOnly_
                                                 : true;
}

bool OsprayBackend::lowQualityWhileInteractingForCurrentMode() const
{
  return (settingsMode_ == SettingsMode::Custom)
      ? customLowQualityWhileInteracting_
      : true;
}

void OsprayBackend::cancelInFlightFrame(const char *reason)
{
  if (frameInFlight_) {
    if (activeRenderRequest_)
      logRenderRequest("cancel", *activeRenderRequest_, reason);
    if (currentFrame_.handle())
      currentFrame_.cancel();
  }

  frameInFlight_ = false;
  currentFrame_ = ospray::cpp::Future();
  inFlightStartValid_ = false;
  activeRenderRequest_.reset();
}

OsprayBackend::RenderRequestType OsprayBackend::currentRenderRequestType() const
{
  if (isInteracting_)
    return RenderRequestType::Preview;
  return renderPhase_ == RenderPhase::Accumulate ? RenderRequestType::Full
                                                 : RenderRequestType::Progressive;
}

const char *OsprayBackend::renderRequestTypeName(RenderRequestType type) const
{
  switch (type) {
  case RenderRequestType::Preview:
    return "preview";
  case RenderRequestType::Progressive:
    return "progressive";
  case RenderRequestType::Full:
    return "full";
  }
  return "unknown";
}

void OsprayBackend::logRenderRequest(const char *event,
    const RenderRequest &request,
    const char *reason) const
{
  std::fprintf(stderr,
      "IBRT render %s: id=%llu type=%s camera=%llu phase=%s scale=%d%s%s\n",
      event,
      static_cast<unsigned long long>(request.id),
      renderRequestTypeName(request.type),
      static_cast<unsigned long long>(request.cameraVersion),
      renderPhase_ == RenderPhase::Accumulate ? "accumulate" : "progressive",
      passScale_,
      reason ? " reason=" : "",
      reason ? reason : "");
}

void OsprayBackend::enqueueLatestRenderRequest(const char *reason)
{
  RenderRequest request;
  request.id = nextRenderRequestId_++;
  request.cameraVersion = cameraVersion_;
  request.type = isInteracting_ ? RenderRequestType::Preview
                                : RenderRequestType::Progressive;
  pendingRenderRequest_ = request;
  logRenderRequest("request", request, reason);
}

void OsprayBackend::setProgressiveScale(int scale)
{
  renderPhase_ = RenderPhase::Progressive;
  currentScaleIndex_ = scaleToIndex(scale);
  progressiveFramesAtCurrentScale_ = 0;
  passScale_ = kProgressiveScales[currentScaleIndex_];
  const int targetW = std::max(1, (fbW_ + passScale_ - 1) / passScale_);
  const int targetH = std::max(1, (fbH_ + passScale_ - 1) / passScale_);
  if (targetW != passW_ || targetH != passH_) {
    passW_ = targetW;
    passH_ = targetH;
    passPixels_.assign(size_t(passW_) * size_t(passH_), 0u);
    passFb_ = ospray::cpp::FrameBuffer();
  } else if (passPixels_.size() != size_t(passW_) * size_t(passH_)) {
    passPixels_.assign(size_t(passW_) * size_t(passH_), 0u);
    passFb_ = ospray::cpp::FrameBuffer();
  }
}

void OsprayBackend::applyDefaultLights()
{
  world_.setParam("light",
      ospray::cpp::CopiedData(makeDefaultLights(currentRenderer_)));
}

void OsprayBackend::applyRendererDefaults()
{
  renderer_.setParam("backgroundColor", 1.0f);
  renderer_.setParam("pixelSamples", configuredPixelSamplesForCurrentMode());
  renderer_.setParam("aoSamples", configuredAoSamplesForCurrentMode());

  if (currentRenderer_ == "scivis") {
    renderer_.setParam("shadows", true);
    renderer_.setParam("visibleLights", false);
  } else if (currentRenderer_ == "ao") {
    renderer_.setParam("aoIntensity", 1.0f);
    renderer_.setParam("lightDirection", normalizeDirection(-kSunLightDirection));
    renderer_.setParam("ambientIntensity", 0.18f);
    renderer_.setParam("directionalIntensity", 0.82f);
  }

  renderer_.commit();
}

void OsprayBackend::applyDefaultMaterial(ospray::cpp::GeometricModel &model)
{
  ospray::cpp::Material material("obj");
  material.setParam("kd", vec3f(0.8f, 0.8f, 0.8f));
  material.commit();
  model.setParam("material", material);
}

void OsprayBackend::applyWorldInstances()
{
  world_.setParam("instance", ospray::cpp::CopiedData(sceneInstances_));
}

void OsprayBackend::resetProgressiveState(bool clearDisplay)
{
  setProgressiveScale(startScaleForCurrentMode());
  slowPassStreak_ = 0;
  accumulatedFrames_ = 0;
  lastFrameTimeMs_ = 0.0f;
  slowFrameStreak_ = 0;
  aoBackoffSteps_ = 0;
  backoffApplied_ = false;
  watchdogTriggered_ = false;
  progressiveFramesAtCurrentScale_ = 0;

  if (clearDisplay)
    std::fill(displayPixels_.begin(), displayPixels_.end(), 0u);
  if (accumFb_.handle())
    accumFb_.resetAccumulation();
}

void OsprayBackend::updateCameraCrop(const vec2f &imageStart, const vec2f &imageEnd)
{
  camera_.setParam("imageStart", imageStart);
  camera_.setParam("imageEnd", imageEnd);
}

void OsprayBackend::beginNextProgressivePass()
{
  const int lastIndex = int(kProgressiveScales.size()) - 1;
  currentScaleIndex_ = std::min(currentScaleIndex_ + 1, lastIndex);
  progressiveFramesAtCurrentScale_ = 0;
  if (currentScaleIndex_ >= lastIndex)
    renderPhase_ = RenderPhase::Accumulate;

  passScale_ = kProgressiveScales[currentScaleIndex_];
  const int targetW = std::max(1, (fbW_ + passScale_ - 1) / passScale_);
  const int targetH = std::max(1, (fbH_ + passScale_ - 1) / passScale_);
  if (targetW != passW_ || targetH != passH_) {
    passW_ = targetW;
    passH_ = targetH;
    passPixels_.assign(size_t(passW_) * size_t(passH_), 0u);
    passFb_ = ospray::cpp::FrameBuffer();
  } else if (passPixels_.size() != size_t(passW_) * size_t(passH_)) {
    passPixels_.assign(size_t(passW_) * size_t(passH_), 0u);
    passFb_ = ospray::cpp::FrameBuffer();
  }
}

void OsprayBackend::prepareTileFrameBuffer(int tileW, int tileH)
{
  if (!passFb_.handle() || tileW != passW_ || tileH != passH_)
    passFb_ = ospray::cpp::FrameBuffer(tileW, tileH, OSP_FB_SRGBA, OSP_FB_COLOR);
}

bool OsprayBackend::startNextRenderWork()
{
  RenderRequest request = pendingRenderRequest_.value_or(RenderRequest{
      nextRenderRequestId_++, cameraVersion_, currentRenderRequestType()});
  request.type = currentRenderRequestType();
  request.cameraVersion = cameraVersion_;

  if (renderPhase_ == RenderPhase::Accumulate) {
    if (!accumFb_.handle())
      return false;
    activeRenderRequest_ = request;
    pendingRenderRequest_.reset();
    logRenderRequest("start", request);
    updateCameraCrop(vec2f(0.f, 0.f), vec2f(1.f, 1.f));
    camera_.commit();
    currentFrame_ = accumFb_.renderFrame(renderer_, camera_, world_);
    frameInFlight_ = true;
    inFlightStart_ = std::chrono::steady_clock::now();
    inFlightStartValid_ = true;
    return true;
  }

  prepareTileFrameBuffer(passW_, passH_);
  activeRenderRequest_ = request;
  pendingRenderRequest_.reset();
  logRenderRequest("start", request);
  updateCameraCrop(vec2f(0.f, 0.f), vec2f(1.f, 1.f));
  camera_.commit();
  currentFrame_ = passFb_.renderFrame(renderer_, camera_, world_);
  frameInFlight_ = true;
  inFlightStart_ = std::chrono::steady_clock::now();
  inFlightStartValid_ = true;
  return true;
}

bool OsprayBackend::finishCompletedRender()
{
  if (!frameInFlight_ || !currentFrame_.handle())
    return false;

  currentFrame_.wait(OSP_FRAME_FINISHED);
  lastFrameTimeMs_ = currentFrame_.duration() * 1000.0f;
  watchdogTriggered_ =
      (lastFrameTimeMs_ >= float(watchdogTimeoutForCurrentMode()));
  if (watchdogTriggered_)
    ++watchdogCancelCount_;

  bool updatedImage = false;

  if (renderPhase_ == RenderPhase::Progressive) {
    void *mapped = passFb_.map(OSP_FB_COLOR);
    std::memcpy(passPixels_.data(),
        mapped,
        size_t(passW_) * size_t(passH_) * sizeof(uint32_t));
    passFb_.unmap(mapped);

    upsamplePassToDisplay();
    ++accumulatedFrames_;
    updatedImage = true;
    if (!isInteracting_)
      beginNextProgressivePass();
  } else {
    void *mapped = accumFb_.map(OSP_FB_COLOR);
    std::memcpy(displayPixels_.data(),
        mapped,
        displayPixels_.size() * sizeof(uint32_t));
    accumFb_.unmap(mapped);
    ++accumulatedFrames_;
    updatedImage = true;
  }

  frameInFlight_ = false;
  inFlightStartValid_ = false;
  currentFrame_ = ospray::cpp::Future();
  applyAoBackoff(watchdogTriggered_);
  if (activeRenderRequest_) {
    char reason[64];
    std::snprintf(reason, sizeof(reason), "frameMs=%.2f", lastFrameTimeMs_);
    logRenderRequest("finish", *activeRenderRequest_, reason);
  }
  activeRenderRequest_.reset();
  return updatedImage;
}

void OsprayBackend::applyPendingState()
{
  if (frameInFlight_)
    return;

  if (pendingResize_) {
    fbW_ = std::max(1, pendingResizeW_);
    fbH_ = std::max(1, pendingResizeH_);
    camera_.setParam("aspect", float(fbW_) / float(fbH_));
    cameraDirty_ = true;
    accumFb_ = ospray::cpp::FrameBuffer(
        fbW_, fbH_, OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
    displayPixels_.assign(size_t(fbW_) * size_t(fbH_), 0u);
    resetProgressiveState(true);
    pendingResize_ = false;
  }

  if (pendingRendererType_) {
    renderer_ = ospray::cpp::Renderer(*pendingRendererType_);
    currentRenderer_ = *pendingRendererType_;
    applyRendererDefaults();
    appliedAoSamples_ = configuredAoSamplesForCurrentMode();
    appliedPixelSamples_ = configuredPixelSamplesForCurrentMode();
    if (world_.handle()) {
      applyWorldInstances();
      applyDefaultLights();
      world_.commit();
    }
    pendingRendererType_.reset();
    pendingResetAccumulation_ = true;
  }

  if (pendingCameraState_) {
    camera_.setParam("position", pendingCameraState_->eye);
    camera_.setParam("direction", pendingCameraState_->center - pendingCameraState_->eye);
    camera_.setParam("up", pendingCameraState_->up);
    camera_.setParam("fovy", pendingCameraState_->fovyDeg);
    cameraDirty_ = true;
    pendingCameraState_.reset();
    if (!isInteracting_)
      pendingResetAccumulation_ = true;
  }

  if (pendingResetAccumulation_) {
    resetProgressiveState(false);
    pendingResetAccumulation_ = false;
  }
}

void OsprayBackend::upsamplePassToDisplay()
{
  if (passPixels_.empty() || displayPixels_.empty())
    return;

  for (int y = 0; y < fbH_; ++y) {
    const int srcY = std::min(passH_ - 1, y / passScale_);
    uint32_t *dstRow = displayPixels_.data() + size_t(y) * size_t(fbW_);
    const uint32_t *srcRow = passPixels_.data() + size_t(srcY) * size_t(passW_);

    for (int x = 0; x < fbW_; ++x)
      dstRow[x] = srcRow[std::min(passW_ - 1, x / passScale_)];
  }
}

void OsprayBackend::applyAoBackoff(bool forcedByWatchdog)
{
  const int configuredAo = configuredAoSamplesForCurrentMode();
  if (configuredAo <= 0)
    return;

  const float frameThreshold = std::max(30.0f, targetFrameTimeForCurrentMode() * 1.8f);

  if (forcedByWatchdog) {
    slowFrameStreak_ = kAoBackoffStreak;
  } else if (lastFrameTimeMs_ > frameThreshold) {
    ++slowFrameStreak_;
  } else {
    slowFrameStreak_ = 0;
    return;
  }

  if (slowFrameStreak_ < kAoBackoffStreak)
    return;

  if (aoBackoffSteps_ >= configuredAo) {
    slowFrameStreak_ = 0;
    return;
  }

  ++aoBackoffSteps_;
  backoffApplied_ = true;
  ++aoAutoReductionCount_;
  slowFrameStreak_ = 0;
}

int& OsprayBackend::getAoSamples()
{
  return customAoSamples_;
}

const std::string &OsprayBackend::lastError() const
{
  return lastError_;
}

void OsprayBackend::setError(std::string message)
{
  lastError_ = std::move(message);
}

uint64_t OsprayBackend::accumulatedFrames() const
{
  return accumulatedFrames_;
}

uint64_t OsprayBackend::watchdogCancelCount() const
{
  return watchdogCancelCount_;
}

uint64_t OsprayBackend::aoAutoReductionCount() const
{
  return aoAutoReductionCount_;
}

std::vector<std::string> OsprayBackend::listBrlcadObjects(
    const std::string &path) const
{
  std::vector<std::string> names;
  if (path.empty())
    return names;

  rt_i *tmpRtip = rt_dirbuild(path.c_str(), nullptr, 0);
  if (!tmpRtip || !tmpRtip->rti_dbip)
    return names;

  directory **dpv = nullptr;
  const size_t count =
      db_ls(tmpRtip->rti_dbip, DB_LS_TOPS | DB_LS_COMB | DB_LS_REGION, nullptr, &dpv);

  for (size_t i = 0; i < count; ++i) {
    if (dpv[i] && dpv[i]->d_namep && *dpv[i]->d_namep)
      names.emplace_back(dpv[i]->d_namep);
  }

  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  if (dpv)
    bu_free(dpv, "db_ls object list");
  rt_free_rti(tmpRtip);
  return names;
}

std::vector<OsprayBackend::BrlcadNode> OsprayBackend::listBrlcadHierarchy(
    const std::string &path) const
{
  std::vector<BrlcadNode> roots;
  if (path.empty())
    return roots;

  rt_i *tmpRtip = rt_dirbuild(path.c_str(), nullptr, 0);
  if (!tmpRtip || !tmpRtip->rti_dbip)
    return roots;

  struct resource localResource;
  rt_init_resource(&localResource, 0, tmpRtip);

  struct CleanupGuard
  {
    rt_i *rtip = nullptr;
    directory **dpv = nullptr;
    resource *resp = nullptr;
    ~CleanupGuard()
    {
      if (dpv)
        bu_free(dpv, "db_ls hierarchy roots");
      if (rtip && resp)
        rt_clean_resource(rtip, resp);
      if (rtip)
        rt_free_rti(rtip);
    }
  } cleanup{tmpRtip, nullptr, &localResource};

  directory **dpv = nullptr;
  cleanup.dpv = dpv;
  const size_t count =
      db_ls(tmpRtip->rti_dbip, DB_LS_TOPS | DB_LS_COMB | DB_LS_REGION, nullptr, &dpv);
  cleanup.dpv = dpv;

  std::function<BrlcadNode(const directory *, std::unordered_set<std::string> &)> buildDirectoryNode;
  std::function<void(const union tree *, BrlcadNode &, std::unordered_set<std::string> &)> appendTreeChildren;

  buildDirectoryNode = [&](const directory *dp,
                           std::unordered_set<std::string> &ancestry) -> BrlcadNode {
    BrlcadNode node;
    if (!dp || !dp->d_namep)
      return node;

    node.name = dp->d_namep;

    struct rt_db_internal intern;
    RT_DB_INTERNAL_INIT(&intern);
    if (rt_db_get_internal(&intern, dp, tmpRtip->rti_dbip, nullptr, &localResource) < 0) {
      node.isPrimitive = true;
      return node;
    }

    const bool isCombination = intern.idb_type == ID_COMBINATION;
    node.isCombination = isCombination;
    node.isPrimitive = !isCombination;

    if (isCombination) {
      const auto *comb = static_cast<const rt_comb_internal *>(intern.idb_ptr);
      if (comb) {
        node.isRegion = comb->region_flag != 0;
        if (ancestry.insert(node.name).second) {
          appendTreeChildren(comb->tree, node, ancestry);
          ancestry.erase(node.name);
        }
      }
    }

    rt_db_free_internal(&intern);
    return node;
  };

  appendTreeChildren = [&](const union tree *tree,
                           BrlcadNode &parent,
                           std::unordered_set<std::string> &ancestry) {
    if (!tree)
      return;

    switch (tree->tr_op) {
    case OP_DB_LEAF: {
      if (!tree->tr_l.tl_name || !*tree->tr_l.tl_name)
        return;
      const directory *leafDp = db_lookup(tmpRtip->rti_dbip, tree->tr_l.tl_name, LOOKUP_QUIET);
      if (!leafDp)
        return;
      parent.children.push_back(buildDirectoryNode(leafDp, ancestry));
      return;
    }
    case OP_UNION:
    case OP_INTERSECT:
    case OP_SUBTRACT:
    case OP_XOR:
      appendTreeChildren(tree->tr_b.tb_left, parent, ancestry);
      appendTreeChildren(tree->tr_b.tb_right, parent, ancestry);
      return;
    case OP_NOT:
    case OP_GUARD:
    case OP_XNOP:
      appendTreeChildren(tree->tr_b.tb_left, parent, ancestry);
      return;
    default:
      return;
    }
  };

  for (size_t i = 0; i < count; ++i) {
    if (!dpv[i] || !dpv[i]->d_namep || !*dpv[i]->d_namep)
      continue;
    std::unordered_set<std::string> ancestry;
    roots.push_back(buildDirectoryNode(dpv[i], ancestry));
  }

  std::sort(roots.begin(), roots.end(), [](const BrlcadNode &a, const BrlcadNode &b) {
    return a.name < b.name;
  });

  return roots;
}
