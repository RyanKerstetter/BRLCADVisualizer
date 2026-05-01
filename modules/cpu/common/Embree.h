#pragma once

#ifdef OSPRAY_TARGET_SYCL
#include <sycl/sycl.hpp>
#endif

#ifdef ISPC
#include "embree4/rtcore.isph"
#else
#include "embree4/rtcore.h"
#endif
