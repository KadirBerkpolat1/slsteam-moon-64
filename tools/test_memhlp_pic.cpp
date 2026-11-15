// TDD regression test for bounded PIC-thunk instruction formatting and
// MemHlp::fixPICThunkCall rel32 decoding / segment safety on the host arch.

#include "log.hpp"
#include "memhlp.hpp"
#include "utils.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

std::unique_ptr<CLog> g_pLog;

CLog::CLog(const char* logPath) : path(logPath) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::None; }
bool CLog::shouldNotify() { return false; }

std::vector<std::string> Utils::strsplit(const char* str, const char* delimiter)
{
	std::vector<std::string> out;
	if (str == nullptr || *str == '\0')
		return out;
	if (delimiter == nullptr || *delimiter == '\0')
	{
		out.emplace_back(str);
		return out;
	}
	const std::string_view sv(str);
	const std::string_view delim(delimiter);
	std::size_t pos = 0;
	while (pos < sv.size())
	{
		const std::size_t next = sv.find_first_not_of(delim, pos);
		if (next == std::string_view::npos)
			break;
		const std::size_t end = sv.find_first_of(delim, next);
		out.emplace_back(sv.substr(next, end - next));
		if (end == std::string_view::npos)
			break;
		pos = end;
	}
	return out;
}

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}

	void writeRel32Call(std::uint8_t* site, lm_address_t fnNext, lm_address_t target)
	{
		const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(fnNext);
		const auto rel32 = static_cast<std::int32_t>(delta);
		site[0] = 0xE8;
		std::memcpy(site + 1, &rel32, sizeof(rel32));
	}
}

