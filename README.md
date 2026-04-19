# BRLCADVisualizer

`BRLCADVisualizer` is a Windows-focused interactive raytracing application for viewing BRL-CAD `.g` databases and standard mesh files through Intel OSPRay. This repository is not just a small viewer app. It also contains:

- a full OSPRay source tree
- a standalone build for a custom `ospray_module_brl_cad` plugin
- a Qt desktop viewer
- a render worker process used to keep expensive rendering work off the UI thread
- superbuild scripts for fetching and building OSPRay dependencies

If you are trying to understand "what this repo actually is", the short answer is:

1. OSPRay provides the rendering engine.
2. A custom BRL-CAD geometry module teaches OSPRay how to render BRL-CAD geometry.
3. The Qt viewer provides the desktop UI.
4. A helper worker process performs rendering and scene loading out-of-process.

This README is intended to be the single detailed reference for building, running, and understanding the project.

## What The Project Does

The viewer can load:

- BRL-CAD `.g` databases
- OBJ meshes
- menu placeholders exist for some other model extensions, but the implemented paths are BRL-CAD and OBJ

When a BRL-CAD file is loaded:

- the app enumerates selectable top-level objects from the database
- the user selects a specific object or region to render
- the custom OSPRay BRL-CAD module loads that object into OSPRay scene data
- the viewer progressively renders the result in a Qt `QOpenGLWidget`

When an OBJ file is loaded:

- the viewer uses TinyObjLoader
- the mesh is converted into OSPRay geometry and models
- the same progressive rendering pipeline is used

## High-Level Architecture

The main moving parts are:

- `QtOsprayViewer/QtOsprayViewer`
  This is the desktop application and worker executable source.
- `brl_cad_standalone`
  This builds the custom `ospray_module_brl_cad.dll`.
- `modules/pluggableGeometryExample/brl_cad_module`
  This is the BRL-CAD geometry module implementation used by the standalone module build.
- `scripts/superbuild`
  This builds OSPRay and its third-party dependencies into an install tree.
- `ospray`, `modules`, `apps`, `cmake`
  These are the OSPRay source tree and related modules included in the repo.

### Runtime Data Flow

At runtime the typical flow is:

1. `IBRT` starts.
2. OSPRay is initialized and the CPU device is created.
3. The main Qt window starts.
4. The UI launches `IBRTRenderWorker.exe`.
5. The UI and worker connect over a Windows named pipe.
6. The user opens a scene.
7. The worker or local backend loads the scene into OSPRay.
8. Camera and renderer settings are pushed to the backend.
9. Frames are progressively rendered.
10. The UI displays the latest image and overlays controls/stats via ImGui.

### Why There Is A Worker Process

The worker exists for responsiveness and isolation.

- scene loading can be expensive
- raytracing can stall the UI if performed directly on the main thread
- a separate process makes it easier to recover from worker crashes
- the UI can restart the worker and replay scene/camera state

The viewer still contains a local `OsprayBackend`, but the intended path is to use the worker when available.

## Repository Layout

This is the practical map of the repository.

### Main Viewer

- `QtOsprayViewer/QtOsprayViewer/main.cpp`
  Application entry point. Initializes OSPRay, installs Windows crash/error helpers, starts Qt.
- `QtOsprayViewer/QtOsprayViewer/mainwindow.cpp`
  Main application window, menus, demo model selection, BRL-CAD object selection.
- `QtOsprayViewer/QtOsprayViewer/renderwidget.cpp`
  Main viewport widget. Handles input, progressive rendering, overlay UI, camera movement, async scene loading.
- `QtOsprayViewer/QtOsprayViewer/ospraybackend.cpp`
  Core rendering backend. Owns OSPRay renderer/camera/world/framebuffers and progressive render logic.
- `QtOsprayViewer/QtOsprayViewer/renderworkerclient.cpp`
  UI-side IPC client that launches and communicates with the worker.
- `QtOsprayViewer/QtOsprayViewer/worker_main.cpp`
  Worker process main loop.
- `QtOsprayViewer/QtOsprayViewer/worker_ipc.cpp`
  Named-pipe message framing and transport helpers.
