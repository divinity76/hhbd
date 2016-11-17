/*
 ============================================================================
 Name        : hhbd
 Author      : hanshenrik
 Version     : 0.1-dev
 Copyright   : public domain ( unlicense.org )
 Description : Hans Henriks Block Device - client
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <error.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/nbd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <pwd.h>
#include <curl/curl.h>
// todo: this code probably don't work
// right on platforms that does not support unaligned memory access
// ( https://www.kernel.org/doc/Documentation/unaligned-memory-access.txt )
// fix that sometime... (this is all theoretical. i don't have such a platform to test on...)
//#include <asm/unaligned.h>

#include "misc.h"
#include "misc_extra.h"
#include "hhbd.h"
#include "ecurl.h"

struct {
	int nbd_fd;
	int localrequestsocket;
	int remoterequestsocket;
} kernelIPC = { .nbd_fd = -1, .localrequestsocket = -1, .remoterequestsocket =
		-1 };

bool is_shutting_down = false;
pthread_mutex_t alter_num_workerthreads_mutex;
static size_t num_workerthreads = 0;
enum blocksizeEnum {
	//4096 is the max block size linux support at the moment, unfortunately
	blocksize = (uint64_t) 4096
};
// 255 was chosen at random(ish)...
struct {
	bool hasinfo;
	size_t totalsize;
	char infourl[255];
	char requestWriteSocketUrl[255];
	char readurl[255];
} serverinfo = { 0 };
void getServerInfo(const char* infourl) {
	static bool isRequestingInfo = false;
	if (unlikely(isRequestingInfo || serverinfo.hasinfo)) {
		myerror(EXIT_FAILURE, errno,
				"changing server at runtime is not supported at this time."
						"code was not written with that in mind, it is probably dangerous.\n");
	}
	isRequestingInfo = true;
	{
		size_t len = strlen(infourl);
		if (unlikely(len < 1 || len - 1 > sizeof(serverinfo.infourl))) {
			myerror(EXIT_FAILURE, EINVAL,
					"length of infourl MUST be between 1 and %zu, but is %zu\n",
					sizeof(serverinfo.infourl) - 1, len);
		}
	}
	strcpy(serverinfo.infourl, infourl);

	CURLcode ret;
	CURL *curlh = ecurl_easy_init();
	ecurl_easy_setopt(curlh, CURLOPT_URL, serverinfo.infourl);
	ecurl_easy_setopt(curlh, CURLOPT_NOPROGRESS, 1L);
	//ecurl_easy_setopt(curlh, CURLOPT_HEADER, 0L);
	ecurl_easy_setopt(curlh, CURLOPT_USERAGENT, "curl/7.50.1");
	ecurl_easy_setopt(curlh, CURLOPT_MAXREDIRS, 50L);
	//optimize note: tls might make a huge overhead for the workers...
	ecurl_easy_setopt(curlh, CURLOPT_HTTP_VERSION,
			(long )CURL_HTTP_VERSION_2TLS);
	//ecurl_easy_setopt(curlh, CURLOPT_SSH_KNOWNHOSTS, "/root/.ssh/known_hosts");
	{
		//unsafe options
		ecurl_easy_setopt(curlh, CURLOPT_SSL_VERIFYHOST, 0L);
		ecurl_easy_setopt(curlh, CURLOPT_SSL_VERIFYPEER, 0L);
		ecurl_easy_setopt(curlh, CURLOPT_SSL_VERIFYSTATUS, 0L);
	}
	//ecurl_easy_setopt(curlh, CURLOPT_FILETIME, 1L);
	//ecurl_easy_setopt(curlh, CURLOPT_TCP_KEEPALIVE, 1L);
	FILE *result = etmpfile();
	ecurl_easy_setopt(curlh, CURLOPT_WRITEDATA, result);
	ret = curl_easy_perform(curlh);
	{
		rewind(result);
		printf("GOT INFO!\n");
		int ret = fscanf(result,
						"hhbd OK\nreadurl:%254s\nrequestWriteSocketUrl:%254s\ntotalSize:%zu\n",
						serverinfo.readurl, serverinfo.requestWriteSocketUrl,
						&serverinfo.totalsize
		);
		printf(
				"readurl: %s\nrequestWriteSocketUrl: %s\ntotalsize: %zu parsed correctly (should be 3): %i\n",
				serverinfo.readurl, serverinfo.requestWriteSocketUrl,
				serverinfo.totalsize, ret);
		if (unlikely(ret != 3)) {
			printf("WARNING: COULD NOT READ ALL 3!\n");
		}
		fflush(stdout);
	}
	{
		curl_easy_cleanup(curlh);
		fclose(result);
	}
	serverinfo.hasinfo = true;
}

pthread_mutex_t single_exit_global_cleanup_mutex;
void exit_global_cleanup(void) {
	{
		int err = pthread_mutex_trylock(&single_exit_global_cleanup_mutex);
		if (unlikely(err != 0)) {
			myerror(0, err,
					"Warning: more than 1 thread tried to run exit_global_cleanup! thread id %zu prevented..\n",
					pthread_self());
			return;
		}
	}

	printf("shutting down, cleaning up.. thread doing the cleanup: %zu \n",
			pthread_self());
	is_shutting_down = true;

//	if (serverinfo.infourl) {
//		free(serverinfo.infourl);
//	}
//	if (serverinfo.readurl) {
//		free(serverinfo.readurl);
//	}
//	if (serverinfo.requestWriteSocketUrl) {
//		free(serverinfo.requestWriteSocketUrl);
//	}

	if (kernelIPC.nbd_fd != -1) {
		int err;
		/*
		 * NBD_DISCONNECT:
		 * NBD_CLEAR_SOCK:
		 * */
		err = ioctl(kernelIPC.nbd_fd, NBD_CLEAR_SOCK);
		if (err == -1) {
			myerror(0, errno, "Warning: NBD_CLEAR_SOCK failed!\n");
		}
