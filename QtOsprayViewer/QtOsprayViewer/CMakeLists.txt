cmake_minimum_required(VERSION 3.20)
project(IBRT VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt: add its prefix to CMAKE_PREFIX_PATH, e.g.:
#   cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/msvc2022_64 ...
# or set Qt6_DIR directly.
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenGL REQUIRED)

# ---- OSPRay / rkcommon / Embree / OpenVKL ----
# Pass via -D flags at configure time, e.g.:
#   -DOSPRAY_PREFIX=... -DRKCOMMON_PREFIX=... -DEMBREE_PREFIX=... -DOPENVKL_PREFIX=...
set(OSPRAY_PREFIX   "" CACHE PATH "OSPRay prefix")
set(RKCOMMON_PREFIX "" CACHE PATH "rkcommon prefix")

foreach(_pkg OSPRAY_PREFIX RKCOMMON_PREFIX)
    if(NOT ${_pkg})
        message(FATAL_ERROR "${_pkg} is not set. Pass it with -D${_pkg}=<path>")
    endif()
endforeach()

list(APPEND CMAKE_PREFIX_PATH "${OSPRAY_PREFIX}" "${RKCOMMON_PREFIX}")
find_package(ospray REQUIRED CONFIG)

if(WIN32)
    set(EMBREE_PREFIX  "" CACHE PATH "Embree prefix")
    set(OPENVKL_PREFIX "" CACHE PATH "OpenVKL prefix")
    set(TBB_ROOT       "" CACHE PATH "TBB root")

    foreach(_pkg EMBREE_PREFIX OPENVKL_PREFIX TBB_ROOT)
        if(NOT ${_pkg})
            message(FATAL_ERROR "${_pkg} is not set. Pass it with -D${_pkg}=<path>")
        endif()
    endforeach()

    find_package(TBB REQUIRED COMPONENTS tbb CONFIG
        HINTS "${TBB_ROOT}/lib/cmake/TBB" "${TBB_ROOT}/lib/cmake")
endif()

# ---- BRL-CAD ----
set(BRLCAD_PREFIX "" CACHE PATH "BRL-CAD prefix")
if(NOT BRLCAD_PREFIX)
    message(FATAL_ERROR "BRLCAD_PREFIX is not set. Pass it with -DBRLCAD_PREFIX=<path>")
endif()
find_path(BRLCAD_INCLUDE_DIR brlcad/rt/db4.h PATHS "${BRLCAD_PREFIX}/include" REQUIRED)
find_library(BRLCAD_RT_LIBRARY NAMES rt librt PATHS "${BRLCAD_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(BRLCAD_BU_LIBRARY NAMES bu libbu PATHS "${BRLCAD_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)

qt_add_executable(QtOsprayViewer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    renderwidget.cpp
    renderwidget.h
    ospraybackend.cpp
    ospraybackend.h
    interactioncontroller.cpp
    interactioncontroller.h
)

set_target_properties(QtOsprayViewer PROPERTIES
    OUTPUT_NAME "IBRT"
)

target_sources(QtOsprayViewer PRIVATE
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(QtOsprayViewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
    "${OSPRAY_PREFIX}/include"
    "${RKCOMMON_PREFIX}/include"
    "${BRLCAD_INCLUDE_DIR}"
    "${BRLCAD_INCLUDE_DIR}/brlcad"
    "${BRLCAD_INCLUDE_DIR}/OpenNURBS"
    $<$<BOOL:${WIN32}>:${TBB_ROOT}/include>
)

target_link_libraries(QtOsprayViewer PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    ospray::ospray
    OpenGL::GL
    ${BRLCAD_RT_LIBRARY}
    ${BRLCAD_BU_LIBRARY}
    $<$<BOOL:${WIN32}>:TBB::tbb>
)

if(WIN32)
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${OSPRAY_PREFIX}/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
        COMMENT "Copying OSPRay runtime DLLs...")

    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${RKCOMMON_PREFIX}/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
        COMMENT "Copying rkcommon runtime DLLs...")

    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EMBREE_PREFIX}/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
        COMMENT "Copying Embree runtime DLLs...")

    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${OPENVKL_PREFIX}/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
        COMMENT "Copying OpenVKL runtime DLLs...")

    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BRLCAD_PREFIX}/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
        COMMENT "Copying BRL-CAD runtime DLLs...")

    # Force the correct rkcommon.dll and embree4.dll — OpenVKL ships older
    # mismatched versions that get copied last and would override them.
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${RKCOMMON_PREFIX}/bin/rkcommon.dll"
            "$<TARGET_FILE_DIR:QtOsprayViewer>/rkcommon.dll"
        COMMENT "Restoring correct rkcommon.dll...")
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${EMBREE_PREFIX}/bin/embree4.dll"
            "$<TARGET_FILE_DIR:QtOsprayViewer>/embree4.dll"
        COMMENT "Restoring correct embree4.dll...")

    # Remove any CRT DLLs copied from third-party packages — always use the
    # system-installed versions to avoid version mismatches with Qt / MSVC.
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:QtOsprayViewer>/vcruntime140.dll"
            "$<TARGET_FILE_DIR:QtOsprayViewer>/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:QtOsprayViewer>/msvcp140.dll"
        COMMENT "Removing bundled CRT DLLs (use system versions)...")

    # Find and run windeployqt from the Qt installation used at build time.
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND "${WINDEPLOYQT}" "$<TARGET_FILE:QtOsprayViewer>"
        COMMENT "Running windeployqt...")
endif()
