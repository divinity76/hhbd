#ifndef CLIENT_MISC_EXTRA_H_
#define CLIENT_MISC_EXTRA_H_

char *get_process_name_by_pid(const int pid, char** failreason);
uint64_t htonll(uint64_t hostlong);
uint64_t ntohll(uint64_t hostlong);

uint64_t htolell(uint64_t hostlong);
uint64_t htobell(uint64_t hostlong);

#if !defined(NTOHL)
#if !defined(__BYTE_ORDER)
#error Failed to detect byte order! fix the code yourself and/or submit a bugreport!
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
/* The host byte order is the same as network byte order,
 so these functions are all just identity.  */
#define NTOHL(x)	((uint32_t)x)
#define NTOHS(x)	((uint16_t)x)
#define HTONL(x)	((uint32_t)x)
#define HTONS(x)	((uint16_t)x)
#define HTONLL(x)	((uint64_t)x)
#define NTOHLL(x)	((uint64_t)x)
#else
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define NTOHL(x)	__bswap_constant_32 ((uint32_t)x)
#define NTOHS(x)	__bswap_constant_16 ((uint16_t)x)
#define HTONL(x)	__bswap_constant_32 ((uint32_t)x)
#define HTONS(x)	__bswap_constant_16 ((uint16_t)x)
#define HTONLL(x)	__bswap_constant_64 ((uint64_t)x)
#define NTOHLL(x)	__bswap_constant_64 ((uint64_t)x)
#else
#error Failed to detect byte order! fix the code yourself and/or submit a bugreport!
#endif
# endif
#endif

#endif /* CLIENT_MISC_EXTRA_H_ */
