if(NOT DEFINED UIFE_EXECUTABLE OR NOT EXISTS "${UIFE_EXECUTABLE}")
    message(FATAL_ERROR "UIFE_EXECUTABLE must point to the built UIFE executable")
endif()

function(expect_argument_error expected_text)
    execute_process(
        COMMAND "${UIFE_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )
    set(combined_output "${output}${error_output}")
    if(result EQUAL 0)
        message(FATAL_ERROR
            "Invalid CLI arguments unexpectedly succeeded: ${ARGN}\n${combined_output}")
    endif()
    if(NOT combined_output MATCHES "${expected_text}")
        message(FATAL_ERROR
            "Expected CLI argument error '${expected_text}' for: ${ARGN}\n${combined_output}")
    endif()
    if(combined_output MATCHES "镜像格式:")
        message(FATAL_ERROR
            "CLI opened the image before rejecting invalid arguments: ${ARGN}")
    endif()
endfunction()

expect_argument_error("多余的位置参数" info missing-image-that-must-not-be-opened.img extra)
expect_argument_error("输出目录选项只能指定一次"
    restore missing-image-that-must-not-be-opened.img -o output -o other)
expect_argument_error("输出目录选项只能指定一次"
    extract missing-image-that-must-not-be-opened.img /file -o)
expect_argument_error("未知选项"
    list missing-image-that-must-not-be-opened.img / --unknown)
expect_argument_error("--password-stdin 只能指定一次"
    list missing-image-that-must-not-be-opened.img / --password-stdin --password-stdin)
expect_argument_error("--password-stdin 仅适用于 list、extract 和 restore"
    info missing-image-that-must-not-be-opened.img --password-stdin)
# 资源预算的取值与适用范围都要在打开镜像之前拒绝，否则会先跑完一遍遍历再报错。
expect_argument_error("--max-entries 必须是大于 0 的整数"
    list missing-image-that-must-not-be-opened.img / --max-entries 0)
expect_argument_error("--max-entries 必须是大于 0 的整数"
    extract missing-image-that-must-not-be-opened.img / -o out --max-entries abc)
expect_argument_error("--time-limit 必须是大于 0 的整数秒"
    list missing-image-that-must-not-be-opened.img / --time-limit -1)
expect_argument_error("--max-entries 只适用于 list/extract/restore"
    probe missing-image-that-must-not-be-opened.img --max-entries 10)
expect_argument_error("--time-limit 只适用于 list/extract/restore"
    info missing-image-that-must-not-be-opened.img --time-limit 10)
expect_argument_error("--log-format 只能是 text 或 json"
    list missing-image-that-must-not-be-opened.img / --log-format xml)
expect_argument_error("--log-format 需要 text 或 json"
    list missing-image-that-must-not-be-opened.img / --log-format)
expect_argument_error("--log-file 需要非空路径"
    list missing-image-that-must-not-be-opened.img / --log-file)

# ---------------------------------------------------------------------------
# CLI 产品化回归：退出码、JSON 错误对象、batch 清单
# ---------------------------------------------------------------------------
function(expect_exit_code expected_code)
    execute_process(
        COMMAND "${UIFE_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )
    if(NOT result EQUAL expected_code)
        message(FATAL_ERROR
            "Expected exit code ${expected_code} but got ${result} for: ${ARGN}\n${output}${error_output}")
    endif()
endfunction()

expect_exit_code(2 bogus some-image.img)
expect_exit_code(2 info)
expect_exit_code(0 --help)
expect_exit_code(0)

# probe / bench 的参数校验必须在打开镜像前完成
expect_argument_error("此命令不接受 --size 选项"
    list missing-image-that-must-not-be-opened.img / --size 1024)
expect_argument_error("--size 必须是大于 0 的整数字节数"
    bench missing-image-that-must-not-be-opened.img --size 0)
expect_argument_error("--size 只能指定一次"
    bench missing-image-that-must-not-be-opened.img --size 1 --size 2)
expect_exit_code(2 probe)

# JSON 模式下错误必须输出 JSON 对象且带退出码
execute_process(
    COMMAND "${UIFE_EXECUTABLE}" --json info missing-image-for-json-error-test.img
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(result EQUAL 0)
    message(FATAL_ERROR "JSON error test unexpectedly succeeded")
endif()
if(NOT output MATCHES "\\{\"error\":\\{\"code\":[0-9]+,\"message\":")
    message(FATAL_ERROR "JSON mode did not emit an error object: ${output}${error_output}")
endif()

# batch：成功行、失败行、注释与空行；JSONL 输出与汇总退出码
set(batch_manifest "${CMAKE_CURRENT_BINARY_DIR}/cli_batch_test.txt")
file(WRITE "${batch_manifest}"
    "# comment line\n"
    "\n"
    "--help\n"
    "info missing-image-for-batch-error-test.img\n")
expect_exit_code(1 --json batch "${batch_manifest}")
execute_process(
    COMMAND "${UIFE_EXECUTABLE}" --json batch "${batch_manifest}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(NOT output MATCHES "\\{\"line\":3,\"command\":\"--help\",\"status\":\"ok\"")
    message(FATAL_ERROR "batch JSONL missing ok line: ${output}${error_output}")
endif()
if(NOT output MATCHES "\\{\"line\":4,\"command\":\"info missing-image-for-batch-error-test.img\",\"status\":\"error\",\"code\":1,")
    message(FATAL_ERROR "batch JSONL missing error line: ${output}${error_output}")
endif()
execute_process(
    COMMAND "${UIFE_EXECUTABLE}" batch "${batch_manifest}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 1)
    message(FATAL_ERROR "batch human mode should exit 1 on failures: ${output}${error_output}")
endif()
if(NOT output MATCHES "批处理完成: 成功 1 行, 失败 1 行")
    message(FATAL_ERROR "batch human mode summary missing: ${output}${error_output}")
endif()
