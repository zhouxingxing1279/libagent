include(GoogleTest)

set(libagent_tests
    test_smoke
    test_types
    test_tool_registry
    test_memory
    test_provider
    test_http_client
    test_openai_provider
    test_anthropic_provider
    test_agent
    test_orchestration
    test_summarizing_memory
    test_rag
    test_vector_retriever
    test_openai_embeddings
    test_memory_io
    test_agent_bus
    test_http_tool)

foreach(t IN LISTS libagent_tests)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE libagent::libagent GTest::gtest_main)
    target_include_directories(${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    libagent_target_warnings(${t})
    gtest_discover_tests(${t})
endforeach()

# The CLI-tool helper depends on Boost.Process v2 (opt-in; not a core dep).
if(LIBAGENT_BUILD_TESTS)
    find_package(Boost CONFIG REQUIRED COMPONENTS process)
    add_executable(test_cli_tool test_cli_tool.cpp)
    target_link_libraries(test_cli_tool PRIVATE
        libagent::libagent Boost::process GTest::gtest_main)
    target_include_directories(test_cli_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
    libagent_target_warnings(test_cli_tool)
    gtest_discover_tests(test_cli_tool)
endif()

# Optional: spdlog logging adapter (opt-in via -DLIBAGENT_WITH_SPDLOG=ON).
if(LIBAGENT_WITH_SPDLOG)
    add_executable(test_logging_spdlog test_logging_spdlog.cpp)
    target_link_libraries(test_logging_spdlog PRIVATE
        libagent::libagent spdlog::spdlog GTest::gtest_main)
    target_include_directories(test_logging_spdlog PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
    libagent_target_warnings(test_logging_spdlog)
    gtest_discover_tests(test_logging_spdlog)
endif()
