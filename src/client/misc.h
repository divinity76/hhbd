#ifndef COMMON_H_
#define COMMON_H_
#include <byteswap.h>
void *emalloc(const size_t size);
void *erealloc(void *ptr, const size_t size);
void *ecalloc(const size_t num, const size_t size);
char *estrdup(const char *s);

#define myerror(status,errnum,...)(error_at_line(status,errnum,__FILE__,__LINE__,__VA_ARGS__))
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

