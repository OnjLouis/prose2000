cmake_minimum_required(VERSION 3.20)
project(prose2k_engine LANGUAGES CXX)

set(PROSE_CPU_SOURCES
	third_party/doubletalk_shim/emu.cpp
	third_party/mame_i86/i86.cpp)

add_library(prose_dsp SHARED prose_dsp.cpp)
add_executable(prose_cli
	prose_cli.cpp
	prose_board.cpp
	prose_dsp.cpp
	${PROSE_CPU_SOURCES})
add_executable(ProseHost
	prose_host.cpp
	prose_engine.cpp
	prose_board.cpp
	prose_dsp.cpp
	${PROSE_CPU_SOURCES})

foreach(target prose_dsp prose_cli ProseHost)
	target_compile_features(${target} PRIVATE cxx_std_20)
	target_include_directories(${target} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/doubletalk_shim
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/mame_i86)
endforeach()
target_compile_definitions(prose_dsp PRIVATE PROSE_DSP_BUILD)
target_compile_definitions(prose_cli PRIVATE PROSE_DSP_BUILD)
target_compile_definitions(ProseHost PRIVATE PROSE_DSP_BUILD)
if(MSVC)
	target_compile_options(prose_dsp PRIVATE /W4 /permissive-)
	target_compile_options(prose_cli PRIVATE /W4 /permissive-)
else()
	foreach(target prose_dsp prose_cli ProseHost)
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
		target_link_options(${target} PRIVATE -static)
	endforeach()
	set_target_properties(prose_dsp PROPERTIES PREFIX "")
endif()
