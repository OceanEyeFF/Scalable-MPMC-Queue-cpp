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
  endif()
  if(lscq_cov_link_opts)
    target_link_options(lscq_unit_tests PRIVATE ${lscq_cov_link_opts})
  endif()

  if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(EXISTS "${lscq_cov_profile_lib}")
      target_link_libraries(lscq_unit_tests PRIVATE "${lscq_cov_profile_lib}")
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

set_target_properties(lscq_unit_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

if(LSCQ_ENABLE_SANITIZERS AND DEFINED LSCQ_ASAN_DLL AND EXISTS "${LSCQ_ASAN_DLL}")
  add_custom_command(TARGET lscq_unit_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LSCQ_ASAN_DLL}" "$<TARGET_FILE_DIR:lscq_unit_tests>"
  )
endif()

include(GoogleTest)
if(LSCQ_ENABLE_COVERAGE)
  gtest_discover_tests(lscq_unit_tests PROPERTIES ENVIRONMENT "LLVM_PROFILE_FILE=coverage-%p.profraw")
else()
  gtest_discover_tests(lscq_unit_tests)
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
