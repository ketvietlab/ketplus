option(KETPLUS_USE_COMPILER_CACHE "Use sccache or ccache when available" ON)

if(KETPLUS_USE_COMPILER_CACHE)
    find_program(KETPLUS_COMPILER_CACHE_PROGRAM NAMES sccache ccache)
    if(KETPLUS_COMPILER_CACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER
            "${KETPLUS_COMPILER_CACHE_PROGRAM}"
            CACHE STRING "C compiler cache launcher" FORCE
        )
        set(CMAKE_CXX_COMPILER_LAUNCHER
            "${KETPLUS_COMPILER_CACHE_PROGRAM}"
            CACHE STRING "C++ compiler cache launcher" FORCE
        )
        message(STATUS "Compiler cache: ${KETPLUS_COMPILER_CACHE_PROGRAM}")
    else()
        message(STATUS "Compiler cache: unavailable (install sccache or ccache)")
    endif()
endif()
