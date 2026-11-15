// Validate the call-site assumptions used by the manifest-pin hooks against
// the installed steamclient image (i386 or x86_64, selected by ELF class).
#include <elf.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

struct Byte
{
	uint8_t value;
	bool wildcard;
};

static std::vector<Byte> parse(const char* pattern)
{
	std::vector<Byte> bytes;
	while (*pattern)
	{
		while (*pattern == ' ') ++pattern;
		if (!*pattern) break;
		const char* end = std::strchr(pattern, ' ');
		const std::string token(pattern, end ? end : pattern + std::strlen(pattern));
		bytes.push_back({static_cast<uint8_t>(
		                     token == "?" ? 0 : std::stoul(token, nullptr, 16)),
		                 token == "?"});
		pattern = end ? end : pattern + std::strlen(pattern);
	}
	return bytes;
}

static std::vector<size_t> findMatches(const std::string& image,
	                                    const std::vector<Byte>& pattern)
{
	std::vector<size_t> matches;
	for (size_t offset = 0; offset + pattern.size() <= image.size(); ++offset)
	{
		bool match = true;
		for (size_t i = 0; i < pattern.size(); ++i)
		{
			if (!pattern[i].wildcard
			    && static_cast<uint8_t>(image[offset + i]) != pattern[i].value)
			{
				match = false;
				break;
			}
		}
		if (match) matches.push_back(offset);
	}
	return matches;
}

static size_t callDestination(const std::string& image, size_t call)
{
	int32_t relative = 0;
	std::memcpy(&relative, image.data() + call + 1, sizeof(relative));
	return static_cast<size_t>(static_cast<int64_t>(call + 5) + relative);
}

static bool hasPushLiteralBefore(const std::string& image, size_t call,
	                              uint8_t literal)
{
	const size_t begin = call > 16 ? call - 16 : 0;
	for (size_t i = begin; i + 1 < call; ++i)
		if (static_cast<uint8_t>(image[i]) == 0x6a
		    && static_cast<uint8_t>(image[i + 1]) == literal)
			return true;
	return false;
}

static int lastEsiFlagBefore(const std::string& image, size_t call)
{
	const size_t begin = call > 80 ? call - 80 : 0;
	size_t lastXor = 0;
	bool foundXor = false;
	size_t lastMov1 = 0;
	bool foundMov1 = false;
	for (size_t i = begin; i + 2 <= call; ++i)
	{
		const auto b0 = static_cast<uint8_t>(image[i]);
		const auto b1 = static_cast<uint8_t>(image[i + 1]);
		if (b0 == 0x31 && b1 == 0xf6)
		{
			lastXor = i;
			foundXor = true;
		}
		if (i + 5 <= call && b0 == 0xbe && b1 == 0x01
		    && static_cast<uint8_t>(image[i + 2]) == 0x00
		    && static_cast<uint8_t>(image[i + 3]) == 0x00
		    && static_cast<uint8_t>(image[i + 4]) == 0x00)
		{
			lastMov1 = i;
			foundMov1 = true;
		}
	}
	if (foundXor && (!foundMov1 || lastXor > lastMov1))
		return 0;
	if (foundMov1 && (!foundXor || lastMov1 > lastXor))
		return 1;
	return -1;
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: test_manifestpin_patterns /path/to/steamclient.so\n";
		return 2;
	}
	std::ifstream input(argv[1], std::ios::binary);
	if (!input)
	{
		std::cerr << "could not open " << argv[1] << '\n';
		return 2;
	}
	const std::string image{std::istreambuf_iterator<char>(input),
	                        std::istreambuf_iterator<char>()};

	if (image.size() < EI_NIDENT
	    || static_cast<uint8_t>(image[EI_MAG0]) != ELFMAG0
	    || static_cast<uint8_t>(image[EI_MAG1]) != ELFMAG1
	    || static_cast<uint8_t>(image[EI_MAG2]) != ELFMAG2
	    || static_cast<uint8_t>(image[EI_MAG3]) != ELFMAG3)
	{
		std::cerr << "not a valid ELF image: " << argv[1] << '\n';
		return 2;
	}

	const auto elfClass = static_cast<uint8_t>(image[EI_CLASS]);
	if (elfClass != ELFCLASS32 && elfClass != ELFCLASS64)
	{
		std::cerr << "unsupported ELF class in " << argv[1] << '\n';
		return 2;
	}

	const char* signature = elfClass == ELFCLASS64
		? "41 57 41 56 41 55 41 54 41 89 F4 55 48 89 FD 53 48 81 EC ? ? 00 00 "
		  "64 4C 8B 2C 25 28 00 00 00 4C 89 AC 24 ? ? 00 00 49 89 CD C6 47 1C "
		  "01 44 8B 72 10"
		: "E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 3C 04 00 00 "
		  "8B 5D 08 8B 75 14 89 85 ? ? FF FF 8B 45 0C";

	const auto builders = findMatches(image, parse(signature));
	if (builders.size() != 1)
	{
		std::cerr << "expected one BuildDepotDependency; found "
		          << builders.size() << '\n';
		return 1;
	}

	size_t flag0 = 0;
	size_t flag1 = 0;
	for (size_t call = 0; call + 5 <= image.size(); ++call)
	{
		if (static_cast<uint8_t>(image[call]) != 0xe8
		    || callDestination(image, call) != builders.front())
			continue;
		if (elfClass == ELFCLASS64)
		{
			const int flag = lastEsiFlagBefore(image, call);
			flag0 += (flag == 0);
			flag1 += (flag == 1);
		}
		else
		{
			flag0 += hasPushLiteralBefore(image, call, 0);
			flag1 += hasPushLiteralBefore(image, call, 1);
		}
	}
	if (flag0 != 4 || flag1 != 4)
	{
		std::cerr << "expected four flag=0 and four flag=1 builder calls; found "
		          << flag0 << " and " << flag1 << '\n';
		return 1;
	}

	std::cout << "manifest-pin call sites match the expected flag pairs\n";
	return 0;
}
