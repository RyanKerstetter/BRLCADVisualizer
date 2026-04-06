// Copyright 2009-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef __cplusplus
#include "common/Data.h"
#endif

#include "geometry/GeometryShared.h"
#include "render/MaterialShared.h"

#ifdef __cplusplus
namespace ispc {
#endif // __cplusplus

struct BRLCAD_sh
{
  Geometry super; // MUST be first (mirrors ispc::Geometry layout)
  void *brlcadSelf; // pointer back to the C++ BRLCAD object
  Data1D regionColors; // per-region RGBA, indexed by ray.primID / reg_bit
#ifdef __cplusplus
  Material **materials;
  uint32_t numMaterials;
#else
  Material **materials;
  uint32 numMaterials;
#endif

#ifdef __cplusplus
  BRLCAD_sh()
      : brlcadSelf(nullptr),
        regionColors{nullptr, 0, 0, false},
        materials(nullptr),
        numMaterials(0)
  {}
#endif
};

#ifdef __cplusplus
} // namespace ispc
#endif // __cplusplus
