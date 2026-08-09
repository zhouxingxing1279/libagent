# ---------------------------------------------------------------------------
# System dependencies
#
# Boost and OpenSSL are found via find_package rather than FetchContent because
# fetching all of Boost is impractical and OpenSSL must use the platform's
# TLS/cert infrastructure. They are PUBLIC deps of libagent, so they are
# resolved again by downstream consumers through libagentConfig.cmake.
#
# CONFIG mode + Boost::headers: since Boost >= 1.69, Boost.System is header-only
# (no separate boost_system component in Boost 1.90), and Boost.Asio / Beast are
# header-only too — so the headers target is all libagent needs.
# ---------------------------------------------------------------------------
find_package(Boost CONFIG REQUIRED COMPONENTS headers)

if(LIBAGENT_WITH_SSL)
    find_package(OpenSSL REQUIRED)
endif()

# ---------------------------------------------------------------------------
# GoogleTest (tests only — never installed or exported)
# ---------------------------------------------------------------------------
if(LIBAGENT_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # On Windows avoid overriding the parent project's runtime library.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # GoogleTest is test-only: never let its install() rules pollute the prefix.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# spdlog (PRIVATE logging) is added when src/ starts using it (Phase 1+).
if(LIBAGENT_WITH_SPDLOG)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()
