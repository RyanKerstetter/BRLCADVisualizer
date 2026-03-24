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
// RTC_GEOMETRY_TYPE_USER
#include "brlcad.h"

#include <atomic>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace ospray {
namespace brlcad {

static inline int getCpuId()
{
  return std::hash<std::thread::id>()(std::this_thread::get_id()) % MAX_PSW;
#if 0
      static std::unordered_map<std::thread::id, int> threadIds;
      static std::atomic<int> nextId{1};
      static std::mutex mtx;


      mtx.lock();
      const auto currentThread = std::this_thread::get_id();
      auto id = threadIds[currentThread];
      if (id == 0) {
	id = ++nextId;
	threadIds[currentThread] = id;
      }
      mtx.unlock();

      return id - 1;
#endif
}

static inline std::vector<std::string> splitCSV(const std::string &input)
{
  std::vector<std::string> out;
  std::stringstream ss(input);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty())
      out.push_back(item);
  }
  return out;
}

static inline int getNumThreads()
{
  const unsigned int hc = std::thread::hardware_concurrency();
  return hc > 0 ? static_cast<int>(hc) : 1;
}

// Local helper functions /////////////////////////////////////////////////

inline static void getRay(
    void *rayhitN, unsigned int N, unsigned int i, RTCRayHit &rayhit)
{
  auto &ray = rayhit.ray;
  auto &hit = rayhit.hit;

  RTCRayHitN *rays = (RTCRayHitN *)rayhitN;
  RTCRayN *rayN = RTCRayHitN_RayN(rays, N);
  RTCHitN *hitN = RTCRayHitN_HitN(rays, N);

  ray.org_x = RTCRayN_org_x(rayN, N, i);
  ray.org_y = RTCRayN_org_y(rayN, N, i);
  ray.org_z = RTCRayN_org_z(rayN, N, i);

  ray.dir_x = RTCRayN_dir_x(rayN, N, i);
  ray.dir_y = RTCRayN_dir_y(rayN, N, i);
  ray.dir_z = RTCRayN_dir_z(rayN, N, i);

  ray.tnear = RTCRayN_tnear(rayN, N, i);
  ray.tfar = RTCRayN_tfar(rayN, N, i);

  ray.time = RTCRayN_time(rayN, N, i);
  ray.mask = RTCRayN_mask(rayN, N, i);
  ray.flags = 0;

  hit.primID = RTCHitN_primID(hitN, N, i);
  hit.geomID = RTCHitN_geomID(hitN, N, i);
  hit.instID[0] = RTCHitN_instID(hitN, N, i, 0);
}

inline static void setRay(
    const RTCRayHit &rayhit, void *rayhitN, unsigned int N, unsigned int i)
{
  // Update to use RTCRayHit, which includes the RTCRay

  const auto &ray = rayhit.ray;
  const auto &hit = rayhit.hit;

  RTCRayHitN *rays = (RTCRayHitN *)rayhitN;
  RTCRayN *rayN = RTCRayHitN_RayN(rays, N);
  RTCHitN *hitN = RTCRayHitN_HitN(rays, N);

  RTCRayN_tfar(rayN, N, i) = ray.tfar;

  if (hit.geomID != RTC_INVALID_GEOMETRY_ID) {
    RTCHitN_Ng_x(hitN, N, i) = hit.Ng_x;
    RTCHitN_Ng_y(hitN, N, i) = hit.Ng_y;
    RTCHitN_Ng_z(hitN, N, i) = hit.Ng_z;

    RTCHitN_primID(hitN, N, i) = hit.primID;
    RTCHitN_geomID(hitN, N, i) = hit.geomID;
    RTCHitN_instID(hitN, N, i, 0) = hit.instID[0];
    RTCHitN_u(hitN, N, i) = hit.u;
    RTCHitN_v(hitN, N, i) = hit.v;
  } else {
    RTCHitN_geomID(hitN, N, i) = RTC_INVALID_GEOMETRY_ID;
    RTCHitN_primID(hitN, N, i) = RTC_INVALID_GEOMETRY_ID;
  }
}

