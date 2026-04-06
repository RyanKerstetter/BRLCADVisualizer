// Copyright 2009-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef __cplusplus
#include "common/Data.h"
#endif

#include "geometry/GeometryShared.h"

#ifdef __cplusplus
namespace ispc {
#endif // __cplusplus

struct BRLCAD_sh
{
  Geometry super; // MUST be first (mirrors ispc::Geometry layout)
  void *brlcadSelf; // pointer back to the C++ BRLCAD object
  Data1D regionColors; // per-region RGBA, indexed by ray.primID / reg_bit
  uint32 colorEnabled;

#ifdef __cplusplus
  BRLCAD_sh()
      : brlcadSelf(nullptr),
        regionColors{nullptr, 0, 0, false},
        colorEnabled(1)
  {}
#endif
};

#ifdef __cplusplus
} // namespace ispc
#endif // __cplusplus
