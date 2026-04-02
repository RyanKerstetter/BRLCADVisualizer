#include "ospraybackend.h"
#include <chrono>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

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
}

void OsprayBackend::init()
{
  try {
    renderer_ = ospray::cpp::Renderer("scivis");
    currentRenderer_ = "scivis";
    renderer_.setParam("aoSamples", configuredAoSamplesForCurrentMode());
    renderer_.setParam("pixelSamples", configuredPixelSamplesForCurrentMode());
    renderer_.setParam("backgroundColor", 1.0f);
    renderer_.commit();
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
    setError("Resize ignored while render is in flight.");
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
}

void OsprayBackend::setCamera(const vec3f &eye, const vec3f &center, const vec3f &up, float fovyDeg)
{
  if (frameInFlight_) {
    setError("Camera update ignored while render is in flight.");
    return;
  }

  camera_.setParam("position", eye);
  camera_.setParam("direction", center - eye);
  camera_.setParam("up", up);
  camera_.setParam("fovy", fovyDeg);
  cameraDirty_ = true;
}

void OsprayBackend::resetAccumulation()
{
  if (frameInFlight_) {
    setError("Reset ignored while render is in flight.");
    return;
  }

  resetProgressiveState(false);
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
      return false;
    }

    if (!renderer_.handle() || !camera_.handle() || !world_.handle()
        || fbW_ <= 0 || fbH_ <= 0
        || displayPixels_.empty()) {
      return false;
    }

    const bool accumulationEnabled = accumulationEnabledForCurrentMode();
    const int maxAccumulationFrames = maxAccumulationFramesForCurrentMode();
    const bool fullResAccumOnly = fullResAccumulationOnlyForCurrentMode();
    const bool lowQualityOnInteract = lowQualityWhileInteractingForCurrentMode();
    const int watchdogTimeoutMs = watchdogTimeoutForCurrentMode();
    const int configuredAo = configuredAoSamplesForCurrentMode();
    const int configuredPixel = configuredPixelSamplesForCurrentMode();
    const int interactionAo = (isInteracting_ && lowQualityOnInteract) ? 0 : configuredAo;
    const int interactionPixel =
        (isInteracting_ && lowQualityOnInteract) ? 1 : configuredPixel;
    dynamicModeActive_ = (settingsMode_ == SettingsMode::Automatic);
    assert(!frameInFlight_);
    frameInFlight_ = true;
    inFlightStartValid_ = false;
    currentFrame_ = ospray::cpp::Future();

    if (cameraDirty_) {
      updateCameraCrop(vec2f(0.f, 0.f), vec2f(1.f, 1.f));
      camera_.commit();
      cameraDirty_ = false;
    }

    const auto passStart = std::chrono::steady_clock::now();
    const int backoffAo = std::max(0, interactionAo - aoBackoffSteps_);
    const int effectiveAoSamples = (passScale_ > 1) ? 0 : backoffAo;
    const int effectivePixelSamples =
        (passScale_ > 1) ? 1 : std::max(1, interactionPixel);
    applyRendererSamplingParams(effectiveAoSamples, effectivePixelSamples);

    if (passScale_ > 1) {
      // Each UI frame renders one coarse full-frame pass, then halves the cell size.
      prepareTileFrameBuffer(passW_, passH_);
      passFb_.renderFrame(renderer_, camera_, world_);

      void *mapped = passFb_.map(OSP_FB_COLOR);
      std::memcpy(passPixels_.data(),
          mapped,
          size_t(passW_) * size_t(passH_) * sizeof(uint32_t));
      passFb_.unmap(mapped);
      upsamplePassToDisplay();

      ++progressiveFramesAtCurrentScale_;
      const int framesPerScale =
          (accumulationEnabled && !fullResAccumOnly) ? 2 : 1;
      if (progressiveFramesAtCurrentScale_ >= framesPerScale) {
        progressiveFramesAtCurrentScale_ = 0;
        beginNextProgressivePass();
      }
    } else {
      // Once the grid reaches 1x1, continue with normal full-resolution accumulation.
      if (!accumFb_.handle() || !accumulationEnabled) {
        prepareTileFrameBuffer(fbW_, fbH_);
        passFb_.renderFrame(renderer_, camera_, world_);
        void *mapped = passFb_.map(OSP_FB_COLOR);
        std::memcpy(displayPixels_.data(),
            mapped,
            displayPixels_.size() * sizeof(uint32_t));
        passFb_.unmap(mapped);
      } else if (maxAccumulationFrames > 0
          && accumulatedFrames_ >= uint64_t(maxAccumulationFrames)) {
        frameInFlight_ = false;
        return false;
      } else {
        renderPhase_ = RenderPhase::Accumulate;
        accumFb_.renderFrame(renderer_, camera_, world_);

        void *mapped = accumFb_.map(OSP_FB_COLOR);
        std::memcpy(displayPixels_.data(),
            mapped,
            displayPixels_.size() * sizeof(uint32_t));
        accumFb_.unmap(mapped);
      }
    }

    const auto passEnd = std::chrono::steady_clock::now();
    lastFrameTimeMs_ =
        std::chrono::duration<float, std::milli>(passEnd - passStart).count();
    watchdogTriggered_ = (lastFrameTimeMs_ >= float(watchdogTimeoutMs));
    if (watchdogTriggered_)
      ++watchdogCancelCount_;
    ++accumulatedFrames_;
    frameInFlight_ = false;
    applyAoBackoff(watchdogTriggered_);

    return true;
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

  world_ = ospray::cpp::World();
  world_.setParam("instance", ospray::cpp::CopiedData(instance));

 std::vector<ospray::cpp::Light> lights;

  ospray::cpp::Light ambient("ambient");
  ambient.setParam("intensity", 0.25f);
  ambient.commit();
  lights.push_back(ambient);

  ospray::cpp::Light distant("distant");
  distant.setParam("direction", vec3f(-0.3f, -1.0f, -0.2f));
  distant.setParam("intensity", 3.0f);
  distant.commit();
  lights.push_back(distant);

  world_.setParam("light", ospray::cpp::CopiedData(lights));
  world_.commit();

  resetAccumulation();
}

