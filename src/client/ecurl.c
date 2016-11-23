#include <curl/curl.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include "misc.h"

CURL *ecurl_easy_init() {
	CURL *ret = curl_easy_init();
	if (unlikely(ret==NULL)) {
		myerror(EXIT_FAILURE, errno, "curl_easy_init failed!");
	}
	return ret;
}
CURLcode ecurl_easy_perform(CURL *easy_handle) {
	CURLcode ret = curl_easy_perform(easy_handle);
	if (unlikely(ret != CURLE_OK)) {
		myerror(EXIT_FAILURE, errno,
				"curl_easy_perform failed! CURLcode: %i. curl_easy_strerror: %s",
				ret, curl_easy_strerror(ret));
	}
	return ret;
}

char *ecurl_easy_escape(CURL * curl, const char * string, const int length) {
	char *ret = curl_easy_escape(curl, string, length);
	if (unlikely(ret==NULL)) {
		myerror(EXIT_FAILURE, errno,
				"curl_easy_escape failed! input string: %s\n", string);
	}
	return ret;
}

