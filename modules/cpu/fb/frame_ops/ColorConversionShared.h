#pragma once

#include "fb/FrameBufferView.ih"
#include "ospray/OSPEnums.h"

#ifdef __cplusplus
namespace ispc {
#endif

struct LiveColorConversion
{
  FrameBufferView super;

  // Target color format
  OSPFrameBufferFormat targetColorFormat;

  // Converted color buffer
  void *convBuffer;
};
#ifdef __cplusplus
}
#endif
