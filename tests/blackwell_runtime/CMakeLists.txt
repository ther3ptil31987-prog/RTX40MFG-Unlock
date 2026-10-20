cmake_minimum_required(VERSION 3.24)
project(BlackwellRuntime LANGUAGES C CXX)
set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native")
# RTX 50: identity, selection, Vulkan admission, OTA and menu policy (no inputs).
add_executable(BlackwellPolicyTests policy.cpp)
# The production RTX 50 adapter check against this machine's NVIDIA adapters.
add_executable(BlackwellAdapterProbe adapter_probe.cpp "${NATIVE}/blackwell_gpu.cpp")
foreach(t BlackwellPolicyTests BlackwellAdapterProbe)
    target_compile_features(${t} PRIVATE cxx_std_20)
    target_compile_definitions(${t} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(${t} PRIVATE "${NATIVE}")
endforeach()
target_link_libraries(BlackwellAdapterProbe PRIVATE d3d12 dxgi)
enable_testing()
add_test(NAME BlackwellPolicyTests COMMAND BlackwellPolicyTests)
# Exit 2 is a setup failure (no NVIDIA D3D12 adapter with a CUDA device), not a pass.
add_test(NAME BlackwellAdapterProbe COMMAND BlackwellAdapterProbe)
