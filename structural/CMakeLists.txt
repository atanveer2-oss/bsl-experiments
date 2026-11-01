cmake_minimum_required(VERSION 3.20)

# macOS 27 with Command Line Tools 27.0: the CLT's SDK stubs (libSystem.tbd,
# libc++.tbd) name an architecture its linker does not know ("arm64e.x1"), so
# every link fails with "tapi error: malformed file". Xcode's own SDK links
# fine, so default to it when it is there. Override with -DCMAKE_OSX_SYSROOT.
if(APPLE AND NOT DEFINED CMAKE_OSX_SYSROOT)
  set(_bsl_xcode_sdk "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk")
  if(EXISTS "${_bsl_xcode_sdk}")
    set(CMAKE_OSX_SYSROOT "${_bsl_xcode_sdk}" CACHE PATH "macOS SDK" FORCE)
  endif()
endif()

project(bsl_experiments LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Header-only core: the skip list, its promotion policies and the workload
# generators.
add_library(bsl_core INTERFACE)
target_include_directories(bsl_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Structural experiment: search cost against the entropy benchmark, height
# gap, depth, hazard, bits per item. The in-process harness behind Table 1
# and the structural figures.
add_executable(structural bench/structural.cpp)
target_link_libraries(structural PRIVATE bsl_core)

# Reseeding experiment: epochal reseeding against the persistent process on
# the persistence-barrier sequence (the barrier figure).
find_package(Threads REQUIRED)
add_executable(reseed bench/reseed.cpp)
target_link_libraries(reseed PRIVATE bsl_core Threads::Threads)

enable_testing()
add_executable(sanity bench/sanity.cpp)
target_link_libraries(sanity PRIVATE bsl_core)
add_test(NAME sanity COMMAND sanity)

# The paper's counter-based estimators: invariant, excess law, fixed-time
# hazard, crossing tail, scale.
add_executable(estimators bench/estimators.cpp)
target_link_libraries(estimators PRIVATE bsl_core)
add_test(NAME estimators COMMAND estimators)
