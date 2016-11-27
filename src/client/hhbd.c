/*
 ============================================================================
 Name        : hhbd
 Author      : hanshenrik
 Version     : 0.1-dev
 Copyright   : public domain ( unlicense.org )
 Description : Hans Henriks Block Device - client
 ============================================================================
 */

// _GNU_SOURCE for sendmmsg
//#define _GNU_SOURCE
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
#include <assert.h>
#include <time.h>
#include <curl/curl.h>

#include "misc.h"
#include "misc_extra.h"
#include "hhbd.h"
#include "ecurl.h"
// todo: this code probably don't work
// right on platforms that does not support unaligned memory access
// ( https://www.kernel.org/doc/Documentation/unaligned-memory-access.txt )
// fix that sometime... (this is all theoretical. i don't have such a platform to test on...)
//#include <asm/unaligned.h>

_Static_assert(sizeof(intptr_t)<=sizeof(void*),"due to SO_LINGER thread optimizations, "
		"this code currently requires that sizeof(intptr_t) <=sizeof(void*), which "
		"is generally true, but not guaranteed by C specs... (easy to fix but would result in slower code)");

struct {
	int nbd_fd;
	int localrequestsocket;
	int remoterequestsocket;
}volatile kernelIPC = { .nbd_fd = -1, .localrequestsocket = -1,
		.remoterequestsocket = -1 };
//maybe make this runtime defined sometime.
#define CompileTimeDefinedNumberOfWriteSockets 10
#define CompileTimeDefinedMaxNumberOfBlocksPerKernelRequest 1000
struct {
	time_t closes_after_ts;
	int sock;
}volatile writeSockets[CompileTimeDefinedNumberOfWriteSockets] = { 0 };

