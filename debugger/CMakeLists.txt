set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(qjs_debug
    src/main.cpp
    src/cdp/websocket_server.cpp
    src/cdp/cdp_handler.cpp
    src/cdp/debug_session.cpp
)

add_executable(debug_session_cycle_test
    tests/debug_session_cycle_test.cpp
    src/cdp/debug_session.cpp
)

target_include_directories(qjs_debug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cdp
    ${CMAKE_SOURCE_DIR}/quickjs
)

target_include_directories(debug_session_cycle_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cdp
    ${CMAKE_SOURCE_DIR}/quickjs
)

target_link_libraries(qjs_debug PRIVATE qjs)
target_link_libraries(debug_session_cycle_test PRIVATE qjs)

if(WIN32)
    target_link_libraries(qjs_debug PRIVATE ws2_32)
    target_compile_definitions(qjs_debug PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
    )
    target_compile_definitions(debug_session_cycle_test PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
    )
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(qjs_debug PRIVATE Threads::Threads)
    target_link_libraries(debug_session_cycle_test PRIVATE Threads::Threads)
endif()

add_test(NAME debug_session_cycle_test COMMAND debug_session_cycle_test)
