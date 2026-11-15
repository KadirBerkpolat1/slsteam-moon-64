#include "CSteamEngine.hpp"

#include "IClientCompat.hpp"
#include "instr.hpp"
#include "CUtl.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"
#include "../vftableinfo.hpp"
#include "../feats/compatlive.hpp"

#include "libmem/libmem.h"

#include <array>
#include <atomic>

namespace
{
// Read the 4-byte displacement encoded in a `mov`/`lea` instruction.
// This is an instruction field, not a pointer: its width is fixed at 32 bits
// by the ISA regardless of the host pointer size.
//
// It is also SIGNED.  A displacement is an offset added to a base register, so
// the encoder emits it as a two's-complement value and the CPU sign-extends it
// before the add.  Several Steam members sit at negative offsets from the
// pointer we hold (IClientUtils::m_PipeIndex is disp -0xc8c on i386 and -0x1028
// on x86_64), so returning the raw value as unsigned would turn `this - 0xc8c`
// into `this + 0xfffff374` and dereference 4 GiB away.  i386 got away with the
// unsigned form only because its pointer width made the wrap-around land back
// on the same address.
inline std::int32_t readDisp32(lm_address_t at)
{
	std::int32_t value = 0;
	if (LM_ReadMemory(at, reinterpret_cast<lm_byte_t*>(&value), sizeof(value))
		!= sizeof(value))
	{
		return 0;
	}
	return value;
}

// Address of a member reached through a signed displacement.  `lm_address_t`
// is unsigned, so `base + negative_int32` converts the negative value to a
// huge positive one before the add; the sum still wraps to the right address
// in unsigned arithmetic, but doing the addition in a signed pointer type makes
// the intent explicit and keeps the intermediate from being UB.
// `T` is the member type the caller wants, which is generally NOT the type of
// `base` (getUtils starts from a CSteamEngine and returns an IClientUtils).
// Taking `base` as void* keeps the two independent.
template <typename T>
inline T* atOffset(void* base, std::int32_t offset)
{
	return reinterpret_cast<T*>(
		reinterpret_cast<std::uintptr_t>(base)
		+ static_cast<std::uintptr_t>(static_cast<std::intptr_t>(offset)));
}

bool executableAddress(lm_address_t address)
{
	lm_segment_t segment{};
	return address != 0 && address != LM_ADDRESS_BAD &&
		LM_FindSegment(address, &segment) &&
		(segment.prot & LM_PROT_XR) == LM_PROT_XR &&
		address >= segment.base && address < segment.end;
}
}

CServerPipe* CSteamEngine::getServerPipe(const HSteamPipe pipe)
{
	const static auto fn = reinterpret_cast<CServerPipe*(*)(void*, HSteamPipe)>(
		Patterns::CSteamEngine::GetServerPipe.address);
	return fn(this, pipe);
}

CUser* CSteamEngine::getUser(uint32_t index)
{
	// The instruction encodes a signed 4-byte displacement (disp32), not a
	// pointer-sized value.  Reading through lm_address_t happened to work on
	// i386 because uintptr_t was 4 bytes there; on x86_64 it would read 8 and
	// fold in the following instruction's bytes.
	const static auto offset = readDisp32(
		Patterns::CSteamEngine::Offset_User.address + kDispAfterOpcodeModrm);
	const auto vec = atOffset<const CUtlVector<CUser*>>(this, offset);
	if (!vec->memory.base || index >= vec->size)
	{
		return nullptr;
	}

	return *(&vec->memory.base[index * 2] + 1);
}