- `QtOsprayViewer/QtOsprayViewer/interactioncontroller.cpp`
  Maps mouse/modifier combinations into translate/rotate/scale actions.

### BRL-CAD OSPRay Module

- `brl_cad_standalone/CMakeLists.txt`
  Standalone build recipe for `ospray_module_brl_cad`.
- `modules/pluggableGeometryExample/brl_cad_module`
  BRL-CAD geometry plugin sources.

### OSPRay And Dependencies

- `ospray`
  OSPRay core API and implementation.
- `modules`
  OSPRay CPU, denoiser, MPI, and example modules.
- `apps`
  OSPRay sample/tutorial applications.
- `scripts/superbuild`
  Convenience build system for OSPRay dependencies.

### Documentation And Misc

- `doc`
  OSPRay/related documentation content.
- `CHANGELOG.md`
  Historical change log.
- `STYLEGUIDE.md`
  Formatting and style notes.

## Build Outputs You Actually Need

To run the viewer successfully on Windows, you need all of these pieces aligned:

1. BRL-CAD build or install
2. OSPRay install
3. rkcommon install
4. Embree install
5. OpenVKL install
6. TBB runtime
7. Qt runtime
8. `ospray_module_brl_cad.dll`
9. `IBRT.exe`
10. `IBRTRenderWorker.exe`

The CMake files in this repo are already set up to copy most of the runtime DLLs into the output folder after build.

## Platform Expectations

The current project is clearly optimized for Windows.

Reasons:

- the worker uses Windows named pipes
- the viewer deploys Windows DLLs after build
- `windeployqt` is used for Qt packaging
- crash dump generation uses `dbghelp`
- some code paths explicitly say the worker currently supports Windows only

You may be able to build portions of the repo elsewhere, but the complete viewer workflow described here is aimed at Windows.

## Toolchain And Prerequisites

You should have the following installed:

1. Visual Studio 2022
   Required workload: `Desktop development with C++`
2. CMake 3.20 or newer
3. Qt 6.x
   Example: `C:\Qt\6.11.0\msvc2022_64`
4. ISPC
   Required to build the BRL-CAD OSPRay module
5. BRL-CAD 7.42.0 or compatible build tree
6. Git

You should also make sure:

- `cmake` is on `PATH`
- `ispc.exe` is on `PATH`
- you are consistently using the same MSVC toolchain across dependencies

## Build Order

The build order matters. Use this order:

1. Build BRL-CAD
2. Build OSPRay and dependencies with the superbuild
3. Build the standalone `ospray_module_brl_cad`
4. Build the Qt viewer

If you skip step 3, the viewer will start but BRL-CAD scene loading will fail because `ospLoadModule("brl_cad")` will not be able to find the module.

## Step 1: Build BRL-CAD

Example:

```powershell
cd C:\brlcad-src
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Important outcome:

- remember the BRL-CAD build/install prefix you will pass as `BRLCAD_PREFIX`

This prefix must expose:

- `include\brlcad`
- `lib\rt.lib` or equivalent
- `lib\bu.lib`
- runtime DLLs under `bin`

## Step 2: Build OSPRay And Dependencies With The Superbuild

This repository already includes `scripts/superbuild`, which is the recommended way to get a matching OSPRay dependency stack.

Example:

```powershell
mkdir C:\ospray-build
cd C:\ospray-build
cmake -G "Visual Studio 17 2022" -A x64 `
  -DINSTALL_IN_SEPARATE_DIRECTORIES=ON `
  C:\BRLCADVisualizer\scripts\superbuild
