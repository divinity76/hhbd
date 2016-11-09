#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <byteswap.h>
#include "misc.h"





uint64_t htonll(uint64_t hostlong) {
#if !defined(__BYTE_ORDER__) || !defined(__ORDER_BIG_ENDIAN__)
#error unable to detect byte order! you will need to manually fix the code (and/or submit a bugreport?)
#endif
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return hostlong;
#else
	return bswap_64(hostlong);
#endif
}
uint64_t ntohll(uint64_t netlong) {
//return htonll(netlong);...
#if !defined(__BYTE_ORDER__) || !defined(__ORDER_BIG_ENDIAN__)
#error unable to detect byte order! you will need to manually fix the code (and/or submit a bugreport?)
#endif
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return netlong;
#else
	return bswap_64(netlong);
#endif
}

#define emsprintf(target,...){ \
target=realloc(target,(size_t)snprintf(NULL,0,__VA_ARGS__)+1); \
sprintf(target,__VA_ARGS__); \
}
#define emsprintferr(target,format,...){ \
	emsprintf(target,format ". errno: %i. strerror: %s",__VA_ARGS__,errno,strerror(errno)); \
}

//free me. always.
char *get_process_name_by_pid(const int pid, char** unused) {
	*unused = ecalloc(1, 1);
	char* name = ecalloc(1024, sizeof(char));
	if (name) {
		sprintf(name, "/proc/%d/cmdline", pid);
		FILE* f = fopen(name, "r");
		if (f) {
			size_t size;
			size = fread(name, sizeof(char), 1024, f);
			if (size > 0) {
				if ('\n' == name[size - 1])
					name[size - 1] = '\0';
			}
			fclose(f);
		}
	}
	return name;
}


////free me. always.
//char *get_process_name_by_pid(const int pid, char** warnings_and_errors) {
//	char *failreason1 = NULL;
//	do {
//		printf("THE PID IS %i\n", pid);
//		char name[snprintf(NULL, 0, "/proc/%i/cmdline", pid) + 1];
//		sprintf(name, "/proc/%i/cmdline", pid);
//		FILE *fd = fopen(name, "r");
//		printf("%i\n", __LINE__);
//		if (!fd) {
//			if (warnings_and_errors) {
//				emsprintferr(failreason1, "failed to open %s", name);
//			}
//			break;
//		}
//		//you can not use SEEK_END, nor can you use fstat() to get the file size of stuff in /proc
//		// they just return 0. (no, not even -1 error, just 0)
//		size_t read = 0;
//		char* buf = NULL;
//		while (true) {
//			buf = erealloc(buf, read + 1);		//<<could probably be optimized
//			size_t newread = fread(buf, 1, 1, fd);
//			if (newread != 1) {
//				break;
//			}
//			++read;
//		}
//		if (read <= 0) {
//			if (warnings_and_errors) {
//				emsprintferr(failreason1, "failed to read any data from %s",
//						name)
//			}
//			free(buf);
//			fclose(fd);
//			break;
//		}
//		fclose(fd);
//		buf[read - 1] = '\0';
//		printf("RETURNING BUF %s\n", buf);
//		return buf;
//	} while (false);
//	printf("%i\n", __LINE__);
//	//failed to get name from /cmdline (which is sometimes empty regardless, idk why. for instance, btrfs-qgroup-re has empty cmdline)..
//	// try /stat (even btrfs-qgroup-re has its name in /stat )
//	char *failreason2 = NULL;
//	do {
//		char name[snprintf(NULL, 0, "/proc/%i/stat", pid) + 1];
//		sprintf(name, "/proc/%i/stat", pid);
//		FILE *fd = fopen(name, "r");
//		if (!fd) {
//			if (warnings_and_errors) {
//				emsprintferr(failreason2, "failed to open %s", name);
//			}
//			break;
//		}
//		//you can not use SEEK_END, nor can you use fstat() to get the file size of stuff in /proc
//		// they just return 0. (no, not even -1 error, just 0)
//		size_t read = 0;
//		char* buf = NULL;
//		while (true) {
//			buf = erealloc(buf, read + 1);		//<<could probably be optimized
//			size_t newread = fread(buf, 1, 1, fd);
//			if (newread != 1) {
//				break;
//			}
//			++read;
//		}
//		if (read <= 0) {
//			if (warnings_and_errors) {
//				emsprintferr(failreason2, "failed to read any data from %s",
//						name)
//			}
//			free(buf);
//			fclose(fd);
//			break;
//		}
//		fclose(fd);
//		if (warnings_and_errors) {
//			*warnings_and_errors = failreason1;
//		} else {
//			free(failreason1);
//		}
//		return buf;
//	} while (false);
//	//i give up.
//	if (warnings_and_errors) {
//		char* finalerr = NULL;
//		emsprintf(finalerr,
//				"failed to find name of pid for 2 reasons. reason 1: %s. reason 2: %s\n",
//				failreason1, failreason2);
//		*warnings_and_errors = finalerr;
//	}
//	free(failreason1);
//	free(failreason2);
//	return calloc(1, 1);
//}
