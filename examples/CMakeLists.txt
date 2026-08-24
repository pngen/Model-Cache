add_executable(example_core example_core.cpp)
target_link_libraries(example_core PRIVATE ModelCache::model_cache)
target_compile_features(example_core PRIVATE cxx_std_20)
add_test(NAME example_core COMMAND example_core)
