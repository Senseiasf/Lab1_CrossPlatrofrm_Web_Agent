cmake_minimum_required(VERSION 3.15)
project(WebAgent)

set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2 # Можно указать конкретную версию или master
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.x
)
FetchContent_MakeAvailable(cpr)

find_package(CURL REQUIRED)

add_executable(${PROJECT_NAME}
    main.cpp
    HTTP_client.cpp
    json_functions.cpp
    consoletab.cpp
    logger.cpp
)
#target_compile_definitions(${PROJECT_NAME} PRIVATE SEQ_OUT)
#Хотите увидеть необработанные логи от сервера - убираете сверху #. q может
#больше не работать, но видите все ответы сервера. Нужно перебилдить через make,
#должна пройти сборка с такой кучей текста (у меня иногда не с 1ого раза собирало)

target_link_libraries(${PROJECT_NAME} PRIVATE cpr::cpr CURL::libcurl)
# В конце CMakeLists.txt должно быть так:
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