//		err = ioctl(kernelIPC.nbd_fd, NBD_DISCONNECT);
//		if (err == -1) {
//			myerror(0, errno, "Warning: NBD_DISCONNECT failed!\n");
//		}
		if (-1 == close(kernelIPC.nbd_fd)) {
			myerror(0, errno, "Warning: failed to close the nbd handle!\n");
		}
	}
	if (kernelIPC.remoterequestsocket != -1) {
		if (-1 == close(kernelIPC.remoterequestsocket)) {
			myerror(0, errno,
					"Warning: failed to close the kernelIPC.global_remoterequestsocket!\n");
		}
	}
	if (kernelIPC.localrequestsocket != -1) {
		if (-1 == close(kernelIPC.localrequestsocket)) {
			myerror(0, errno,
					"Warning: failed to close the kernelIPC.global_localrequestsocket!\n");
		}
	}
	curl_global_cleanup();
}
pthread_mutex_t recursive_shutdown_signal_protector_mutex;

void shutdown_signal_handler(int sig, siginfo_t *siginfo, void *context) {
	(void) context;
	{
		int err = pthread_mutex_trylock(
				&recursive_shutdown_signal_protector_mutex);
		if (unlikely(err != 0)) {
			//when handling a shutdown signal triggers another shutdown signal̃
			myerror(EXIT_FAILURE, errno,
					"tried to run sutdown_signal_handler twice! probably a problem. prevented. signal: %i. strsignal: %s\n",
					sig, strsignal(sig));
		}
	}
	errno = 0;
	char *issuer_username_buf;
	struct passwd* info = getpwuid(siginfo->si_uid);
	if (info && info->pw_name) {
		issuer_username_buf = info->pw_name;
	} else {
		issuer_username_buf = emalloc(
				((size_t) snprintf(NULL, 0,
						"(failed to get username, errno: %i , strerror: %s)",
						errno, strerror(errno))) + 1);
		sprintf(issuer_username_buf,
				"(failed to get username, errno: %i , strerror: %s)",
				errno, strerror(errno));
	}
	char issuer_username[strlen(issuer_username_buf) + 1];
	strcpy(issuer_username, issuer_username_buf);
	if (!info) {
		free(issuer_username_buf);
	}
	///printf("SHUTDOWN SIGNAL!\n");
	char* warnings_and_errors;
	char *issuer_exe_buf = get_process_name_by_pid((int) siginfo->si_pid,
			&warnings_and_errors);
	size_t issuer_exe_len = strlen(issuer_exe_buf);
	if (issuer_exe_len < 1) {
		free(issuer_exe_buf);
		issuer_exe_buf = warnings_and_errors;
		issuer_exe_len = strlen(issuer_exe_buf);
	} else {
		free(warnings_and_errors);
	}
	char issuer_exe[issuer_exe_len + 1];
	strcpy(issuer_exe, issuer_exe_buf);
	free(issuer_exe_buf);
	///TODO: figure out why removing the following 3 lines makes it unable to
	// print shutdown signal info.... which is really weird..
	fprintf(stderr, "%s", "");
	fflush(stdout);
	fflush(stderr);
	myerror(EXIT_FAILURE, errno,
			"received shutdown signal %i (%s) from PID %i / UID %i (%s). issuing executable file: %s\n",
			sig, strsignal(sig), (int) siginfo->si_pid, (int) siginfo->si_uid,
			issuer_username, issuer_exe);
	UNREACHABLE();
}

