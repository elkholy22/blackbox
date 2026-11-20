cmake_minimum_required(VERSION 3.25)
project(blackbox VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLACKBOX_WERROR "Treat compiler warnings as errors" OFF)
option(BLACKBOX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BLACKBOX_TESTS "Build the unit tests and register the component tests" ON)

if(BLACKBOX_SANITIZE)
    # Global on purpose, so GoogleTest is instrumented the same way as our code.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Warnings apply to our targets only, not to fetched third party code.
add_library(blackbox_warnings INTERFACE)
target_compile_options(blackbox_warnings INTERFACE -Wall -Wextra -Wpedantic $<$<BOOL:${BLACKBOX_WERROR}>:-Werror>)

# Everything that can be tested without starting processes. The unit tests link this.
add_library(blackbox_core STATIC
    src/dump.cpp
    src/exit_status.cpp
    src/line_assembler.cpp
    src/options.cpp
)
target_include_directories(blackbox_core PUBLIC src)
target_link_libraries(blackbox_core PRIVATE blackbox_warnings)

add_executable(blackbox src/main.cpp src/supervisor.cpp)
target_compile_definitions(blackbox PRIVATE BLACKBOX_VERSION="${PROJECT_VERSION}")
target_link_libraries(blackbox PRIVATE blackbox_core blackbox_warnings)

install(TARGETS blackbox RUNTIME DESTINATION bin)

if(BLACKBOX_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)

    # Uses the Python of an active virtualenv if there is one (see CONTRIBUTING.md).
    find_package(Python3 3.10 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME component_tests
                 COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_SOURCE_DIR}/tests/component
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(component_tests PROPERTIES
            ENVIRONMENT "BLACKBOX_BIN=$<TARGET_FILE:blackbox>;BLACKBOX_SANITIZED=$<BOOL:${BLACKBOX_SANITIZE}>"
            TIMEOUT 300)
    else()
        message(WARNING "Python 3.10+ not found, component tests are not registered")
    endif()
endif()
