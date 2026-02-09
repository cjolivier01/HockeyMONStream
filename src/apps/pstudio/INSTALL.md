# Installation Instructions

## Building from Source

### Prerequisites

Before building, you need to install the following dependencies:

#### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config
sudo apt install qtbase6-dev qt6-base-dev-tools
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
sudo apt install graphviz-dev
```

#### macOS (with Homebrew)

```bash
brew install cmake qt6 gstreamer gst-plugins-base graphviz
```

#### Windows (with MSVC and vcpkg)

1. Install Visual Studio with C++ support
2. Install CMake
3. Install vcpkg
4. Install dependencies:

```bash
vcpkg install qt6 gstreamer gstreamer[plugins-base] graphviz
```

### Building

```bash
# Clone the repository
git clone https://github.com/yourusername/GstPipelineStudio.git
cd GstPipelineStudio

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
cmake --build . --config Release

# Install (optional)
sudo cmake --install .
```

## Packaging

### Creating a Debian Package

```bash
# Install packaging tools
sudo apt install devscripts debhelper

# In the project root
debuild -us -uc
```

### Creating an AppImage (Linux)

```bash
# Install linuxdeploy and Qt plugin
wget https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x linuxdeploy*.AppImage

# Build the project with install target
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr
make
make DESTDIR=AppDir install

# Create AppImage
cd ..
./linuxdeploy-x86_64.AppImage --appdir=build/AppDir --plugin=qt --output=appimage
```

### Creating a DMG (macOS)

```bash
# In the build directory
cpack -G DragNDrop
```

### Creating an NSIS Installer (Windows)

```bash
# In the build directory
cpack -G NSIS
```