IClientUtils* CSteamEngine::getUtils()
{
	// Validate by vtable, not by getUser(0): the engine can hand us a utils
	// sub-object before any CUser exists.  A successful result is stable for the
	// process lifetime, so cache it and keep LM_FindSegment off the hot path;
	// never cache a failure, because the engine may not be built yet.
	static std::atomic<IClientUtils*> cached{nullptr};
	if (IClientUtils* const hit = cached.load(std::memory_order_acquire))
	{
		return hit;
	}

	const lm_address_t instruction =
		Patterns::CSteamEngine::Offset_ClientUtils.address;
	if (instruction == 0 || instruction == LM_ADDRESS_BAD)
		return nullptr;

	const std::int32_t offset = readDisp32(
		instruction
		#if defined(__x86_64__)
		+ kDispAfterRexOpcodeModrm
		#else
		+ kDispAfterOpcodeModrm
		#endif
	);
	auto* const utils = atOffset<IClientUtils>(this, offset);

	// The vtable pointer lives in .data.rel.ro (not executable), so check the
	// first virtual function it points at instead: THAT lands in .text.
	lm_address_t vtable = 0;
	if (LM_ReadMemory(
		reinterpret_cast<lm_address_t>(utils),
		reinterpret_cast<lm_byte_t*>(&vtable), sizeof(vtable)) !=
		sizeof(vtable) || vtable == 0 || vtable == LM_ADDRESS_BAD)
	{
		return nullptr;
	}

	lm_address_t slot0 = 0;
	if (LM_ReadMemory(vtable, reinterpret_cast<lm_byte_t*>(&slot0),
		sizeof(slot0)) != sizeof(slot0) || !executableAddress(slot0))
	{
		return nullptr;
	}

	cached.store(utils, std::memory_order_release);
	return utils;
}

void CSteamEngine::setAppIdForCurrentPipe(uint32_t appId)
{
	//Last argument needs to be 0, otherwise steam crashes.
	//Might be only 1 when steam first sets it, then 0
	Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(this, appId, 0);
}

CSteamEngine* g_pSteamEngine = nullptr;
CUser* g_pLocalUser = nullptr;

CUser* getLocalUser()
{
	// Prefer the engine-resolved user when the Init hook caught the
	// engine pointer.  Otherwise fall back to the user captured from
	// CheckAppOwnership.  Both point at the same pipe-0 CUser.
	if (g_pSteamEngine != nullptr)
	{
		CUser* user = g_pSteamEngine->getUser(0);
		if (user != nullptr)
		{
			return user;
		}
	}

	return g_pLocalUser;
}

IClientCompat* getLocalClientCompat()
{
	CUser* const user = getLocalUser();
	const lm_address_t instruction =
		Patterns::CUser::Offset_CompatManager.address;
	if (user == nullptr || instruction == 0 || instruction == LM_ADDRESS_BAD)
		return nullptr;

	std::array<std::uint8_t, 6> bytes{};
	if (LM_ReadMemory(instruction, bytes.data(), bytes.size()) != bytes.size())
		return nullptr;
	const auto offset = CompatLive::decodeManagerOffset(bytes);
	if (!offset.has_value()) return nullptr;

	auto* const compat = reinterpret_cast<IClientCompat*>(
		reinterpret_cast<std::uint8_t*>(user) + *offset);
	lm_address_t vtable = 0;
	if (LM_ReadMemory(
		reinterpret_cast<lm_address_t>(compat),
		reinterpret_cast<lm_byte_t*>(&vtable), sizeof(vtable)) !=
		sizeof(vtable) || vtable == 0 || vtable == LM_ADDRESS_BAD)
	{
		return nullptr;
	}

	for (const int index : {
		VFTIndexes::IClientCompat::SpecifyCompatTool,
		VFTIndexes::IClientCompat::GetCompatToolName})
	{
		lm_address_t target = 0;
		const lm_address_t slot = vtable +
			static_cast<lm_address_t>(index) * sizeof(lm_address_t);
		if (LM_ReadMemory(slot, reinterpret_cast<lm_byte_t*>(&target),
			sizeof(target)) != sizeof(target) ||
			!executableAddress(target))
		{
			return nullptr;
		}
	}
	return compat;
}
