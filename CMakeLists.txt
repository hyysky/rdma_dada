cmake_minimum_required(VERSION 3.5)
project(rdma_dada C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to enable CUDA support; default is OFF for non-CUDA systems
option(USE_CUDA "Enable CUDA support (requires CUDA toolkit)" OFF) # Default is OFF， change to ON if have CUDA

# If CUDA is not used, define NO_CUDA so source files skip CUDA includes/calls
if(NOT USE_CUDA)
    add_compile_definitions(NO_CUDA)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PSRDADA psrdada)

# If pkg-config can't find psrdada, try manual search
if(NOT PSRDADA_FOUND)
    message(STATUS "psrdada not found via pkg-config, trying manual search...")
    
    # Try to find psrdada headers
    find_path(PSRDADA_INCLUDE_DIR 
        NAMES dada_hdu.h
        PATHS /usr/local/include /usr/include
    )
    
    # Try to find psrdada library
    find_library(PSRDADA_LIBRARY
        NAMES psrdada
        PATHS /usr/local/lib /usr/lib /usr/local/lib64 /usr/lib64
    )
    
    if(PSRDADA_INCLUDE_DIR AND PSRDADA_LIBRARY)
        set(PSRDADA_FOUND TRUE)
        set(PSRDADA_INCLUDE_DIRS ${PSRDADA_INCLUDE_DIR})
        set(PSRDADA_LIBRARIES ${PSRDADA_LIBRARY})
        message(STATUS "Found psrdada manually:")
        message(STATUS "  Include: ${PSRDADA_INCLUDE_DIRS}")
        message(STATUS "  Library: ${PSRDADA_LIBRARIES}")
    else()
        message(FATAL_ERROR "psrdada not found! Headers: ${PSRDADA_INCLUDE_DIR}, Library: ${PSRDADA_LIBRARY}")
    endif()
endif()

find_package(Threads REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${PSRDADA_INCLUDE_DIRS})

set(SRCS
    src/ibv_utils.cpp
    src/pkt_gen.cpp
    src/RoCEv2Dada.cpp
    src/psrdada_ringbuf.cpp
    src/dada_header.cpp
)

add_executable(Demo_psrdada_online demo/Demo_psrdada_online.cpp ${SRCS})
target_include_directories(Demo_psrdada_online PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(Demo_psrdada_online PRIVATE _GNU_SOURCE)
target_link_libraries(Demo_psrdada_online ${PSRDADA_LIBRARIES} ibverbs ${CMAKE_THREAD_LIBS_INIT} rt)
