# BRLCADVisualizer

A Qt + Intel OSPRay viewer for BRL-CAD `.g` files. Renders BRL-CAD geometry using OSPRay's raytracing pipeline via a custom OSPRay geometry module.

---

## Building on Windows

### Prerequisites

Install the following before building:

1. **Visual Studio 2022** — with the "Desktop development with C++" workload
2. **CMake 3.20+** — [cmake.org](https://cmake.org), add to PATH
3. **Qt 6.x** — [qt.io](https://qt.io) installer, select the `msvc2022_64` component (e.g. `C:/Qt/6.11.0/msvc2022_64`)
4. **ISPC** — [ispc.github.io](https://ispc.github.io), add `ispc.exe` to PATH (required to compile the BRL-CAD OSPRay module)
5. **BRL-CAD 7.42.0** — download the source and build:
   ```powershell
   cd C:\brlcad-src
   mkdir build; cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   cmake --build . --config Release
   ```
   Note the build directory path — you will need it in later steps.

---

### Step 1: Clone the repo

```powershell
git clone https://github.com/RyanKerstetter/BRLCADVisualizer.git
cd BRLCADVisualizer
git checkout Windows+ospray+qt
```

---

### Step 2: Build OSPRay and dependencies (superbuild)

This builds OSPRay, rkcommon, Embree, OpenVKL, and TBB in one step:

```powershell
mkdir C:\ospray-build
cd C:\ospray-build
cmake -G "Visual Studio 17 2022" -A x64 -DINSTALL_IN_SEPARATE_DIRECTORIES=ON C:\path\to\BRLCADVisualizer\scripts\superbuild
cmake --build . --config Release
```

After this completes you will have `C:\ospray-build\install\` with subdirectories:
`ospray\`, `rkcommon\`, `embree\`, `openvkl\`, `tbb\`

---

### Step 3: Build the BRL-CAD OSPRay module

```powershell
mkdir C:\build-brlcad-module
cd C:\build-brlcad-module
cmake C:\path\to\BRLCADVisualizer\brl_cad_standalone -G "Visual Studio 17 2022" -A x64 `
  -DOSPRAY_PREFIX=C:\ospray-build\install\ospray `
  -DRKCOMMON_PREFIX=C:\ospray-build\install\rkcommon `
  -DEMBREE_PREFIX=C:\ospray-build\install\embree `
  -DOPENVKL_PREFIX=C:\ospray-build\install\openvkl `
  -DBRLCAD_PREFIX=C:\path\to\brlcad\build
cmake --build . --config Release
cmake --install . --config Release
```

`cmake --install` copies `ospray_module_brl_cad.dll` directly into OSPRay's lib folder so `ospLoadModule` can find it automatically.

---

### Step 4: Build the Qt viewer

```powershell
mkdir C:\build-qt-viewer
cd C:\build-qt-viewer
cmake C:\path\to\BRLCADVisualizer\QtOsprayViewer\QtOsprayViewer -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.11.0\msvc2022_64 `
  -DOSPRAY_PREFIX=C:\ospray-build\install\ospray `
  -DRKCOMMON_PREFIX=C:\ospray-build\install\rkcommon `
  -DEMBREE_PREFIX=C:\ospray-build\install\embree `
  -DOPENVKL_PREFIX=C:\ospray-build\install\openvkl `
  -DTBB_ROOT=C:\ospray-build\install\tbb `
  -DBRLCAD_PREFIX=C:\path\to\brlcad\build
cmake --build . --config Release
```

The post-build steps automatically copy all required DLLs (OSPRay, rkcommon, Embree, OpenVKL, BRL-CAD, Qt) into the output directory.

---

### Run

```powershell
cd C:\path\to\BRLCADVisualizer\build-qt-viewer\Release
.\QtOsprayViewer.exe
```

Use **File > Open Model** to load a `.g` file. You will be prompted to enter an object name to render (e.g. `all.g`, `component`, or a region name).

---
