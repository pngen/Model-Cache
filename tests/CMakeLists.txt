# Model Cache test suite.
add_executable(model_cache_tests
  test_main.cpp
  test_basic.cpp
  test_compatibility.cpp
  test_manifest.cpp
  test_dependency.cpp
  test_dedup_refcount.cpp
  test_alias_generation.cpp
  test_validation_quarantine.cpp
  test_pinning.cpp
  test_eviction_admission.cpp
  test_snapshot_stats.cpp
  test_serialization_adversarial.cpp
  test_concurrency.cpp
  test_property.cpp
)
target_link_libraries(model_cache_tests PRIVATE ModelCache::model_cache)
target_include_directories(model_cache_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(model_cache_tests PRIVATE cxx_std_20)
add_test(NAME model_cache_tests COMMAND model_cache_tests)
