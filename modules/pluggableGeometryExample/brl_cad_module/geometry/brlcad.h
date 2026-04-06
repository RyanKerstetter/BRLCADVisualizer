// ======================================================================== //
// Copyright 2009-2018 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include <string>
#include <vector>

#include "geometry/Geometry.h"
#include "rkcommon/math/box.h"
#include "rkcommon/math/vec.h"

// Shared C++/ISPC struct for BRLCAD geometry
#include "BRLCADShared.h"

#undef UNUSED
#undef _USE_MATH_DEFINES
#include "brlcad/common.h"
#include "brlcad/raytrace.h" /* librt interface definitions */
#include "brlcad/vmath.h" /* vector math macros */

#include <embree4/rtcore.h>
#include <embree4/rtcore_ray.h>

namespace ospray {
namespace brlcad {

// BRLCAD extends AddStructShared<Geometry, ispc::BRLCAD_sh> so that:
//  - getSh() returns ispc::BRLCAD_sh* (which starts with ispc::Geometry super)
//  - Geometry_dispatch_postIntersect / _intersect can use getSh()->super.*
//  - BRLCAD_intersect (ISPC) reads getSh()->brlcadSelf to reach this object
struct BRLCAD : public AddStructShared<Geometry, ispc::BRLCAD_sh>
{
  BRLCAD(api::ISPCDevice &device);
  ~BRLCAD() override;

  void commit() override;
  size_t numPrimitives() const override;

  // Scene bounds (populated in commit from BRL-CAD rtip)
  rkcommon::math::box3f bounds;

  // BRL-CAD ray-trace instance
  application ap;
  rt_i *rtip{nullptr};

  std::vector<rkcommon::math::vec4f> regionColors;
  mutable std::vector<resource> resources;
  std::vector<std::string> objects;
};

} // namespace brlcad
} // namespace ospray
