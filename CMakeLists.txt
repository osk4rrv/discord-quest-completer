cmake_minimum_required(VERSION 3.24)
project(QuestCompleter VERSION 4.6.1 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Quest Completer is a native Windows application.")
endif()

add_executable(QuestCompleter WIN32
    src/main.cpp
    src/services.hpp
    src/catalog.inc
    src/executable_sources.inc
    src/runtime.inc
    src/steam_placement.inc
    src/runtime_tests.inc
    src/resources.rc
)
target_compile_features(QuestCompleter PRIVATE cxx_std_17)
target_compile_definitions(QuestCompleter PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_link_libraries(QuestCompleter PRIVATE
    d2d1 d3d11 dxgi dwrite dwmapi windowscodecs ole32 shell32 user32 gdi32 winhttp advapi32 uuid
)
if(MSVC)
    set_property(TARGET QuestCompleter PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(QuestCompleter PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(QuestCompleter PRIVATE /MANIFEST:NO)
endif()

enable_testing()
add_test(NAME interface_state COMMAND QuestCompleter --self-test)
add_test(NAME native_window COMMAND QuestCompleter --window-test)
add_test(NAME resize_stress COMMAND QuestCompleter --resize-test)
add_test(NAME session_runtime COMMAND QuestCompleter --runtime-test)
set_tests_properties(interface_state native_window PROPERTIES TIMEOUT 15)
set_tests_properties(resize_stress PROPERTIES TIMEOUT 25)
set_tests_properties(session_runtime PROPERTIES TIMEOUT 25)
install(TARGETS QuestCompleter RUNTIME DESTINATION .)
