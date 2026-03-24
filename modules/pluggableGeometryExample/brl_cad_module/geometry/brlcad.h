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

#undef UNUSED
#undef _USE_MATH_DEFINES
#include "brlcad/common.h"
#include "brlcad/raytrace.h" /* librt interface definitions */
#include "brlcad/vmath.h" /* vector math macros */

// #include "embree2/rtcore_ray.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_ray.h>

namespace ospray {
namespace brlcad {

struct BRLCAD : public ospray::Geometry
{
  BRLCAD(api::ISPCDevice &device);
  ~BRLCAD() override;

  void commit() override;
  size_t numPrimitives() const override;

  // Data members //

  uint geomID{0};
  rkcommon::math::box3f bounds;

  application ap;
  rt_i *rtip{nullptr};

  mutable std::vector<resource> resources;

  std::vector<std::string> objects;
};

} // namespace brlcad
} // namespace ospray
