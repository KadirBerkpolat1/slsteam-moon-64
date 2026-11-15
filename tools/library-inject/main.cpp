#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <link.h>
#include <set>
#include <string>
#include <unordered_set>

static std::unordered_set<std::string> LIBS =
{
	"libcurl.so",
	"libcurl.so.3",
	"libcurl.so.4",
	"libcurl.so.4.0.0",
	"libcurl.so.4.1.0",
	"libcurl.so.4.2.0",
	"libcurl.so.4.3.0",
	"libcurl.so.4.4.0",
	"libcurl.so.4.5.0",
	"libcurl.so.4.6.0",
	"libcurl.so.4.7.0",
	"libcurl.so.4.8.0",
};

static std::unordered_set<std::string> DIRS =
{
	//Fallback for 32 bit OS?
	"/usr/local/lib",
	"/usr/lib",

	"/usr/local/lib32", //Bazzite
	"/usr/lib/i386-linux-gnu/", //Mint
	"/usr/lib32", //SteamOS
};

char *la_objsearch(const char *name, __attribute__((unused)) uintptr_t *cookie, unsigned int flag)
{
	if (flag != LA_SER_ORIG)
	{
		return const_cast<char*>(name);
	}

	if(LIBS.contains(name))
	{
		for(const auto& dir : DIRS)
		{
			char path[255];
			snprintf(path, sizeof(path), "%s/%s", dir.c_str(), name);

			if (!std::filesystem::exists(path))
			{
				continue;
			}

			char* n = reinterpret_cast<char*>(malloc(sizeof(path)));
			if (!n)
			{
				break;
			}

			strcpy(n, path);

			return n;
		}
	}

	return const_cast<char*>(name);
}

// Answer with the loader's interface version, capped at ours: a loader offering
// LAV 1 (upstream glibc <= 2.34) refuses a module answering 2, and on 2.31/2.33
// that refusal aborts the whole process.  la_objsearch exists in LAV 1.
unsigned int la_version(unsigned int version)
{
	return version == 0 ? LAV_CURRENT : (version < LAV_CURRENT ? version : LAV_CURRENT);
}

//void la_preinit(__attribute__((unused)) uintptr_t *cookie)
//{
//}
