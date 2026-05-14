## Copyright (c) 2026 BRL-CAD Visualizer contributors
## SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(ospray_module_brl_cad CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Installed OSPRay / rkcommon / embree ----
# Pass these via -D flags or CMAKE_PREFIX_PATH at configure time, e.g.:
#   cmake -DOSPRAY_PREFIX=... -DRKCOMMON_PREFIX=... -DEMBREE_PREFIX=...
set(OSPRAY_PREFIX   "" CACHE PATH "OSPRay install prefix")
set(RKCOMMON_PREFIX "" CACHE PATH "rkcommon install prefix")
set(EMBREE_PREFIX   "" CACHE PATH "Embree install prefix")
set(OPENVKL_PREFIX  "" CACHE PATH "OpenVKL install prefix, required only when OSPRay was built with volumes")

foreach(_pkg OSPRAY_PREFIX RKCOMMON_PREFIX EMBREE_PREFIX)
    if(NOT ${_pkg})
        message(FATAL_ERROR "${_pkg} is not set. Pass it with -D${_pkg}=<path>")
    endif()
endforeach()

list(APPEND CMAKE_PREFIX_PATH "${OSPRAY_PREFIX}" "${RKCOMMON_PREFIX}" "${EMBREE_PREFIX}")
if(OPENVKL_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${OPENVKL_PREFIX}")
endif()

# Find embree and rkcommon first so that osprayUse.cmake can find their targets
find_package(embree 4.3 REQUIRED CONFIG)
find_package(rkcommon REQUIRED CONFIG)
find_package(ispcrt REQUIRED CONFIG)
get_target_property(RKCOMMON_INCLUDE_DIRS rkcommon::rkcommon INTERFACE_INCLUDE_DIRECTORIES)

find_package(ospray REQUIRED CONFIG)   # provides ospray::ospray_module_cpu

set(_optional_openvkl_include_dirs)
if(OSPRAY_ENABLE_VOLUMES)
    if(NOT OPENVKL_PREFIX)
        message(FATAL_ERROR "This OSPRay build has volumes enabled; pass -DOPENVKL_PREFIX=<path>")
    endif()
    list(APPEND _optional_openvkl_include_dirs
        "${OPENVKL_PREFIX}/include"
        "${OPENVKL_PREFIX}/include/openvkl/devices/cpu")
endif()

# ---- ISPC setup (must happen before adding ISPC sources) ----
# The installed OSPRay prefix does not ship ispc.cmake; use the copy in this repo.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/compiler")
include(ispc)

# Match the ISA targets OSPRay was built with
set(ISPC_TARGET_LIST ${OSPRAY_ISPC_TARGET_LIST})

# ISPC include paths for OSPRay SDK headers the .ispc file needs
include_directories_ispc(
  "${OSPRAY_PREFIX}/include"
  "${OSPRAY_PREFIX}/include/ispcrt"
  "${OSPRAY_PREFIX}/include/ospray/SDK"
  "${OSPRAY_PREFIX}/include/ospray/SDK/modules/cpu"
  "${RKCOMMON_INCLUDE_DIRS}"
  "${EMBREE_PREFIX}/include"
  ${_optional_openvkl_include_dirs}
  # BRLCADShared.h is included as "BRLCADShared.h" from the geometry/ dir
  "${CMAKE_CURRENT_SOURCE_DIR}/../modules/pluggableGeometryExample/brl_cad_module/geometry"
)

# Required definitions matching OSPRay's CPU module ISPC build
add_definitions_ispc(
  -DOSPRAY_BEGIN_ISPC_NAMESPACE=
  -DOSPRAY_END_ISPC_NAMESPACE=
  -DSYCL_EXTERNAL=
)

if(OSPRAY_ENABLE_VOLUMES)
    add_definitions_ispc(-DOSPRAY_ENABLE_VOLUMES)
endif()

# ---- BRL-CAD ----
# Pass via -DBRLCAD_PREFIX=<path> at configure time.
set(BRLCAD_PREFIX "" CACHE PATH "BRL-CAD install prefix")
if(NOT BRLCAD_PREFIX)
    message(FATAL_ERROR "BRLCAD_PREFIX is not set. Pass it with -DBRLCAD_PREFIX=<path>")
endif()
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
if(OSPRAY_ENABLE_VOLUMES)
    target_compile_definitions(ospray_module_brl_cad PRIVATE OSPRAY_ENABLE_VOLUMES)
endif()

# Compile brlcad.ispc and add resulting objects to the library.
# ispc_compile prepends CMAKE_CURRENT_SOURCE_DIR, so pass a relative path.
file(RELATIVE_PATH BRLCAD_ISPC_REL
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MODULE_DIR}/geometry/brlcad.ispc"
)
ispc_target_add_sources(
    ospray_module_brl_cad
    "${BRLCAD_ISPC_REL}")

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
    "${OSPRAY_PREFIX}/include/ispcrt"
    "${RKCOMMON_INCLUDE_DIRS}"
    "${EMBREE_PREFIX}/include"
    ${_optional_openvkl_include_dirs}
)

target_link_libraries(ospray_module_brl_cad PRIVATE
    ospray::ospray
    ospray::ospray_module_cpu
    rkcommon::rkcommon
    ispcrt::ispcrt
    ${BRLCAD_LIBRT}
    ${BRLCAD_LIBBU}
    ${BRLCAD_LIBBN}
)

set_target_properties(ospray_module_brl_cad PROPERTIES
    VERSION ${OSPRAY_VERSION}
    SOVERSION ${OSPRAY_SOVERSION}
    BUILD_RPATH "${OSPRAY_PREFIX}/lib;${RKCOMMON_PREFIX}/lib;${BRLCAD_PREFIX}/lib"
    INSTALL_RPATH "${OSPRAY_PREFIX}/lib;${RKCOMMON_PREFIX}/lib;${BRLCAD_PREFIX}/lib"
)

# Install next to the other OSPRay modules so ospLoadModule finds it
install(TARGETS ospray_module_brl_cad
    LIBRARY DESTINATION "${OSPRAY_PREFIX}/lib"
    RUNTIME DESTINATION "${OSPRAY_PREFIX}/lib"
)
