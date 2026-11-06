# 看门狗自检。
#
# 让 uife_tests 在自检模式下故意挂住 60 秒，预算压到 1 秒。它必须：
#   1. 在 1 秒多就被掐掉，退出码 4（不是 ctest 的 1500 秒超时）；
#   2. 输出里点名当前用例，否则"哪一例挂死"仍然要靠猜。
#
# 这条用例守的是"发现挂死的能力"本身：真挂死没法按需制造，而看门狗一旦失效，
# 下次 CI 上又是一条 25 分钟寂静的 ***Timeout。

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env UIFE_TEST_WATCHDOG_SELFTEST=1 "${UIFE_TESTS}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE selftest_output
    ERROR_VARIABLE selftest_error)

if(NOT result EQUAL 4)
    message(FATAL_ERROR
        "看门狗没有按预期掐掉挂住的用例（退出码 ${result}，期望 4）\n"
        "--- stdout ---\n${selftest_output}\n"
        "--- stderr ---\n${selftest_error}")
endif()

if(NOT selftest_error MATCHES "watchdog_selftest")
    message(FATAL_ERROR
        "看门狗退出了，但没点名出问题的用例\n"
        "--- stderr ---\n${selftest_error}")
endif()

message(STATUS "看门狗自检通过：${selftest_error}")
