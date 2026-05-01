# Define an option to enable/disable this module.
#
# Though not actually required we guard each module 'moduleName' with
# a OSPRAY_MODULE_MODULENAME flag that enables/disables its building.
#
cmake_dependent_option(
  OSPRAY_MODULE_BILINEAR_PATCH
  "Build pluggable geometry example module  (a Bilinear Patch)"
  OFF
  OSPRAY_MODULE_CPU
  OFF
)

if (NOT OSPRAY_MODULE_BILINEAR_PATCH)
  set(_build_bilinear OFF)
else()
  set(_build_bilinear ON)
endif()

cmake_dependent_option(
  OSPRAY_MODULE_BRL_CAD
  "Build pluggable BRL-CAD geometry module"
  OFF
  OSPRAY_MODULE_CPU
  OFF
)

if (_build_bilinear)
  # build the actual new ospray geometry type.  since this plug in
  # directly into the ospray core, we'll put it into a
  # '<module>/ospray' subdirectory.
  #
  # This subdirectory will eventually build the
  # libospray_module_<moduleName>.so library that defiens the actual
  # core geometry type.
  #
  add_subdirectory(ospray)
endif()

if (OSPRAY_MODULE_BRL_CAD)
  add_subdirectory(brl_cad_module)
endif()
