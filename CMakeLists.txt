cmake_minimum_required(VERSION 3.25)

# Model Cache 1.0.0 — vendor-neutral runtime for reusable model artifacts.
project(model_cache
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral runtime for caching, validating, versioning, persisting, and reusing model artifacts"
  LANGUAGES CXX)

option(MODEL_CACHE_BUILD_TESTS "Build Model Cache tests" ON)
option(MODEL_CACHE_BUILD_EXAMPLES "Build Model Cache examples" ON)
option(MODEL_CACHE_BUILD_BENCHMARKS "Build Model Cache benchmarks" ON)
option(MODEL_CACHE_BUILD_CLI "Build the model-cache CLI" ON)
option(MODEL_CACHE_SANITIZERS "Enable address/undefined-behavior sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /WX /EHsc /utf-8)
  add_compile_options(/Zi)
  # Multi-config VS generator: set the runtime library per config (/MD Release,
  # /MDd Debug). Optimization (/O2, /Od) and runtime checks (/RTC1) follow the
  # default MSVC per-config flags, so no manual /O2 + /RTC1 conflict occurs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

if(MODEL_CACHE_SANITIZERS AND MSVC)
  add_compile_options(/fsanitize=address)
  add_link_options(/fsanitize=address)
elseif(MODEL_CACHE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(GNUInstallDirs)

add_library(model_cache STATIC
  src/error.cpp
  src/digest.cpp
  src/types.cpp
  src/serialization.cpp
  src/identity.cpp
  src/compatibility.cpp
  src/platform.cpp
  src/storage.cpp
  src/dependency.cpp
  src/events.cpp
  src/stats.cpp
  src/cache.cpp
  src/config.cpp
)
add_library(ModelCache::model_cache ALIAS model_cache)

target_include_directories(model_cache
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(model_cache PUBLIC cxx_std_20)

if(MODEL_CACHE_BUILD_CLI)
  add_executable(model-cache cli/main.cpp)
  target_link_libraries(model-cache PRIVATE ModelCache::model_cache)
  set_target_properties(model-cache PROPERTIES OUTPUT_NAME "model-cache")
  target_compile_features(model-cache PRIVATE cxx_std_20)
endif()

if(MODEL_CACHE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MODEL_CACHE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MODEL_CACHE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

install(TARGETS model_cache
  EXPORT ModelCacheTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MODEL_CACHE_BUILD_CLI)
  install(TARGETS model-cache
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/model_cache
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ModelCacheTargets
  FILE ModelCacheTargets.cmake
  NAMESPACE ModelCache::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_cache)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/model_cache-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/model_cache-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_cache)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/model_cache-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/model_cache-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/model_cache-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_cache)
