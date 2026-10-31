# test-harness: test-only plugin side of d2rAI-toolkit's harness protocol. Never released.
#
# Build:  powershell -ExecutionPolicy Bypass -File build.ps1
# Output: build/Release/test-harness.dll -> copy it to <slot>/d2rloader/plugins/
cmake_minimum_required(VERSION 3.29)
project(test_harness LANGUAGES CXX RC)

set(D2RL_SDK_DIR "" CACHE PATH "D2RLoader PluginSDK directory (github.com/D2RLoader/PluginSDK at 717f727, SDK 0.3.0)")
if(NOT D2RL_SDK_DIR)
	message(FATAL_ERROR "Pass -DD2RL_SDK_DIR=<PluginSDK checkout>")
endif()

set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${D2RL_SDK_DIR} PluginSDK EXCLUDE_FROM_ALL)

add_library(test_harness SHARED
	src/plugin.cpp
	src/log.cpp
	src/test-harness.rc
)
target_link_libraries(test_harness PRIVATE D2RLPlugin::D2RLPlugin user32 ole32)
set_target_properties(test_harness PROPERTIES OUTPUT_NAME "test-harness")
if(MSVC)
	target_compile_options(test_harness PRIVATE /W4 /permissive- /EHsc /utf-8)
	set_property(TARGET test_harness PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: no VC++ runtime dependency
endif()
