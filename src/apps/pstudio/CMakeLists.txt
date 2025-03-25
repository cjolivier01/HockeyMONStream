cmake_minimum_required(VERSION 3.16)
project(GstPipelineStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find dependencies
find_package(Qt6 COMPONENTS Core Widgets Gui REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GSTREAMER_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
)

# Source files
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/PipelineEditor.cpp
    src/GstElementWidget.cpp
    src/GstPipelineModel.cpp
    src/ElementLibrary.cpp
    src/Pipeline.cpp
    src/Element.cpp
    src/Connection.cpp
)

# Resource files
set(RESOURCES
    resources/resources.qrc
)

# Create executable
add_executable(GstPipelineStudio ${SOURCES} ${RESOURCES})

# Link libraries
target_link_libraries(GstPipelineStudio PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    ${GSTREAMER_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
)

# Install
install(TARGETS GstPipelineStudio
    RUNTIME DESTINATION bin
)

# Testing
option(BUILD_TESTS "Build the tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

