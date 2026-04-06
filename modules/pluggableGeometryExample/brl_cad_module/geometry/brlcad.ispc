// Copyright 2009-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "common/DifferentialGeometry.ih"
#include "common/Data.ih"
#include "common/Ray.ih"
// c++ shared
#include "BRLCADShared.h"

OSPRAY_BEGIN_ISPC_NAMESPACE

// postIntersect: called by Geometry_dispatch_postIntersect for GEOMETRY_TYPE_UNKNOWN.
// Fill dg.Ng and dg.Ns from the intersection normal stored in ray.Ng.
// Normalization and face-forwarding are handled by OSPRay's World::postIntersect.
SYCL_EXTERNAL void BRLCAD_postIntersect(const Geometry *uniform _self,
    varying DifferentialGeometry &dg,
    const varying Ray &ray,
    uniform int64 flags)
{
  const BRLCAD_sh *uniform self = (const BRLCAD_sh *uniform)_self;
  dg.Ng = dg.Ns = ray.Ng;
  if ((flags & DG_COLOR) && self->colorEnabled != 0) {
    dg.color = make_vec4f(0.8f, 0.8f, 0.8f, 1.0f);
    if (valid(self->regionColors) && self->regionColors.numItems > 0) {
      dg.color = get_vec4f(
          self->regionColors, min(ray.primID, self->regionColors.numItems - 1));
    }
  }
}

export void *uniform BRLCAD_postIntersect_addr()
{
  return (void *uniform)BRLCAD_postIntersect;
}

// C bridge: implemented in brlcad.cpp with extern "C" linkage.
// The ISPC intersect function calls into C++ BRL-CAD ray tracing via this bridge.
extern "C" void brlcadIntersectN_C(void *uniform self,
    const struct RTCIntersectFunctionNArguments *uniform args,
    uniform bool isOcclusionTest);

// intersect: called by Geometry_dispatch_intersect/occluded for GEOMETRY_TYPE_UNKNOWN.
// geometryUserPtr is the BRLCAD_sh* (set by createEmbreeUserGeometry).
void unmasked BRLCAD_intersect(
    const struct RTCIntersectFunctionNArguments *uniform args,
    uniform bool isOcclusionTest)
{
  BRLCAD_sh *uniform sh = (BRLCAD_sh *uniform) args->geometryUserPtr;
  brlcadIntersectN_C(sh->brlcadSelf, args, isOcclusionTest);
}

export void *uniform BRLCAD_intersect_addr()
{
  return (void *uniform)BRLCAD_intersect;
}

OSPRAY_END_ISPC_NAMESPACE