pthread_mutex_t nbd_do_it_thread_mutex;
void* nbd_doit_thread(void *arg) {
	{
		int err;
		err = pthread_mutex_trylock(&nbd_do_it_thread_mutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"nbd_do_it_thread failed to acquire lock! this should never happen and may imply that 2 threads tried to call"
							" nbd_doit_thread(), which only one thread should be doing... something is very wrong, terminating.\n");
		}
	}
	installShutdownSignalHandlers();
	if (unlikely(
			0 != ioctl(kernelIPC.nbd_fd, NBD_SET_SOCK, kernelIPC.remoterequestsocket))) {
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(kernelIPC.nbd_fd, NBD_SET_SOCK, kernelIPC.global_remoterequestsocket) !\n");
	}
//	_Static_assert(totalsize % blocksize == 0,
//			"the kernel is dividing totalsize by blocksize to get number of sectors. "
//					"if totalsize is not a multiple of blocksize, you are probably doing something wrong!!!");
	if (unlikely(0 != ioctl(kernelIPC.nbd_fd, NBD_SET_BLKSIZE, blocksize))) { //should check if higher is possible too...
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(kernelIPC.nbd_fd, NBD_SET_BLKSIZE, %i) !\n",
				blocksize);
	}
	if (unlikely(
			0 != ioctl(kernelIPC.nbd_fd, NBD_SET_SIZE, serverinfo.totalsize))) {
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(kernelIPC.nbd_fd, NBD_SET_SIZE:, %zu) !\n",
				serverinfo.totalsize);
	}

	//TODO: figure out what NBD_SET_SIZE_BLOCKS is for..
	{
		//by unlocking this, we tell the mainthread that we are done setting things up, which the main thread is waiting for.
		int err = pthread_mutex_unlock(arg);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"failed to unlock unlock_before_doingit_mutex!");
		}
	}
	//should block indefinitely on NBD_DO_IT
	if (unlikely(0 != ioctl(kernelIPC.nbd_fd, NBD_DO_IT))) {
		myerror(EXIT_FAILURE, errno,
				"nbd_do_it_thread failed to ioctl(kernelIPC.nbd_fd, NBD_DO_IT) !\n");
	}
	myerror(0, errno,
			"Warning: nbd_do_it_thread thread shutting down, but was supposed to be blocking...\n");
	{
		int err = pthread_mutex_unlock(&nbd_do_it_thread_mutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"nbd_do_it_thread failed to unlock the mutex! should never happen...\n");
		}
	}
	return NULL;
}
void install_shutdown_signal_handler(const int sig) {
//yes, void. i terminate if there's an error.
	struct sigaction act = { 0 };
	act.sa_sigaction = &shutdown_signal_handler;
	act.sa_flags = SA_SIGINFO;
	if (unlikely(-1==sigaction(sig, &act, NULL))) {
		myerror(EXIT_FAILURE, errno,
				"failed to install signal handler for %i (%s)\n", sig,
				strsignal(sig));
	}
}
void installShutdownSignalHandlers(void) {
#if defined(_POSIX_VERSION)
#if _POSIX_VERSION>=199009L
	//daemon mode not supported (yet?)
	install_shutdown_signal_handler(SIGHUP);
	install_shutdown_signal_handler(SIGINT);
	install_shutdown_signal_handler(SIGQUIT);
	install_shutdown_signal_handler(SIGILL); //?
	install_shutdown_signal_handler(SIGABRT);
	install_shutdown_signal_handler(SIGFPE); //?
	//SIGKILL/SIGSTOP is not catchable anyway
	install_shutdown_signal_handler(SIGSEGV);		//?
	install_shutdown_signal_handler(SIGPIPE);		//?
	install_shutdown_signal_handler(SIGALRM);
	install_shutdown_signal_handler(SIGTERM);
	//default action for SIGUSR1/SIGUSR2 is to terminate, so, until i have something better to do with them..
	install_shutdown_signal_handler(SIGUSR1);
	install_shutdown_signal_handler(SIGUSR2);
	//ignored: SIGCHLD
#if _POSIX_VERSION >=200112L
	install_shutdown_signal_handler(SIGBUS);		//?
	install_shutdown_signal_handler(SIGPOLL);		//?
	install_shutdown_signal_handler(SIGSYS);		//?
	install_shutdown_signal_handler(SIGTRAP);		//?
	//ignored: SIGURG
	install_shutdown_signal_handler(SIGVTALRM);
	install_shutdown_signal_handler(SIGXCPU);	//not sure this 1 is catchable..
	install_shutdown_signal_handler(SIGXFSZ);
#endif
#endif
#endif
	//Now there are more non-standard signals who's default action is to terminate the process
	// which we probably should look out for, but.... cba now. they shouldn't happen anyway (like some 99% of the list above)
}
//this 1 must be packed. (because i'm sending it all in a single send() for thread sync issues)
struct myreply {
	struct nbd_reply nbdreply;
	char buffer[blocksize];
}__attribute((packed));
//this 1 also needs to be packed because i'll be sending everything from myrequest.nbdrequest.from
// to the location of the end of myrequest.nbdrequest.len + myrequest.nbdrequest.len bytes
// in a single send.
struct myrequest {
	struct nbd_request nbdrequest;
	char buffer[blocksize];
}__attribute((packed));
void print_request_data(struct myrequest *request) {
	printf("request->magic: %i\n", ntohl(request->nbdrequest.magic));
	printf("request->type: %i\n", ntohl(request->nbdrequest.type));
	printf("request->handle: %zu\n",
			ntohll(*(uint64_t*) request->nbdrequest.handle));
	printf("request->from: %zu\n", ntohll(request->nbdrequest.from));
	printf("request->len: %ul\n", ntohl(request->nbdrequest.len));
}

