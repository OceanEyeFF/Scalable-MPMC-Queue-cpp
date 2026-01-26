add_executable(lscq_unit_tests
  unit/test_smoke.cpp
  unit/test_cas2.cpp
  unit/test_ncq.cpp
  unit/test_scq.cpp
  unit/test_scqp.cpp
  unit/test_scqp_perf.cpp
  unit/test_msqueue.cpp
  unit/test_ebr.cpp
  unit/test_lscq.cpp
  test_mutex_queue.cpp
)

add_executable(test_object_pool
  unit/test_object_pool.cpp
  unit/test_object_pool_core.cpp
)

add_executable(test_object_pool_map
  unit/test_object_pool_map.cpp
)

add_executable(test_object_pool_tls
  unit/test_object_pool_tls.cpp
)

option(LSCQ_ENABLE_COVERAGE "Enable compiler instrumentation for llvm-cov/llvm-profdata" OFF)
if(LSCQ_ENABLE_COVERAGE)
  set(lscq_cov_compile_opts "")
  set(lscq_cov_link_opts "")

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(MSVC)
      set(lscq_cov_compile_opts /clang:-fprofile-instr-generate /clang:-fcoverage-mapping)
      # When building with clang-cl, the link step is typically driven by lld-link directly (no clang driver),
      # so `/clang:` flags are not valid linker arguments. Instead, link the profile runtime explicitly.
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
        OUTPUT_VARIABLE lscq_clang_resource_dir_coverage
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      set(lscq_cov_rt_dir "${lscq_clang_resource_dir_coverage}/lib/windows")
      set(lscq_cov_profile_lib "${lscq_cov_rt_dir}/clang_rt.profile-x86_64.lib")
    else()
      set(lscq_cov_compile_opts -fprofile-instr-generate -fcoverage-mapping)
      set(lscq_cov_link_opts -fprofile-instr-generate -fcoverage-mapping)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(lscq_cov_compile_opts --coverage)
    set(lscq_cov_link_opts --coverage)
  else()
    message(WARNING "LSCQ_ENABLE_COVERAGE=ON but unsupported compiler (${CMAKE_CXX_COMPILER_ID}); skipping coverage flags.")
  endif()

  if(lscq_cov_compile_opts)
    target_compile_options(lscq_unit_tests PRIVATE ${lscq_cov_compile_opts})
    target_compile_options(test_object_pool PRIVATE ${lscq_cov_compile_opts})
    target_compile_options(test_object_pool_map PRIVATE ${lscq_cov_compile_opts})
    target_compile_options(test_object_pool_tls PRIVATE ${lscq_cov_compile_opts})
  endif()
  if(lscq_cov_link_opts)
    target_link_options(lscq_unit_tests PRIVATE ${lscq_cov_link_opts})
    target_link_options(test_object_pool PRIVATE ${lscq_cov_link_opts})
    target_link_options(test_object_pool_map PRIVATE ${lscq_cov_link_opts})
    target_link_options(test_object_pool_tls PRIVATE ${lscq_cov_link_opts})
  endif()

  if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(EXISTS "${lscq_cov_profile_lib}")
      target_link_libraries(lscq_unit_tests PRIVATE "${lscq_cov_profile_lib}")
      target_link_libraries(test_object_pool PRIVATE "${lscq_cov_profile_lib}")
      target_link_libraries(test_object_pool_map PRIVATE "${lscq_cov_profile_lib}")
      target_link_libraries(test_object_pool_tls PRIVATE "${lscq_cov_profile_lib}")
    else()
      message(WARNING "LSCQ_ENABLE_COVERAGE=ON but clang profile runtime was not found at: ${lscq_cov_profile_lib}")
    endif()
  endif()

  # The code under test lives in lscq_impl; instrument it as well so llvm-cov can report on NCQ sources.
  if(TARGET lscq_impl AND lscq_cov_compile_opts)
    target_compile_options(lscq_impl PRIVATE ${lscq_cov_compile_opts})
  endif()
  if(TARGET lscq_impl AND lscq_cov_link_opts)
    target_link_options(lscq_impl PRIVATE ${lscq_cov_link_opts})
  endif()
endif()

target_link_libraries(lscq_unit_tests
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

target_link_libraries(test_object_pool
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

target_link_libraries(test_object_pool_map
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

target_link_libraries(test_object_pool_tls
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

set_target_properties(lscq_unit_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

set_target_properties(test_object_pool PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

set_target_properties(test_object_pool_map PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

set_target_properties(test_object_pool_tls PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET lscq_unit_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:lscq_unit_tests>"
  )
  add_custom_command(TARGET test_object_pool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:test_object_pool>"
  )
  add_custom_command(TARGET test_object_pool_map POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:test_object_pool_map>"
  )
  add_custom_command(TARGET test_object_pool_tls POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:test_object_pool_tls>"
  )
endif()

include(GoogleTest)
if(LSCQ_ENABLE_COVERAGE)
  gtest_discover_tests(lscq_unit_tests PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=coverage-%p.profraw")
  gtest_discover_tests(test_object_pool
    TEST_PREFIX "test_object_pool."
    PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=coverage-%p.profraw"
  )
  gtest_discover_tests(test_object_pool_map
    TEST_PREFIX "test_object_pool_map."
    PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=coverage-%p.profraw"
  )
  gtest_discover_tests(test_object_pool_tls
    TEST_PREFIX "test_object_pool_tls."
    PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=coverage-%p.profraw"
  )
else()
  gtest_discover_tests(lscq_unit_tests)
  gtest_discover_tests(test_object_pool TEST_PREFIX "test_object_pool.")
  gtest_discover_tests(test_object_pool_map TEST_PREFIX "test_object_pool_map.")
  gtest_discover_tests(test_object_pool_tls TEST_PREFIX "test_object_pool_tls.")
endif()

# Debug test for LSCQ Pair mode
add_executable(debug_lscq_pair
  debug_lscq_pair.cpp
)

target_link_libraries(debug_lscq_pair
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
)

set_target_properties(debug_lscq_pair PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

# Phase 0: LSCQ usage pattern analysis for ObjectPool optimization
# Analyzes Get/Put ratio and cache hit potential
add_executable(test_lscq_usage_pattern
  analysis/test_lscq_usage_pattern.cpp
)

target_link_libraries(test_lscq_usage_pattern
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

set_target_properties(test_lscq_usage_pattern PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET test_lscq_usage_pattern POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:test_lscq_usage_pattern>"
  )
endif()

gtest_discover_tests(test_lscq_usage_pattern TEST_PREFIX "test_lscq_usage_pattern.")

# Phase 1: High-load stability stress tests for ObjectPool implementations
add_executable(test_object_pool_stress
  stress/test_object_pool_stress.cpp
)

target_link_libraries(test_object_pool_stress
  PRIVATE
    lscq::lscq
    lscq::lscq_impl
    GTest::gtest_main
)

set_target_properties(test_object_pool_stress PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET test_object_pool_stress POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:test_object_pool_stress>"
  )
endif()

# Stress tests have longer timeouts
gtest_discover_tests(test_object_pool_stress
  TEST_PREFIX "test_object_pool_stress."
  PROPERTIES TIMEOUT 300  # 5 minutes timeout for stress tests
)