cmake --build . --config Release
```

Expected result:

- `C:\ospray-build\install\ospray`
- `C:\ospray-build\install\rkcommon`
- `C:\ospray-build\install\embree`
- `C:\ospray-build\install\openvkl`
- `C:\ospray-build\install\tbb`

### Important Superbuild Notes

- `INSTALL_IN_SEPARATE_DIRECTORIES=ON` is useful because the Qt viewer CMake expects separate prefixes for multiple dependencies.
- The superbuild can also build more than you need, but for this project the key outputs are OSPRay, rkcommon, Embree, OpenVKL, and TBB.
- The included `scripts/superbuild/README.md` describes other options such as building TBB or Embree from source.

## Step 3: Build `ospray_module_brl_cad`

This produces the custom geometry module that OSPRay loads at runtime for BRL-CAD scenes.

Example:

```powershell
mkdir C:\build-brlcad-module
cd C:\build-brlcad-module
cmake C:\BRLCADVisualizer\brl_cad_standalone -G "Visual Studio 17 2022" -A x64 `
  -DOSPRAY_PREFIX=C:\ospray-build\install\ospray `
  -DRKCOMMON_PREFIX=C:\ospray-build\install\rkcommon `
  -DEMBREE_PREFIX=C:\ospray-build\install\embree `
  -DOPENVKL_PREFIX=C:\ospray-build\install\openvkl `
  -DBRLCAD_PREFIX=C:\path\to\brlcad\build
cmake --build . --config Release
cmake --install . --config Release
```

What this build does:

- builds a shared library named `ospray_module_brl_cad`
- compiles ISPC sources needed by the BRL-CAD geometry path
- links against OSPRay, `ospray_module_cpu`, rkcommon, and BRL-CAD libraries
- installs the DLL into `${OSPRAY_PREFIX}/lib` so `ospLoadModule("brl_cad")` can find it

### Why The Module Matters

OSPRay itself does not natively understand BRL-CAD databases. The module is the bridge.

Without it:

- OBJ loading can still work
- BRL-CAD `.g` loading will fail

## Step 4: Build The Qt Viewer

Example:

```powershell
mkdir C:\build-qt-viewer
cd C:\build-qt-viewer
cmake C:\BRLCADVisualizer\QtOsprayViewer\QtOsprayViewer -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.11.0\msvc2022_64 `
  -DOSPRAY_PREFIX=C:\ospray-build\install\ospray `
  -DRKCOMMON_PREFIX=C:\ospray-build\install\rkcommon `
  -DEMBREE_PREFIX=C:\ospray-build\install\embree `
  -DOPENVKL_PREFIX=C:\ospray-build\install\openvkl `
  -DTBB_ROOT=C:\ospray-build\install\tbb `
  -DBRLCAD_PREFIX=C:\path\to\brlcad\build
cmake --build . --config Release
```

If the BRL-CAD module DLL is not in the expected auto-detected locations, provide it explicitly:

```powershell
cmake C:\BRLCADVisualizer\QtOsprayViewer\QtOsprayViewer -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.11.0\msvc2022_64 `
  -DOSPRAY_PREFIX=C:\ospray-build\install\ospray `
  -DRKCOMMON_PREFIX=C:\ospray-build\install\rkcommon `
  -DEMBREE_PREFIX=C:\ospray-build\install\embree `
  -DOPENVKL_PREFIX=C:\ospray-build\install\openvkl `
  -DTBB_ROOT=C:\ospray-build\install\tbb `
  -DBRLCAD_PREFIX=C:\path\to\brlcad\build `
  -DBRLCAD_OSPRAY_MODULE_DLL=C:\full\path\to\ospray_module_brl_cad.dll
