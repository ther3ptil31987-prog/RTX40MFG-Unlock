cmake_minimum_required(VERSION 3.24)
project(MfgLocalizationTests LANGUAGES CXX)
# Menu localisation: runtime lookup and Auto rules (fixture catalogs), the shipped
# catalogs and their Windows fonts (pinned ImGui font loader), the catalog
# generator's rejections, and that every catalog matches the sources.
set(NATIVE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native" CACHE PATH "Release native source")
set(IMGUI_ROOT "" CACHE PATH "Pinned ImGui tree (catalog font coverage)")
if(NOT EXISTS "${IMGUI_ROOT}/imgui.h")
    message(FATAL_ERROR "Set IMGUI_ROOT to the pinned ImGui tree")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(GENERATED_SHIPPED "${CMAKE_CURRENT_BINARY_DIR}/generated-shipped")
set(GENERATED_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/generated-fixture")
foreach(pair "${NATIVE_SOURCE}/locale|${GENERATED_SHIPPED}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures|${GENERATED_FIXTURE}")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 locale)
    list(GET pair 1 output)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${NATIVE_SOURCE}/generate_locale.py" generate
        --locale "${locale}" --output "${output}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
file(GLOB LOCALE_INPUTS CONFIGURE_DEPENDS "${NATIVE_SOURCE}/locale/*.po" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*.po")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NATIVE_SOURCE}/generate_locale.py" ${LOCALE_INPUTS})

function(localization_test target generated)
    add_executable(${target} ${ARGN} "${NATIVE_SOURCE}/localization.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    target_include_directories(${target} PRIVATE "${NATIVE_SOURCE}" "${generated}")
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()

localization_test(LocalizationRuntimeTests "${GENERATED_FIXTURE}" runtime_tests.cpp)
localization_test(LocalizationCatalogTests "${GENERATED_SHIPPED}" catalog_tests.cpp)
add_library(LocalizationImGui STATIC "${IMGUI_ROOT}/imgui.cpp" "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp" "${IMGUI_ROOT}/imgui_tables.cpp")
target_include_directories(LocalizationImGui PUBLIC "${IMGUI_ROOT}")
set_target_properties(LocalizationImGui PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(LocalizationCatalogTests PRIVATE LocalizationImGui)

enable_testing()
add_test(NAME runtime COMMAND LocalizationRuntimeTests "${CMAKE_CURRENT_SOURCE_DIR}/format_cases.txt")
add_test(NAME shipped_catalogs COMMAND LocalizationCatalogTests)
add_test(NAME generator COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_generate_locale.py"
    --native "${NATIVE_SOURCE}" --cases "${CMAKE_CURRENT_SOURCE_DIR}/format_cases.txt")
add_test(NAME sources_current COMMAND "${Python3_EXECUTABLE}" -I "${NATIVE_SOURCE}/generate_locale.py" check
    --locale "${NATIVE_SOURCE}/locale" --source "${NATIVE_SOURCE}" --require-complete)
