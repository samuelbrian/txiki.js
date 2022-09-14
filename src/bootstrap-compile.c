#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "../deps/quickjs/include/quickjs.h"

void usage() {
	fprintf(stderr, "usage: bootstrap-compile INPUT_FILENAME OUTPUT_FILENAME\n");
	exit(1);
}

char *file_map(const char *filename, size_t *len)
{
	int fd = -1;
	char *data = MAP_FAILED;
	struct stat sb;

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "Failed to open file '%s': %m\n", filename);
		goto error;
	}

	if (fstat(fd, &sb) == -1) {
		fprintf(stderr, "Failed to stat file '%s': %m\n", filename);
		goto error;
	}

	if (sb.st_size == 0) {
		fprintf(stderr, "Cannot map empty file '%s'\n", filename);
		goto error;
	}

	data = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "Failed to map file '%s' into memory: %m\n", filename);
		goto error;
	}

	*len = sb.st_size;
	return data;

error:
	if (fd != -1)
		close(fd);
	return NULL;
}

void file_unmap(char *data, size_t len)
{
	if (data != NULL)
		munmap(data, len);
}

int main(int argc, char **argv)
{
	const char *input_filename;
	const char *output_filename;
	const char *output_bswap_filename = NULL;
	const char *module_name;
	JSRuntime *rt = NULL;
	JSContext *ctx = NULL;
	JSValue val;
	char *buf = NULL;
	size_t buf_len;
	char *data = NULL;
	size_t data_len;
	int flags;
	FILE *f = NULL;
	bool ret = 255;

	if (argc < 3)
		usage();

	input_filename = argv[1];
	output_filename = argv[2];
	if (argc > 3)
		output_bswap_filename = argv[3];
	if (argc > 4)
		module_name = argv[4];
	else
		module_name = input_filename;

	buf = file_map(input_filename, &buf_len);
	if (buf == NULL) {
		fprintf(stderr, "Failed to open input file: %s\n", input_filename);
		goto done;
	}

	rt = JS_NewRuntime();
	if (rt == NULL) {
		fprintf(stderr, "Failed to create the JavaScript runtime.\n");
		goto done;
	}

	ctx = JS_NewContext(rt);
	if (ctx == NULL) {
		fprintf(stderr, "Failed to create the JavaScript context.\n");
		goto done;
	}

	flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY; // | JS_EVAL_FLAG_STRIP;
	val = JS_Eval(ctx, buf, buf_len, module_name, flags);

	if (JS_IsException(val)) {
		fprintf(stderr, "Exception occured parsing script.\n");
		//js_std_dump_error(ctx);
		goto done;
	}

	/* Write bytecode with build machine endianess */
	flags = JS_WRITE_OBJ_BYTECODE;
	data = (char *)JS_WriteObject(ctx, &data_len, val, flags);
	if (data == NULL) {
		fprintf(stderr, "Failed to build bytecode.\n");
		//tjs_dump_error(ctx);
		goto done;
	}

	f = fopen(output_filename, "wb");
	if (f == NULL) {
		fprintf(stderr, "Failed to open output file: %m\n");
		goto done;
	}

	if (fwrite(data, sizeof(*data), data_len, f) != data_len) {
		fprintf(stderr, "Failed to write bytecode to file: %m\n");
		goto done;
	}

	fclose(f);
	f = NULL;

	if (output_bswap_filename != NULL) {
		/* Write bytecode with swapped endianess */
		flags = JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_BSWAP;
		data = (char *)JS_WriteObject(ctx, &data_len, val, flags);
		if (data == NULL) {
			fprintf(stderr, "Failed to build bswap bytecode.\n");
			//tjs_dump_error(ctx);
			goto done;
		}

		f = fopen(output_bswap_filename, "wb");
		if (f == NULL) {
			fprintf(stderr, "Failed to open bswap output file: %m\n");
			goto done;
		}

		if (fwrite(data, sizeof(*data), data_len, f) != data_len) {
			fprintf(stderr, "Failed to write bswap bytecode to file: %m\n");
			goto done;
		}
	}

	ret = 0;

done:
	file_unmap(buf, buf_len);

	if (f != NULL)
		fclose(f);

	if (data != NULL)
		js_free(ctx, data);

	if (ctx != NULL)
		JS_FreeContext(ctx);

	if (rt != NULL)
		JS_FreeRuntime(rt);

	return ret;
}
