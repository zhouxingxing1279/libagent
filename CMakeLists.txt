cmake_minimum_required(VERSION 3.22)

project(libagent
    VERSION 0.1.0
    DESCRIPTION "A C++20 AI agent framework"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_SHARED_LIBS           "Build libagent as a shared library" OFF)
option(LIBAGENT_BUILD_TESTS        "Build unit tests"                    ON)
option(LIBAGENT_BUILD_EXAMPLES     "Build examples"                      OFF)
option(LIBAGENT_WITH_SSL           "Enable TLS (HTTPS) support"          ON)
option(LIBAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"   ON)
option(LIBAGENT_BUILD_DOCS         "Build API documentation (Doxygen)"   OFF)
option(LIBAGENT_WITH_SPDLOG        "Enable the spdlog logging adapter"   OFF)

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hide all symbols by default; only LIBAGENT_API symbols cross the boundary.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Position-independent code so the static lib can be linked into shared libs.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libagent/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/libagent/version.h
    @ONLY)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
# Core abstractions + HTTP client + OpenAI-compatible provider.
add_library(libagent
    src/version.cpp
    src/types.cpp
    src/tool.cpp
    src/memory.cpp
    src/http/https_client.cpp
    src/http/retry.cpp
    src/providers/openai/openai_provider.cpp
    src/providers/openai/openai_embeddings.cpp
    src/providers/anthropic/anthropic_provider.cpp
    src/agent.cpp
    src/orchestration.cpp
    src/summarizing_memory.cpp
    src/tools/http.cpp)
add_library(libagent::libagent ALIAS libagent)

libagent_target_warnings(libagent)

target_compile_features(libagent PUBLIC cxx_std_20)

target_include_directories(libagent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Public dependencies (appear in downstream find_package(libagent)).
# nlohmann/json is vendored under third_party/ (include path only, no link
# target) so it never leaks into the export set. Boost is header-only for our
# usage (Asio/Beast/System are all header-only since Boost 1.69).
target_link_libraries(libagent
    PUBLIC
        Boost::headers)

if(LIBAGENT_WITH_SSL)
    target_link_libraries(libagent
        PUBLIC
            OpenSSL::SSL
            OpenSSL::Crypto)
    target_compile_definitions(libagent PUBLIC LIBAGENT_WITH_SSL=1)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LIBAGENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LIBAGENT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# API documentation (Doxygen)
# ---------------------------------------------------------------------------
if(LIBAGENT_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    set(DOXYGEN_PROJECT_NAME libagent)
    set(DOXYGEN_PROJECT_VERSION ${PROJECT_VERSION})
    set(DOXYGEN_OUTPUT_DIRECTORY docs)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_EXTRACT_PRIVATE NO)
    set(DOXYGEN_GENERATE_TREEVIEW YES)
    set(DOXYGEN_QUIET YES)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    doxygen_add_docs(libagent_docs
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        COMMENT "Generating libagent API documentation")
endif()

# ---------------------------------------------------------------------------
# Install / Export
# ---------------------------------------------------------------------------
install(TARGETS libagent
    EXPORT libagentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Public headers (.in template excluded by the file pattern).
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
# Vendored json header ships inside the include tree.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")
# Generated version.h.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/libagent/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libagent)

install(EXPORT libagentTargets
    FILE libagentTargets.cmake
    NAMESPACE libagent::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libagent)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libagentConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libagentConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libagent)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/libagentConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/libagentConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/libagentConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libagent)
