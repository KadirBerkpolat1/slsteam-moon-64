// Validate the optional runtime-capability locators against the installed
// Steam client (i386 or x86_64, selected by the ELF class of argv[1]).
// These are the compiled locators whose failure silently degrades a
// feature instead of failing the load, so a signature that stops matching after
// a client update is easy to miss:
//
//   CUser::ProcessPendingLicenseUpdates  PackagePatch loses the runtime license
//                                        refresh, so adding a game needs a
//                                        Steam restart again.
//   CCMInterface::RecvPkt                Family Share message filtering turns
//                                        off.
//   CSteamApp::OwnershipFlagsReference    LibraryRemoval cannot derive the
//                                        ownership-flags field offset, so
//                                        visual library removal turns off.
//   ParentalSignatureCheck               the local parental unlock turns off.
//   CAPIJob::GetPlayerStats              the stats-job debug trace turns off.
//   CUser::m_CompatManager               live compatibility-tool mapping for
//                                        newly added titles turns off.
//   CDepotDownloadMgr::BuildDepotDependency
//                                        install-plan pinning, empty-depot
//                                        pruning and event-driven manifest
//                                        staging turn off.
//
// Each signature is asserted to be present verbatim in src/patterns.cpp, so
// this test can never pass against a stale private copy of the pattern, and to
// resolve to exactly one match in the module it belongs to.
//
// Build and run with:
//   make test-optional-locators
// or point it at another install:
//   make test-optional-locators STEAMCLIENT=... STEAMUI=...

#include "feats/compatlive.hpp"
#include "feats/libraryremoval_policy.hpp"
#include <elf.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}

struct Byte
{
	std::uint8_t value;
	bool wildcard;
};

std::vector<Byte> parse(std::string_view pattern)
{
	std::vector<Byte> bytes;
	std::size_t cursor = 0;
	while (cursor < pattern.size())
	{
		while (cursor < pattern.size() && pattern[cursor] == ' ')
			++cursor;
		if (cursor >= pattern.size())
			break;
		const std::size_t end = pattern.find(' ', cursor);
		const std::string token(pattern.substr(
			cursor, end == std::string_view::npos ? end : end - cursor));
		bytes.push_back({
			static_cast<std::uint8_t>(
				token == "?" ? 0 : std::stoul(token, nullptr, 16)),
			token == "?",
		});
		cursor = end == std::string_view::npos ? pattern.size() : end;
	}
	return bytes;
}

std::vector<std::size_t> findMatches(
	const std::string& image, const std::vector<Byte>& pattern)
{
	std::vector<std::size_t> matches;
	if (pattern.empty() || image.size() < pattern.size())
		return matches;
	for (std::size_t offset = 0; offset + pattern.size() <= image.size(); ++offset)
	{
		bool match = true;
		for (std::size_t index = 0; index < pattern.size(); ++index)
		{
			if (!pattern[index].wildcard
			    && static_cast<std::uint8_t>(image[offset + index])
			           != pattern[index].value)
			{
				match = false;
				break;
			}
		}
		if (match)
			matches.push_back(offset);
	}
	return matches;
}

std::string readFile(const char* path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		std::cerr << "could not open " << path << '\n';
		std::exit(2);
	}
	return std::string{std::istreambuf_iterator<char>(input),
	                   std::istreambuf_iterator<char>()};
}
enum class ElfArch
{
	Invalid,
	I386,
	X86_64,
};

ElfArch detectElfArch(const std::string& image)
{
	if (image.size() < EI_NIDENT)
		return ElfArch::Invalid;
	const auto* ident = reinterpret_cast<const unsigned char*>(image.data());
	if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1
	    || ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
	{
		return ElfArch::Invalid;
	}
	if (ident[EI_CLASS] == ELFCLASS32)
		return ElfArch::I386;
	if (ident[EI_CLASS] == ELFCLASS64)
		return ElfArch::X86_64;
	return ElfArch::Invalid;
}

// Returns the single match offset, or npos after reporting the failure.
std::size_t resolveUnique(
	const std::string& image,
	std::string_view name,
	std::string_view signature,
	const std::string& source)
{
	expect(source.find(signature) != std::string::npos,
	       std::string(name) + ": signature is not the one compiled in "
	                           "src/patterns.cpp");
	const auto matches = findMatches(image, parse(signature));
	if (matches.size() == 1)
		return matches.front();
	std::cerr << "FAIL: " << name << ": expected exactly one match, found "
	          << matches.size() << '\n';
	++failures;
	return std::string::npos;
}

