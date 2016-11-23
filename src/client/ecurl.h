/*
 * ecurl.h
 *
 *  Created on: Nov 10, 2016
 *      Author: hanshenrik←
 */

#ifndef CLIENT_ECURL_H_
#include <curl/curl.h>
#define CLIENT_ECURL_H_
CURL *ecurl_easy_init();
CURLcode ecurl_easy_perform(CURL *easy_handle);
char *ecurl_easy_escape(CURL * curl, const char * string, const int length);


//this 1 almost has to be a macro because of how curl_easy_setopt is made (a macro taking different kinds of parameter types)
#define ecurl_easy_setopt(handle, option, parameter){ \
CURLcode ret=curl_easy_setopt(handle,option,parameter); \
if(unlikely( ret  != CURLE_OK)){ \
	 myerror(EXIT_FAILURE,errno,"curl_easy_setopt failed to set option %i. CURLCode: %i curl_easy_strerror: %s\n", option, ret, curl_easy_strerror(ret));   \
	} \
}

#define ecurl_easy_getinfo(curl,info,...){ \
	CURLcode ret=curl_easy_getinfo(curl,info,__VA_ARGS__); \
	if(unlikely(ret!=CURLE_OK)){ \
		 myerror(EXIT_FAILURE,errno,"curl_easy_getinfo failed to get info %i. CURLCode: %i curl_easy_strerror: %s\n", info, ret, curl_easy_strerror(ret));   \
	} \
}


#endif /* CLIENT_ECURL_H_ */
