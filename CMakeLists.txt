cmake_minimum_required(VERSION 3.24)

# macOS: one universal binary for Apple Silicon and Intel, macOS 11 or later.
# These must be set before project(); Windows ignores them.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

project(MultiTransform VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The GPU path is per platform: CUDA on Windows, Metal on macOS. Everything
# else -- the engine, the sampler on the CPU, presets, the overlay -- is shared.
if(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    # Real code for Turing/Ampere/Ada/Blackwell (the dev machine is an RTX 5070
    # Ti, sm_120), plus virtual PTX for architectures newer than this toolkit.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 75 86 89 120 120-virtual)
    endif()
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    enable_language(CUDA)
else()
    message(FATAL_ERROR "This project targets Windows and macOS only.")
endif()

set(OFX_ROOT     "${CMAKE_SOURCE_DIR}/third_party/openfx")
set(OFX_INCLUDE  "${OFX_ROOT}/OpenFX-1.4/include")
set(OFX_SUPPORT  "${OFX_ROOT}/Support")

# --- OFX C++ support library -------------------------------------------------
# NOTE: ofxsHWNDInteract.cpp is deliberately excluded. It references symbols
# (gHWNDInteractSuite, kOfxHWndInteract*) that are declared nowhere in the SDK,
# so it does not compile. Blackmagic's own Makefile omits it too.
add_library(ofxsupport STATIC
    "${OFX_SUPPORT}/Library/ofxsCore.cpp"
    "${OFX_SUPPORT}/Library/ofxsImageEffect.cpp"
    "${OFX_SUPPORT}/Library/ofxsInteract.cpp"
    "${OFX_SUPPORT}/Library/ofxsLog.cpp"
    "${OFX_SUPPORT}/Library/ofxsMultiThread.cpp"
    "${OFX_SUPPORT}/Library/ofxsParams.cpp"
    "${OFX_SUPPORT}/Library/ofxsProperty.cpp"
    "${OFX_SUPPORT}/Library/ofxsPropertyValidation.cpp"
)
target_include_directories(ofxsupport PUBLIC "${OFX_INCLUDE}" "${OFX_SUPPORT}/include")
set_target_properties(ofxsupport PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# --- GPU render path ---------------------------------------------------------
# Kept in its own library so both the plugin and the parity test link the exact
# same compiled kernels rather than two separate builds of them.
if(APPLE)
    # The Metal shader is embedded in the binary as a string, compiled once per
    # device at first use: no .metallib to find next to the bundle at runtime.
    set(MTX_METAL_SRC "${CMAKE_SOURCE_DIR}/src/render/MetalKernels.metal")
    file(READ "${MTX_METAL_SRC}" MTX_METAL_SOURCE)
    configure_file(src/render/MetalKernelsSource.h.in
                   "${CMAKE_BINARY_DIR}/generated/render/MetalKernelsSource.h" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MTX_METAL_SRC}")

    add_library(mtxgpu STATIC src/render/MetalRender.mm)
    target_include_directories(mtxgpu PUBLIC src "${CMAKE_BINARY_DIR}/generated")
    target_compile_options(mtxgpu PRIVATE -fobjc-arc)
    target_link_libraries(mtxgpu PUBLIC "-framework Metal" "-framework Foundation")
    set_target_properties(mtxgpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
    add_library(mtxgpu STATIC src/render/CudaKernel.cu)
    target_include_directories(mtxgpu PUBLIC src)
    set_target_properties(mtxgpu PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# --- The plugin --------------------------------------------------------------
set(MTX_PLUGIN_SOURCES
    src/MultiTransformPlugin.cpp
    src/HostProbe.cpp
    src/Preset.cpp
    src/PresetIO.cpp
    src/Settings.cpp
    src/CurvePreset.cpp
    src/interact/OverlayInteract.cpp
    src/interact/TimelineWidget.cpp
    src/interact/CurveWidget.cpp
    src/interact/GizmoWidget.cpp
    src/interact/PathWidget.cpp
    src/interact/LibraryWidget.cpp
    src/interact/OpacityWidget.cpp
    src/interact/DefocusWidget.cpp
    src/interact/QuickWidget.cpp
)
if(APPLE)
    list(APPEND MTX_PLUGIN_SOURCES src/platform/PlatformMac.mm)
    set_source_files_properties(src/platform/PlatformMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
else()
    list(APPEND MTX_PLUGIN_SOURCES src/platform/PlatformWin.cpp)
endif()

add_library(MultiTransform MODULE ${MTX_PLUGIN_SOURCES})
target_include_directories(MultiTransform PRIVATE src)

if(APPLE)
    target_link_libraries(MultiTransform PRIVATE ofxsupport mtxgpu
        "-framework AppKit" "-framework Foundation" "-framework CoreFoundation" "-framework Metal")
else()
    # ole32/shell32: the preset-folder picker is IFileDialog, which is COM.
    target_link_libraries(MultiTransform PRIVATE ofxsupport mtxgpu comdlg32 ole32 shell32)
endif()

# A linker map alongside the binary, so an address out of a Resolve crash dump
# can be turned into a function name without a debugger. It cost nothing until
# the day it was needed, and that day came.
if(MSVC)
    target_link_options(MultiTransform PRIVATE /MAP)
endif()

# OFX plugins are shared libraries with a .ofx extension and a single exported
# entry point (a DLL on Windows, a Mach-O bundle on macOS).
set_target_properties(MultiTransform PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
    OUTPUT_NAME "MultiTransform"
)

# --- Tests -------------------------------------------------------------------
# The animation engine is pure, header-only maths with no OFX dependency, so it
# can be tested without Resolve in the loop at all.
enable_testing()
add_executable(test_anim_engine tests/test_anim_engine.cpp)
target_include_directories(test_anim_engine PRIVATE src)
add_test(NAME anim_engine COMMAND test_anim_engine)

# CPU vs GPU parity -- CUDA on Windows, Metal on macOS. The whole argument for
# header-only maths is that both paths agree; this is what actually holds us to it.
add_executable(test_render_parity tests/test_render_parity.cpp)
target_include_directories(test_render_parity PRIVATE src)
target_link_libraries(test_render_parity PRIVATE mtxgpu)
add_test(NAME render_parity COMMAND test_render_parity)

# Preset serialisation is OFX-free for exactly this reason: a preset that half
# loads or mis-scales is worse than one that refuses, so the rules get tested
# directly rather than only through the host.
add_executable(test_preset tests/test_preset.cpp src/Preset.cpp src/Settings.cpp src/CurvePreset.cpp)
target_include_directories(test_preset PRIVATE src)
add_test(NAME preset COMMAND test_preset)

# Benchmark, not a test: run it by hand to decide where optimisation is worth
# spending. Deliberately not registered with CTest. CUDA only for now.
if(WIN32)
    add_executable(bench_render tests/bench_render.cpp)
    target_include_directories(bench_render PRIVATE src)
    target_link_libraries(bench_render PRIVATE mtxgpu)
endif()

# --- Assemble the .ofx.bundle ------------------------------------------------
# Per-config path: with a multi-config generator a single shared bundle dir means
# a Debug build silently overwrites the Release one (and vice versa), and the
# install scripts cannot tell which config they are actually shipping.
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/MultiTransform.ofx.bundle")
if(APPLE)
    set(BUNDLE_ARCH_DIR "MacOS")
else()
    set(BUNDLE_ARCH_DIR "Win64")
endif()
add_custom_command(TARGET MultiTransform POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}/Contents/${BUNDLE_ARCH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MultiTransform>" "${BUNDLE_DIR}/Contents/${BUNDLE_ARCH_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/Info.plist" "${BUNDLE_DIR}/Contents/"
    COMMENT "Assembling MultiTransform.ofx.bundle"
)
if(APPLE)
    # Apple Silicon refuses to load an unsigned binary. An ad-hoc signature is
    # enough on this machine; giving the bundle to anyone else needs a
    # Developer ID signature and notarisation instead.
    add_custom_command(TARGET MultiTransform POST_BUILD
        COMMAND codesign --force --sign - "${BUNDLE_DIR}"
        COMMENT "Ad-hoc signing MultiTransform.ofx.bundle"
    )
endif()
