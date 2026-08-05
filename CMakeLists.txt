cmake_minimum_required(VERSION 3.24)
project(MultiTransform VERSION 0.1.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Real code for Turing/Ampere/Ada/Blackwell (the dev machine is an RTX 5070 Ti,
# sm_120), plus virtual PTX for architectures newer than this CUDA toolkit.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 75 86 89 120 120-virtual)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "This project currently targets Windows only.")
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
target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# --- CUDA render path --------------------------------------------------------
# Kept in its own library so both the plugin and the parity test link the exact
# same compiled kernel rather than two separate builds of it.
add_library(mtxcuda STATIC src/render/CudaKernel.cu)
target_include_directories(mtxcuda PUBLIC src)
set_target_properties(mtxcuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION OFF
    POSITION_INDEPENDENT_CODE ON
)

# --- The plugin --------------------------------------------------------------
add_library(MultiTransform MODULE
    src/MultiTransformPlugin.cpp
    src/HostProbe.cpp
    src/interact/OverlayInteract.cpp
    src/interact/TimelineWidget.cpp
    src/interact/CurveWidget.cpp
    src/interact/GizmoWidget.cpp
)
target_include_directories(MultiTransform PRIVATE src)
target_link_libraries(MultiTransform PRIVATE ofxsupport mtxcuda)

# OFX plugins are DLLs with a .ofx extension and a single exported entry point.
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

# CPU vs CUDA parity. The whole argument for header-only device-safe maths is
# that both paths agree; this is what actually holds us to it.
add_executable(test_render_parity tests/test_render_parity.cpp)
target_include_directories(test_render_parity PRIVATE src)
target_link_libraries(test_render_parity PRIVATE mtxcuda)
add_test(NAME render_parity COMMAND test_render_parity)

# Benchmark, not a test: run it by hand to decide where optimisation is worth
# spending. Deliberately not registered with CTest.
add_executable(bench_render tests/bench_render.cpp)
target_include_directories(bench_render PRIVATE src)
target_link_libraries(bench_render PRIVATE mtxcuda)

# --- Assemble the .ofx.bundle ------------------------------------------------
# Per-config path: with a multi-config generator a single shared bundle dir means
# a Debug build silently overwrites the Release one (and vice versa), and the
# install scripts cannot tell which config they are actually shipping.
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/MultiTransform.ofx.bundle")
add_custom_command(TARGET MultiTransform POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}/Contents/Win64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MultiTransform>" "${BUNDLE_DIR}/Contents/Win64/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/Info.plist" "${BUNDLE_DIR}/Contents/"
    COMMENT "Assembling MultiTransform.ofx.bundle"
)
