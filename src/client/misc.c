#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include "misc.h"

#if defined(__GLIBC__)
#define macrobacktrace() { \
void *array[20]; \
int traces=backtrace(array,sizeof(array)/sizeof(array[0])); \
if(traces<=0) { \
	fprintf(stderr,"failed to get a backtrace!"); \
} else { \
backtrace_symbols_fd(array,traces,STDERR_FILENO); \
} \
fflush(stderr); \
}
#endif

void *emalloc(const size_t size) {
	void *ret = malloc(size);
	if (unlikely(size && !ret)) {
		fprintf(stderr,
				"malloc failed to allocate %zu bytes. errno: %i. strerror: %s. terminating...\n",
				size, errno, strerror(errno));
#if defined(__GLIBC__)
		macrobacktrace()
#endif
		fflush(stderr);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}
	return ret;
}
void *erealloc(void *ptr, const size_t size) {
	void *ret = realloc(ptr, size);
	if (unlikely(size && !ret)) {
		fprintf(stderr,
				"realloc failed to allocate %zu bytes. errno: %i. strerror: %s. terminating...\n",
				size, errno, strerror(errno));
#if defined(__GLIBC__)
		macrobacktrace()
#endif
		fflush(stderr);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}
	return ret;
}
void *ecalloc(const size_t num, const size_t size) {
	void *ret = calloc(num, size);
	if (unlikely(num > 0 && size > 0 && !ret)) {
		fprintf(stderr,
				"calloc failed to allocate %zu*%zu bytes. errno: %i. strerror: %s. terminating...\n",
				num, size, errno, strerror(errno));
#if defined(__GLIBC__)
		macrobacktrace()
#endif
		fflush(stderr);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}
	return ret;
}

char *estrdup(const char *s) {
	char *ret = strdup(s);
	if (unlikely(!ret)) {
		fprintf(stderr,
				"strdup failed to copy string, will terminate... string: >>> %s <<< errno: %i strerror: %s.\n",
				s, errno, strerror(errno));
#if defined(__GLIBC__)
		macrobacktrace()
#endif
		fflush(stderr);
		fflush(stdout);
		exit(EXIT_FAILURE);
	}
	return ret;
}
