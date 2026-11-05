cmake_minimum_required(VERSION 3.10)
project(renderdoc_mobile_jni LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "This target is only for Android")
endif()

if(NOT DEFINED RENDERDOC_SOURCE_ROOT)
    message(FATAL_ERROR "Define RENDERDOC_SOURCE_ROOT to the RenderDoc checkout")
endif()

if(NOT DEFINED RENDERDOC_CORE_LIBRARY OR NOT EXISTS "${RENDERDOC_CORE_LIBRARY}")
    message(FATAL_ERROR "Pass RENDERDOC_CORE_LIBRARY to the built ARM64 RenderDoc .so")
endif()

add_library(renderdoc_core SHARED IMPORTED)
set_target_properties(renderdoc_core PROPERTIES
    IMPORTED_LOCATION "${RENDERDOC_CORE_LIBRARY}")

add_library(renderdoc_mobile_jni SHARED renderdoc_mobile_jni.cpp)
target_compile_definitions(renderdoc_mobile_jni PRIVATE RENDERDOC_PLATFORM_ANDROID)
target_include_directories(renderdoc_mobile_jni PRIVATE
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api"
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api/replay"
)
target_compile_features(renderdoc_mobile_jni PRIVATE cxx_std_14)
target_link_libraries(renderdoc_mobile_jni PRIVATE renderdoc_core dl log)
set_target_properties(renderdoc_mobile_jni PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Real Vulkan replay bridge in a dedicated Android process.
add_library(renderdoc_replay_jni SHARED renderdoc_replay_jni.cpp)
target_compile_definitions(renderdoc_replay_jni PRIVATE RENDERDOC_PLATFORM_ANDROID)
target_include_directories(renderdoc_replay_jni PRIVATE
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api"
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api/replay")
target_compile_features(renderdoc_replay_jni PRIVATE cxx_std_14)
target_link_libraries(renderdoc_replay_jni PRIVATE renderdoc_core dl log)
set_target_properties(renderdoc_replay_jni PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Dedicated app-local Vulkan capture test, isolated from the replay reader.
add_library(renderdoc_vulkan_demo SHARED vk_demo.cpp)
target_include_directories(renderdoc_vulkan_demo PRIVATE
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api/app")
target_compile_features(renderdoc_vulkan_demo PRIVATE cxx_std_14)
target_link_libraries(renderdoc_vulkan_demo PRIVATE vulkan dl log)
set_target_properties(renderdoc_vulkan_demo PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
