cmake_minimum_required(VERSION 3.20)
project(QtOsprayViewer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64")
else()
    list(APPEND CMAKE_PREFIX_PATH "/home/adeeb/Qt/6.10.2/gcc_64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenGL REQUIRED)

if(WIN32)
    set(OSPRAY_PREFIX "C:/ospray/build/install/ospray" CACHE PATH "OSPRay prefix")
    set(RKCOMMON_PREFIX "C:/ospray/build/install/rkcommon" CACHE PATH "rkcommon prefix")
else()
    set(OSPRAY_PREFIX "/home/adeeb/workspace/csce482/build/install/ospray" CACHE PATH "OSPRay prefix")
    set(RKCOMMON_PREFIX "/home/adeeb/workspace/csce482/build/install/rkcommon" CACHE PATH "rkcommon prefix")
endif()
list(APPEND CMAKE_PREFIX_PATH "${OSPRAY_PREFIX}" "${RKCOMMON_PREFIX}")

find_package(ospray REQUIRED CONFIG)

# BRL-CAD
set(BRLCAD_PREFIX "/usr/brlcad/rel-7.42.0" CACHE PATH "BRL-CAD prefix")
find_path(BRLCAD_INCLUDE_DIR brlcad/rt/db4.h PATHS "${BRLCAD_PREFIX}/include" REQUIRED)
find_library(BRLCAD_RT_LIBRARY  NAMES rt  PATHS "${BRLCAD_PREFIX}/lib" REQUIRED)
find_library(BRLCAD_BU_LIBRARY  NAMES bu  PATHS "${BRLCAD_PREFIX}/lib" REQUIRED)

qt_add_executable(QtOsprayViewer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    renderwidget.cpp
    renderwidget.h
    ospraybackend.cpp
    ospraybackend.h
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
)

target_link_libraries(QtOsprayViewer PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    ospray::ospray
    OpenGL::GL
    ${BRLCAD_RT_LIBRARY}
    ${BRLCAD_BU_LIBRARY}
)

if(WIN32)
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "C:/ospray/build/install/ospray/bin"
            "$<TARGET_FILE_DIR:QtOsprayViewer>"
    )
    add_custom_command(TARGET QtOsprayViewer POST_BUILD
        COMMAND "C:/Qt/6.10.2/msvc2022_64/bin/windeployqt.exe"
                "$<TARGET_FILE:QtOsprayViewer>"
        COMMENT "Running windeployqt..."
    )
endif()