static int hitCallback(application *ap, partition *PartHeadp, seg *segs)
{
  /* will contain surface curvature information at the entry */
#if 0
      curvature cur = RT_CURVATURE_INIT_ZERO;
#endif

  auto &rayhit = *static_cast<RTCRayHit *>(ap->a_uptr);
  auto &ray = rayhit.ray;
  auto &hit = rayhit.hit;

  /* iterate over each partition until we get back to the head.
   * each partition corresponds to a specific homogeneous region of
   * material.
   */
  for (auto *pp = PartHeadp->pt_forw; pp != PartHeadp; pp = pp->pt_forw) {
    /* entry hit point, so we type less */
    auto *hitp = pp->pt_inhit;

#if 0
        /* construct the actual (entry) hit-point from the ray and the
         * distance to the intersection point (i.e., the 't' value).
         */
        point_t pt;
        VJOIN1(pt, ap->a_ray.r_pt, hitp->hit_dist, ap->a_ray.r_dir);
#endif

    /* primitive we encountered on entry */
    auto *stp = pp->pt_inseg->seg_stp;

    /* compute the normal vector at the entry point, flipping the
     * normal if necessary.
     */
    vect_t inormal;
    RT_HIT_NORMAL(inormal, hitp, stp, &(ap->a_ray), pp->pt_inflip);

    ray.tfar = hitp->hit_dist;
    hit.Ng_x = inormal[0];
    hit.Ng_y = inormal[1];
    hit.Ng_z = inormal[2];
    return 1;

    /* ...COLOR... */
    // pp->pt_regionp->reg_mater->ma_color
#if 0
        /* This next macro fills in the curvature information which
         * consists on a principle direction vector, and the inverse
         * radii of curvature along that direction and perpendicular
         * to it.  Positive curvature bends toward the outward
         * pointing normal.
         */
        RT_CURVATURE(&cur, hitp, pp->pt_inflip, stp);

        /* exit point, so we type less */
        hitp = pp->pt_outhit;

        /* construct the actual (exit) hit-point from the ray and the
         * distance to the intersection point (i.e., the 't' value).
         */
        VJOIN1(pt, ap->a_ray.r_pt, hitp->hit_dist, ap->a_ray.r_dir);

        /* primitive we exited from */
        stp = pp->pt_outseg->seg_stp;

        /* compute the normal vector at the exit point, flipping the
         * normal if necessary.
         */
        vect_t onormal;
        RT_HIT_NORMAL(onormal, hitp, stp, &(ap->a_ray), pp->pt_outflip);
#endif
  }

  // Return '1' for hit
  return 1;
}

static int missCallback(application *ap)
{
  // Return '0' for miss
  return 0;
}

static void traceRay(const BRLCAD &geom, RTCRayHit &rayhit)
{
  // Update to use RTCRayHit, which includes the RTCRay

  auto &ray = rayhit.ray;
  auto &hit = rayhit.hit;
  application ap;

  RT_APPLICATION_INIT(&ap);

  ap.a_rt_i = geom.rtip;
  ap.a_onehit = 1;
  ap.a_resource = &geom.resources[getCpuId()];

  VSET(ap.a_ray.r_pt, ray.org_x, ray.org_y, ray.org_z);
  VSET(ap.a_ray.r_dir, ray.dir_x, ray.dir_y, ray.dir_z);
  ap.a_ray.r_min = ray.tnear;
  ap.a_ray.r_max = ray.tfar;

  ap.a_hit = hitCallback;
  ap.a_miss = missCallback;
  ap.a_logoverlap = rt_silent_logoverlap;
  ap.a_uptr = &rayhit;

  // Reset hit info so misses cannot leak stale data.
  hit.geomID = RTC_INVALID_GEOMETRY_ID;
  hit.primID = RTC_INVALID_GEOMETRY_ID;

  auto didHit = rt_shootray(&ap);
  if (didHit) {
    hit.geomID = geom.geomID;
    hit.primID = 0;
  }
}

