#pragma once

#include "fb/FrameBufferView.ih"

#ifdef __cplusplus
namespace ispc {
#endif

#define BLUR_RADIUS 4
struct LiveBlur
{
  FrameBufferView super;
  vec4f *scratchBuffer;
  float weights[BLUR_RADIUS + 1];
};
#ifdef __cplusplus
}
#endif