ssize_t ewrite(const int fd, const void *buf, const size_t count) {
	const ssize_t ret = write(fd, buf, count);
	if (unlikely(ret != (ssize_t )count)) {
		myerror(EXIT_FAILURE, errno,
				"failed to write() all data! tried to write %zu bytes, but could only write %zd bytes!",
				count, ret);
	}
	return ret;
}

pthread_mutex_t process_request_mutex;
void *process_requests(void *unused) {
	(void) unused;
	//global variables often cannot be held in cpu registers, and thus are more difficult to optimize.
	//so, make a local copy of it.
	const int localrequestsocket = kernelIPC.localrequestsocket;
	//optimization note: add POSIX_MADV_SEQUENTIAL to request.buffer and reply?
	struct myrequest request = { 0 };
	struct myreply reply = { 0 };
	reply.nbdreply.magic = HTONL(NBD_REPLY_MAGIC);
	reply.nbdreply.error = HTONL(0);

	++num_workerthreads;
	while (1) {
		{
			if (unlikely(is_shutting_down)) {
				break;
			}
			int err = pthread_mutex_lock(&process_request_mutex);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"Failed to lock process_request_mutex!\n");
			}
			if (unlikely(is_shutting_down)) {
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
				break;
			}
		}
		ssize_t bytes_read = recv(localrequestsocket, &request.nbdrequest,
				sizeof(request.nbdrequest), MSG_WAITALL);
		if (unlikely(is_shutting_down)) {
			reply.nbdreply.error = HTONL(ESHUTDOWN);
			*(uint64_t*) reply.nbdreply.handle =
					*(uint64_t*) request.nbdrequest.handle;
			write(localrequestsocket, &reply.nbdreply, sizeof(reply.nbdreply));
			int err = pthread_mutex_unlock(&process_request_mutex);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"Failed to unlock process_request_mutex!\n");
			}
			break;
		}
		if (unlikely(bytes_read != sizeof(request.nbdrequest))) {
			myerror(0, errno,
					"got invalid request! all requests must be at minimum %zu bytes, but got a request with only %zu bytes! reply bytes follow: ",
					sizeof(request.nbdrequest), bytes_read);
			if (bytes_read <= 0) {
				fprintf(stderr,
						"(not printed because the read size was <=0)\n");
			} else {
				fwrite(&request.nbdrequest, (size_t) bytes_read, 1, stderr);
			}
			fflush(stdout);
			fflush(stderr);
			exit(EXIT_FAILURE);
		}
		if (unlikely(request.nbdrequest.magic!=HTONL(NBD_REQUEST_MAGIC))) {
			myerror(EXIT_FAILURE, errno,
					"got invalid request! the request magic contained an invalid value. must be %ul , but got %ul\n",
					HTONL(NBD_REQUEST_MAGIC), htonl(request.nbdrequest.magic));
		}
		switch (request.nbdrequest.type) {
		case HTONL(NBD_CMD_READ): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_READ REQUEST\n");
			print_request_data(&request);
#endif
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}
			request.nbdrequest.len = ntohl(request.nbdrequest.len);
			if (unlikely(request.nbdrequest.len > blocksize)) {
				myerror(EXIT_FAILURE, errno,
						"got a (read) request bigger than blocksize! blocksize: %i. requestsize: %ul.\n",
						blocksize, request.nbdrequest.len);
			}
			//TODO: read and respond
			break;
		}
		case HTONL(NBD_CMD_WRITE): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_WRITE REQUEST\n");
			print_request_data(&request);
