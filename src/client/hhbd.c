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
#include <unistd.h>
#include <error.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/nbd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <pwd.h>

#include "misc.h"
#include "misc_extra.h"
#include "hhbd.h"
int nbd_fd = -1;
int localrequestsocket = -1;
int remoterequestsocket = -1;
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
	if (nbd_fd != -1) {
		int err;
		/*
		 * NBD_DISCONNECT:
		 * NBD_CLEAR_SOCK:
		 * */
		err = ioctl(nbd_fd, NBD_CLEAR_SOCK);
		if (err == -1) {
			myerror(0, errno, "Warning: NBD_CLEAR_SOCK failed!\n");
		}
//		err = ioctl(nbd_fd, NBD_DISCONNECT);
//		if (err == -1) {
//			myerror(0, errno, "Warning: NBD_DISCONNECT failed!\n");
//		}
		if (-1 == close(nbd_fd)) {
			myerror(0, errno, "Warning: failed to close the nbd handle!\n");
		}
	}
	if (remoterequestsocket != -1) {
		if (-1 == close(remoterequestsocket)) {
			myerror(0, errno,
					"Warning: failed to close the remoterequestsocket!\n");
		}
	}
	if (localrequestsocket != -1) {
		if (-1 == close(localrequestsocket)) {
			myerror(0, errno,
					"Warning: failed to close the localrequestsocket!\n");
		}
	}
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
	char *issuer_username_;
	struct passwd* info = getpwuid(siginfo->si_uid);
	if (info && info->pw_name) {
		issuer_username_ = info->pw_name;
	} else {
		issuer_username_ = emalloc(
				((size_t) snprintf(NULL, 0,
						"(failed to get username, errno: %i , strerror: %s)",
						errno, strerror(errno))) + 1);
		sprintf(issuer_username_,
				"(failed to get username, errno: %i , strerror: %s)",
				errno, strerror(errno));
	}
	char issuer_username[strlen(issuer_username_) + 1];
	strcpy(issuer_username, issuer_username_);
	if (!info) {
		free(issuer_username_);
	}
	///printf("SHUTDOWN SIGNAL!\n");
	char* warnings_and_errors;
	char *issuer_exe_ = get_process_name_by_pid((int) siginfo->si_pid,
			&warnings_and_errors);
	size_t issuer_exe_len = strlen(issuer_exe_);
	if (issuer_exe_len < 1) {
		free(issuer_exe_);
		issuer_exe_ = warnings_and_errors;
		issuer_exe_len = strlen(issuer_exe_);
	} else {
		free(warnings_and_errors);
	}
	char issuer_exe[issuer_exe_len + 1];
	strcpy(issuer_exe, issuer_exe_);
	free(issuer_exe_);
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
void* nbd_doit_thread(void* arg) {
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
	if (unlikely(0 != ioctl(nbd_fd, NBD_SET_SOCK, remoterequestsocket))) {
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(nbd_fd, NBD_SET_SOCK, remoterequestsocket) !\n");
	}
	enum {
		blocksize = 4096, totalsize = blocksize * 1999
	};
	_Static_assert(totalsize % blocksize == 0,
			"the kernel is dividing totalsize by blocksize to get number of sectors. "
					"if totalsize is not a multiple of blocksize, you are probably doing something wrong!!!");
	//TODO: check, is 4096 the max block size?
	if (unlikely(0 != ioctl(nbd_fd, NBD_SET_BLKSIZE, blocksize))) { //should check if higher is possible too...
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(nbd_fd, NBD_SET_BLKSIZE, %i) !\n", blocksize);
	}
	if (unlikely(0 != ioctl(nbd_fd, NBD_SET_SIZE, totalsize))) { //should check if higher is possible too...
		myerror(EXIT_FAILURE, errno,
				"failed to ioctl(nbd_fd, NBD_SET_SIZE:, %i) !\n", totalsize);
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
	if (unlikely(0 != ioctl(nbd_fd, NBD_DO_IT))) {
		myerror(EXIT_FAILURE, errno,
				"nbd_do_it_thread failed to ioctl(nbd_fd, NBD_DO_IT) !\n");
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
		install_shutdown_signal_handler(SIGXCPU);//not sure this 1 is catchable..
		install_shutdown_signal_handler(SIGXFSZ);
#endif
#endif
#endif
		//Now there are more non-standard signals who's default action is to terminate the process
		// which we probably should look out for, but.... cba now. they shouldn't happen anyway (like some 99% of the list above)
}
int main(int argc, char* argv[]) {
	installShutdownSignalHandlers();
	atexit(exit_global_cleanup);
	{
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
	}
	if (argc != 3) {
		myerror(EXIT_FAILURE, EINVAL,
				"wrong number of input arguments.\n need 2, got %i\n usage: %s /dev/nbdX number_of_threads\n",
				argc - 1, argv[0]);
	}
	nbd_fd = open(argv[1], O_RDWR);
	if (nbd_fd == -1) {
		myerror(EXIT_FAILURE, errno, "failed to open %s in O_RDWR!!\n",
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
	{
		int socks[2];
		int err = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
		if (unlikely(err == -1)) {
			myerror(EXIT_FAILURE, errno,
					"Failed to create IPC unix socket pair!! \n");
		}
		localrequestsocket = socks[0];
		remoterequestsocket = socks[1];
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
		//WARNING when changing INIT_PASSWD: PER SPECS, IT MUST BE EXACTLY 8 BYTES LONG.
		//  because its a big-endian 64bit unsigned integer, really. manually add NULL bytes to it if needed.
		const char INIT_PASSWD_C[8] = "NBDMAGIC";
		_Static_assert(sizeof(INIT_PASSWD_C) == 8,
				"PER SPECS, INIT_PASSWD MUST BE EXACTLY 8 BYTES LONG. because its a big-endian 64bit unsigned integer, really. manually add NULL bytes to it if needed.");
		//Todo: compile-time htonl & co...
		const uint64_t INIT_PASSWD = htonll(*(uint64_t* )INIT_PASSWD_C);
		//_Static_assert(INIT_PASSWD[strlen(INIT_PASSWD)] != '\0',"If you are __SURE__ there is a null terminator, remove this assert. more likely, it is a coding error.");
		char buf[sizeof(INIT_PASSWD)];
		ssize_t err = read(localrequestsocket, buf, sizeof(INIT_PASSWD));
		if (err != sizeof(INIT_PASSWD)) {
			fprintf(
			stderr,
					"Error: need to read %zu bytes for the init password, but could only read %zd bytes. read buffer follows:\n",
					sizeof(INIT_PASSWD), err);
			if (err <= -1) {
				fprintf(
				stderr,
						"(buffer not printed because <= -1 was returned, so the buffer probably contains the initial bytes, not what was read)\n");
			} else {
				fwrite(buf, 1, MIN((size_t )(err), sizeof(INIT_PASSWD)),
				stderr);
			}
			myerror(EXIT_FAILURE, errno, " ");
		} else {
			myerror(EXIT_FAILURE, errno, "GOT MESSAGE FROM SERVER!");
		}
		//if(memcmp~
	}

	printf(
			"main thread has finished. will sleep until a signal is received. (by issuing pause();...)\n");
	fflush(stdout);
	return EXIT_SUCCESS;
}
