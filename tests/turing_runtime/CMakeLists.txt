cmake_minimum_required(VERSION 3.24)
project(TuringRuntime LANGUAGES C CXX)
set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native")
add_executable(TuringPolicyTests policy.cpp)
add_executable(TuringDeclarationAudit declaration.cpp "${NATIVE}/ampere_provider_arch.cpp"
    "${NATIVE}/dlssg_provider_policy.cpp" "${NATIVE}/third_party/minhook/src/hde/hde64.c")
add_executable(TuringProgramAudit program.cpp "${NATIVE}/dlssg_provider_policy.cpp"
    "${NATIVE}/ampere_native_cache.cpp" "${NATIVE}/ampere_cuda_program.cpp"
    "${NATIVE}/ampere_mask_transform.cpp" "${NATIVE}/third_party/minhook/src/hde/hde64.c")
add_executable(TuringCombinedPublication publication.cpp "${NATIVE}/dlssg_provider_policy.cpp"
    "${NATIVE}/ampere_native_cache.cpp" "${NATIVE}/ampere_cuda_program.cpp"
    "${NATIVE}/ampere_mask_transform.cpp" "${NATIVE}/third_party/minhook/src/hde/hde64.c")
foreach(t TuringPolicyTests TuringProgramAudit TuringDeclarationAudit TuringCombinedPublication)
    target_compile_features(${t} PRIVATE cxx_std_20)
    target_compile_definitions(${t} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(${t} PRIVATE "${NATIVE}")
    target_link_libraries(${t} PRIVATE bcrypt version)
endforeach()
enable_testing()
add_test(NAME TuringPolicyTests COMMAND TuringPolicyTests)