#endif
			request.nbdrequest.len = ntohl(request.nbdrequest.len);
			if (unlikely(request.nbdrequest.len > blocksize)) {
				myerror(EXIT_FAILURE, errno,
						"got a (write) request bigger than blocksize! blocksize: %i. requestsize: %ul.\n",
						blocksize, request.nbdrequest.len);
			}
			ssize_t bytes_read = recv(localrequestsocket, request.buffer,
					request.nbdrequest.len, MSG_WAITALL);
			if (unlikely(bytes_read != request.nbdrequest.len)) {
				myerror(0, errno,
						"failed to read all the bytes of a WRITE request! the server said the request was %i bytes long, but could only read %zd bytes. read bytes follow:\n",
						request.nbdrequest.len, bytes_read);
				if (bytes_read <= 0) {
					fprintf(stderr,
							"(not printed because the read size was <=0)\n");
				} else {
					fwrite(&request.buffer, (size_t) bytes_read, 1, stderr);
				}
				fflush(stdout);
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
			//TODO: write and respond. its in request.buffer
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}

			break;
		}
		case HTONL(NBD_CMD_DISC): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_DISC REQUEST\n");
			print_request_data(&request);
#endif
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}
			//this is a disconnect request..
			//there is no reply to NBD_CMD_DISC...
			break;
		}
		case HTONL(NBD_CMD_FLUSH): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_FLUSH REQUEST\n");
			print_request_data(&request);
#endif
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}
			reply.nbdreply.error = HTONL(0);
			*(uint64_t*) reply.nbdreply.handle =
					*(uint64_t*) request.nbdrequest.handle;
			ewrite(localrequestsocket, &reply.nbdreply, sizeof(reply.nbdreply));
			break;
		}
		case HTONL(NBD_CMD_TRIM): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_TRIM REQUEST\n");
			print_request_data(&request);
