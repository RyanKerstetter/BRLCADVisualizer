#include "brlcad.h"

#include <atomic>
#include <cstdio>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "brlcad/rt/mater.h"

// Declare ISPC-exported function addresses (generated from brlcad.ispc).
// ISPC exports use C linkage; extern "C" inside namespace ispc maps
// the C symbol to the ispc:: namespace for C++ call sites.
namespace ispc {
extern "C" void *BRLCAD_postIntersect_addr();
extern "C" void *BRLCAD_intersect_addr();
} // namespace ispc

namespace ospray {
namespace brlcad {

// ---------------------------------------------------------------------------
// Thread-local CPU index for per-thread BRL-CAD resources
// ---------------------------------------------------------------------------

static inline int getCpuId()
{
  static std::atomic<int> nextCpuId{0};
  thread_local int cpuId = nextCpuId.fetch_add(1);
  return cpuId;
}

// ---------------------------------------------------------------------------
// CSV helper
// ---------------------------------------------------------------------------

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

static constexpr bool kVerboseBRLCADLogging = false;

static unsigned int currentContextInstID(
    const RTCIntersectFunctionNArguments *args)
{
  if (!args || !args->context)
    return RTC_INVALID_GEOMETRY_ID;
#if RTC_MAX_INSTANCE_LEVEL_COUNT > 1
  if (args->context->instStackSize > 0)
    return args->context->instID[args->context->instStackSize - 1];
#endif
  return args->context->instID[0];
}

static inline uint32_t packRegionColor(
    float r, float g, float b, float a = 1.0f)
{
  const auto toByte = [](float v) -> uint32_t {
    const float clamped = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
  };
  return toByte(r) | (toByte(g) << 8) | (toByte(b) << 16) | (toByte(a) << 24);
}

static inline uint32_t fallbackRegionColor()
{
  // DEBUG: bright red so we can tell if postIntersect runs but ma_color_valid
  // is 0
  return packRegionColor(1.0f, 0.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Embree ray-packet helpers
// ---------------------------------------------------------------------------
// This just retrieves ray i from the args and stores it in rayhit
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
  const auto &ray = rayhit.ray;
  const auto &hit = rayhit.hit;

  RTCRayHitN *rays = (RTCRayHitN *)rayhitN;
  RTCRayN *rayN = RTCRayHitN_RayN(rays, N);
  RTCHitN *hitN = RTCRayHitN_HitN(rays, N);

  RTCRayN_tfar(rayN, N, i) = ray.tfar;
  // put the brlcad ray information into embree ray
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
    RTCHitN_instID(hitN, N, i, 0) = hit.instID[0];
  }
}

// ---------------------------------------------------------------------------
// BRL-CAD hit / miss callbacks
// ---------------------------------------------------------------------------

static int hitCallback(application *ap, partition *PartHeadp, seg * /*segs*/)
{
  auto &rayhit = *static_cast<RTCRayHit *>(ap->a_uptr);
  auto &ray = rayhit.ray;
  auto &hit = rayhit.hit;

  for (auto *pp = PartHeadp->pt_forw; pp != PartHeadp; pp = pp->pt_forw) {
    auto *hitp = pp->pt_inhit;
    auto *stp = pp->pt_inseg->seg_stp;

    vect_t inormal;
    RT_HIT_NORMAL(inormal, hitp, stp, &(ap->a_ray), pp->pt_inflip);

    ray.tfar = hitp->hit_dist;
    hit.Ng_x = inormal[0];
    hit.Ng_y = inormal[1];
    hit.Ng_z = inormal[2];
    hit.geomID = static_cast<unsigned int>(ap->a_user);
    const region *region = pp->pt_regionp;
    hit.primID = fallbackRegionColor();
    if (region && region->reg_mater.ma_color_valid) {
      hit.primID = packRegionColor(region->reg_mater.ma_color[0],
          region->reg_mater.ma_color[1],
          region->reg_mater.ma_color[2],
          1.0f);
    }
    return 1;
  }

  return 1;
}

static int missCallback(application * /*ap*/)
{
  return 0;
}

// ---------------------------------------------------------------------------
// Single-ray BRL-CAD trace (called per lane from brlcadIntersectN_C)
// ---------------------------------------------------------------------------

static void traceRay(const BRLCAD &geom, RTCRayHit &rayhit, unsigned int geomID)
{
  auto &ray = rayhit.ray;
  auto &hit = rayhit.hit;
  application ap;
  // rtexample.c
  RT_APPLICATION_INIT(&ap);

  ap.a_rt_i = geom.rtip;
  ap.a_onehit = 1;
  const size_t resourceIndex =
      geom.resources.empty() ? 0 : (size_t(getCpuId()) % geom.resources.size());
  ap.a_resource = geom.resources.empty()
      ? nullptr
      : const_cast<resource *>(&geom.resources[resourceIndex]);
  ap.a_user = static_cast<int>(geomID);

  VSET(ap.a_ray.r_pt, ray.org_x, ray.org_y, ray.org_z);
  VSET(ap.a_ray.r_dir, ray.dir_x, ray.dir_y, ray.dir_z);
  ap.a_ray.r_min = ray.tnear;
  ap.a_ray.r_max = ray.tfar;

  ap.a_hit = hitCallback; // called on hit
  ap.a_miss = missCallback; // called on miss
  ap.a_logoverlap = rt_silent_logoverlap;
  ap.a_uptr = &rayhit; // where the actual ray is stored

  // Reset hit info so misses cannot leak stale data.
  hit.geomID = RTC_INVALID_GEOMETRY_ID;
  hit.primID = RTC_INVALID_GEOMETRY_ID;
  // shoot the ray
  auto didHit = rt_shootray(&ap);
  if (didHit <= 0) {
    hit.geomID = RTC_INVALID_GEOMETRY_ID;
    hit.primID = RTC_INVALID_GEOMETRY_ID;
    return;
  }
}

// ---------------------------------------------------------------------------
// Embree bounds callback
// geometryUserPtr = getSh() = ispc::BRLCAD_sh*  (set by
// createEmbreeUserGeometry)
// Compute the bounds of a BRL-CAD object
// ---------------------------------------------------------------------------

static void brlcadBounds(const struct RTCBoundsFunctionArguments *args)
{
  const ispc::BRLCAD_sh *sh =
      static_cast<const ispc::BRLCAD_sh *>(args->geometryUserPtr);
  const BRLCAD *geom = static_cast<const BRLCAD *>(sh->brlcadSelf);

  RTCBounds *bounds_o = args->bounds_o;
  bounds_o->lower_x = geom->bounds.lower.x;
  bounds_o->lower_y = geom->bounds.lower.y;
  bounds_o->lower_z = geom->bounds.lower.z;
  bounds_o->upper_x = geom->bounds.upper.x;
  bounds_o->upper_y = geom->bounds.upper.y;
  bounds_o->upper_z = geom->bounds.upper.z;
}

// ---------------------------------------------------------------------------
// C bridge called from ISPC BRLCAD_intersect.
// args->geometryUserPtr = getSh() = BRLCAD_sh* (not used here; geom via self)
// If intersection with BRL-CAD object, then call BRL-CAD raytracing
// functionallity
// ---------------------------------------------------------------------------

extern "C" void brlcadIntersectN_C(void *self,
    const RTCIntersectFunctionNArguments *args,
    bool isOcclusionTest)
{
  // args points to a bunch of rays
  const BRLCAD *geom = static_cast<const BRLCAD *>(
      self); // pointer to custom geometry (BRL-CAD object)

  const unsigned int N = args->N;
  const unsigned int geomID = args->geomID;

  if (isOcclusionTest) {
    // args was actually RTCOccludedFunctionNArguments* cast to intersect args.
    // The rayhit field maps to ray in the occluded struct.
    RTCRayN *rayN = (RTCRayN *)args->rayhit;
    for (unsigned int i = 0; i < N; ++i) {
      if (args->valid && !args->valid[i])
        continue;

      RTCRayHit rayhit{};
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
      rayhit.hit.instID[0] = currentContextInstID(args);

      traceRay(*geom, rayhit, geomID);

      // Embree occlusion convention: tfar = -inf means occluded
      if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID)
        RTCRayN_tfar(rayN, N, i) = -std::numeric_limits<float>::infinity();
    }
  } else {
    for (unsigned int i = 0; i < N; ++i) {
      if (!args->valid || args->valid[i]) {
        RTCRayHit rayhit; // stores the ray information
        // retrieve the ray information from Embree
        getRay(args->rayhit, N, i, rayhit);
        rayhit.hit.instID[0] = currentContextInstID(args);
        // rayhit gives us the information from the ray hitting the object
        traceRay(*geom, rayhit, geomID);
        // rayhit - the ray with information from BRL-CAD, args, stors the rays
        // from embree
        setRay(rayhit, args->rayhit, N, i);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// BRLCAD class implementation
// ---------------------------------------------------------------------------

BRLCAD::BRLCAD(api::ISPCDevice &device)
    : AddStructShared(device.getDRTDevice(), device, FFG_BOX)
{
#ifndef OSPRAY_TARGET_SYCL
  getSh()->super.type = ispc::GEOMETRY_TYPE_UNKNOWN;
  getSh()->super.postIntersect =
      reinterpret_cast<ispc::Geometry_postIntersectFct>(
          ispc::BRLCAD_postIntersect_addr());
  getSh()->super.intersect = reinterpret_cast<ispc::Geometry_IntersectFct>(
      ispc::BRLCAD_intersect_addr());
#endif
}

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
  getSh()->super.type = ispc::GEOMETRY_TYPE_UNKNOWN;
#ifndef OSPRAY_TARGET_SYCL
  getSh()->super.postIntersect =
      reinterpret_cast<ispc::Geometry_postIntersectFct>(
          ispc::BRLCAD_postIntersect_addr());
  getSh()->super.intersect = reinterpret_cast<ispc::Geometry_IntersectFct>(
      ispc::BRLCAD_intersect_addr());
#endif
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
  const bool colorEnabled = getParam<bool>("colorEnabled", true);
  const int nThreads = getNumThreads();

  rtip = rt_dirbuild(filename.c_str(), nullptr, 0);
  if (rtip == nullptr)
    throw std::runtime_error("Failed to open BRL-CAD database: " + filename);

  resources.clear();

  objects.clear();
  auto objNames = splitCSV(objectList);
  for (const auto &obj : objNames) {
    if (obj.empty())
      continue;
    objects.push_back(obj);
    const char *object = obj.c_str();
    if (rt_gettrees(rtip, 1, &object, nThreads) < 0)
      throw std::runtime_error("Failed to load BRL-CAD object: " + obj);
  }

  if (objects.empty()) {
    static const char *allObj = "all";
    if (rt_gettrees(rtip, 1, &allObj, nThreads) < 0)
      throw std::runtime_error("Failed to load default BRL-CAD object: all");
    objects.emplace_back(allObj);
  }

  resources.resize(nThreads);
  for (int i = 0; i < nThreads; ++i) {
    rt_init_resource(&resources[i], i, rtip);
  }
  rt_prep_parallel(rtip, nThreads);

  int coloredRegions = 0;
  if (rtip->Regions && rtip->nregions > 0) {
    for (size_t i = 0; i < rtip->nregions; ++i) {
      region *reg = rtip->Regions[i];
      if (!reg)
        continue;

      rt_region_color_map(reg);

      if (reg->reg_mater.ma_color_valid) {
        ++coloredRegions;
        fprintf(stderr,
            "  region[%zu] '%s' color=(%.2f, %.2f, %.2f)\n",
            i,
            reg->reg_name ? reg->reg_name : "?",
            reg->reg_mater.ma_color[0],
            reg->reg_mater.ma_color[1],
            reg->reg_mater.ma_color[2]);
      }
    }
  }
  fprintf(stderr,
      "BRLCAD: %zu regions total, %d have colors\n",
      rtip->nregions,
      coloredRegions);

  bounds.lower.x = rtip->mdl_min[0];
  bounds.lower.y = rtip->mdl_min[1];
  bounds.lower.z = rtip->mdl_min[2];
  bounds.upper.x = rtip->mdl_max[0];
  bounds.upper.y = rtip->mdl_max[1];
  bounds.upper.z = rtip->mdl_max[2];

  if (kVerboseBRLCADLogging) {
    fprintf(stderr,
        "BRLCAD bounds min=(%f,%f,%f) max=(%f,%f,%f)\n",
        bounds.lower.x,
        bounds.lower.y,
        bounds.lower.z,
        bounds.upper.x,
        bounds.upper.y,
        bounds.upper.z);
  }

  getSh()->brlcadSelf = this;
  getSh()->colorEnabled = colorEnabled ? 1u : 0u;
  // set the bounds function
  createEmbreeUserGeometry((RTCBoundsFunction)brlcadBounds);

  getSh()->super.numPrimitives = static_cast<int>(numPrimitives());
}
} // namespace brlcad
} // namespace ospray
