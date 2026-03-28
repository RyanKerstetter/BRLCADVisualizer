// Copyright 2009-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "common/DifferentialGeometry.ih"
#include "common/Ray.ih"
// c++ shared
#include "BRLCADShared.h"

OSPRAY_BEGIN_ISPC_NAMESPACE

// postIntersect: called by Geometry_dispatch_postIntersect for GEOMETRY_TYPE_UNKNOWN.
// Fill dg.Ng and dg.Ns from the intersection normal stored in ray.Ng.
// Normalization and face-forwarding are handled by OSPRay's World::postIntersect.
SYCL_EXTERNAL void BRLCAD_postIntersect(const Geometry *uniform,
    varying DifferentialGeometry &dg,
    const varying Ray &ray,
    uniform int64)
{
  dg.Ng = dg.Ns = ray.Ng;
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
