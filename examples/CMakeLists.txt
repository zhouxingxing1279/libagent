add_executable(hello_agent 01_hello_agent.cpp)
target_link_libraries(hello_agent PRIVATE libagent::libagent)
libagent_target_warnings(hello_agent)
