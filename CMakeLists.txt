cmake_minimum_required(VERSION 3.24)
project(dtls_repro CXX)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(WIDEN_WINDOW "Apply widen-window.patch to libdatachannel" OFF)
option(FIX "Apply fix.patch to libdatachannel" OFF)

# fix.patch goes first: widen-window.patch puts its sleep next to the lines fix.patch moves
set(patches "")
if(FIX)
	list(APPEND patches ${CMAKE_CURRENT_SOURCE_DIR}/fix.patch)
endif()
if(WIDEN_WINDOW)
	list(APPEND patches ${CMAKE_CURRENT_SOURCE_DIR}/widen-window.patch)
endif()
set(patch_command PATCH_COMMAND git checkout -- .) # start clean, also when reconfiguring
if(patches)
	list(APPEND patch_command COMMAND git apply ${patches})
endif()

# libdatachannel v0.24.6, static, with OpenSSL (its default), no media, no websockets
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(libdatachannel
	GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
	GIT_TAG v0.24.6
	GIT_SHALLOW TRUE
	GIT_SUBMODULES deps/plog deps/usrsctp deps/libjuice
	${patch_command})
FetchContent_MakeAvailable(libdatachannel)

add_executable(repro repro.cpp)
set_target_properties(repro PROPERTIES CXX_STANDARD 17)
find_package(OpenSSL REQUIRED) # only to print its version
target_link_libraries(repro PRIVATE LibDataChannel::LibDataChannel OpenSSL::Crypto)
