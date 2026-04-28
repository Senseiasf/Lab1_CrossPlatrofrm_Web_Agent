cmake_minimum_required(VERSION 3.15)
project(WebAgent)

set(CMAKE_CXX_STANDARD 17)

# ─── Зависимости ────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.x
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPR_FORCE_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

# На Linux предпочитаем системный libcurl, на Windows — тот, что тянет cpr.
if(UNIX)
    find_package(CURL REQUIRED)
endif()

# ─── Основное приложение ────────────────────────────────────
add_executable(${PROJECT_NAME}
    main.cpp
    HTTP_client.cpp
    json_functions.cpp
    consoletab.cpp
    logger.cpp
)
# 1. Принудительно задаем расширение .exe для основного файла (чтобы он работал на Windows)
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".exe")

# 2. Создаем копию файла с расширением .out сразу после билда
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}> 
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.out"
    COMMENT "Generating additional .out file..."
)
#target_compile_definitions(${PROJECT_NAME} PRIVATE SEQ_OUT)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
)
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
endif()

# ─── Тесты ──────────────────────────────────────────────────
option(BUILD_TESTS "Собрать юнит-тесты" ON)

if(BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # Не устанавливать GTest глобально
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(WebAgentTests
        tests/tests.cpp
        json_functions.cpp
        HTTP_client.cpp
        consoletab.cpp
        logger.cpp
    )

    target_include_directories(WebAgentTests PRIVATE ${CMAKE_SOURCE_DIR})

    if(MSVC)
        target_compile_definitions(WebAgentTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    target_link_libraries(WebAgentTests PRIVATE
        GTest::gtest
        GTest::gtest_main
        nlohmann_json::nlohmann_json
        cpr::cpr
    )

    # Регистрация в CTest
    include(GoogleTest)
    gtest_discover_tests(WebAgentTests)
endif()