```

### What The Viewer CMake Does For You

The viewer CMake is more than a simple compile step. It also:

- builds the UI executable
- builds the worker executable
- copies OSPRay DLLs
- copies rkcommon DLLs
- copies Embree DLLs
- copies OpenVKL DLLs
- copies BRL-CAD DLLs
- copies TBB DLLs
- copies the BRL-CAD module DLL when available
- runs `windeployqt`
- copies `IBRTRenderWorker.exe` next to the UI executable
- removes bundled CRT DLLs that could conflict with the system-installed MSVC runtime

### Output Names

The Qt viewer target is `QtOsprayViewer`, but its output executable name is set to:

- `IBRT.exe`

The worker executable is:

- `IBRTRenderWorker.exe`

## Running The Application

After build:

```powershell
cd C:\build-qt-viewer\Release
.\IBRT.exe
```

On startup:

- OSPRay initializes
- the CPU device is created
- the main window opens
- the worker process starts
- the app may load a startup demo if one is found

## Loading Scenes

### BRL-CAD

Use:

- `File > Open Model...`
- choose a `.g` file
- select the desired object from the hierarchy dialog

The code prefers these object names when present:

- `all.g`
- `all`

If present, demo models may also appear under:

- `File > Demo Models`

The app looks for demo models in:

- `models` next to the application binary
- or `share/db` under `BRLCAD_INSTALL_PREFIX`

### OBJ

Use:

- `File > Open Model...`
- choose an `.obj` file

OBJ loading does not require the BRL-CAD module, but still requires the OSPRay runtime stack.

## Viewer Controls And Interaction Model

The viewer supports two camera modes:

- `Orbit`
- `Fly`

It also supports two world up-axis conventions:

- `Z-Up`
- `Y-Up`

### Orbit Mode

Orbit mode keeps a pivot around the scene center.

- rotating moves the camera around the pivot
- panning moves the pivot
- zoom changes camera distance

This is the default mode and is best for inspecting a loaded model.

### Fly Mode

Fly mode treats the camera like a free-moving observer.

- camera orientation is represented by yaw/pitch
- movement keys translate through the scene

This is better for interior or first-person exploration.

### Gesture Classification

Mouse gestures are abstracted through `InteractionController`.

The code classifies a combination of:

- mouse buttons
- `Shift`
- `Ctrl`
- `Alt`

into:

- translate
- rotate
- scale
- optional axis constraints

### Overlay UI

The viewer uses ImGui to draw a control overlay on top of the rendered image.

The overlay includes:

- stats
- render path information
- frame times
- accumulated frame counts
- renderer selection
- other runtime controls

## Renderers

The viewer exposes at least these renderer modes:

- `ao`
- `scivis`
- `pathtracer`

### `ao`

Fast, simple ambient occlusion rendering.

### `scivis`

General scientific visualization style renderer with default lighting.

### `pathtracer`

Higher quality physically-based rendering. Slower, but more realistic lighting.

The backend applies default light setups when scenes do not supply their own lighting.

## Progressive Rendering Model

Rendering is progressive, not single-shot.

That means:

- the app starts with lower-cost preview passes
- it refines over time
- once interaction stops, the renderer may accumulate more full-quality frames

`OsprayBackend` manages:

- renderer selection
- camera state
- scene world and instances
- framebuffers
- current render future
- progressive scale ladder
- accumulation behavior
- watchdog timeouts
- dynamic AO/sample backoff

### Progressive Scales

The backend uses a progressive scale ladder:

- `16`
- `8`
- `4`
- `2`
- `1`

It starts coarse, then refines to full resolution.

### Interaction-Aware Quality Reduction

When the user is actively manipulating the camera:

- the backend can temporarily reduce quality
- a preview render request may preempt a more expensive in-flight render
- accumulation is reset after camera changes

### Watchdog

The backend tracks slow frames and has a watchdog timeout.

This is used to:

- cancel frames that are taking too long
- reduce AO cost if the renderer becomes too expensive for interactive use

## Worker IPC Protocol

The viewer and worker communicate using a Windows named pipe.

Shared protocol code lives in:

- `worker_ipc.h`
- `worker_ipc.cpp`

### Message Framing

Each message has:

- a magic number
- protocol version
- message type
- request ID
- payload size
- payload bytes

### Main Message Types

- `Ping`
- `Pong`
- `Shutdown`
- `ListBrlcadObjects`
- `BrlcadObjectList`
- `LoadObj`
- `LoadBrlcad`
- `LoadResult`
- `Resize`
- `SetCamera`
- `ResetAccumulation`
- `RequestFrame`
- `FrameData`
- `SetRenderer`
- `SetRenderSettings`

### Why Request IDs Matter

The protocol is synchronous and request/response based.

- the client sends one request
- the worker returns one matching response
- request IDs ensure the response corresponds to the correct request

## Main Code Components In Detail

### `main.cpp`

Responsibilities:

- install stderr filtering on Windows
- install unhandled-exception crash dump handling
- initialize OSPRay
- create the CPU device
- load the OSPRay CPU module
- start the Qt event loop

### `MainWindow`

Responsibilities:

- own the main render widget
- own the render worker client
- create menus
- open models
- choose BRL-CAD objects
- expose demo models
- recover from worker disconnects

### `RenderWidget`

Responsibilities:

- present the rendered image
- handle mouse/keyboard input
- manage orbit/fly camera state
- start asynchronous scene loads
- maintain scene metadata
- show the ImGui overlay
- push camera/renderer state to the backend or worker

### `OsprayBackend`

Responsibilities:

- own OSPRay `Renderer`, `Camera`, `World`, `Instance`, `FrameBuffer`, and `Future`
- load OBJ scenes
- load BRL-CAD scenes
- compute bounds
- manage progressive rendering
- manage accumulation
- manage quality adaptation and watchdog behavior

### `RenderWorkerClient`

Responsibilities:

- launch the worker executable
- connect to the named pipe
- send render-control requests
- request frames
- decode binary payloads returned by the worker

### `worker_main.cpp`

Responsibilities:

- create the named pipe
- initialize OSPRay in the worker process
- create a single `OsprayBackend`
- service incoming requests
- serialize responses back to the UI

## CMake Variables You Will Most Likely Use

### Viewer Configure Variables

- `CMAKE_PREFIX_PATH`
  Qt prefix, usually enough for `find_package(Qt6 ...)`
- `OSPRAY_PREFIX`
- `RKCOMMON_PREFIX`
- `EMBREE_PREFIX`
- `OPENVKL_PREFIX`
- `TBB_ROOT`
- `BRLCAD_PREFIX`
- `BRLCAD_OSPRAY_MODULE_DLL`
  Optional explicit full path to the module DLL
- `BRLCAD_OSPRAY_MODULE_DEPLOY_DIR`
  Optional extra directory of dependent DLLs to copy next to the viewer

### Standalone BRL-CAD Module Configure Variables

- `OSPRAY_PREFIX`
- `RKCOMMON_PREFIX`
- `EMBREE_PREFIX`
- `OPENVKL_PREFIX`
- `BRLCAD_PREFIX`

## Typical Output Folder Contents

After a successful viewer build, the output directory should contain at least:

- `IBRT.exe`
- `IBRTRenderWorker.exe`
- OSPRay DLLs
- rkcommon DLLs
- Embree DLLs
- OpenVKL DLLs
- BRL-CAD DLLs
- Qt DLLs
- `ospray_module_brl_cad.dll`

If the app starts but cannot load BRL-CAD scenes, the first thing to check is whether `ospray_module_brl_cad.dll` is present next to the executable or in a location where OSPRay can load it.

## Troubleshooting

### The App Starts But BRL-CAD Scene Loading Fails

Likely causes:

- `ospray_module_brl_cad.dll` was not built
- the module DLL was not copied next to the executable
- the module DLL depends on other missing DLLs
- `BRLCAD_PREFIX` points to the wrong BRL-CAD build/install tree

What to check:

1. Verify `ospray_module_brl_cad.dll` exists.
2. Verify it is deployed next to `IBRT.exe` and `IBRTRenderWorker.exe`, or in the expected OSPRay module path.
3. Reconfigure the viewer with `-DBRLCAD_OSPRAY_MODULE_DLL=...`.

### The Worker Fails To Start

Likely causes:

- missing runtime DLLs
- `IBRTRenderWorker.exe` was not copied next to the viewer
- a dependency mismatch between OSPRay/rkcommon/Embree/OpenVKL/TBB

What to check:

1. Confirm `IBRTRenderWorker.exe` exists next to `IBRT.exe`.
2. Confirm the required DLLs are present in the same output folder.
3. Launch `IBRTRenderWorker.exe` manually from a terminal if needed to inspect basic startup failure behavior.

### The App Runs But Rendering Is Slow Or Stalls

Possible reasons:

- `pathtracer` is selected
- AO/pixel sample settings are too high
- the scene is very large or complex
- the worker is repeatedly hitting watchdog limits

What to do:

- switch to `ao` or `scivis`
- reduce custom settings if exposed in the overlay
- verify the worker stays connected

### CRT Or DLL Conflicts

The CMake intentionally removes bundled CRT DLLs copied by some third-party packages because they can conflict with the Qt/MSVC runtime expected on the machine.

If you see odd runtime loader issues:

- make sure the correct Visual Studio runtime is installed
- rebuild with a consistent toolchain
- verify that stale DLLs from older builds are not left in the output folder

### `rkcommon.dll` Or `embree4.dll` Mismatch

The CMake explicitly recopies:

- `rkcommon.dll`
- `embree4.dll`

after other third-party copies because some packages may overwrite them with incompatible versions.

If you see startup or symbol errors, re-check those DLLs first.

## Development Notes

### The Repo Contains OSPRay Source

This repo includes a large OSPRay tree. That does not mean you must build the whole repo from the root `CMakeLists.txt` to use the viewer.

For the viewer workflow, the practical builds are:

- `scripts/superbuild`
- `brl_cad_standalone`
- `QtOsprayViewer/QtOsprayViewer`

The root `CMakeLists.txt` is the OSPRay project root.

### Why Some Files Look Like OSPRay Internals

Because they are. The repository includes OSPRay source and modules, and the BRL-CAD integration reuses OSPRay SDK/module internals to stay ABI-compatible with the CPU runtime.

### ISPC Requirement

The BRL-CAD module build uses ISPC sources. If ISPC is not installed or not on `PATH`, the module build is expected to fail.

## Suggested First-Time Build Recipe

If you want the simplest practical sequence:

1. Build BRL-CAD.
2. Run the OSPRay superbuild.
3. Build and install `ospray_module_brl_cad`.
4. Build the Qt viewer.
5. Run `IBRT.exe` from the viewer build output directory.

## Suggested First-Time Verification Checklist

Before debugging deeper issues, verify:

1. `IBRT.exe` launches.
2. `IBRTRenderWorker.exe` launches automatically.
3. The status bar says the worker connected.
4. A demo model loads or an OBJ loads.
5. A BRL-CAD `.g` file shows an object picker.
6. Selecting a BRL-CAD object produces an image.
7. Switching between `ao`, `scivis`, and `pathtracer` changes the output.

## Where To Extend The Project

If you want to modify behavior, these are the most important starting points:

- add new UI/menu behavior:
  `QtOsprayViewer/QtOsprayViewer/mainwindow.cpp`
- change camera/input behavior:
  `QtOsprayViewer/QtOsprayViewer/renderwidget.cpp`
- change render scheduling or quality adaptation:
  `QtOsprayViewer/QtOsprayViewer/ospraybackend.cpp`
- change worker messages:
  `QtOsprayViewer/QtOsprayViewer/worker_ipc.h`
  `QtOsprayViewer/QtOsprayViewer/worker_main.cpp`
  `QtOsprayViewer/QtOsprayViewer/renderworkerclient.cpp`
- change BRL-CAD geometry integration:
  `modules/pluggableGeometryExample/brl_cad_module`
  `brl_cad_standalone/CMakeLists.txt`

## Licensing And Third-Party Material

See:

- `LICENSE.txt`
- `third-party-programs.txt`
- other `third-party-programs-*.txt` files

for the licensing and attribution details associated with included third-party code and dependencies.

## Final Summary

This repository is a hybrid of:

- OSPRay source
- a BRL-CAD-to-OSPRay geometry module
- a Qt desktop viewer
- a worker-based rendering architecture

The shortest reliable mental model is:

- build BRL-CAD
- build OSPRay dependencies
- build the BRL-CAD module
- build the viewer
- run `IBRT.exe`
- load `.g` or `.obj` scenes

If you are maintaining the project, the files that matter most for day-to-day work are:

- `QtOsprayViewer/QtOsprayViewer/mainwindow.cpp`
- `QtOsprayViewer/QtOsprayViewer/renderwidget.cpp`
- `QtOsprayViewer/QtOsprayViewer/ospraybackend.cpp`
- `QtOsprayViewer/QtOsprayViewer/renderworkerclient.cpp`
- `QtOsprayViewer/QtOsprayViewer/worker_main.cpp`
- `brl_cad_standalone/CMakeLists.txt`
- `modules/pluggableGeometryExample/brl_cad_module`
