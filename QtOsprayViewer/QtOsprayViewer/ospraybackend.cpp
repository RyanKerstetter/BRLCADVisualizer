#include "ospraybackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

using rkcommon::math::vec3f;
using rkcommon::math::vec3ui;
using rkcommon::math::vec4f;

int aoSamples_ = 1;

void OsprayBackend::init()
{
  renderer_ = ospray::cpp::Renderer("ao");
  renderer_.setParam("aoSamples", 0);
  renderer_.setParam("backgroundColor", 1.0f);
  renderer_.commit();


  camera_ = ospray::cpp::Camera("perspective");
  camera_.setParam("fovy", 60.f);
  camera_.commit();

  loadTestMesh();
}

void OsprayBackend::resize(int w, int h)
{
  fbW_ = std::max(1, w);
  fbH_ = std::max(1, h);

  camera_.setParam("aspect", float(fbW_) / float(fbH_));
  camera_.commit();

  fb_ = ospray::cpp::FrameBuffer(
      fbW_, fbH_, OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
  fb_.clear();

  pixels_.assign(size_t(fbW_) * size_t(fbH_), 0u);
}

void OsprayBackend::setCamera(const vec3f &eye, const vec3f &center, const vec3f &up, float fovyDeg)
{
  camera_.setParam("position", eye);
  camera_.setParam("direction", center - eye);
  camera_.setParam("up", up);
  camera_.setParam("fovy", fovyDeg);
  camera_.commit();
}

void OsprayBackend::resetAccumulation()
{
  if (fb_.handle())
    fb_.clear();
}

const uint32_t *OsprayBackend::render()
{
  fb_.renderFrame(renderer_, camera_, world_);

  void *mapped = fb_.map(OSP_FB_COLOR);
  std::memcpy(pixels_.data(), mapped, pixels_.size() * sizeof(uint32_t));
  fb_.unmap(mapped);

  return pixels_.data();
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

  ospray::cpp::Light light("ambient");
  light.commit();

  world_.setParam("light", ospray::cpp::CopiedData(light));
  world_.commit();

  resetAccumulation();
}

bool OsprayBackend::loadObj(const std::string &path)
{
  tinyobj::ObjReader reader;
  tinyobj::ObjReaderConfig config;
  config.triangulate = true;

  if (!reader.ParseFromFile(path, config))
    return false;

  if (!reader.Error().empty())
    return false;

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

      if (i0.vertex_index < 0 || i1.vertex_index < 0 || i2.vertex_index < 0) {
        indexOffset += fv;
        continue;
      }

      indices.emplace_back(static_cast<unsigned>(i0.vertex_index),
          static_cast<unsigned>(i1.vertex_index),
          static_cast<unsigned>(i2.vertex_index));

      indexOffset += fv;
    }
  }

  if (vertices.empty() || indices.empty())
    return false;

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

  ospray::cpp::Light light("ambient");
  light.commit();
  world_.setParam("light", ospray::cpp::CopiedData(light));
  world_.commit();

  resetAccumulation();
  return true;
}

void OsprayBackend::setRenderer(const std::string &type)
{
  renderer_ = ospray::cpp::Renderer(type);

  // re-apply basic params
  renderer_.setParam("aoSamples", aoSamples_); // if you have this
  renderer_.commit();

  resetAccumulation();
}

void OsprayBackend::setAoSamples(int samples)
{
  aoSamples_ = samples;

  renderer_.setParam("aoSamples", aoSamples_);
  renderer_.commit();

  resetAccumulation();
}

int& OsprayBackend::getAoSamples()
{
  return aoSamples_;
}