# Build only static Chiaki dependencies with the public PS5 payload SDK.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED ENV{PS5_PAYLOAD_SDK} OR NOT DEFINED ENV{PACBREW_SYSROOT})
  message(FATAL_ERROR "Set PS5_PAYLOAD_SDK and PACBREW_SYSROOT")
endif()
set(_sdk "$ENV{PS5_PAYLOAD_SDK}")
set(_ports "$ENV{PACBREW_SYSROOT}")

set(CMAKE_C_COMPILER /usr/bin/clang-18)
set(CMAKE_CXX_COMPILER /usr/bin/clang++-18)
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_AR "${_sdk}/bin/prospero-ar")
set(CMAKE_RANLIB /usr/bin/llvm-ranlib-18)

set(_target_flags "-DCHIAKI_PS5=1 -fvisibility-nodllstorageclass=default -isysroot ${_sdk} -isystem ${_sdk}/target/include/c++/v1 -isystem ${_sdk}/target/include -fno-stack-protector -fno-plt -femulated-tls")
set(CMAKE_C_FLAGS_INIT "${_target_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_target_flags}")

set(CMAKE_FIND_ROOT_PATH "${_ports}" "${_sdk}")
set(CMAKE_PREFIX_PATH "/user/homebrew" "/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
set(CMAKE_HAVE_LIBC_PTHREAD TRUE CACHE BOOL "" FORCE)
