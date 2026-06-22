cmake_minimum_required(VERSION 3.16)
project(ToyCCompiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

file(GLOB_RECURSE TOYC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(compiler ${TOYC_SOURCES})
target_include_directories(compiler PRIVATE "${CMAKE_SOURCE_DIR}/src")

# Place the executable at the build root so the judge can find it easily.
set_target_properties(compiler PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
