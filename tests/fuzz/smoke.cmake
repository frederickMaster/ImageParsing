# 每次提交跑的那一小批变异：先用纯 Python 生成确定性语料，再跑固定 seed 的
# 100 多例。固定 seed 是刻意的——它让"这次失败"和"上次失败"是同一批输入，
# 而不是每次换一批、看起来永远有新问题。
#
# 长跑（夜间、几十分钟）用同一套参数，只把 --iterations/--seconds 调大，
# 见 docs/FUZZING.md。
if(NOT UIFE_FUZZER)
    message(FATAL_ERROR "缺少 -DUIFE_FUZZER=<可执行文件>")
endif()
if(NOT PYTHON OR NOT GENERATOR OR NOT WORK)
    message(FATAL_ERROR "smoke.cmake 需要 -DPYTHON / -DGENERATOR / -DWORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

execute_process(
    COMMAND "${PYTHON}" "${GENERATOR}" --output "${WORK}/corpus" --no-external-tools
    RESULT_VARIABLE corpus_result
    OUTPUT_VARIABLE corpus_output
    ERROR_VARIABLE corpus_error)
if(NOT corpus_result EQUAL 0)
    message(FATAL_ERROR "生成语料失败（${corpus_result}）: ${corpus_error}")
endif()

# 单个用例 10 s：CI runner 负载高，正常镜像远低于这个数；真卡住时看门狗会
# 保存输入并以退出码 3 结束，不会把 ctest 拖到超时。
execute_process(
    COMMAND "${UIFE_FUZZER}"
        --seeds "${WORK}/corpus"
        --work "${WORK}/run"
        --iterations 120
        --seed 20261106
        --case-timeout-ms 10000
    RESULT_VARIABLE fuzz_result
    OUTPUT_VARIABLE fuzz_output
    ERROR_VARIABLE fuzz_error)
message(STATUS "${fuzz_output}")
if(NOT fuzz_result EQUAL 0)
    # stdout 也要带出来：失败原因常常就写在统计行里（哪一类输入、卡在哪个用例），
    # 只报退出码的话 CI 上还得去翻构建产物。
    message(FATAL_ERROR
        "变异用例失败（退出码 ${fuzz_result}）\n"
        "--- stdout ---\n${fuzz_output}\n"
        "--- stderr ---\n${fuzz_error}\n"
        "崩溃/超时的输入保存在 ${WORK}/run/crash，可用 --replay 复核")
endif()

# 看门狗自检：故意让用例卡 600 ms，把预算压到 200 ms。它必须被掐掉（退出码 3）
# 并把输入留档。这条用例守的是"发现死循环的能力"本身——真死循环没法按需制造，
# 而这个机制一旦失效，长跑就等于永远卡在第一个坏输入上。
execute_process(
    COMMAND "${UIFE_FUZZER}"
        --seeds "${WORK}/corpus/fs/fat16-minimal"
        --work "${WORK}/selftest"
        --iterations 1
        --seed 1
        --case-timeout-ms 200
        --selftest-hang 600
    RESULT_VARIABLE watchdog_result
    OUTPUT_VARIABLE watchdog_output
    ERROR_VARIABLE watchdog_error)
if(watchdog_result EQUAL 0)
    message(FATAL_ERROR
        "看门狗没有中止卡住的用例（本该以退出码 3 结束）: ${watchdog_output}")
endif()
if(NOT EXISTS "${WORK}/selftest/crash")
    message(FATAL_ERROR "看门狗中止了用例，却没有把输入留档")
endif()
message(STATUS "看门狗自检通过（退出码 ${watchdog_result}，输入已留档）")
