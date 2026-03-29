#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ospray/ospray_cpp.h>
#include <ospray/ospray_cpp/ext/rkcommon.h>

class OsprayBackend
{
 public:
  OsprayBackend() = default;

  void init();
  void resize(int w, int h);
  const std::string &currentRenderer() const;
  void setCamera(const rkcommon::math::vec3f &eye,
      const rkcommon::math::vec3f &center,
      const rkcommon::math::vec3f &up,
      float fovyDeg);

  void resetAccumulation();
  const uint32_t *render();

  bool loadObj(const std::string &path);
  bool loadBrlcad(const std::string &path, const std::string &topObject = "");
  void loadTestMesh();

  rkcommon::math::vec3f getBoundsCenter() const;
  float getBoundsRadius() const;

  rkcommon::math::vec3f getBoundsMin() const;
  rkcommon::math::vec3f getBoundsMax() const;
  float getBoundsMaxExtent() const;

  void setRenderer(const std::string &type);
  void setAoSamples(int samples);
  
  const std::string &lastError() const;

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
  void setError(std::string message);

  int fbW_ = 1;
  int fbH_ = 1;

  rkcommon::math::vec3f boundsMin_{0.f, 0.f, 0.f};
  rkcommon::math::vec3f boundsMax_{0.f, 0.f, 0.f};

  ospray::cpp::Renderer renderer_;
  ospray::cpp::Camera camera_;
  ospray::cpp::World world_;
  ospray::cpp::FrameBuffer fb_;

  std::vector<uint32_t> pixels_;
  std::string lastError_;
  float lastFrameTimeMs_ = 0.0f;
  std::string currentRenderer_ = "scivis";
  
};
