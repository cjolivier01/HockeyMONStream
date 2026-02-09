# GstPipelineStudio

A C++ application for visually creating and testing GStreamer pipelines. This project is a C++ port of the Rust application [GstPipelineStudio](https://github.com/dabrain34/GstPipelineStudio).

## Features

- Visual editor for creating GStreamer pipelines
- Browse and add GStreamer elements from a library
- Connect elements with a simple click interface
- View and edit element properties
- Build, run, pause, and stop pipelines
- Visualize pipeline graph using dot
- Save and load pipeline configurations

## Requirements

- C++17 or higher
- Qt6
- GStreamer 1.0 or higher
- CMake 3.16 or higher

## Building

```bash
# Install dependencies
# Ubuntu/Debian:
sudo apt install build-essential cmake qt6-base-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev graphviz

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
cmake --build .

# Run
./GstPipelineStudio
```

## Usage

1. Start the application
2. Browse elements in the Element Library panel
3. Double-click or drag elements to add them to the pipeline
4. Connect elements by dragging from output port to input port
5. Edit element properties in the Properties panel
6. Build the pipeline using the Build button
7. Run the pipeline using the Run button
8. View the pipeline visualization using the Visualize button
9. Save your pipeline using File > Save

## Command Line Options

```
GstPipelineStudio [options]
  -h, --help             Display this help message
  -v, --version          Display version information
  -p, --pipeline=STRING  Load pipeline from string
  -f, --file=FILE        Load pipeline from file
```

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Acknowledgements

- Original Rust version by [dabrain34](https://github.com/dabrain34)
- GStreamer team for the excellent multimedia framework
- Qt team for the Qt framework
