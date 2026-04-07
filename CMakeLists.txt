cmake_minimum_required(VERSION 3.19)
project(zen-browser LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CEF root directory
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef")

# Determine CEF binary and resource directories early
if(EXISTS "${CEF_ROOT}/Release")
    set(CEF_BINARY_DIR_LOCAL "${CEF_ROOT}/Release")
elseif(EXISTS "${CEF_ROOT}/Debug")
    set(CEF_BINARY_DIR_LOCAL "${CEF_ROOT}/Debug")
else()
    message(FATAL_ERROR "CEF binary distribution not found. Download it to third_party/cef/")
endif()
set(CEF_RESOURCE_DIR "${CEF_ROOT}/Resources")

# Load CEF cmake configuration via find_package
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# Build the CEF wrapper library
add_subdirectory(${CEF_ROOT}/libcef_dll libcef_dll_wrapper)

# Source files
set(ZEN_SOURCES
    src/main.cc
    src/browser/browser_app.cc
    src/browser/browser_client.cc
    src/browser/browser_window.cc
    src/browser/tab_manager.cc
    src/browser/scheme_handler.cc
    src/browser/query_handler.cc
    src/blocker/filter_rules.cc
    src/blocker/request_filter.cc
)

# Find GTK3
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# Main executable
add_executable(zen-browser ${ZEN_SOURCES})

target_include_directories(zen-browser PRIVATE
    ${CEF_ROOT}
    ${CMAKE_SOURCE_DIR}/src
    ${GTK3_INCLUDE_DIRS}
)

# Link CEF + GTK
target_link_libraries(zen-browser
    libcef_dll_wrapper
    "${CEF_BINARY_DIR_LOCAL}/libcef.so"
    pthread
    ${GTK3_LIBRARIES}
)

# CEF helper: set RPATH so libcef.so is found at runtime
set_target_properties(zen-browser PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
    INSTALL_RPATH "$ORIGIN"
)

# Copy CEF binaries to build directory
file(GLOB CEF_BINARY_FILES "${CEF_BINARY_DIR_LOCAL}/*")
foreach(_file ${CEF_BINARY_FILES})
    get_filename_component(_name ${_file} NAME)
    if(NOT IS_DIRECTORY ${_file})
        configure_file(${_file} "${CMAKE_BINARY_DIR}/${_name}" COPYONLY)
    endif()
endforeach()

# Copy CEF resources to build directory
if(EXISTS "${CEF_RESOURCE_DIR}")
    file(GLOB CEF_RESOURCE_FILES "${CEF_RESOURCE_DIR}/*")
    foreach(_file ${CEF_RESOURCE_FILES})
        get_filename_component(_name ${_file} NAME)
        if(IS_DIRECTORY ${_file})
            file(COPY ${_file} DESTINATION "${CMAKE_BINARY_DIR}")
        else()
            configure_file(${_file} "${CMAKE_BINARY_DIR}/${_name}" COPYONLY)
        endif()
    endforeach()
endif()

# Copy UI assets to build directory (at configure time)
file(COPY "${CMAKE_SOURCE_DIR}/src/ui/" DESTINATION "${CMAKE_BINARY_DIR}/ui")

# Always sync UI assets on every build (even if no C++ changed)
add_custom_target(sync_ui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/ui" "${CMAKE_BINARY_DIR}/ui"
    COMMENT "Syncing UI assets"
)

# Copy filter list resources (if they exist)
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/")
    file(COPY "${CMAKE_SOURCE_DIR}/resources/" DESTINATION "${CMAKE_BINARY_DIR}/resources")
endif()
