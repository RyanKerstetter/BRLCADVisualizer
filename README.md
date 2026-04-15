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

## Building on Linux

### Prerequisites

Install the following before building:

1. **GCC or Clang** with C++17 support — `sudo apt install build-essential`
2. **CMake 3.20+** — `sudo apt install cmake`
3. **Qt 6.x** — download from [qt.io](https://qt.io) installer, select the `gcc_64` component (e.g. `~/Qt/6.10.2/gcc_64`)
4. **ISPC 1.23.0+** — [ispc.github.io](https://ispc.github.io), download v1.23.0 and add `ispc` to PATH. The build requires exactly 1.23.0 or newer — earlier versions will fail with ISPC compile errors.
   ```bash
   wget https://github.com/ispc/ispc/releases/download/v1.23.0/ispc-v1.23.0-linux.tar.gz && tar -xzf ispc-v1.23.0-linux.tar.gz -C ~/
   export PATH=~/ispc-v1.23.0-linux/bin:$PATH
   ```
5. **BRL-CAD 7.42.0** — install to `/usr/brlcad/rel-7.42.0` or download the pre-built Linux package

---

### Step 1: Clone the repo

```bash
git clone https://github.com/RyanKerstetter/BRLCADVisualizer.git
cd BRLCADVisualizer
git checkout windows+ospray+qt
```

---

### Step 2: Fetch the glfw patch

The superbuild requires a patch file that is not committed to the repo. Fetch it from upstream OSPRay:

```bash
wget -O scripts/superbuild/dependencies/glfw.patch "https://raw.githubusercontent.com/ospray/ospray/master/scripts/superbuild/dependencies/glfw.patch"
```

---

### Step 3: Build OSPRay and dependencies (superbuild)

This builds OSPRay, rkcommon, Embree, OpenVKL, and TBB in one step:

```bash
mkdir ~/ospray-build
cd ~/ospray-build
cmake -DINSTALL_IN_SEPARATE_DIRECTORIES=ON ~/path/to/BRLCADVisualizer/scripts/superbuild
cmake --build . -j$(nproc)
```

After this completes you will have `~/ospray-build/install/` with subdirectories:
`ospray/`, `rkcommon/`, `embree/`, `openvkl/`, `tbb/`

---

### Step 4: Build the BRL-CAD OSPRay module

> **Important:** You must explicitly pass BRL-CAD's own `librt.so` via `-DBRLCAD_LIBRT`. CMake's `find_library` will otherwise pick up the system POSIX `librt`, which is missing BRL-CAD symbols and will cause a runtime load failure.

```bash
mkdir ~/build-brlcad-module
cd ~/build-brlcad-module
cmake ~/path/to/BRLCADVisualizer/brl_cad_standalone \
  -DOSPRAY_PREFIX=~/ospray-build/install/ospray \
  -DRKCOMMON_PREFIX=~/ospray-build/install/rkcommon \
  -DEMBREE_PREFIX=~/ospray-build/install/embree \
  -DOPENVKL_PREFIX=~/ospray-build/install/openvkl \
  -DBRLCAD_PREFIX=/usr/brlcad/rel-7.42.0 \
  -DBRLCAD_LIBRT=/usr/brlcad/rel-7.42.0/lib/librt.so
cmake --build . -j$(nproc)
cmake --install .
```

After installing, create the versioned symlink that rkcommon requires at runtime:

```bash
ln -s ~/ospray-build/install/ospray/lib/libospray_module_brl_cad.so ~/ospray-build/install/ospray/lib/libospray_module_brl_cad.so.3.3.0
```

---

### Step 5: Build the Qt viewer

```bash
mkdir ~/build-qt-viewer
cd ~/build-qt-viewer
cmake ~/path/to/BRLCADVisualizer/QtOsprayViewer/QtOsprayViewer \
  -DCMAKE_PREFIX_PATH=~/Qt/6.10.2/gcc_64 \
  -DOSPRAY_PREFIX=~/ospray-build/install/ospray \
  -DRKCOMMON_PREFIX=~/ospray-build/install/rkcommon \
  -DBRLCAD_PREFIX=/usr/brlcad/rel-7.42.0
cmake --build . -j$(nproc)
```

---

### Run

```bash
LD_LIBRARY_PATH=~/ospray-build/install/ospray/lib:/usr/brlcad/rel-7.42.0/lib:$LD_LIBRARY_PATH ~/build-qt-viewer/IBRT
```

To avoid setting `LD_LIBRARY_PATH` each time, add it to your `~/.bashrc`:

```bash
export LD_LIBRARY_PATH=~/ospray-build/install/ospray/lib:/usr/brlcad/rel-7.42.0/lib:$LD_LIBRARY_PATH
```

Use **File > Open Model** to load a `.g` file. You will be prompted to enter an object name to render (e.g. `all.g`, `component`, or a region name).

---
