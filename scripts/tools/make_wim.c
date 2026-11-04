/* make_wim: 用 wimlib 把一个目录打包成 WIM 样本。
 * 用法: make_wim <源目录> <输出.wim>
 */
#include <wimlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	WIMStruct *wim = NULL;
	wchar_t source[512];
	wchar_t output[512];
	wchar_t name[] = L"UIFE-SAMPLE";
	int ret;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <source-dir> <output.wim>\n", argv[0]);
		return 1;
	}
	if (mbstowcs(source, argv[1], 512) == (size_t)-1 ||
	    mbstowcs(output, argv[2], 512) == (size_t)-1) {
		fprintf(stderr, "path conversion failed\n");
		return 1;
	}
	ret = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &wim);
	if (ret != 0) {
		fprintf(stderr, "create_new_wim: %s\n", wimlib_get_error_string(ret));
		return 1;
	}
	ret = wimlib_add_image(wim, source, name, NULL, 0);
	if (ret != 0) {
		fprintf(stderr, "add_image: %s\n", wimlib_get_error_string(ret));
		wimlib_free(wim);
		return 1;
	}
	ret = wimlib_write(wim, output, WIMLIB_ALL_IMAGES, 0, 1);
	if (ret != 0) {
		fprintf(stderr, "write: %s\n", wimlib_get_error_string(ret));
		wimlib_free(wim);
		return 1;
	}
	wimlib_free(wim);
	printf("ok %s\n", argv[2]);
	return 0;
}
