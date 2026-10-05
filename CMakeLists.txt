cmake_minimum_required(VERSION 3.16)
project(ProcessPilotPro VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Process Pilot Pro targets Linux only (it uses procfs, sysfs, pipe2, accept4, ...).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PROCESS_PILOT_WERROR "Treat compiler warnings as errors" ON)
option(PROCESS_PILOT_SANITIZERS "Enable AddressSanitizer + UBSan (Debug builds)" OFF)
option(PROCESS_PILOT_BUILD_TESTS "Build the unit and integration tests" ON)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(Threads REQUIRED)

# Warnings / sanitizers are attached to one interface target and inherited everywhere.
add_library(pilot_options INTERFACE)
target_compile_options(pilot_options INTERFACE -Wall -Wextra -Wpedantic)
if(PROCESS_PILOT_WERROR)
    target_compile_options(pilot_options INTERFACE -Werror)
endif()
if(PROCESS_PILOT_SANITIZERS)
    target_compile_options(pilot_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pilot_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(pilot_core STATIC
    src/config/service_config.cpp
    src/config/config_parser.cpp
    src/dependency/dependency_graph.cpp
    src/process/process_state.cpp
    src/process/process_registry.cpp
    src/process/process_manager.cpp
    src/restart/restart_policy.cpp
    src/logging/logger.cpp
    src/linux/proc_reader.cpp
    src/linux/sysfs_reader.cpp
    src/linux/signal_pipe.cpp
    src/monitor/health_monitor.cpp
    src/supervisor/control_server.cpp
    src/supervisor/supervisor.cpp
    src/cli/cli.cpp
)
target_include_directories(pilot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pilot_core PUBLIC Threads::Threads pilot_options)

add_executable(process-pilot src/main.cpp)
target_link_libraries(process-pilot PRIVATE pilot_core)

# Small C++ program used as a controllable "service" by the examples and tests.
add_executable(demo-service tools/demo_service.cpp)
target_link_libraries(demo-service PRIVATE pilot_options)

if(PROCESS_PILOT_BUILD_TESTS)
    enable_testing()

    function(pilot_add_test name source)
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${name} PRIVATE pilot_core)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    pilot_add_test(test_config        tests/config/test_config_parser.cpp)
    pilot_add_test(test_dependency    tests/dependency/test_dependency_graph.cpp)
    pilot_add_test(test_registry      tests/process/test_process_registry.cpp)
    pilot_add_test(test_restart       tests/restart/test_restart_policy.cpp)
    pilot_add_test(test_monitor       tests/monitor/test_monitor.cpp)
    pilot_add_test(test_integration   tests/integration/test_supervisor_integration.cpp)

    target_compile_definitions(test_integration PRIVATE
        DEMO_SERVICE_PATH="$<TARGET_FILE:demo-service>")
    add_dependencies(test_integration demo-service)
endif()
