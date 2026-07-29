add_executable(hello_agent 01_hello_agent.cpp)
target_link_libraries(hello_agent PRIVATE libagent::libagent)
libagent_target_warnings(hello_agent)

add_executable(tool_agent 02_tool_agent.cpp)
target_link_libraries(tool_agent PRIVATE libagent::libagent)
libagent_target_warnings(tool_agent)

add_executable(handoff_agent 03_handoff.cpp)
target_link_libraries(handoff_agent PRIVATE libagent::libagent)
libagent_target_warnings(handoff_agent)

# 04_cli_agent uses cli::command, which depends on Boost.Process v2 (opt-in).
find_package(Boost CONFIG REQUIRED COMPONENTS process)
add_executable(cli_agent 04_cli_agent.cpp)
target_link_libraries(cli_agent PRIVATE libagent::libagent Boost::process)
libagent_target_warnings(cli_agent)
