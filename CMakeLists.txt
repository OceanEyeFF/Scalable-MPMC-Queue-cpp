cmake_minimum_required(VERSION 3.20)

project(lscq VERSION 0.1.0 LANGUAGES CXX)

option(LSCQ_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(LSCQ_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" ON)
option(LSCQ_BUILD_EXAMPLES "Build example programs" ON)
option(LSCQ_ENABLE_CAS2 "Enable CAS2 implementation (compile-time feature flag)" ON)
option(LSCQ_ENABLE_SANITIZERS "Enable AddressSanitizer when supported" OFF)
option(LSCQ_ENABLE_PERF_OPTS "Enable aggressive performance optimizations for benchmarking" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(lscq_is_clang 0)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(lscq_is_clang 1)
endif()

message(STATUS "Compiler ID: ${CMAKE_CXX_COMPILER_ID}")
if(lscq_is_clang)
  message(STATUS "Clang detected: yes")
else()
  message(STATUS "Clang detected: no")
endif()

# Performance optimization flags for benchmarking
add_library(lscq_perf_opts INTERFACE)
if(LSCQ_ENABLE_PERF_OPTS)
  message(STATUS "Performance optimizations: ENABLED")
  if(MSVC AND lscq_is_clang)
    # clang-cl: use /clang: prefix for Clang-specific flags
    # -O3 is already implied by Release builds (/O2), but we add extra optimizations
    target_compile_options(lscq_perf_opts INTERFACE
      /O2                           # Maximum optimization (clang-cl)
      /clang:-march=native          # Target current CPU architecture (AMD Ryzen 9 5900X: AVX2, etc.)
      /clang:-mtune=native          # Tune for current CPU
      /clang:-ffast-math            # Aggressive floating-point optimizations
      /clang:-funroll-loops         # Unroll loops for performance
    )
    # Note: LTO with clang-cl requires careful setup and may cause issues with CMake's
    # internal compiler tests. We enable it only for the final targets, not globally.
    # Users can manually add /clang:-flto=thin if needed.
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Standard Clang/GCC on non-MSVC platforms
    target_compile_options(lscq_perf_opts INTERFACE
      -O3                           # Maximum optimization
      -march=native                 # Target current CPU architecture
      -mtune=native                 # Tune for current CPU
      -ffast-math                   # Aggressive floating-point optimizations
      -funroll-loops                # Unroll loops for performance
    )
    # LTO can be enabled by user if needed: -flto=thin
  else()
    message(STATUS "Performance optimizations: compiler not supported, skipping")
  endif()
else()
  message(STATUS "Performance optimizations: DISABLED")
endif()

add_library(lscq INTERFACE)
add_library(lscq::lscq ALIAS lscq)

target_include_directories(lscq INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(lscq INTERFACE cxx_std_17)
target_compile_definitions(lscq INTERFACE
  LSCQ_ENABLE_CAS2=$<BOOL:${LSCQ_ENABLE_CAS2}>
  LSCQ_ENABLE_SANITIZERS=$<BOOL:${LSCQ_ENABLE_SANITIZERS}>
  LSCQ_COMPILER_CLANG=${lscq_is_clang}
)
if(MSVC AND lscq_is_clang AND LSCQ_ENABLE_CAS2 AND (CMAKE_SIZEOF_VOID_P EQUAL 8))
  # clang-cl does not always lower 16-byte atomic operations to CMPXCHG16B unless cx16 is enabled.
  # We guard execution at runtime (has_cas2_support()), but we still need the instruction to be
  # available for codegen to avoid libcall dependencies like __atomic_compare_exchange_16.
  target_compile_options(lscq INTERFACE /clang:-mcx16)
endif()

add_library(lscq_warnings INTERFACE)
if(MSVC)
  target_compile_options(lscq_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(lscq_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(lscq INTERFACE lscq_warnings)
target_link_libraries(lscq INTERFACE lscq_perf_opts)

add_library(lscq_toolchain_runtime INTERFACE)
if(MSVC AND lscq_is_clang)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
    OUTPUT_VARIABLE lscq_clang_resource_dir_builtins
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  set(lscq_clang_rt_dir_builtins "${lscq_clang_resource_dir_builtins}/lib/windows")
  set(lscq_clang_builtins_lib "${lscq_clang_rt_dir_builtins}/clang_rt.builtins-x86_64.lib")
  if(EXISTS "${lscq_clang_builtins_lib}")
    target_link_libraries(lscq_toolchain_runtime INTERFACE "${lscq_clang_builtins_lib}")
  endif()
endif()
target_link_libraries(lscq INTERFACE lscq_toolchain_runtime)

add_library(lscq_sanitizers INTERFACE)
if(LSCQ_ENABLE_SANITIZERS)
  if(MSVC AND NOT lscq_is_clang)
    message(STATUS "LSCQ_ENABLE_SANITIZERS=ON but MSVC sanitizers are not supported; skipping sanitizer flags.")
  elseif(WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang") AND (NOT MSVC))
    message(STATUS "LSCQ_ENABLE_SANITIZERS=ON but AddressSanitizer is not supported for this Windows clang++ toolchain; skipping sanitizer flags.")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(lscq_asan_compile_flag "-fsanitize=address")
    set(lscq_asan_link_flag "-fsanitize=address")
    if(MSVC)
      set(lscq_asan_compile_flag "/fsanitize=address")
      set(lscq_asan_link_flag "/fsanitize=address")
    endif()

    set(lscq_asan_probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/lscq_asan_probe.cpp")
    set(lscq_asan_probe_bin "${CMAKE_BINARY_DIR}/CMakeFiles/lscq_asan_probe_build")
    file(WRITE "${lscq_asan_probe_src}" "int main() { return 0; }\n")

    if(MSVC)
      # On Windows, validating ASan requires the runtime DLL to be present at runtime.
      # Also, some toolchain/runtime combinations can compile+link but crash at startup,
      # so we explicitly compile+run a minimal ASan binary here.
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
        OUTPUT_VARIABLE lscq_clang_resource_dir_probe
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      set(lscq_asan_rt_dir_probe "${lscq_clang_resource_dir_probe}/lib/windows")
      set(lscq_asan_dynamic_dll_probe "${lscq_asan_rt_dir_probe}/clang_rt.asan_dynamic-x86_64.dll")

      set(lscq_asan_check_dir "${CMAKE_BINARY_DIR}/CMakeFiles/lscq_asan_run_check")
      file(MAKE_DIRECTORY "${lscq_asan_check_dir}")
      set(lscq_asan_check_src "${lscq_asan_check_dir}/asan_check.cpp")
      set(lscq_asan_check_exe "${lscq_asan_check_dir}/asan_check.exe")
      file(WRITE "${lscq_asan_check_src}" "int main() { return 0; }\n")

      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} /nologo /fsanitize=address /MD /Od /Zi /U_DEBUG
                "${lscq_asan_check_src}" /Fe:${lscq_asan_check_exe}
        WORKING_DIRECTORY "${lscq_asan_check_dir}"
        RESULT_VARIABLE lscq_asan_probe_compile_result
        OUTPUT_VARIABLE lscq_asan_probe_output
        ERROR_VARIABLE lscq_asan_probe_run_output
      )

      if(lscq_asan_probe_compile_result EQUAL 0)
        execute_process(
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lscq_asan_dynamic_dll_probe}" "${lscq_asan_check_dir}"
          RESULT_VARIABLE lscq_asan_probe_copy_result
        )
        if(lscq_asan_probe_copy_result EQUAL 0)
          execute_process(
            COMMAND "${lscq_asan_check_exe}"
            WORKING_DIRECTORY "${lscq_asan_check_dir}"
            RESULT_VARIABLE lscq_asan_probe_run_result
          )
          set(lscq_asan_probe_ok TRUE)
        else()
          set(lscq_asan_probe_ok FALSE)
          set(lscq_asan_probe_run_result 1)
        endif()
      else()
        set(lscq_asan_probe_ok FALSE)
        set(lscq_asan_probe_run_result 1)
      endif()
    else()
      set(lscq_asan_probe_extra_cmake_flags "")
      try_run(
        lscq_asan_probe_run_result
        lscq_asan_probe_ok
        "${lscq_asan_probe_bin}"
        "${lscq_asan_probe_src}"
        CMAKE_FLAGS
          "-DCMAKE_CXX_STANDARD=17"
          "-DCMAKE_CXX_FLAGS=${lscq_asan_compile_flag}"
          "-DCMAKE_EXE_LINKER_FLAGS=${lscq_asan_link_flag}"
          "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
          ${lscq_asan_probe_extra_cmake_flags}
        RUN_OUTPUT_VARIABLE lscq_asan_probe_run_output
        COMPILE_OUTPUT_VARIABLE lscq_asan_probe_output
      )
    endif()

    file(REMOVE "${lscq_asan_probe_src}")

    if(lscq_asan_probe_ok AND (lscq_asan_probe_run_result EQUAL 0))
      if(MSVC)
        # AddressSanitizer on Windows does not support linking with the debug CRT.
        # Force a non-debug CRT and undefine `_DEBUG` so the MSVC STL doesn't pull in
        # debug-only symbols from the debug CRT.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
        add_compile_options(/U_DEBUG)

        # clang-cl: link step is driven via lld-link directly, so `/fsanitize=address`
        # must be a compile option (clang driver injects the right runtimes). Also, all
        # linked objects must be built with ASan to avoid `/failifmismatch` errors.
        add_compile_options(/fsanitize=address)
        target_compile_options(lscq_sanitizers INTERFACE /fsanitize=address)

        execute_process(
          COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
          OUTPUT_VARIABLE lscq_clang_resource_dir
          OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(lscq_asan_rt_dir "${lscq_clang_resource_dir}/lib/windows")
        set(lscq_asan_dynamic_lib "${lscq_asan_rt_dir}/clang_rt.asan_dynamic-x86_64.lib")
        set(lscq_asan_thunk_lib "${lscq_asan_rt_dir}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        set(lscq_asan_dynamic_dll "${lscq_asan_rt_dir}/clang_rt.asan_dynamic-x86_64.dll")

        set(LSCQ_ASAN_RUNTIME_DIR "${lscq_asan_rt_dir}" CACHE PATH "ASan runtime directory (internal)" FORCE)
        set(LSCQ_ASAN_DLL "${lscq_asan_dynamic_dll}" CACHE FILEPATH "ASan runtime DLL (internal)" FORCE)

        target_link_libraries(lscq_sanitizers INTERFACE
          "${lscq_asan_dynamic_lib}"
          "${lscq_asan_thunk_lib}"
        )
        target_link_options(lscq_sanitizers INTERFACE
          "/INFERASANLIBS:NO"
          "/INCLUDE:__asan_seh_interceptor"
          "/WHOLEARCHIVE:${lscq_asan_thunk_lib}"
        )
      else()
        target_compile_options(lscq_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(lscq_sanitizers INTERFACE -fsanitize=address)
      endif()

      if(WIN32)
        # The shipped Windows ASan runtimes are built with iterator debugging off.
        add_compile_definitions(
          _ITERATOR_DEBUG_LEVEL=0
          _HAS_ITERATOR_DEBUGGING=0
        )
      endif()
      message(STATUS "Sanitizers enabled: AddressSanitizer")
    else()
      string(REPLACE "\r\n" "\n" lscq_asan_probe_output_nl "${lscq_asan_probe_output}")
      string(REPLACE "\r\n" "\n" lscq_asan_probe_run_output_nl "${lscq_asan_probe_run_output}")
      message(STATUS "LSCQ_ENABLE_SANITIZERS=ON but AddressSanitizer probe failed (compile or run); skipping sanitizer flags.\n${lscq_asan_probe_output_nl}\n${lscq_asan_probe_run_output_nl}")
    endif()
  else()
    message(STATUS "LSCQ_ENABLE_SANITIZERS=ON but unsupported compiler (${CMAKE_CXX_COMPILER_ID}); skipping sanitizer flags.")
  endif()
endif()
target_link_libraries(lscq INTERFACE lscq_sanitizers)

add_library(lscq_impl STATIC
  src/cas2.cpp
  src/ebr.cpp
  src/lscq.cpp
  src/msqueue.cpp
  src/ncq.cpp
  src/scq.cpp
  src/scqp.cpp
)
add_library(lscq::lscq_impl ALIAS lscq_impl)
target_link_libraries(lscq_impl PUBLIC lscq::lscq)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

if(LSCQ_BUILD_TESTS)
  enable_testing()
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.12.1
  )
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()

if(LSCQ_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_FORCE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.4
  )
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(benchmarks)
endif()

if(LSCQ_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  add_custom_target(doxygen
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/build/docs"
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
  )
else()
  add_custom_target(doxygen
    COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found. Install it and re-configure to generate docs."
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
  )
endif()
