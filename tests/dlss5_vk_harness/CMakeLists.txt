cmake_minimum_required(VERSION 3.24)
project(Dlss5VkHarness LANGUAGES CXX)
# vkdlss5.1: DLSS 5 on Vulkan through MFG Unlocker on a real GPU. A Vulkan app runs DLSS Super Resolution on the
# NGX Vulkan route with MFGUnlocker.dll as its winmm.dll and a DLSSNR provider (nvngx_dlssnr.dll) beside it, turns
# DLSS 5 on and off through RTXMFG-Universal.json and compares the DLSS output read back. Needs an NVIDIA GPU, the
# driver NGX runtime and the Vulkan SDK; not a CTest (the provider is not shipped). Run it through a fresh run folder.
set(STREAMLINE_ROOT "" CACHE PATH "Streamline SDK 2.14.1 root (external/ngx-sdk)")
set(VULKAN_SDK_ROOT "" CACHE PATH "Vulkan SDK root (Include, Lib/vulkan-1.lib)")
set(NGX "${STREAMLINE_ROOT}/external/ngx-sdk")
if(NOT EXISTS "${NGX}/include/nvsdk_ngx_vk.h" OR NOT EXISTS "${VULKAN_SDK_ROOT}/Include/vulkan/vulkan.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT and VULKAN_SDK_ROOT")
endif()
add_executable(Dlss5VkHarness main.cpp)
target_compile_features(Dlss5VkHarness PRIVATE cxx_std_20)
target_compile_definitions(Dlss5VkHarness PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(Dlss5VkHarness PRIVATE "${NGX}/include" "${VULKAN_SDK_ROOT}/Include")
# nvsdk_ngx_d.lib is built against the dynamic CRT.
set_target_properties(Dlss5VkHarness PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_link_libraries(Dlss5VkHarness PRIVATE "${NGX}/lib/Windows_x86_64/nvsdk_ngx_d.lib" "${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib" winmm)

# The D3D12 twin: the same scene, settings and frame counts through the D3D12 stage, for compare_outputs.py.
add_executable(Dlss5D3d12Harness d3d12.cpp)
target_compile_features(Dlss5D3d12Harness PRIVATE cxx_std_20)
target_compile_definitions(Dlss5D3d12Harness PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(Dlss5D3d12Harness PRIVATE "${NGX}/include")
set_target_properties(Dlss5D3d12Harness PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_link_libraries(Dlss5D3d12Harness PRIVATE "${NGX}/lib/Windows_x86_64/nvsdk_ngx_d.lib" d3d12 dxgi winmm)
