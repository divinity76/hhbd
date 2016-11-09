#ifndef CLIENT_MISC_EXTRA_H_
#define CLIENT_MISC_EXTRA_H_

char *get_process_name_by_pid(const int pid, char** failreason);
uint64_t htonll(uint64_t hostlong);
uint64_t ntohll(uint64_t hostlong);

#endif /* CLIENT_MISC_EXTRA_H_ */