int main()
{
	g_pLog = std::make_unique<CLog>("/dev/null");

	char instruction[64] = {};
	check(MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "mov", "eax", 0x401234),
	      "valid instruction is formatted");
	check(std::string(instruction) == "mov eax, 0x401234",
	      "formatted instruction preserves operands");

	char tiny[8] = {};
	check(!MemHlp::formatPICThunkInstruction(
	              tiny, sizeof(tiny), "mov", "eax", 0x401234),
	      "oversized instruction is rejected");
	check(!MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "", "eax", 0x401234),
	      "empty mnemonic is rejected");
	check(!MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "mov", "", 0x401234),
	      "empty operand is rejected");

	// Pure rel32 target resolution checks.
	check(MemHlp::resolveRel32Target(0x401005, 0x120) == static_cast<lm_address_t>(0x401125),
	      "positive rel32 resolves forward target");
	check(MemHlp::resolveRel32Target(0x401005, -0x45) == static_cast<lm_address_t>(0x400fc0),
	      "negative rel32 sign-extends and resolves backward target");
	if constexpr (sizeof(lm_address_t) > sizeof(std::int32_t))
	{
		check(MemHlp::resolveRel32Target(0x7fff00001005ULL, -0x1000) == 0x7fff00000005ULL,
		      "64-bit negative rel32 subtracts without 32-bit wrap");
		check(MemHlp::resolveRel32Target(0x1005ULL, -0x2000) == LM_ADDRESS_BAD,
		      "64-bit rel32 underflow returns LM_ADDRESS_BAD");
	}

	// Exercise MemHlp::fixPICThunkCall on an executable mmap page followed by a
	// PROT_NONE guard page so any garbage-address dereference faults immediately.
	const long rawPageSize = ::sysconf(_SC_PAGESIZE);
	const std::size_t pageSize = rawPageSize > 0 ? static_cast<std::size_t>(rawPageSize) : 4096u;
	void* mapping = ::mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
	                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(mapping != MAP_FAILED, "mmap test code region succeeds");
	if (mapping != MAP_FAILED)
	{
		auto* page = static_cast<std::uint8_t*>(mapping);
		check(::mprotect(page + pageSize, pageSize, PROT_NONE) == 0,
		      "mprotect guard page succeeds");

		const auto base = reinterpret_cast<lm_address_t>(page);

		// Forward call in trampoline with non-zero trailing bytes after rel32.
		// Previously an 8-byte read pulled those trailing bytes into the high
		// half of followAddress and crashed LM_Disassemble.
		const lm_address_t forwardTarget = base + 0x200;
		page[0x200] = 0x31; // xor eax, eax
		page[0x201] = 0xC0;
		page[0x202] = 0xC3; // ret
		const lm_address_t fnForward = base + 0x100;
		const lm_address_t trampForward = base + 0x400;
		writeRel32Call(page + 0x400, fnForward + 5, forwardTarget);
		// Non-zero bytes immediately after the 5-byte call instruction.
		const std::array<std::uint8_t, 5> trailing { 0xDE, 0xAD, 0xBE, 0xEF, 0xC3 };
		std::memcpy(page + 0x405, trailing.data(), trailing.size());
		std::array<std::uint8_t, 10> origForward {};
		std::memcpy(origForward.data(), page + 0x400, origForward.size());

		check(!MemHlp::fixPICThunkCall("forward-non-thunk", fnForward, trampForward),
		      "forward call with non-zero trailing bytes resolves safely and leaves non-thunk alone");
		check(std::memcmp(page + 0x400, origForward.data(), origForward.size()) == 0,
		      "forward non-thunk trampoline bytes are untouched");

		// Backward (negative rel32) call with non-zero trailing bytes.
		const lm_address_t backwardTarget = base + 0x080;
		page[0x080] = 0x31; // xor eax, eax
		page[0x081] = 0xC0;
		page[0x082] = 0xC3; // ret
		const lm_address_t fnBackward = base + 0x180;
		const lm_address_t trampBackward = base + 0x420;
		writeRel32Call(page + 0x420, fnBackward + 5, backwardTarget);
		page[0x425] = 0x48; // mov rbp, rsp; ret
		page[0x426] = 0x89;
		page[0x427] = 0xE5;
		page[0x428] = 0xC3;

		check(!MemHlp::fixPICThunkCall("backward-non-thunk", fnBackward, trampBackward),
		      "negative rel32 call with non-zero trailing bytes sign-extends safely");

		// Call whose computed followAddress lands in the PROT_NONE guard page.
		const lm_address_t trampGuard = base + 0x440;
		writeRel32Call(page + 0x440, fnForward + 5, base + pageSize + 0x10);
		page[0x445] = 0xC3;
		check(!MemHlp::fixPICThunkCall("guard-target", fnForward, trampGuard),
		      "call targeting PROT_NONE guard page bails out safely");

		// Call whose computed followAddress underflows (synthetic bad fn).
		const lm_address_t trampUnderflow = base + 0x460;
		const std::int32_t negDisp = -0x2000;
		page[0x460] = 0xE8;
		std::memcpy(page + 0x461, &negDisp, sizeof(negDisp));
		page[0x465] = 0xC3;
		check(!MemHlp::fixPICThunkCall("underflow-target", 0x1000, trampUnderflow),
		      "call with underflowing target bails out safely");

		// Target starts with `mov eax, eax` right at the page boundary so the
		// second follow-up disassembly step reaches the PROT_NONE guard page.
		page[pageSize - 2] = 0x89; // mov eax, eax
		page[pageSize - 1] = 0xC0;
		const lm_address_t trampBoundary = base + 0x480;
		writeRel32Call(page + 0x480, fnForward + 5, base + pageSize - 2);
		page[0x485] = 0xC3;
		check(!MemHlp::fixPICThunkCall("boundary-target", fnForward, trampBoundary),
		      "follow-up loop crossing into PROT_NONE guard page bails out safely");

		// Indirect call (FF D0 = call rax) must not be treated as E8 rel32.
		const lm_address_t trampIndirect = base + 0x4A0;
		page[0x4A0] = 0xFF;
		page[0x4A1] = 0xD0;
		page[0x4A2] = 0x90;
		page[0x4A3] = 0x90;
		page[0x4A4] = 0x90;
		page[0x4A5] = 0xC3;
		check(!MemHlp::fixPICThunkCall("indirect-call", fnForward, trampIndirect),
		      "indirect call in copied prologue is skipped safely");

		::munmap(mapping, pageSize * 2);
	}

	if (failures != 0)
	{
		std::fprintf(stderr, "test_memhlp_pic: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("memhlp PIC tests passed");
	return 0;
}
