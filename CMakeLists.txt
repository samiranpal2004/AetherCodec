# CMakeLists.txt — AetherCodec top-level build
cmake_minimum_required(VERSION 3.16)
project(aethercodec C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address")

# Feature toggles (see HLD §13.2)
option(AETHER_ENABLE_PIPEWIRE "Build PipeWire sink/source integration" ON)
option(AETHER_ENABLE_ALSA     "Build ALSA fallback plugin"             ON)

find_package(PkgConfig REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Bluetooth (BlueZ) and FFTW are always required for the codec + transport.
pkg_check_modules(BLUEZ REQUIRED bluez)
pkg_check_modules(FFTW  REQUIRED fftw3f)   # single-precision FFT for MDCT

if(AETHER_ENABLE_PIPEWIRE)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
endif()

if(AETHER_ENABLE_ALSA)
    pkg_check_modules(ALSA REQUIRED alsa)
endif()

include_directories(include)
include_directories(${BLUEZ_INCLUDE_DIRS})
include_directories(${FFTW_INCLUDE_DIRS})
if(AETHER_ENABLE_PIPEWIRE)
    include_directories(${PIPEWIRE_INCLUDE_DIRS})
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(tests)
