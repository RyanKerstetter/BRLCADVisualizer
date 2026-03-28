// Copyright 2009-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "geometry/GeometryShared.h"

#ifdef __cplusplus
namespace ispc {
#endif // __cplusplus

struct BRLCAD_sh
{
  Geometry super; // MUST be first (mirrors ispc::Geometry layout)
  void *brlcadSelf; // pointer back to the C++ BRLCAD object

#ifdef __cplusplus
  BRLCAD_sh() : brlcadSelf(nullptr) {}
#endif
};

#ifdef __cplusplus
} // namespace ispc
#endif // __cplusplus
