cmake_minimum_required(VERSION 3.24)
project(AmpereRuntimeRegression LANGUAGES C CXX RC)
set(NATIVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
add_executable(AmpereBatchTests batch_tests.cpp)
add_executable(AmpereOptionsTests options_tests.cpp)
add_executable(AmpereFlipMeteringTests flip_metering_tests.cpp)
add_executable(AmpereStatusFieldsTests status_fields_tests.cpp
    "${NATIVE_SOURCE}/ampere_provider_arch.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_link_libraries(AmpereStatusFieldsTests PRIVATE version psapi)
add_executable(StreamlineOtaTests ota_tests.cpp "${NATIVE_SOURCE}/nvidia_mfg_policy.cpp"
    "${NATIVE_SOURCE}/optiscaler_host.cpp")
add_executable(AmpereArchitectureTests architecture_tests.cpp
    "${NATIVE_SOURCE}/ampere_provider_arch.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_link_libraries(AmpereArchitectureTests PRIVATE version psapi)
add_executable(AmpereWrapperTests wrapper_tests.cpp
    "${NATIVE_SOURCE}/ampere_wrapper_capacity.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
add_executable(DlssgSelectionTests selection_tests.cpp
    "${NATIVE_SOURCE}/dlssg_provider_selection.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp")
target_link_libraries(DlssgSelectionTests PRIVATE version wintrust crypt32)
target_compile_definitions(DlssgSelectionTests PRIVATE MFG_PROVIDER_SELECTION_TESTING=1)
foreach(target AmpereBatchTests AmpereOptionsTests AmpereFlipMeteringTests AmpereStatusFieldsTests StreamlineOtaTests AmpereArchitectureTests AmpereWrapperTests DlssgSelectionTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${NATIVE_SOURCE}")
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
foreach(target SelectionOld SelectionNew SelectionUnsupported SelectionSibling)
    add_library(${target} SHARED selection_fixture.cpp selection_fixture.rc)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
target_compile_definitions(SelectionOld PRIVATE FIXTURE_MINOR=1 FIXTURE_BUILD=0)
target_compile_definitions(SelectionNew PRIVATE FIXTURE_MINOR=9 FIXTURE_BUILD=1)
target_compile_definitions(SelectionUnsupported PRIVATE FIXTURE_MINOR=10 FIXTURE_BUILD=0)
target_compile_definitions(SelectionSibling PRIVATE FIXTURE_MINOR=9 FIXTURE_BUILD=1 FIXTURE_SIBLING=1)