volatile bool is_shutting_down = false;
pthread_mutex_t alter_num_workerthreads_mutex;
volatile size_t num_workerthreads = 0;
enum blocksizeEnum {
	//4096 is the max block size linux support at the moment, unfortunately
	blocksize = 4096
};
// 255 was chosen at random(ish)...
struct {
	bool hasinfo;
	size_t totalsize;
	char infourl[255];
	char requestWriteSocketUrl[255];
	char readurl[255];
	char myIP[255];
	uint16_t myport;
}volatile serverinfo = { 0 };
void getServerInfo(const char* infourl) {
	static volatile bool isRequestingInfo = false;
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
	//compiler: expected ‘char * restrict’ but argument is of type ‘volatile char *’
	//not sure what to make of that..
	strcpy((char*) serverinfo.infourl, infourl);
	CURLcode ret;
	CURL *curlh = ecurl_easy_init();
	//i wonder, is THIS what really should be in serverinfo.infourl ?
	//TODO: curl_easy_escape(serverinfo.myIP)
	char *myIP_escaped = ecurl_easy_escape(curlh, (const char*) serverinfo.myIP,
			(int) strlen((const char*) serverinfo.myIP));
	char *infoRequestUrl = emalloc(
			(size_t) snprintf(NULL, 0, "%s?ip=%s&port=%" PRIu16,
					serverinfo.infourl, myIP_escaped, serverinfo.myport) + 1);
	sprintf(infoRequestUrl, "%s?ip=%s&port=%" PRIu16, serverinfo.infourl,
			myIP_escaped, serverinfo.myport);
	curl_free(myIP_escaped);
	ecurl_easy_setopt(curlh, CURLOPT_URL, infoRequestUrl);
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
	if (unlikely(ret != CURLE_OK)) {
		myerror(EXIT_FAILURE, errno,
				"curl failed to fetch %s ! return value: %i. strerror: %s\n",
				infoRequestUrl, ret, curl_easy_strerror(ret));
	}
	{
		rewind(result);
		int ret =
				fscanf(result,
						"hhbd OK\nreadurl:%254s\nrequestWriteSocketUrl:%254s\ntotalSize:%zu\n",
						serverinfo.readurl, serverinfo.requestWriteSocketUrl,
						&serverinfo.totalsize);
		printf(
				"readurl: %s\nrequestWriteSocketUrl: %s\ntotalsize: %zu\n parsed correctly (should be 3): %i\n",
				serverinfo.readurl, serverinfo.requestWriteSocketUrl,
				serverinfo.totalsize, ret);
		if (unlikely(ret != 3)) {
			fseek(result, 0, SEEK_END);
			size_t size;
			{
				off_t off = ftello(result);
				if (unlikely(off == -1)) {
					//sigh
					myerror(0, errno,
							"unable to get size of data downloaded by curl!\n");
					size = 0;
				} else {
					size = (size_t) off;
				}
			}
			char* buf = emalloc(size + 1);
			buf[size] = '\0';
			rewind(result);
			fread(buf, size, 1, result);
			myerror(EXIT_FAILURE, errno,
					"invalid response from readurl (%s)! fscanf could only parse %i of 3 required parameters. response: %s",
					infoRequestUrl, ret, buf);
			UNREACHABLE();
			free(buf);
		}
		fflush(stdout);
	}
	{
		curl_easy_cleanup(curlh);
		fclose(result);
		free(infoRequestUrl);
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
					"Warning: failed to close the kernelIPC.remoterequestsocket!\n");
		}
	}
	if (kernelIPC.localrequestsocket != -1) {
		if (-1 == close(kernelIPC.localrequestsocket)) {
			myerror(0, errno,
					"Warning: failed to close the kernelIPC.localrequestsocket!\n");
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
	assert(
			serverinfo.totalsize % blocksize == 0
					&& "the kernel is dividing totalsize by blocksize to get number of sectors. totalsize is not a multiple of blocksize, you are probably doing something wrong!!!");
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
struct mybuffer {
	size_t buffer_size;
	char* buffer;
};
struct myreply {
	struct nbd_reply nbdreply __attribute((packed));
	struct mybuffer mybuf;
	size_t write_pos;
};

struct myrequest {
	struct nbd_request nbdrequest;
	struct mybuffer mybuf;
};
void print_request_data(struct myrequest *request) {
	printf("request->magic: %i\n", ntohl(request->nbdrequest.magic));
	printf("request->type: %i\n", ntohl(request->nbdrequest.type));
	{
		uint64_t nhandle;
		_Static_assert(sizeof(nhandle) == sizeof(request->nbdrequest.handle),
				"if this fails, the the code around needs to get updated.");
		memcpy(&nhandle, request->nbdrequest.handle, sizeof(nhandle));
		printf("request->handle: %zu\n", ntohll(nhandle));
	}
	printf("request->from: %zu\n", ntohll(request->nbdrequest.from));
	printf("request->len: %ul\n", ntohl(request->nbdrequest.len));
}

//void ewrite3(const int sockfd, const struct iovec iov) {
//	struct mmsghdr header;
//	header.msg_len = 1;
//
//	//header.msg_name=NULL;
//	header.msg_hdr.msg_namelen = 0;
//	header.msg_hdr.msg_iov = (struct iovec*) &iov;
//	header.msg_hdr.msg_iovlen = 1;
//	//header.msg_control=NULL;
//	header.msg_hdr.msg_controllen = 0;
//	// some time in the future, it wouldn't surprise me if
//	// msg_flags were no longer ignored.
//	// after which, msg_flags would need to be initialized...
//	// but ever since sendmsg was introduced, and to kernel 4.8, this is not the case..
//	// and afaik, it wont be the case any time in the near future...
//	// thus, as it currently stands, initializing it is a waste of cpu...
//	header.msg_hdr.msg_flags = 0;
//	const int sent1 = sendmmsg(sockfd, &header, 1, 0);
//	if (unlikely(sent1 != 1)) {
//		myerror(EXIT_FAILURE, errno,
//				"failed to sendmsg() all data! tried to write %zu bytes. sendmmsg() was supposed to return 1, but returned %i\n",
//				iov.iov_len, sent1);
//	}
//}

ssize_t ewrite2(const int fd, const struct iovec iov) {
	struct msghdr header;
	//header.msg_name=NULL;
	header.msg_namelen = 0;
	header.msg_iov = (struct iovec*) &iov;
	header.msg_iovlen = 1;
	//header.msg_control=NULL;
	header.msg_controllen = 0;
	// some time in the future, it wouldn't surprise me if
	// msg_flags were no longer ignored.
	// after which, msg_flags would need to be initialized...
	// but ever since sendmsg was introduced, and to kernel 4.8, this is not the case..
	// and afaik, it wont be the case any time in the near future...
	// thus, as it currently stands, initializing it is a waste of cpu...
	header.msg_flags = 0;
	const ssize_t ret = sendmsg(fd, &header, 0);
	if (unlikely(ret != iov.iov_len)) {
		myerror(EXIT_FAILURE, errno,
				"failed to sendmsg() all data! tried to write %zu bytes, but could only write %zd bytes!",
				iov.iov_len, ret);
	}
	return ret;
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
pthread_mutex_t replymutex;
void nbdreply(const int fd, const void *buf1, const size_t buf1_size,
		const void *buf2, const size_t buf2_size) {
	{
		int err;
		err = pthread_mutex_lock(&replymutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err, "reply failed to lock replymutex!\n");
		}
	}

	if (likely(buf1_size > 0)) {
		size_t written_total = 0;
		do {
			ssize_t written = write(fd, &(((const char*) buf1)[written_total]),
					buf1_size - written_total);
			if (unlikely(written < 0)) {
				myerror(EXIT_FAILURE, errno, "nbdreply write returned <0!!\n");
			}
			written_total += written;
		} while (written_total < buf1_size);
	}
	if (likely(buf2_size > 0)) {
		size_t written_total = 0;
		do {
			ssize_t written = write(fd, &(((const char*) buf2)[written_total]),
					buf2_size - written_total);
			if (unlikely(written < 0)) {
				myerror(EXIT_FAILURE, errno, "nbdreply write returned <0!!\n");
			}
			written_total += written;
		} while (written_total < buf2_size);
	}
	{
		int err;
		err = pthread_mutex_unlock(&replymutex);
		if (unlikely(err != 0)) {
			myerror(EXIT_FAILURE, err, "reply failed to unlock replymutex!\n");
		}
	}
	return;
}
pthread_mutex_t process_request_mutex;
size_t curl_myreply_writer_callback(const char *ptr, const size_t size,
		const size_t nmemb, struct myreply *userdata) {
	size_t datatowrite = size * nmemb;
	//Todo: should check actual requested size, not buffer size, really...
	if (unlikely(
			userdata->write_pos + datatowrite > userdata->mybuf.buffer_size)) {
		//...
		myerror(0, 0,
				"curl got more data than the size of reply buffer! should never happen, something is wrong.. will retry request."
						" number of bytes recieved: %zu. blocksize: %zu.\n",
				(size_t )(userdata->write_pos + datatowrite),
				(size_t )blocksize);
		fprintf(stderr, "recieved data:");
		fwrite(&userdata->mybuf.buffer[0], userdata->write_pos, 1, stderr);
		fwrite(ptr, datatowrite, 1, stderr);
		fflush(stderr);
		// 0 here means 0 bytes written, not success.
		return 0;
	}
	memcpy(&userdata->mybuf.buffer[userdata->write_pos], ptr, datatowrite);
	userdata->write_pos += datatowrite;
	return datatowrite;
}
//a resize is unlikely because it may happen a few times in the beginning,
//but once it reaches the kernel max request/response size, it will never happen again..
//lets hope the cpu prefetcher catches up on that eventually
// #define putsize(x) _Generic((x), size_t: printf("%zu\n", x), default: assert(!"test requires size_t")) \n putsize (sizeof 0);
#define REALBUF_MINSIZE(minsize){                             \
			_Static_assert(sizeof(minsize) == sizeof(request.nbdrequest.len), \
"should be a uint32_t..."); \
if (unlikely(realbuffer.buffer_size < minsize)) { \
	free(realbuffer.buffer); \
	realbuffer.buffer = emalloc(minsize); \
	realbuffer.buffer_size = minsize; \
	request.mybuf = realbuffer; \
	reply.mybuf = realbuffer; \
} \
};

void *process_requests(void *unused) {
	(void) unused;
	//volatile global variables often cannot be held in cpu registers (for long), and thus are more difficult to optimize.
	//so, make a local copy of it, since it wont change at this point anyway.
	const int localrequestsocket = kernelIPC.localrequestsocket;
	//optimization note: add POSIX_MADV_SEQUENTIAL to request.buffer and reply?
	struct myrequest request = { 0 };
	struct myreply reply = { 0 };
	struct mybuffer realbuffer = { 0 };	//<not a performance critical piece of code..
	//32 is NOT random. its the highest i've ever seen from my own local amd64 system.
	//the kernel first try blocksize*4. if i return EINVAL, it tries blocksize*1, but if success,
	//it tries blocksize*8, then blocksize*16 , then blocksize*32
	//and stops there and just issue *32 indefinitely.
	//TODO: optimization note: make the socketpair buffer at least MAX(blocksize*32,currentbuffersize);
	realbuffer.buffer = emalloc(blocksize * 32);
	realbuffer.buffer_size = blocksize * 32;
	request.mybuf = realbuffer;
	reply.mybuf = realbuffer;
	reply.nbdreply.magic = HTONL(NBD_REPLY_MAGIC);
	reply.nbdreply.error = HTONL(0);
	CURL *curlh = ecurl_easy_init();
	ecurl_easy_setopt(curlh, CURLOPT_URL, (const char* )serverinfo.readurl);
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
	ecurl_easy_setopt(curlh, CURLOPT_WRITEDATA, &reply);
	ecurl_easy_setopt(curlh, CURLOPT_WRITEFUNCTION,
			curl_myreply_writer_callback);
	// not sure if i should ecurl_easy_setopt(curlh, CURLOPT_BUFFERSIZE, blocksize);
	//should i set CURLOPT_MAXFILESIZE ?
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
			memcpy(reply.nbdreply.handle, request.nbdrequest.handle,
					sizeof(request.nbdrequest.handle));
			send(localrequestsocket, &reply.nbdreply, sizeof(reply.nbdreply),
					0);
			int err = pthread_mutex_unlock(&process_request_mutex);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"Failed to unlock process_request_mutex!\n");
			}
			break;
		}
		if (unlikely(bytes_read != sizeof(request.nbdrequest))) {
			myerror(0, errno,
					"got invalid request! all requests must be at minimum %zu bytes, but got a request with only %zu bytes! reply bytes follow:",
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
			request.nbdrequest.from = ntohll(request.nbdrequest.from);
			_Static_assert(
					sizeof(reply.nbdreply.handle)
							== sizeof(request.nbdrequest.handle),
					"if this fails, the code needs to get updated.");
			memcpy(reply.nbdreply.handle, request.nbdrequest.handle,
					sizeof(request.nbdrequest.handle));
//			if (unlikely(
//					request.nbdrequest.len > sizeof(reply.myreply.buffer))) {
//				// kernel requested to read more than CompileTimeDefinedMaxNumberOfBlocksPerKernelRequest blocks at once.
//				// to tell the kernel that we don't support reading
//				// that many blocks at once, we just return EINVAL
//				// the kernel will re-issue lower and lower until
//				// read success or until blocksize, whichever comes first.
//				// (completely transparently to the user process)
//				fprintf(stderr, "foo!\n");
//				fprintf(stdout, "foo!\n");
//				reply.myreply.nbdreply.error = HTONL(EINVAL);
//				ewrite(localrequestsocket, &reply.myreply.nbdreply,
//						sizeof(reply.myreply.nbdreply));
//				break;
//			}
			// likely because i'm not even sure the kernel
			// EVER will request to read 0 bytes. but IF, against expectation, it ever does,
			// the code inside would fail.
			// (rangebuf would contain an invalid range for CURLOPT_RANGE. invalid range per the http specifications. etc)
			if (likely(request.nbdrequest.len > 0)) {
				REALBUF_MINSIZE(request.nbdrequest.len);
				//string(39) "9223372036854775807-9223372036854775807"
				char rangebuf[40];
				sprintf(rangebuf, "%" PRIu64 "-%" PRIu64,
						(uint64_t) request.nbdrequest.from,
						(uint64_t) ((request.nbdrequest.from
								+ request.nbdrequest.len) - 1));
				ecurl_easy_setopt(curlh, CURLOPT_RANGE, rangebuf);
				long httpresponse;
				CURLcode err;
				do {
					reply.write_pos = 0;
					err = curl_easy_perform(curlh);
					ecurl_easy_getinfo(curlh, CURLINFO_RESPONSE_CODE,
							&httpresponse);
					if (likely(err == CURLE_OK && httpresponse == 206)) {
						break;
					} else {
						//something went wrong...
						fprintf(stderr,
								"ERROR: curl failed to fetch range %s. curl_easy_perform expected %ul, got %ul, strerror: %s. http response code expected 206, got %li. \n. will retry...",
								rangebuf, CURLE_OK, err,
								curl_easy_strerror(err), httpresponse);
						fflush(stderr);
					}
				} while (unlikely(err != CURLE_OK || httpresponse != 206));
			}
			{
				reply.nbdreply.error = HTONL(0);
				nbdreply(localrequestsocket, &reply.nbdreply,
						sizeof(reply.nbdreply), reply.mybuf.buffer,
						request.nbdrequest.len);
			}
			break;
		}
		case HTONL(NBD_CMD_WRITE): {
#ifdef DEBUG
			printf("GOT A NBD_CMD_WRITE REQUEST\n");
			print_request_data(&request);
#endif
			request.nbdrequest.len = ntohl(request.nbdrequest.len);
			REALBUF_MINSIZE(request.nbdrequest.len);

//			if (unlikely(request.nbdrequest.len > blocksize)) {
//				myerror(EXIT_FAILURE, errno,
//						"got a (write) request bigger than blocksize! blocksize: %i. requestsize: %ul.\n",
//						blocksize, request.nbdrequest.len);
//			}
			ssize_t bytes_read = recv(localrequestsocket, request.mybuf.buffer,
					request.nbdrequest.len, MSG_WAITALL);
			if (unlikely(bytes_read != request.nbdrequest.len)) {
				myerror(0, errno,
						"failed to read all the bytes of a WRITE request! the server said the request was %i bytes long, but could only read %zd bytes. read bytes follow:\n",
						request.nbdrequest.len, bytes_read);
				if (bytes_read <= 0) {
					fprintf(stderr,
							"(not printed because the read size was <=0)\n");
				} else {
					fwrite(&request.mybuf.buffer, (size_t) bytes_read, 1,
					stderr);
				}
				fflush(stdout);
				fflush(stderr);
				exit(EXIT_FAILURE);
			}
			{
				//let another thread read new requests
				int err = pthread_mutex_unlock(&process_request_mutex);
				if (unlikely(err != 0)) {
					myerror(EXIT_FAILURE, err,
							"Failed to unlock process_request_mutex!\n");
				}
			}
			{
				//TODO: write and respond. its in request.buffer
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
			_Static_assert(
					sizeof(reply.nbdreply.handle)
							== sizeof(request.nbdrequest.handle),
					"if this fails, the code needs to get updated.");
			memcpy(reply.nbdreply.handle, request.nbdrequest.handle,
					sizeof(request.nbdrequest.handle));
			nbdreply(localrequestsocket, &reply.nbdreply,
					sizeof(reply.nbdreply), NULL, 0);
//			ewrite(localrequestsocket, &reply.myreply.nbdreply,
//					sizeof(reply.myreply.nbdreply));
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
			_Static_assert(
					sizeof(reply.nbdreply.handle)
							== sizeof(request.nbdrequest.handle),
					"if this fails, the code needs to get updated.");
			memcpy(reply.nbdreply.handle, request.nbdrequest.handle,
					sizeof(request.nbdrequest.handle));
//			*(uint64_t*) reply.myreply.nbdreply.handle =
//					*(uint64_t*) request.nbdrequest.handle;
			nbdreply(localrequestsocket, (const char*) &reply.nbdreply,
					sizeof(reply.nbdreply), NULL, 0);
//			ewrite(localrequestsocket, &reply.myreply.nbdreply,
//					sizeof(reply.myreply.nbdreply));
			break;
		}
		default: {
			//implement NBD_CMD_WRITE_ZEROES ? its not accepted mainline, experimental, etcetc
			//implement NBD_CMD_STRUCTURED_REPLY? same as above
			//implement NBD_CMD_INFO ? same as above
			//implement NBD_CMD_CACHE ? same as above
			//send EINVAL?
			print_request_data(&request);
			myerror(EXIT_FAILURE, errno,
					"got a request type i did not understand!: %ul (see the source code for a list of requests i DO understand, in the switch case's)",
					request.nbdrequest.type);
			UNREACHABLE();
			break;
		}
		}
	}