// static void brlcadIntersect(
//     const BRLCAD *geom_i, RTCRayHit &rayhit, size_t item)
// {
//   const BRLCAD &geom = geom_i[item];
//   traceRay(geom, rayhit);
// }

// static void brlcadIntersectN(const int *valid, const BRLCAD *geom_i,
//                              const RTCIntersectContext *context, RTCRayNp
//                              *rays, size_t N, size_t item) {
//   const BRLCAD &geom = geom_i[item];

//   for (size_t i = 0; i < N; ++i) {
//     if (valid[i]) {
//       RTCRay ray;
//       getRay(*rays, ray, i);
//       traceRay(geom, ray);
//       setRay(ray, *rays, i);
//     }
//   }
// }
static void brlcadIntersectN(const RTCIntersectFunctionNArguments *args)
{
  int *valid = args->valid;
  unsigned int N = args->N;
  void *rayhitN = args->rayhit;

  const auto *geom = static_cast<const BRLCAD *>(args->geometryUserPtr);

  for (size_t i = 0; i < N; ++i) {
    if (!valid || valid[i]) {
      RTCRayHit rayhit;
      getRay(rayhitN, N, static_cast<unsigned int>(i), rayhit);
      traceRay(*geom, rayhit);
      setRay(rayhit, rayhitN, N, static_cast<unsigned int>(i));
    }
  }
}

static void brlcadOccludedN(const RTCOccludedFunctionNArguments *args)
{
  const int *valid = args->valid;
  const unsigned int N = args->N;
  RTCRayN *rayN = (RTCRayN *)args->ray;
  const auto *geom = static_cast<const BRLCAD *>(args->geometryUserPtr);

  for (unsigned int i = 0; i < N; ++i) {
    if (valid && !valid[i])
      continue;

    RTCRayHit rayhit{};
    // Build scalar ray from occlusion packet lane (RTCRayN layout).
    rayhit.ray.org_x = RTCRayN_org_x(rayN, N, i);
    rayhit.ray.org_y = RTCRayN_org_y(rayN, N, i);
    rayhit.ray.org_z = RTCRayN_org_z(rayN, N, i);
    rayhit.ray.dir_x = RTCRayN_dir_x(rayN, N, i);
    rayhit.ray.dir_y = RTCRayN_dir_y(rayN, N, i);
    rayhit.ray.dir_z = RTCRayN_dir_z(rayN, N, i);
    rayhit.ray.tnear = RTCRayN_tnear(rayN, N, i);
    rayhit.ray.tfar = RTCRayN_tfar(rayN, N, i);
    rayhit.ray.time = RTCRayN_time(rayN, N, i);
    rayhit.ray.mask = RTCRayN_mask(rayN, N, i);
    rayhit.ray.id = 0;
    rayhit.ray.flags = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    traceRay(*geom, rayhit);

    // Embree occlusion convention: tfar = -inf means occluded
    if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
      RTCRayN_tfar(rayN, N, i) = -std::numeric_limits<float>::infinity();
    }
  }
}

// static void brlcadBounds(void *geom_i, size_t item, RTCBounds &bounds_o) {
//   const auto &geom = ((const BRLCAD *)geom_i)[item];
//   bounds_o.lower_x = geom.bounds.lower.x;
//   bounds_o.lower_y = geom.bounds.lower.y;
//   bounds_o.lower_z = geom.bounds.lower.z;
//   bounds_o.upper_x = geom.bounds.upper.x;
//   bounds_o.upper_y = geom.bounds.upper.y;
//   bounds_o.upper_z = geom.bounds.upper.z;
// }

