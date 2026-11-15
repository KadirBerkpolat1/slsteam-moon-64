#pragma once

// NSS host-module warm-up for libcurl.
//
// libcurl's threaded resolver runs more than one getaddrinfo() at once. When
// the NSS host modules from nsswitch.conf (libnss_mymachines, libnss_resolve,
// ...) are not loaded yet in this link-map namespace, those threads dlopen
// the same module concurrently. Measured on glibc 2.44: libnss_mymachines'
// constructor calls dlsym() while the other thread is inside dlopen(), the
// two threads take the loader's locks in opposite orders, both block forever,
// and the caller hangs in curl_easy_cleanup(). SLSsteam is loaded through
// LD_AUDIT into its own namespace, so the modules Steam itself loaded do not
// help.
//
// One lookup on a single thread loads the modules before any resolver thread
// exists. Call this from every libcurl loader, before the first request.

#include <mutex>

#include <netdb.h>
#include <sys/socket.h>

namespace nss
{

inline void warmHostModules()
{
	static std::once_flag once;
	std::call_once(once, [] {
		addrinfo hints {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* res = nullptr;
		if (getaddrinfo("api.steampowered.com", "443", &hints, &res) == 0)
			freeaddrinfo(res);
	});
}

} // namespace nss
