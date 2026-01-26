add_executable(lscq_benchmarks
  benchmark_main.cpp
  benchmark_smoke.cpp
  benchmark_pair.cpp
  benchmark_mixed.cpp
  benchmark_empty.cpp
  benchmark_memory.cpp
  benchmark_object_pool_map.cpp
)

target_link_libraries(lscq_benchmarks
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    benchmark::benchmark
)

set_target_properties(lscq_benchmarks PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET lscq_benchmarks POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:lscq_benchmarks>"
  )
endif()

add_executable(benchmark_cas2
  benchmark_cas2.cpp
)

target_link_libraries(benchmark_cas2
  PRIVATE
    lscq::lscq
    benchmark::benchmark_main
)

set_target_properties(benchmark_cas2 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET benchmark_cas2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:benchmark_cas2>"
  )
endif()

# Simplified LSCQ benchmark for debugging
add_executable(benchmark_lscq_simple
  benchmark_lscq_simple.cpp
)

target_link_libraries(benchmark_lscq_simple
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    benchmark::benchmark
)

set_target_properties(benchmark_lscq_simple PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

# Independent stress test suite for 70E30D scenarios
# Only unbounded queues (MSQueue, LSCQ) are suitable for high enqueue pressure
add_executable(benchmark_stress
  benchmark_main.cpp
  benchmark_stress.cpp
)

target_link_libraries(benchmark_stress
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    benchmark::benchmark
)

set_target_properties(benchmark_stress PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET benchmark_stress POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:benchmark_stress>"
  )
endif()

# Phase 0: Component overhead microbenchmarks for ObjectPool optimization
# Measures shared_mutex, unordered_map, thread_local, and atomic operation overhead
add_executable(benchmark_components
  benchmark_components.cpp
)

target_link_libraries(benchmark_components
  PRIVATE
    lscq::lscq
    benchmark::benchmark
)

set_target_properties(benchmark_components PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET benchmark_components POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:benchmark_components>"
  )
endif()

add_executable(benchmark_object_pool_tls
  benchmark_object_pool_tls.cpp
)

target_link_libraries(benchmark_object_pool_tls
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    benchmark::benchmark
)

set_target_properties(benchmark_object_pool_tls PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET benchmark_object_pool_tls POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:benchmark_object_pool_tls>"
  )
endif()

add_executable(benchmark_object_pool_tls_v2
  benchmark_object_pool_tls_v2.cpp
)

target_link_libraries(benchmark_object_pool_tls_v2
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    benchmark::benchmark
)

set_target_properties(benchmark_object_pool_tls_v2 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET benchmark_object_pool_tls_v2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:benchmark_object_pool_tls_v2>"
  )
endif()
