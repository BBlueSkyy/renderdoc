cmake_minimum_required(VERSION 3.10)
project(renderdoc_mobile_jni LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "This target is only for Android")
endif()

if(NOT DEFINED RENDERDOC_SOURCE_ROOT)
    message(FATAL_ERROR "Define RENDERDOC_SOURCE_ROOT to the RenderDoc checkout")
endif()

add_library(renderdoc_mobile_jni SHARED renderdoc_mobile_jni.cpp)
target_include_directories(renderdoc_mobile_jni PRIVATE
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api"
    "${RENDERDOC_SOURCE_ROOT}/renderdoc/api/replay"
)
target_compile_features(renderdoc_mobile_jni PRIVATE cxx_std_14)
target_link_libraries(renderdoc_mobile_jni PRIVATE dl log)
set_target_properties(renderdoc_mobile_jni PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