bool OsprayBackend::loadObj(const std::string &path)
{
  if (frameInFlight_) {
    setError("OBJ load ignored while render is in flight.");
    return false;
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

    world_ = ospray::cpp::World();
    world_.setParam("instance", ospray::cpp::CopiedData(instance));

    std::vector<ospray::cpp::Light> lights;

    ospray::cpp::Light ambient("ambient");
    ambient.setParam("intensity", 0.25f);
    ambient.commit();
    lights.push_back(ambient);

    ospray::cpp::Light distant("distant");
    distant.setParam("direction", vec3f(-0.3f, -1.0f, -0.2f));
    distant.setParam("intensity", 3.0f);
    distant.commit();
    lights.push_back(distant);

    world_.setParam("light", ospray::cpp::CopiedData(lights));
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
    setError("BRL-CAD load ignored while render is in flight.");
    return false;
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
  world_ = ospray::cpp::World();
  world_.setParam("instance", ospray::cpp::CopiedData(instance));

  fprintf(stderr, "STEP 15: Adding light\n");
  std::vector<ospray::cpp::Light> lights;

  ospray::cpp::Light ambient("ambient");
  ambient.setParam("intensity", 0.25f);
  ambient.commit();
  lights.push_back(ambient);

  ospray::cpp::Light distant("distant");
  distant.setParam("direction", vec3f(-0.3f, -1.0f, -0.2f));
  distant.setParam("intensity", 3.0f);
  distant.commit();
  lights.push_back(distant);
  world_.setParam("light", ospray::cpp::CopiedData(lights));

  fprintf(stderr, "STEP 16: Commit world\n");
  world_.commit();

  fprintf(stderr, "STEP 16B: Reading world bounds\n");
  const OSPBounds worldBounds = ospGetBounds(world_.handle());
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
    setError("Renderer change ignored while render is in flight.");
    return;
  }

  try {
    renderer_ = ospray::cpp::Renderer(type);
    currentRenderer_ = type;

    renderer_.setParam("backgroundColor", 1.0f);
    renderer_.setParam("pixelSamples", configuredPixelSamplesForCurrentMode());
    renderer_.setParam("aoSamples", configuredAoSamplesForCurrentMode());
    renderer_.commit();
    appliedAoSamples_ = configuredAoSamplesForCurrentMode();
    appliedPixelSamples_ = configuredPixelSamplesForCurrentMode();

    resetAccumulation();
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

void OsprayBackend::cancelInFlightFrame()
{
  if (frameInFlight_) {
    setError("Cancel ignored in crash-isolation mode while render is in flight.");
    return;
  }

  frameInFlight_ = false;
  currentFrame_ = ospray::cpp::Future();
  inFlightStartValid_ = false;
}

void OsprayBackend::resetProgressiveState(bool clearDisplay)
{
  renderPhase_ = RenderPhase::Progressive;
  currentScaleIndex_ = scaleToIndex(startScaleForCurrentMode());
  slowPassStreak_ = 0;
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
  if (renderPhase_ == RenderPhase::Accumulate) {
    if (!accumFb_.handle())
      return false;
    updateCameraCrop(vec2f(0.f, 0.f), vec2f(1.f, 1.f));
    camera_.commit();
    currentFrame_ = accumFb_.renderFrame(renderer_, camera_, world_);
    frameInFlight_ = true;
    inFlightStart_ = std::chrono::steady_clock::now();
    inFlightStartValid_ = true;
    return true;
  }

  prepareTileFrameBuffer(passW_, passH_);
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
  applyAoBackoff(false);
  return updatedImage;
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