void checkI386(
	const std::string& client,
	const std::string& ui,
	const std::string& source)
{
	// The trailing `lea eax,[ebx+disp32]` reaches a data symbol through the PIC
	// base, so its displacement moves whenever the GOT layout shifts. The hook
	// only calls the resolved entry, so it stays wildcarded. The two CUser
	// member offsets it loads (count near 0x1C7C, array base near 0x1C70) drift
	// by 4 across builds (0x1C7C->0x1C78, 0x1C70->0x1C6C on a later beta); they
	// are internal to the function, so only their low byte is masked while the
	// 0x1C high byte stays pinned.
	(void)resolveUnique(
		client, "CUser::ProcessPendingLicenseUpdates",
		"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 44 24 40 8B 88 ? 1C "
		"00 00 85 C9 0F 8E ? ? ? ? 05 ? 1C 00 00 89 44 24 18 8D 83 ? ? ? ? 8B 30",
		source);

	// The stack frame grew on a later beta (sub esp,0x4cc -> larger), drifting
	// both the frame immediate and the frame-relative spill of the saved
	// argument together. Both are local-frame layout, not values the hook reads,
	// so mask the frame immediate and the spill displacement (high bytes kept).
	(void)resolveUnique(
		client, "CCMInterface::RecvPkt",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC ? ? 00 00 8B 45 08 "
		"8B 7D 0C 89 85 ? ? FF FF 65 A1 14 00 00 00 89 45 E4 31 C0 8B 86",
		source);

	// FillInAppOverview serializes several fields with this very shape, one
	// after another, so the discriminator has to be the field offset. The
	// ownership read stays at 0x18 on both the 2026-08-03 and the 2026-08-16
	// client, while the block immediately after it reads 0x50. What moved is
	// only the register holding the app pointer (ecx -> edx), so the modrm byte
	// is the wildcard and the displacement stays pinned. Wildcarding the
	// displacement instead also matches exactly once on the newer client, but
	// it lands on that neighbouring field, and LibraryRemoval would then write
	// hidden ownership bits into it.
	const std::size_t ownership = resolveUnique(
		ui, "CSteamApp::OwnershipFlagsReference",
		"8B ? 18 8B 9D ? ? ? ? 83 EC 04 50 8D 83 ? ? ? ? 50 FF B5 ? ? ? ? "
		"E8 ? ? ? ?",
		source);
	if (ownership != std::string::npos)
	{
		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(ui.data()) + ownership;
		const auto offset = LibraryRemovalPolicy::deriveOwnershipOffset(
			std::span<const std::uint8_t>(bytes, 3));
		expect(offset.has_value(),
		       "CSteamApp::OwnershipFlagsReference: the matched instruction does "
		       "not decode to a usable ownership-flags offset");
		if (offset)
		{
			std::cout << "CSteamApp::OwnershipFlags offset = 0x" << std::hex
			          << *offset << std::dec << '\n';
		}
	}

	// The failure path pushes a member near 0x182 (0x166 on the 2026-10
	// client) for its log line; only the branch at +2 is patched, so the
	// member offset is masked. The jne distance (0x27) keeps the sibling
	// check (jne 0x63) out.
	const std::size_t parental = resolveUnique(
		client, "ParentalSignatureCheck",
		"84 C0 75 27 8B 85 ? ? ? ? 8D 9D ? ? ? ? 83 EC 04 FF B0 ? ? 00 00 "
		"8D 86 ? ? ? ? 50 53 E8 ? ? ? ?",
		source);
	if (parental != std::string::npos)
	{
		expect(static_cast<std::uint8_t>(client[parental + 2]) == 0x75,
		       "ParentalSignatureCheck: the patched byte is not the jne");
	}

	// Direct entry of the CAPIJobRequestUserStats job body (the function
	// carrying "Player.GetUserStats#1"); its only call site was rewritten on
	// the 2026-10 client, so the call-site form no longer exists.
	(void)resolveUnique(
		client, "CAPIJob::GetPlayerStats",
		"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC ? ? 00 00 8B 75 08 "
		"89 B5 ? ? FF FF 89 FB 65 A1 14 00 00 00 89 45 E4 31 C0 8D 85 ? ? FF "
		"FF 6A 00 50",
		source);

	// The CUser constructor's embedded CCompatManager; the matched LEA must
	// decode to the member offset CSteamEngine uses at runtime.
	const std::size_t compat = resolveUnique(
		client, "CUser::m_CompatManager",
		"8D 8E ? ? ? ? 58 56 51 89 8D ? ? ? ? E8 ? ? ? ? 58 5A C7 86 ? ? ? ? "
		"FF FF FF FF",
		source);
	if (compat != std::string::npos)
	{
		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(client.data()) + compat;
		expect(CompatLive::decodeManagerOffset(
			       std::span<const std::uint8_t>(bytes, 6)).has_value(),
		       "CUser::m_CompatManager: the matched LEA does not decode");
	}

	// The install-plan builder (eight flag-0/flag-1 callers). The 2026-10
	// client shrank its frame to 0x43c and reassigned its argument registers;
	// the yielding sibling that kept the old 0x48c frame is not this hook.
	(void)resolveUnique(
		client, "CDepotDownloadMgr::BuildDepotDependency",
		"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 3C 04 00 00 8B 5D 08 "
		"8B 75 14 89 85 ? ? FF FF 8B 45 0C",
		source);
	(void)resolveUnique(
		client, "CAppInfoCache::GetOrAddAppData",
		"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 2C 8B 75 0C 8B 7D 10 "
		"89 45 D0 8B 80 ? ? 00 00 8B 00 85 C0 0F 85 ? ? ? ?",
		source);
	(void)resolveUnique(
		client, "CUser::NotifyLicensesUpdated",
		"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC BC 01 00 00 8B 45 08 "
		"8B ? ? 1B 00 00 89 9D 54 FE FF FF 85 ? 0F 8E ? ? ? ? 8D 83 ? ? ? ? "
		"83 EC 08 6A 7D 50",
		source);
	(void)resolveUnique(
		client, "CUser::MarkLicenseAsChanged",
		"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 74 24 40 8B 44 24 48 "
		"8B BE ? ? 00 00 88 44 24 1C 8D 86 ? ? 00 00 89 44 24 0C 85 FF 0F 84 ? ? ? ?",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::ProcessDepotManifest",
		"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 55 1C 89 45 C0 "
		"8B 45 18 89 55 CC 89 45 C8",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::PrepareDepotDownload",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 60 8B 7D 08 8B 45 18 "
		"8B 55 1C FF 75 20 89 45 98 52 50 FF 75 14 89 55 9C FF 75 10 FF 75 0C "
		"57 E8 ? ? ? ? 8B 47 4C 83 C4 20 83 F8 FF",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::EvaluateConfigChanges",
		"55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 "
		"89 85 40 FF FF FF 8B 45 08 8B 40 04",
		source);
	(void)resolveUnique(
		client, "CAppInfoCache::ThreadedReadFromDisk",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 0C 11 00 00 8B 45 08 "
		"89 85 0C EF FF FF",
		source);
}

