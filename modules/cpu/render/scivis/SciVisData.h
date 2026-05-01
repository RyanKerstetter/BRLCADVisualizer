#pragma once

#include "common/StructShared.h"
#include "render/RendererData.h"
// ispc shared
#include "SciVisDataShared.h"

namespace ospray {

struct World;

struct SciVisData : public AddStructShared<RendererData, ispc::SciVisData>
{
  SciVisData(const World &world);
  ~SciVisData() override;

 private:
  BufferSharedUq<ispc::Light *> lightArray;
};

} // namespace ospray
