#ifndef COMMON_H_
#define COMMON_H_
#include <byteswap.h>
#include <error.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#include <unistd.h>
#endif
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
#define myerror(status,errnum,...){macrobacktrace();error_at_line(status,errnum,__FILE__,__LINE__,__VA_ARGS__);}
#else
#define myerror(status,errnum,...){error_at_line(status,errnum,__FILE__,__LINE__,__VA_ARGS__);}
#endif
void *emalloc(const size_t size);
void *erealloc(void *ptr, const size_t size);
void *ecalloc(const size_t num, const size_t size);
char *estrdup(const char *s);
FILE *etmpfile();
#if !defined(UNREACHABLE)
//TODO: check MSVC/ICC
#if defined(__GNUC__) || defined(__clang__)
#define UNREACHABLE() (__builtin_unreachable())
#else
//not sure what to do here...
#define UNREACHABLE() ()
#endif
#endif

#if !defined(likely)
#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
#define likely(x)       __builtin_expect(!!(x),1)
#define unlikely(x)     __builtin_expect(!!(x),0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif
#endif

#if !defined(MAX)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif



#endif /* COMMON_H_ */

