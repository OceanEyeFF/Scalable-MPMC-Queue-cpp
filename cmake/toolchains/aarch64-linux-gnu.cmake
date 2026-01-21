set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Ensure CMake doesn't attempt to execute target binaries during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Typical sysroot layout on Ubuntu when using gcc-aarch64-linux-gnu.
if(EXISTS "/usr/aarch64-linux-gnu")
  set(CMAKE_SYSROOT "/usr/aarch64-linux-gnu")
  set(CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")
endif()
