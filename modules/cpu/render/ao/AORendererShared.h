#pragma once

#include "render/RendererShared.h"

#ifdef __cplusplus
namespace ispc {
#endif // __cplusplus

struct AORenderer
{
  Renderer super;
  int aoSamples;
  float aoRadius;
  float aoIntensity;
  float volumeSamplingRate;
  vec3f lightDirection;
  float ambientIntensity;
  float directionalIntensity;

#ifdef __cplusplus
  AORenderer()
      : aoSamples(1),
        aoRadius(1e20f),
        aoIntensity(1.f),
        volumeSamplingRate(1.f),
        lightDirection(0.3f, 1.0f, 0.2f),
        ambientIntensity(0.18f),
        directionalIntensity(0.82f)
  {}
};
} // namespace ispc
#else
};
#endif // __cplusplus
