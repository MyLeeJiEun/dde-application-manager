set(BIN_NAME "ut-ddeam")

include(FindGTest)
find_package(GTest REQUIRED)

add_compile_definitions(DDEAM_UNIT_TESTING)

file(GLOB_RECURSE TESTS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

add_executable(${BIN_NAME} ${TESTS})

target_include_directories(${BIN_NAME} PRIVATE
    ${PROJECT_BINARY_DIR}/
    ${PROJECT_BINARY_DIR}/src/dbus
)

target_link_libraries(${BIN_NAME} PRIVATE
    GTest::gtest
    dde_am_static
)

target_compile_options(${BIN_NAME} PRIVATE
    -fno-access-control
)

include(GoogleTest)
gtest_discover_tests(${BIN_NAME} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# DBus 接口测试(deepin-dbus-testing):对刚构建出的二进制在私有总线上跑
# tests/dbus/ 的声明式用例,并与 contract.xml 基线做接口比对。
# 需要 PATH 上有 dbus-testing;框架自带隔离总线,无需 dbus-run-session。
find_program(DBUS_TESTING_EXECUTABLE dbus-testing)
if(DBUS_TESTING_EXECUTABLE)
    add_test(NAME dbus-interface
             COMMAND ${DBUS_TESTING_EXECUTABLE} run
                     ${CMAKE_SOURCE_DIR}/tests/dbus
                     --build-dir ${CMAKE_BINARY_DIR})
else()
    message(STATUS "未找到 dbus-testing,跳过 DBus 接口测试(dbus-interface)")
endif()
