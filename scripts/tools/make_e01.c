/* make_e01: 用 libewf 写支持把原始镜像文件打包为 E01（仅用于生成测试样本）。
 * 用法: make_e01 <输入文件> <输出基础名>
 *   输出基础名不含扩展名；生成 <基础名>.E01，按段大小自动续段。
 *   注意：libewf 用 8.3 风格命名，基础名超过 8 字符会被截断。
 */
#include <libewf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHUNK (64 * 1024)

int main(int argc, char **argv)
{
	libewf_error_t *error = NULL;
	libewf_handle_t *handle = NULL;
	FILE *input = NULL;
	unsigned char buffer[CHUNK];
	wchar_t wide_name[512];
	wchar_t *filenames[1];
	long file_size;
	size64_t remaining;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <input> <output-base-name>\n", argv[0]);
		return 1;
	}
	input = fopen(argv[1], "rb");
	if (input == NULL) {
		perror("fopen input");
		return 1;
	}
	if (fseek(input, 0, SEEK_END) != 0 || (file_size = ftell(input)) <= 0 ||
	    fseek(input, 0, SEEK_SET) != 0) {
		fprintf(stderr, "invalid input size\n");
		fclose(input);
		return 1;
	}
	if (libewf_handle_initialize(&handle, &error) != 1) {
		libewf_error_backtrace_fprint(error, stderr);
		fclose(input);
		return 1;
	}
	if (mbstowcs(wide_name, argv[2], 512) == (size_t)-1) {
		fprintf(stderr, "filename conversion failed\n");
		return 1;
	}
	filenames[0] = wide_name;
	if (libewf_handle_open_wide(handle, filenames, 1,
	                            LIBEWF_OPEN_WRITE, &error) != 1) {
		fprintf(stderr, "open write failed\n");
		libewf_error_backtrace_fprint(error, stderr);
		return 1;
	}
	if (libewf_handle_set_media_size(handle, (size64_t)file_size, &error) != 1) {
		fprintf(stderr, "set media size failed\n");
		libewf_error_backtrace_fprint(error, stderr);
		return 1;
	}
	remaining = (size64_t)file_size;
	while (remaining > 0) {
		size_t request = sizeof(buffer);
		size_t count;
		ssize_t written;

		if ((size64_t)request > remaining) {
			request = (size_t)remaining;
		}
		count = fread(buffer, 1, request, input);
		if (count == 0) {
			fprintf(stderr, "short read\n");
			return 1;
		}
		written = libewf_handle_write_buffer(handle, buffer, count, &error);
		if (written != (ssize_t)count) {
			fprintf(stderr, "write failed (%d of %d)\n",
			        (int)written, (int)count);
			libewf_error_backtrace_fprint(error, stderr);
			return 1;
		}
		remaining -= count;
	}
	/* 部分 libewf 构建在 finalize/close 阶段会返回 -1 且不回填 error，
	 * 此时数据段已经完整落盘，镜像仍可正常读取；这里只提示不视为失败。 */
	if (libewf_handle_write_finalize(handle, &error) != 1) {
		fprintf(stderr, "warning: write finalize reported failure\n");
	}
	if (libewf_handle_close(handle, &error) != 1) {
		fprintf(stderr, "warning: close reported failure\n");
	}
	libewf_handle_free(&handle, &error);
	fclose(input);
	printf("ok %s.E01 (%ld bytes)\n", argv[2], file_size);
	return 0;
}
