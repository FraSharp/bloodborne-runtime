# bbport: the game (out/bb-probe), the CPU layer around the translator (bbcpu), the GPU library
# (gpu/, a CMake project of its own) and the tools the launcher runs. The libraries come from deps/
# on macOS (deps/packages.cmake) and from the environment on Linux (shell.nix).
#
#   bash build.sh                     # the preset of this platform
#   bash build.sh --test              # and the tests, through ctest
#   cmake --preset macos-arm64-asan   # the same, with the address and undefined sanitizers
#   cmake --build --preset macos-arm64 --target bb-tests && ctest --preset macos-arm64
#
# The binaries land in out/, where run.sh and the launcher look for them, and the GPU library in
# out/gpu(-<arch>); object files and the GPU library's build tree stay in build/<preset>/.
cmake_minimum_required(VERSION 3.24)

# The minimum macOS, the launcher's minimumSystemVersion and deps/'s deployment target are one
# number: without it the SDK of the machine that builds decides.
if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
endif()

project(bbport LANGUAGES C CXX)
# bbcpu's host call is assembler (recomp/src/cpu/hostcall.S); its .S goes through the C compiler.
enable_language(ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ctest, and BUILD_TESTING, before gpu/ (which registers its own tests): bash build.sh --test
# builds and runs them.
include(CTest)

# arm64 by default (bbcpu translates the game's x86-64 code); x86_64 is the Rosetta 2 build, where
# the game's code runs as it is (docs/ARM64_NATIVE.md). The libraries in deps/macos-<arch> and the
# GPU library's output directory follow it.
set(BB_ARCH "" CACHE STRING "arm64 or x86_64 (default: what the build is for)")
if (NOT BB_ARCH)
    if (CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(BB_ARCH x86_64)
    elseif (APPLE)
        set(BB_ARCH arm64)
    else()
        set(BB_ARCH ${CMAKE_SYSTEM_PROCESSOR})
    endif()
endif()

# Everything is built into build/<preset>/; bash build.sh publishes bb-probe, bb-gpu-capabilities
# and the GPU library to out/, the layout run.sh and the launcher use (and the game's data lives
# there too). A preset never writes another preset's files.
set(BB_GPU_OUT ${CMAKE_BINARY_DIR}/gpu)
set(BB_BENCH_OUT ${CMAKE_SOURCE_DIR}/out/gpu)
if (APPLE)
    set(BB_DEPS ${CMAKE_SOURCE_DIR}/deps/macos-${BB_ARCH} CACHE PATH "The libraries deps/ builds")
    list(APPEND CMAKE_PREFIX_PATH ${BB_DEPS})
    list(APPEND CMAKE_PROGRAM_PATH ${BB_DEPS}/bin)
    set(ENV{PKG_CONFIG_LIBDIR} "${BB_DEPS}/lib/pkgconfig:${BB_DEPS}/share/pkgconfig")
endif()

set(BB_SANITIZE "" CACHE STRING "Sanitizers for every target (address,undefined)")
if (BB_SANITIZE)
    message(STATUS "Sanitizers: ${BB_SANITIZE}")
    add_compile_options(-fsanitize=${BB_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${BB_SANITIZE})
endif()

# bbport's own C: strict, and a warning is an error. -UNDEBUG: the game has always kept its
# asserts (the tests are written with them), and CMake's RelWithDebInfo would remove them.
set(BB_WARNINGS -Wall -Wextra -Werror -UNDEBUG)
if (APPLE)
    list(APPEND BB_WARNINGS -Wno-unknown-warning-option -Wno-unused-but-set-global)
else()
    list(APPEND BB_WARNINGS -D_POSIX_C_SOURCE=200809L) # POSIX rwlocks and clocks in glibc
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT APPLE)
    pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
endif()

# The GPU library (gpu/CMakeLists.txt): shadPS4's video core, the Metal backend, FSR. It installs
# itself and its tools in BB_GPU_OUT.
add_subdirectory(gpu)

# bbcpu (recomp/src/cpu, the bloodborne-recomp submodule): runs the game's x86-64 code through the
# translator. macOS only: the Rosetta 2 build runs the guest code as it is, and Linux has none.
# Zydis decodes the guest's instructions.
if (APPLE)
    find_package(Zydis REQUIRED)
    file(GLOB BB_CPU_SOURCES CONFIGURE_DEPENDS recomp/src/cpu/*.c recomp/src/cpu/*.S)
    add_library(bbcpu STATIC ${BB_CPU_SOURCES})
    # C flags for the C sources only: the assembler would warn that -std=gnu11 does nothing, and
    # here a warning is an error.
    target_compile_options(bbcpu PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11;-Wno-unused-parameter>
                                        ${BB_WARNINGS})
    target_compile_options(bbcpu PRIVATE -UNDEBUG)
    if (BB_ARCH STREQUAL "x86_64")
        # recomp's AFP helper is arm64 only and unused under Rosetta; the other warnings stay fatal.
        target_compile_options(bbcpu PRIVATE -Wno-error=unused-function)
    endif()
    target_include_directories(bbcpu PRIVATE ${BB_DEPS}/include)
endif()

# The ATRAC9 decoder (third-party, compiled without this project's warnings).
file(GLOB BB_ATRAC9_SOURCES CONFIGURE_DEPENDS third_party/LibAtrac9/C/src/*.c)
add_library(atrac9 STATIC ${BB_ATRAC9_SOURCES})
target_compile_options(atrac9 PRIVATE -std=c99 -w -UNDEBUG)

# bbport's own C, and what the game links: the decoders, the GPU library, SDL3, the CPU layer.
add_library(bb-c INTERFACE)
target_compile_options(bb-c INTERFACE -std=c11 -pthread ${BB_WARNINGS})
target_include_directories(bb-c INTERFACE . src ${CMAKE_SOURCE_DIR}/recomp/src)
add_library(bb-runtime INTERFACE)
target_link_libraries(bb-runtime INTERFACE bb-c atrac9 bbgpu PkgConfig::SDL3 m)
if (APPLE)
    target_link_directories(bb-runtime INTERFACE ${BB_DEPS}/lib) # Zydis names Zycore by -lZycore
    # The GPU library resolves the runtime's symbols against the program that links it
    # (gpu/CMakeLists.txt: -undefined dynamic_lookup), so they have to be exported.
    target_link_options(bb-runtime INTERFACE -Wl,-export_dynamic)
    target_link_libraries(bb-runtime INTERFACE bbcpu Zydis::Zydis "-framework Foundation")
else()
    target_link_libraries(bb-runtime INTERFACE PkgConfig::VULKAN)
endif()

# The game: src/runtime*.c are its services, probe.c the program.
file(GLOB BB_RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime*.c)
add_executable(bbprobe src/probe.c ${BB_RUNTIME_SOURCES} src/platform.c src/vulkan_smoke.c)
target_link_libraries(bbprobe PRIVATE bb-runtime)
if (NOT APPLE)
    # -no-pie keeps the loader and its brk heap below 1 TiB; -rdynamic lets the GPU library find
    # the runtime's symbols.
    target_link_options(bbprobe PRIVATE -no-pie -rdynamic)
endif()
target_compile_options(bbprobe PRIVATE -UNDEBUG)
set_target_properties(bbprobe PROPERTIES OUTPUT_NAME bb-probe)
if (APPLE)
    # The GPU library sits in gpu/ next to the executable, in the build directory and in out/
    # alike; the libraries deps/ installed are @rpath ones in their prefix.
    set_target_properties(bbprobe PROPERTIES
        BUILD_RPATH "@executable_path/gpu;${BB_DEPS}/lib" INSTALL_RPATH "@executable_path/gpu")
endif()

# The GPU check for run.sh (live_resolution=auto) and the launcher's gamepad list (--gamepads).
add_executable(bb-gpu-capabilities tools/gpu_capabilities.c)
target_compile_options(bb-gpu-capabilities PRIVATE -std=c11 ${BB_WARNINGS})
target_link_libraries(bb-gpu-capabilities PRIVATE PkgConfig::SDL3)

# The DLSS bridge (NVIDIA RTX, Linux): its own project, built when the SDK is at hand.
set(BB_DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK checkout (builds the DLSS bridge)")
if (BB_DLSS_SDK_ROOT)
    if (APPLE)
        message(FATAL_ERROR "The DLSS bridge is for NVIDIA GPUs (Linux and Windows)")
    endif()
    add_subdirectory(gpu/dlss_bridge)
endif()

# --- tests -----------------------------------------------------------------------------------
if (BUILD_TESTING)
    set(BB_TESTS "")

    # The tests that run the runtime's services link them as the game does.
    foreach(test runtime sema online)
        add_executable(${test}-test EXCLUDE_FROM_ALL tests/test_${test}.c ${BB_RUNTIME_SOURCES}
                       src/platform.c)
        target_link_libraries(${test}-test PRIVATE bb-runtime)
        list(APPEND BB_TESTS ${test}-test)
    endforeach()

    # The ones that only need the runtime's headers (and SDL3, for the pad's rumble).
    add_executable(pad-test EXCLUDE_FROM_ALL tests/test_pad.c src/platform.c)
    target_link_libraries(pad-test PRIVATE bb-c PkgConfig::SDL3)
    add_executable(file-mods-test EXCLUDE_FROM_ALL tests/test_file_mods.c src/platform.c)
    target_link_libraries(file-mods-test PRIVATE bb-c)
    add_executable(content-test EXCLUDE_FROM_ALL tests/test_content.c src/runtime_content.c
                   src/platform.c)
    target_link_libraries(content-test PRIVATE bb-c)
    target_compile_options(content-test PRIVATE -D_GNU_SOURCE)
    # x86-64 only: the test executes the guest's instructions, which arm64 cannot.
    add_executable(guest-tls-test EXCLUDE_FROM_ALL tests/test_guest_tls.c src/platform.c)
    target_link_libraries(guest-tls-test PRIVATE bb-c)
    list(APPEND BB_TESTS pad-test file-mods-test content-test guest-tls-test)

    foreach(test IN LISTS BB_TESTS)
        target_compile_options(${test} PRIVATE -UNDEBUG) # the tests are asserts
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
    if (APPLE AND NOT BB_ARCH STREQUAL "x86_64")
        # The test executes x86-64 instructions; the native arm64 build has no way to run them.
        set_tests_properties(guest-tls-test PROPERTIES DISABLED TRUE)
    endif()
    if (APPLE AND BB_SANITIZE)
        # The runtime maps the guest's address space at fixed addresses and sets up its module TLS;
        # under the sanitizers (which keep the address space for themselves) that cannot be done.
        set_tests_properties(runtime-test sema-test file-mods-test PROPERTIES DISABLED TRUE)
    endif()

    # The loader test builds a guest image with 4 KiB pages; macOS has 16 KiB ones.
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        add_test(NAME loader
                 COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/test_loader.py
                         $<TARGET_FILE:bbprobe>)
        if (APPLE)
            set_tests_properties(loader PROPERTIES DISABLED TRUE)
        endif()
    endif()

    # What a build of the tests has to build: the GPU library's own (gpu/CMakeLists.txt) included.
    set(BB_TESTS_DEPENDS ${BB_TESTS})
    if (TARGET bbgpu-tests)
        list(APPEND BB_TESTS_DEPENDS bbgpu-tests)
    endif()
    add_custom_target(bb-tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS ${BB_TESTS_DEPENDS})
endif()