//is_shutting_down should be true at this point
	--num_workerthreads;
	curl_easy_cleanup(curlh);
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
	err = pthread_mutex_init(&replymutex, NULL);
	if (unlikely(err != 0)) {
		myerror(EXIT_FAILURE, err, "failed to initialize mutex replymutex!\n");
	}
}
#define ARGV_NBD argv[1]
#define ARGV_THREADS argv[2]
#define ARGV_SERVERINFO argv[3]
#define ARGV_MYIP argv[4]
#define ARGV_MYPORT argv[5]
int main(int argc, char *argv[]) {
	{
		CURLcode err = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (unlikely(err != CURLE_OK)) {
			myerror(EXIT_FAILURE, errno,
					"curl_global_init failed! error number: %i. strerror: %s",
					err, curl_easy_strerror(err));
		}
	}
	init_mutexes();
	atexit(exit_global_cleanup);
	installShutdownSignalHandlers();
	if (argc - 1 != 5) {
		myerror(EXIT_FAILURE, EINVAL,
				"wrong number of input arguments.\n need 5, got %i\n usage: %s /dev/nbdX number_of_threads http://example.org/foo/serverinfo.php MyGlobalHostNameOrIP portnum\n",
				argc - 1, argv[0]);
	}
	kernelIPC.nbd_fd = open(ARGV_NBD, O_RDWR);
	if (unlikely(kernelIPC.nbd_fd == -1)) {
		myerror(EXIT_FAILURE, errno,
				"failed to open argument 1 (/dev/nbdX) in O_RDWR!: %s (maybe nbd module is not loaded? \"modprobe nbd\")\n",
				ARGV_NBD);
	}
	int workerthreads_num;
	{
		int scanres = sscanf(ARGV_THREADS, "%i", &workerthreads_num);
		if (unlikely(scanres == EOF || scanres < 1)) {
			myerror(EXIT_FAILURE, EINVAL,
			"failed to parse argument 2 (number_of_threads) as an integer!: %s\n",
			ARGV_THREADS);
		}
		if (unlikely(workerthreads_num < 1)) {
			myerror(EXIT_FAILURE, EINVAL,
			"number of worker threads MUST BE >=1  and <= max number of threads a single mutex lock can hold (and on my linux system, per glibc source, it is %ul (UINT_MAX), but i do not have the resources required to test it)\n",
			UINT_MAX);
		}
		printf("workerthreads: %i\n", workerthreads_num);
	}
	{
		if (unlikely(strlen(ARGV_MYIP)>(int)sizeof(serverinfo.myIP)-1)) {
			myerror(EXIT_FAILURE, EINVAL,
					"the length of ip/hostname MUST be <= %li but is %li",
					sizeof(serverinfo.myIP) - 1, strlen(ARGV_MYIP));
		}
		strcpy((char*) serverinfo.myIP, ARGV_MYIP);
	}
	{
		if (unlikely(1!=sscanf(ARGV_MYPORT,"%" SCNu16, &serverinfo.myport))) {
			myerror(EXIT_FAILURE, EINVAL,
					"failed to parse argument 5 (myport) as an unsigned 16 bit integer!: %s\n",
					ARGV_MYPORT);
		}
		if (unlikely(serverinfo.myport == 0)) {
			myerror(EXIT_FAILURE, EINVAL,
					"argument 5 (myport) CAN NOT BE 0. if i request port 0, the kernel will "
							"do funny stuff and give me a random port! not programmed for that.\n");
		}

	}
	getServerInfo(ARGV_SERVERINFO);
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
		pthread_t doitthread;
		pthread_mutex_t unlock_before_doingit_mutex;
		pthread_attr_t doitthread_attributes;
		{
			int err = pthread_attr_init(&doitthread_attributes);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed pthread_attr_init(&doitthread_attributes); ");
			}
			//1 meg should be plenty. a safeguard against small default stack sizes, and a memory saving feature of big stack sizes..
			//to put things in perspective, if the size of a pointer is 8 bytes (64bit), we can now hold 131,072 pointers.
			// (default on my system is 8 meg)
			err = pthread_attr_setstacksize(&doitthread_attributes,
					1 * 1024 * 1024);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed to set doitthread stack size! ");
			}
		}
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
		{
			int err = pthread_attr_destroy(&doitthread_attributes);
			if (unlikely(err != 0)) {
				myerror(EXIT_FAILURE, err,
						"failed pthread_attr_destroy(&doitthread_attributes); ");
			}
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
			//1 meg should be plenty. a safeguard against small default stack sizes, and a memory saving feature of big stack sizes..
			//to put things in perspective, if the size of a pointer is 8 bytes (64bit), we can now hold 131,072 pointers.
			// (default on my system is 8 meg)
			err = pthread_attr_setstacksize(&worker_attributes,
					(1 * 1024 * 1024));
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

	printf("CompileTimeDefinedNumberOfWriteSockets: %i\n",
	CompileTimeDefinedNumberOfWriteSockets);
	printf("CompileTimeDefinedMaxNumberOfBlocksPerKernelRequest: %i\n",
	CompileTimeDefinedMaxNumberOfBlocksPerKernelRequest);

	printf(
			"main thread has finished. will sleep until a signal is received. (by issuing pause();...)\n");
	fflush(stdout);
	pause();
	return EXIT_SUCCESS;
}
