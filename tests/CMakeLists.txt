include(GoogleTest)

set(libagent_tests
    test_smoke
    test_types
    test_tool_registry
    test_memory
    test_provider
    test_http_client
    test_openai_provider
    test_agent
    test_orchestration
    test_summarizing_memory
    test_rag)

foreach(t IN LISTS libagent_tests)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE libagent::libagent GTest::gtest_main)
    target_include_directories(${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    libagent_target_warnings(${t})
    gtest_discover_tests(${t})
endforeach()