void checkX86_64(
	const std::string& client,
	const std::string& ui,
	const std::string& source)
{
	(void)resolveUnique(
		client, "CUser::ProcessPendingLicenseUpdates",
		"41 57 48 89 F8 41 56 41 55 41 54 55 53 48 83 EC 28 8B 97 ? 25 00 00 48 89 7C 24 18 85 D2 0F 8E ? ? ? ? 48 05 ? 25 00 00 48 89 44 24 08 4C 8D 25 ? ? ? ?",
		source);
	(void)resolveUnique(
		client, "CCMInterface::RecvPkt",
		"41 57 41 56 41 55 41 54 49 89 FC 55 53 48 89 F3 48 81 EC ? ? 00 00 48 8B 2D ? ? ? ? 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0",
		source);
	const std::size_t ownership = resolveUnique(
		ui, "CSteamApp::OwnershipFlagsReference",
		"41 8B ? 1C 48 8B BD ? ? ? ? 48 8D 35 ? ? ? ? E8 ? ? ? ?",
		source);
	if (ownership != std::string::npos)
	{
		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(ui.data()) + ownership;
		const auto decoded = LibraryRemovalPolicy::deriveOwnershipOffset64(
			std::span<const std::uint8_t>(bytes, 4));
		expect(decoded.has_value(),
		       "CSteamApp::OwnershipFlagsReference: the matched instruction does "
		       "not decode to a usable ownership-flags offset");
		if (decoded)
		{
			expect(decoded->offset == 0x1C && decoded->length == 4,
			       "CSteamApp::OwnershipFlagsReference: expected offset 0x1C, "
			       "length 4");
			std::cout << "CSteamApp::OwnershipFlags offset = 0x" << std::hex
			          << decoded->offset << " (length " << std::dec
			          << decoded->length << ")\n";
		}
	}

	const std::size_t parental = resolveUnique(
		client, "ParentalSignatureCheck",
		"84 C0 75 24 8B 93 ? ? 00 00 48 8D AC 24 ? ? 00 00 31 C0 48 8D 35 ? ? ? ? 48 89 EF E8 ? ? ? ?",
		source);
	if (parental != std::string::npos)
	{
		expect(static_cast<std::uint8_t>(client[parental + 2]) == 0x75,
		       "ParentalSignatureCheck: the patched byte is not the jne");
	}
	(void)resolveUnique(
		client, "CAPIJob::GetPlayerStats",
		"41 57 31 F6 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 64 48 8B 1C 25 28 00 00 00 48 89 9C 24 ? ? 00 00 48 89 FB 48 8D 44 24 ? 48 89 C7 48 89 04 24 E8 ? ? ? ? 48 8D 84 24 ? ? 00 00 31 F6 48 89 C7 48 89 44 24 08 E8 ? ? ? ? 48 8B AB ? ? 00 00",
		source);
	const std::size_t compat = resolveUnique(
		client, "CUser::m_CompatManager",
		"48 8D 83 ? ? 00 00 48 89 DE 48 89 C7 48 89 84 24 ? ? 00 00 E8 ? ? ? ? 66 0F 6F 1D",
		source);
	if (compat != std::string::npos)
	{
		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(client.data()) + compat;
		const auto offset = CompatLive::decodeManagerOffset64(
			std::span<const std::uint8_t>(bytes, 7));
		expect(offset.has_value() && *offset == 0x4A58,
		       "CUser::m_CompatManager: the matched x86_64 LEA does not decode "
		       "to 0x4A58");
	}
	(void)resolveUnique(
		client, "CDepotDownloadMgr::BuildDepotDependency",
		"41 57 41 56 41 55 41 54 41 89 F4 55 48 89 FD 53 48 81 EC ? ? 00 00 64 4C 8B 2C 25 28 00 00 00 4C 89 AC 24 ? ? 00 00 49 89 CD C6 47 1C 01 44 8B 72 10",
		source);
	(void)resolveUnique(
		client, "CAppInfoCache::GetOrAddAppData",
		"41 57 41 56 41 55 41 54 49 89 FC 55 89 F5 53 48 83 EC 18 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 08 89 D3 8B 10 31 C0 85 D2 74 14 48 8D 3D ? ? ? ? 48 89 E2 BE 04 00 00 00 E8 ? ? ? ? 85 ED 48 89 04 24 0F 84 ? ? ? ? 49 63 44 24 68 83 F8 FF",
		source);
	(void)resolveUnique(
		client, "CUser::NotifyLicensesUpdated",
		"41 57 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 44 8B 8F ? 23 00 00 48 89 7C 24 ? 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0 45 85 C9 0F 8E ? ? ? ? 48 8D 3D ? ? ? ? BE 7D 00 00 00 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 5C 24 ? 45 31 C0 B9 01 00 00 00 BE 7D 00 00 00",
		source);
	(void)resolveUnique(
		client, "CUser::MarkLicenseAsChanged",
		"41 57 48 8D 87 ? 23 00 00 41 89 F7 41 56 41 55 41 54 55 53 48 83 EC 28 88 54 24 ? 8B 97 ? 23 00 00 89 74 24 ? 48 89 44 24 ? 85 D2 0F 84 ? ? ? ?",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::ProcessDepotManifest",
		"41 57 41 56 41 89 CE 41 55 41 54 49 89 FC 55 53 4C 89 C3 48 83 EC 38 48 89 34 24 89 54 24 0C 4C 89 4C 24 10 64 48 8B 04 25 28 00 00 00 48 89 44 24 28 48 63 47 60 83 F8 FF",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::PrepareDepotDownload",
		"41 57 41 56 41 55 41 54 55 53 48 89 FB 48 83 EC 78 48 89 74 24 08 89 4C 24 04 64 4C 8B 24 25 28 00 00 00 4C 89 64 24 68 4D 89 C4 E8 ? ? ? ? 48 63 43 60 83 F8 FF",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::EvaluateConfigChanges",
		"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 89 95 ? ? FF FF 8B 57 08 48 89 B5 ? ? FF FF 64 48 8B 04 25 28 00 00 00 48 89 45 C8 31 C0 41 89 D4 41 83 E4 01 75 ? 83 BF ? ? 00 00 19",
		source);
	(void)resolveUnique(
		client, "CAppInfoCache::ThreadedReadFromDisk",
		"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC 88 11 00 00 48 89 BD ? ? FF FF 64 48 8B 04 25 28 00 00 00 48 89 45 C8",
		source);
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3 || argc > 4)
	{
		std::cerr << "usage: test_optional_locators <steamclient.so> "
		             "<steamui.so> [src/patterns.cpp]\n";
		return 2;
	}
	const std::string client = readFile(argv[1]);
	const std::string ui = readFile(argv[2]);
	const std::string source = readFile(argc == 4 ? argv[3] : "src/patterns.cpp");

	const ElfArch clientArch = detectElfArch(client);
	const ElfArch uiArch = detectElfArch(ui);
	if (clientArch == ElfArch::Invalid || uiArch == ElfArch::Invalid
	    || clientArch != uiArch)
	{
		std::cerr << "FAIL: invalid or mismatched ELF class between "
		          << argv[1] << " and " << argv[2] << '\n';
		return 2;
	}

	if (clientArch == ElfArch::X86_64)
		checkX86_64(client, ui, source);
	else
		checkI386(client, ui, source);

	if (failures != 0)
	{
		std::cerr << failures << " optional locator check(s) failed\n";
		return 1;
	}
	std::cout << "optional runtime-capability locators resolve uniquely\n";
	return 0;
}
