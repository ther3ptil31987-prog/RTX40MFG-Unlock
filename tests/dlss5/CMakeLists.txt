cmake_minimum_required(VERSION 3.24)
project(Dlss5Tests LANGUAGES C CXX)
set(STREAMLINE_ROOT "" CACHE PATH "Pinned Streamline SDK 2.14.1")
set(NATIVE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
if(NOT EXISTS "${STREAMLINE_ROOT}/external/ngx-sdk/include/nvsdk_ngx.h")
 message(FATAL_ERROR "Set STREAMLINE_ROOT to the pinned SDK")
endif()
add_library(Dlss5TestMinHook STATIC
 "${NATIVE}/third_party/minhook/src/buffer.c" "${NATIVE}/third_party/minhook/src/hook.c"
 "${NATIVE}/third_party/minhook/src/trampoline.c" "${NATIVE}/third_party/minhook/src/hde/hde64.c")
target_include_directories(Dlss5TestMinHook PUBLIC "${NATIVE}/third_party/minhook/include")
add_executable(Dlss5Tests main.cpp "${NATIVE}/dlss5.cpp" "${NATIVE}/dlss5_provider.cpp")
target_compile_features(Dlss5Tests PRIVATE cxx_std_20)
target_include_directories(Dlss5Tests PRIVATE "${NATIVE}" "${STREAMLINE_ROOT}/external/ngx-sdk/include")
target_compile_definitions(Dlss5Tests PRIVATE MFG_DLSS5_TESTING=1 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(Dlss5Tests PRIVATE Dlss5TestMinHook d3d12 dxgi d3dcompiler)
enable_testing()
add_test(NAME dlss5-policy-and-warp-lifecycle COMMAND Dlss5Tests)
add_test(NAME dlss5-create-failure COMMAND Dlss5Tests create-failure)
add_test(NAME dlss5-evaluate-failure COMMAND Dlss5Tests evaluate-failure)
add_test(NAME dlss5-release-failure-retains-ownership COMMAND Dlss5Tests release-failure)
add_test(NAME dlss5-present-stage COMMAND Dlss5Tests present)
# Monster Hunter Wilds: Streamline's proxy queue over the native device NGX holds.
add_test(NAME dlss5-present-streamline-proxy COMMAND Dlss5Tests present-streamline)
add_test(NAME dlss5-slots-full COMMAND Dlss5Tests slots)
# Witcher 3: our hair runtime's table hook holds the queue's ExecuteCommandLists slot.
add_test(NAME dlss5-own-queue-layer COMMAND Dlss5Tests own-queue-layer)
# autoscale.1: no replacement beside the live feature; in place from then on.
add_test(NAME dlss5-replace-failure COMMAND Dlss5Tests replace-failure)
# hdrbridge.1: unbounded color keeps its hue through the HDR bridge.
add_test(NAME dlss5-hdr-bridge-hue COMMAND Dlss5Tests hdr-bridge)

add_executable(Dlss5AutoTests auto_tests.cpp)
target_compile_features(Dlss5AutoTests PRIVATE cxx_std_20)
target_include_directories(Dlss5AutoTests PRIVATE "${NATIVE}")
target_compile_definitions(Dlss5AutoTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME dlss5-auto-policy COMMAND Dlss5AutoTests)

# Shared OptiScaler menu key (dlss5.8c).
add_executable(OptiScalerMenuKeyTests menu_key_tests.cpp)
target_compile_features(OptiScalerMenuKeyTests PRIVATE cxx_std_20)
target_include_directories(OptiScalerMenuKeyTests PRIVATE "${NATIVE}")
target_compile_definitions(OptiScalerMenuKeyTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME optiscaler-shared-menu-key COMMAND OptiScalerMenuKeyTests)

add_executable(Dlss5ControlTests control_tests.cpp)
target_compile_features(Dlss5ControlTests PRIVATE cxx_std_20)
target_include_directories(Dlss5ControlTests PRIVATE "${NATIVE}")
target_compile_definitions(Dlss5ControlTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set(DLSS5_CANDIDATE_DLL "" CACHE FILEPATH "Exact fresh unified DLL for config/status checks")
if(EXISTS "${DLSS5_CANDIDATE_DLL}")
 add_test(NAME dlss5-exact-dll-control COMMAND Dlss5ControlTests "${DLSS5_CANDIDATE_DLL}")
endif()
