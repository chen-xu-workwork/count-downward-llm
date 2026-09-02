cmake_minimum_required(VERSION 3.5)

if(NOT FAST_DOWNWARD_MAIN_CMAKELISTS_READ)
    message(
        FATAL_ERROR
        "Run cmake on the CMakeLists.txt in the 'src' directory, "
        "not the one in 'src/search'. Please delete CMakeCache.txt "
        "from the current directory and restart cmake.")
endif()


## == Project ==

project(downward)
fast_downward_set_compiler_flags()
fast_downward_set_linker_flags()

# Collect source files needed for the active plugins.
include("${CMAKE_CURRENT_SOURCE_DIR}/DownwardFiles.cmake")
add_executable(downward ${PLANNER_SOURCES})

## == Includes ==

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ext)

## == Libraries ==

# The LLM bridge uses std::thread. Relying on the compiler to add the native
# thread library implicitly is not portable (in particular with Conda GCC on
# Linux), so make the dependency explicit for every platform.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(downward Threads::Threads)

# Some Conda GCC toolchains run CMake's feature checks against a host glibc
# where pthread symbols are part of libc, but link the final executable with a
# sysroot that still needs the compiler's pthread option. In that mixed setup
# Threads::Threads can be empty, so pass -pthread explicitly on Linux.
if(UNIX AND NOT APPLE)
    target_compile_options(downward PRIVATE -pthread)
    target_link_libraries(downward -pthread)
endif()

# On Linux, find the rt library for clock_gettime().
if(UNIX AND NOT APPLE)
    target_link_libraries(downward rt)
endif()

# On Windows, find the psapi library for determining peak memory.
if(WIN32)
    target_link_libraries(downward psapi)
endif()

# If any enabled plugin requires an LP solver, compile with all
# available LP solvers. If no solvers are installed, the planner will
# still compile, but using heuristics that depend on an LP solver will
# cause an error.
if(PLUGIN_LP_SOLVER_ENABLED)
    find_package(OSI OPTIONAL_COMPONENTS Cpx Clp)
    if(OSI_FOUND AND (OSI_Cpx_FOUND OR OSI_Clp_FOUND))
        foreach(SOLVER Cpx Clp IPCpx)
            if(OSI_${SOLVER}_FOUND)
                string(TOUPPER ${SOLVER} TMP_SOLVER_UPPER_CASE)
                mark_as_advanced(TMP_SOLVER_UPPER_CASE)
                add_definitions("-D COIN_HAS_${TMP_SOLVER_UPPER_CASE}")
                include_directories(${OSI_${SOLVER}_INCLUDE_DIRS})
                target_link_libraries(downward ${OSI_${SOLVER}_LIBRARIES})
            endif()
        endforeach()

        # Note that basic OSI libs must be added after (!) all OSI solver libs.
        add_definitions("-D USE_LP")
        include_directories(${OSI_INCLUDE_DIRS})
        target_link_libraries(downward ${OSI_LIBRARIES})
    endif()

    ## ADD COMPONENTS FOR IPCplex
    if(OSI_FOUND AND OSI_Cpx_FOUND)
	find_package(Concert)
	find_package(IloCplex)
    	if (CONCERT_FOUND AND ILOCPLEX_FOUND)
	    add_definitions("-D COIN_HAS_IPCPLEX")
            include_directories(${ILOCPLEX_INCLUDE_DIRS})
            include_directories(${CONCERT_INCLUDE_DIRS})
            target_link_libraries(downward ${ILOCPLEX_LIBRARIES})
            target_link_libraries(downward ${CONCERT_LIBRARIES})

	endif()
    endif()

    if(OSI_Cpx_FOUND AND CPLEX_RUNTIME_LIBRARY)
        add_custom_command(TARGET downward POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${CPLEX_RUNTIME_LIBRARY}
            $<TARGET_FILE_DIR:downward>
        )
    endif()
endif()

if(PLUGIN_GUROBI_IP_COMPILATION_ENABLED)
    find_package(Gurobi)
    if (GUROBI_FOUND)
        include_directories(downward ${GUROBI_INCLUDE_DIRS})
        target_link_libraries(downward ${GUROBI_LIBRARIES})
    endif()
endif()

if(PLUGIN_SYMMETRIES_ENABLED)
    include(ExternalProject)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/bliss-0.73)
    ExternalProject_add(
            bliss
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bliss-0.73/
            CONFIGURE_COMMAND ""
            BUILD_COMMAND make
            INSTALL_COMMAND ""
            BUILD_IN_SOURCE 1
    )
    add_dependencies(downward bliss)
    target_link_libraries(downward ${CMAKE_CURRENT_SOURCE_DIR}/bliss-0.73/libbliss.a)
endif()
