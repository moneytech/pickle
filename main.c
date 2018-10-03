#include "pickle.h"
#include "block.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define FILE_SZ (1024 * 16)

static int picolCommandPuts(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	assert(pd);
	if (argc != 2) 
		return pickle_arity_error(i, argv[0]);
	fprintf((FILE*)pd, "%s\n", argv[1]);
	return 0;
}

static int picolCommandGets(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	assert(pd);
	if (argc != 1) 
		return pickle_arity_error(i, argv[0]);
	char buf[1024];
	fgets(buf, sizeof buf, (FILE*)pd);
	pickle_set_result(i, buf);
	return 0;
}

static int picolCommandSystem(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	(void)pd;
	if (argc != 2)
		return pickle_arity_error(i, argv[0]);
	char v[64];
	const int r = system(argv[1]);
	snprintf(v, sizeof v, "%d", r);
	pickle_set_result(i, v);
	return 0;
}

static int picolCommandRand(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	(void)pd;
	if (argc != 1)
		return pickle_arity_error(i, argv[0]);
	char v[64];
	snprintf(v, sizeof v, "%d", rand());
	pickle_set_result(i, v);
	return 0;
}

static int picolCommandExit(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	(void)pd;
	if (argc != 2)
		return pickle_arity_error(i, argv[0]);
	exit(atoi(argv[1]));
	return 0;
}

static int picolCommandGetEnv(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	(void)pd;
	if (argc != 2)
		return pickle_arity_error(i, argv[0]);
	char *env = getenv(argv[1]);
	pickle_set_result(i, env ? env : "");
	return 0;
}

static int picolCommandStrftime(pickle_t *i, int argc, char **argv, void *pd) {
	assert(i);
	assert(argv);
	(void)pd;
	if (argc != 2)
		return pickle_arity_error(i, argv[0]);
	char buf[1024] = { 0 };
	time_t rawtime;
	time(&rawtime);
	struct tm *timeinfo = gmtime(&rawtime);
	strftime(buf, sizeof buf, argv[1], timeinfo);
	pickle_set_result(i, buf);
	return 0;
}

void *custom_calloc(void *a, size_t length) { return block_arena_calloc_block(a, length); }
void custom_free(void *a, void *v)          { block_arena_free_block(a, v); }
void *custom_realloc(void *a, void *v, size_t length) { return block_arena_realloc_block(a, v, length); }

int main(int argc, char **argv) {
	int r = 0;
	FILE *input = stdin, *output = stdout;
	int use_custom_allocator = 1;

	allocator_t allocator = {
		.free    = custom_free,
		.realloc = custom_realloc,
		.calloc  = custom_calloc,
		.arena   = use_custom_allocator ? block_arena_allocate(1024, 2*1024) : NULL,
	};

	/*if(use_custom_allocator)
		block_test();*/

	pickle_t interp = { .initialized = 0 };
	pickle_initialize(&interp, use_custom_allocator ? &allocator : NULL);

	pickle_register_command(&interp, "puts",     picolCommandPuts,     stdout);
	pickle_register_command(&interp, "gets",     picolCommandGets,     stdin);
	pickle_register_command(&interp, "system",   picolCommandSystem,   NULL);
	pickle_register_command(&interp, "exit",     picolCommandExit,     NULL);
	pickle_register_command(&interp, "getenv",   picolCommandGetEnv,   NULL);
	pickle_register_command(&interp, "rand",     picolCommandRand,     NULL);
	pickle_register_command(&interp, "strftime", picolCommandStrftime, NULL);

	if (argc == 1) {
		for(;;) {
			char clibuf[1024];
			fprintf(output, "pickle> "); 
			fflush(output);
			if (!fgets(clibuf, sizeof clibuf, input))
				goto end;
			const int retcode = pickle_eval(&interp,clibuf);
			if (interp.result[0] != '\0')
				fprintf(output, "[%d] %s\n", retcode, interp.result);
		}
	}
 	if(argc != 2) {
		fprintf(stderr, "usage: %s file\n", argv[0]);
		r = -1;
		goto end;
	}	
	FILE *fp = fopen(argv[1], "r");
	if (!fp) {
		fprintf(stderr, "failed to open file %s: %s\n", argv[1], strerror(errno));
		r = -1;
		goto end;
	}
	char buf[1024*16];
	buf[fread(buf, 1, 1024*16, fp)] = '\0';
	fclose(fp);
	if (pickle_eval(&interp, buf) != 0) 
		fprintf(output, "%s\n", interp.result);
end:
	pickle_deinitialize(&interp);
	if(use_custom_allocator)
		block_arena_free(allocator.arena);
	return r;
}