#endif
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}
			//...
			reply.nbdreply.error = HTONL(0);
			*(uint64_t*) reply.nbdreply.handle =
					*(uint64_t*) request.nbdrequest.handle;
			ewrite(localrequestsocket, &reply.nbdreply, sizeof(reply.nbdreply));
			break;
		}
		default: {
			//implement NBD_CMD_WRITE_ZEROES ? its not accepted mainline, experimental, etcetc
			//implement NBD_CMD_STRUCTURED_REPLY? same as above
			//implement NBD_CMD_INFO ? same as above
			//implement NBD_CMD_CACHE ? same as above
			print_request_data(&request);
			myerror(EXIT_FAILURE, errno,
					"got a request type i did not understand!: %ul (see the source code for a list of requests i DO understand, in the switch case's)",
					request.nbdrequest.type);
			UNREACHABLE();
			break;
		}
		}
	}
	//is_shutting_down is true at this point
	--num_workerthreads;
	return NULL;
}
void init_mutexes(void) {
	int err;
	err = pthread_mutex_init(&nbd_do_it_thread_mutex, NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err,
				"failed to initialize mutex nbd_do_it_thread_mutex!\n");
	}
	err = pthread_mutex_init(&recursive_shutdown_signal_protector_mutex,
	NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err,
				"failed to initialize mutex recursive_shutdown_signal_protector_mutex!\n");
	}
	err = pthread_mutex_init(&single_exit_global_cleanup_mutex, NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err,
				"failed to initialize mutex single_exit_cleanup_mutex!\n");
	}
	err = pthread_mutex_init(&process_request_mutex, NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err,
				"failed to initialize mutex process_request_mutex!\n");
	}
	err = pthread_mutex_init(&alter_num_workerthreads_mutex, NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err,
				"failed to initialize mutex alter_num_workerthreads_mutex!\n");
	}
}
int main(int argc, char *argv[]) {
	{
		CURLcode err = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err, "curl_global_init failed!");
		}
	}
	init_mutexes();
	atexit(exit_global_cleanup);
	installShutdownSignalHandlers();
	if (argc - 1 != 5) {
		myerror(EXIT_FAILURE, EINVAL,
				"wrong number of input arguments.\n need 5, got %i\n usage: %s /dev/nbdX number_of_threads http://example.org/foo/serverinfo.php MyGlobalHostNameOrIP.com portnum\n",
				argc - 1, argv[0]);
	}
	myerror(1, 0, "hi");
	kernelIPC.nbd_fd = open(argv[1], O_RDWR);
	if (unlikely(kernelIPC.nbd_fd == -1)) {
		myerror(EXIT_FAILURE, errno,
				"failed to open %s in O_RDWR!! (maybe nbd module is not loaded? \"modprobe nbd\")\n",
				argv[1]);
	}
	int workerthreads_num;
	{
		int scanres = sscanf(argv[2], "%i", &workerthreads_num);
		if (unlikely(scanres == EOF || scanres < 1)) {
			myerror(EXIT_FAILURE, EINVAL,
					"failed to parse argument as an integer!: %s\n", argv[2]);
		}
		if (unlikely(workerthreads_num < 1)) {
			myerror(EXIT_FAILURE, EINVAL,
					"number of worker threads MUST BE >=1  and <= max number of threads a single mutex lock can hold (and on my linux system, per glibc source, it is %ul (UINT_MAX), but i do not have the resources required to test it)\n",
					UINT_MAX);
		}
		printf("workerthreads: %i\n", workerthreads_num);
	}
	getServerInfo(argv[3]);
	myerror(EXIT_FAILURE, errno, "info...");
	{
		int socks[2];
		int err = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
		if (unlikely(err == -1)) {
			myerror(EXIT_FAILURE, errno,
					"Failed to create IPC unix socket pair!! \n");
		}
		kernelIPC.localrequestsocket = socks[0];
		kernelIPC.remoterequestsocket = socks[1];
	}
	{
		//optimizeme? doitthread requires a very small stack size, could probably save a few megabytes of ram here.
		pthread_t doitthread;
		pthread_mutex_t unlock_before_doingit_mutex;
		int err;
		err = pthread_mutex_init(&unlock_before_doingit_mutex, NULL);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"failed to initialize mutex unlock_before_doingit_mutex!\n");
		}
		err = pthread_mutex_trylock(&unlock_before_doingit_mutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"failed to lock unlock_before_doingit_mutex before starting doit thread!\n");
		}
		err = pthread_create(&doitthread, NULL, nbd_doit_thread,
				&unlock_before_doingit_mutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"pthread_create failed to create the NBD_DO_IT thread!\n");
		}
		//we wait for doitthread to unlock it..
		err = pthread_mutex_lock(&unlock_before_doingit_mutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err,
					"failed to wait for doitthread to unlock unlock_before_doingit_mutex!\n");
		}
	}
	{
		//now to start the worker threads.
		pthread_t latestworkerthread;
		pthread_attr_t worker_attributes;
		{
			int err = pthread_attr_init(&worker_attributes);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed pthread_attr_init(&worker_attributes); ");
			}
			//2 meg should be plenty. a safeguard against small default stack sizes, and a memory saving feature of big stack sizes..
			// (default on my system is 8 meg)
			err = pthread_attr_setstacksize(&worker_attributes,
					2 * 1024 * 1024);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err, "failed to set worker stack size! ");
			}
		}

		printf("starting worker threads... ");
		fflush(stdout);
		for (int i = 0; i < workerthreads_num; ++i) {
			int err = pthread_create(&latestworkerthread, &worker_attributes,
					process_requests,
					NULL);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed to create worker threads! # of threads created before failure: %i",
						i);
			}
		}
		{
			int err = pthread_attr_destroy(&worker_attributes);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed pthread_attr_destroy(&worker_attributes); ");
			}
		}
		printf("done.\n");
		fflush(stdout);
	}

	printf(
			"main thread has finished. will sleep until a signal is received. (by issuing pause();...)\n");
	fflush(stdout);
	pause();
	return EXIT_SUCCESS;
}