static void brlcadBounds(const struct RTCBoundsFunctionArguments *args)
{
  // Updated bounds function
  const BRLCAD *geoms = (const BRLCAD *)args->geometryUserPtr;
  RTCBounds *bounds_o = args->bounds_o;
  const BRLCAD &geom = geoms[args->primID];
  bounds_o->lower_x = geom.bounds.lower.x;
  bounds_o->lower_y = geom.bounds.lower.y;
  bounds_o->lower_z = geom.bounds.lower.z;
  bounds_o->upper_x = geom.bounds.upper.x;
  bounds_o->upper_y = geom.bounds.upper.y;
  bounds_o->upper_z = geom.bounds.upper.z;
}

BRLCAD::BRLCAD(api::ISPCDevice &device) : Geometry(device, FFG_NONE) {}

BRLCAD::~BRLCAD()
{
  if (rtip) {
    for (auto &res : resources)
      rt_clean_resource_complete(rtip, &res);
    rt_free_rti(rtip);
    rtip = nullptr;
  }
}

size_t BRLCAD::numPrimitives() const
{
  return rtip ? 1 : 0;
}

void BRLCAD::commit()
{
  if (rtip) {
    for (auto &res : resources)
      rt_clean_resource_complete(rtip, &res);
    rt_free_rti(rtip);
    rtip = nullptr;
  }

  const std::string filename = getParam<std::string>("filename", "");
  if (filename.empty())
    throw std::runtime_error("BRL-CAD geometry requires 'filename' parameter");

  std::string objectList = getParam<std::string>("objects", "all");

  rtip = rt_dirbuild(filename.c_str(), nullptr, 0);
  if (rtip == nullptr)
    throw std::runtime_error("Failed to open BRL-CAD database: " + filename);

  const int nThreads = getNumThreads();

  std::cout << "nthreads is " << nThreads << std::endl;

  resources.clear();
  resources.resize(MAX_PSW);
  for (int i = 0; i < MAX_PSW; ++i)
    rt_init_resource(&resources[i], i, rtip);

  objects.clear();
  auto objNames = splitCSV(objectList);
  for (const auto &obj : objNames) {
    if (obj.empty())
      continue;
    objects.push_back(obj);
    const char *object = obj.c_str();
    if (rt_gettrees(rtip, 1, &object, 1) < 0)
      throw std::runtime_error("Failed to load BRL-CAD object: " + obj);
  }

  if (objects.empty()) {
    static const char *allObj = "all";
    if (rt_gettrees(rtip, 1, &allObj, 1) < 0)
      throw std::runtime_error("Failed to load default BRL-CAD object: all");
    objects.emplace_back(allObj);
  }

  rt_prep_parallel(rtip, nThreads);

  bounds.lower.x = rtip->mdl_min[0];
  bounds.lower.y = rtip->mdl_min[1];
  bounds.lower.z = rtip->mdl_min[2];
  bounds.upper.x = rtip->mdl_max[0];
  bounds.upper.y = rtip->mdl_max[1];
  bounds.upper.z = rtip->mdl_max[2];

  if (embreeGeometry) {
    rtcReleaseGeometry(embreeGeometry);
    embreeGeometry = nullptr;
  }

  RTCDevice device = getISPCDevice().getEmbreeDevice();
  if (!device)
    throw std::runtime_error("invalid Embree device");

  RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
  rtcSetGeometryUserPrimitiveCount(geometry, numPrimitives());
  rtcSetGeometryUserData(geometry, this);
  rtcSetGeometryBoundsFunction(geometry, brlcadBounds, nullptr);
  rtcSetGeometryIntersectFunction(geometry, brlcadIntersectN);
  rtcSetGeometryOccludedFunction(geometry, brlcadOccludedN);
  rtcCommitGeometry(geometry);

  embreeGeometry = geometry;
  geomID = 0;
}

} // namespace brlcad
} // namespace ospray
