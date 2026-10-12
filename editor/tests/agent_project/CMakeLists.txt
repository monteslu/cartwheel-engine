# The game as a loadable module: the editor loads its node types from it and
# play-in-editor runs it under cwhot, which swaps rebuilt code in live. Engine
# symbols come from the host (editor / cwhot), so only headers are needed:
#   cmake -S . -B build -G Ninja -DCARTWHEEL_ROOT=<cartwheel source tree>
cmake_minimum_required(VERSION 3.20)
project(agent_demo C)
set(CARTWHEEL_ROOT "" CACHE PATH "the cartwheel source tree (headers)")
if(NOT CARTWHEEL_ROOT)
  message(FATAL_ERROR "pass -DCARTWHEEL_ROOT=<cartwheel source tree>")
endif()
set(CMAKE_C_STANDARD 17)
# with its three.c program tables (programs/: the states the game draws), which
# cwhot registers when it loads the module
add_library(game_hot MODULE game.c programs/agent_demo_programs.c programs/agent_demo_gl.c programs/agent_demo_wgpu.c)
# the three.c headers the engine was built with (CW_THREE_DIR: a worktree's pin, cmake/deps.cmake)
if(DEFINED ENV{CW_THREE_DIR})
  set(_t3_inc $ENV{CW_THREE_DIR}/include)
else()
  set(_t3_inc ${CARTWHEEL_ROOT}/deps/three.c/include)
endif()
set_target_properties(game_hot PROPERTIES PREFIX "")
target_include_directories(game_hot PRIVATE ${CARTWHEEL_ROOT}/include ${_t3_inc} ${_t3_inc}/../src
  ${CARTWHEEL_ROOT}/deps/box2d/include ${CARTWHEEL_ROOT}/deps/box3d/include ${CARTWHEEL_ROOT}/deps/miniaudio)
target_compile_options(game_hot PRIVATE -O1 -ffp-contract=off -fno-fast-math)
target_compile_definitions(game_hot PRIVATE CW_HOT_MODULE=1)
