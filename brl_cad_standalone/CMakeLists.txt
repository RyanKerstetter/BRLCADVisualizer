cmake_minimum_required(VERSION 3.20)
project(ospray_module_brl_cad CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Installed OSPRay / rkcommon / embree ----
set(OSPRAY_PREFIX   "/home/adeeb/workspace/csce482/build/install/ospray"   CACHE PATH "OSPRay install prefix")
set(RKCOMMON_PREFIX "/home/adeeb/workspace/csce482/build/install/rkcommon" CACHE PATH "rkcommon install prefix")
set(EMBREE_PREFIX   "/home/adeeb/workspace/csce482/build/install/embree"   CACHE PATH "Embree install prefix")
set(OPENVKL_PREFIX  "/home/adeeb/workspace/csce482/build/install/openvkl"  CACHE PATH "OpenVKL install prefix")

list(APPEND CMAKE_PREFIX_PATH "${OSPRAY_PREFIX}" "${RKCOMMON_PREFIX}" "${EMBREE_PREFIX}" "${OPENVKL_PREFIX}")

# Find embree and rkcommon first so that osprayUse.cmake can find their targets
find_package(embree 4.3.3 REQUIRED CONFIG)
find_package(rkcommon REQUIRED CONFIG)
get_target_property(RKCOMMON_INCLUDE_DIRS rkcommon::rkcommon INTERFACE_INCLUDE_DIRECTORIES)

find_package(ospray REQUIRED CONFIG)   # provides ospray::ospray_module_cpu

# ---- ISPC setup (must happen before adding ISPC sources) ----
# The installed OSPRay prefix does not ship ispc.cmake; use the copy in this repo.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/compiler")
include(ispc)

# Match the ISA targets OSPRay was built with
set(ISPC_TARGET_LIST ${OSPRAY_ISPC_TARGET_LIST})

# ISPC include paths for OSPRay SDK headers the .ispc file needs
include_directories_ispc(
  "${OSPRAY_PREFIX}/include"
  "${OSPRAY_PREFIX}/include/ospray/SDK"
  "${OSPRAY_PREFIX}/include/ospray/SDK/modules/cpu"
  "${RKCOMMON_INCLUDE_DIRS}"
  "${EMBREE_PREFIX}/include"
  # BRLCADShared.h is included as "BRLCADShared.h" from the geometry/ dir
  "${CMAKE_CURRENT_SOURCE_DIR}/../modules/pluggableGeometryExample/brl_cad_module/geometry"
)

# Required definitions matching OSPRay's CPU module ISPC build
add_definitions_ispc(
  -DOSPRAY_BEGIN_ISPC_NAMESPACE=
  -DOSPRAY_END_ISPC_NAMESPACE=
  -DSYCL_EXTERNAL=
)

# ---- BRL-CAD ----
set(BRLCAD_PREFIX "/usr/brlcad/rel-7.42.0" CACHE PATH "BRL-CAD install prefix")
find_path(BRLCAD_INCLUDE_DIR brlcad/common.h PATHS "${BRLCAD_PREFIX}/include" REQUIRED)
find_library(BRLCAD_LIBBU bu  PATHS "${BRLCAD_PREFIX}/lib" REQUIRED)
find_library(BRLCAD_LIBBN bn  PATHS "${BRLCAD_PREFIX}/lib" REQUIRED)
find_library(BRLCAD_LIBRT rt  PATHS "${BRLCAD_PREFIX}/lib" REQUIRED)

# ---- Module sources ----
set(MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../modules/pluggableGeometryExample/brl_cad_module")

add_library(ospray_module_brl_cad SHARED
    "${MODULE_DIR}/geometry/brlcad.cpp"
    "${MODULE_DIR}/moduleInit.cpp"
)

target_compile_definitions(ospray_module_brl_cad PRIVATE OBJECTFACTORY_IMPORT)

# Compile brlcad.ispc and add resulting objects to the library.
# ispc_compile prepends CMAKE_CURRENT_SOURCE_DIR, so pass a relative path.
file(RELATIVE_PATH BRLCAD_ISPC_REL
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MODULE_DIR}/geometry/brlcad.ispc"
)
ispc_target_add_sources(ospray_module_brl_cad "${BRLCAD_ISPC_REL}")

target_include_directories(ospray_module_brl_cad PRIVATE
    "${MODULE_DIR}"
    "${MODULE_DIR}/geometry"
    "${BRLCAD_INCLUDE_DIR}"
    "${BRLCAD_INCLUDE_DIR}/brlcad"
    "${BRLCAD_INCLUDE_DIR}/OpenNURBS"
    # OSPRay SDK — all subdirs needed due to flat relative includes
    "${OSPRAY_PREFIX}/include/ospray/SDK"
    "${OSPRAY_PREFIX}/include/ospray/SDK/api"
    "${OSPRAY_PREFIX}/include/ospray/SDK/common"
    "${OSPRAY_PREFIX}/include/ospray/SDK/fb"
    "${OSPRAY_PREFIX}/include/ospray/SDK/modules/cpu"
    "${OSPRAY_PREFIX}/include"
    "${RKCOMMON_INCLUDE_DIRS}"
    "${EMBREE_PREFIX}/include"
    "${OPENVKL_PREFIX}/include"
    "${OPENVKL_PREFIX}/include/openvkl/devices/cpu"
)

target_link_libraries(ospray_module_brl_cad PRIVATE
    ospray::ospray
    ospray::ospray_module_cpu
    rkcommon::rkcommon
    ${BRLCAD_LIBRT}
    ${BRLCAD_LIBBU}
    ${BRLCAD_LIBBN}
)

# Install next to the other OSPRay modules so ospLoadModule finds it
install(TARGETS ospray_module_brl_cad
    LIBRARY DESTINATION "${OSPRAY_PREFIX}/lib"
)
