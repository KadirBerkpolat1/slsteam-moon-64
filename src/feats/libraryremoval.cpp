// SPDX-License-Identifier: AGPL-3.0-only

#include "libraryremoval.hpp"

#include "hotreload_inputs.hpp"
#include "hotreload_capabilities.hpp"
#include "libraryremoval_policy.hpp"

#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace
{
// x86-32 has more than one calling convention; x86-64 has exactly one, and GCC
// warns that the attribute is ignored there.  Normalise it to a single name so
// the detour signatures stay identical to the target functions on both.
#if defined(__x86_64__)
#define SLS_CDECL
#else
#define SLS_CDECL __attribute__((cdecl))
#endif

using GetAppByIDFn = void* (SLS_CDECL *)(
	void*, std::uint32_t, bool);
using MarkAppChangeFn = void (SLS_CDECL *)(
	void*, std::uint32_t, std::uint32_t);
using RunFrameFn = void* (SLS_CDECL *)(void*);

template<typename Fn>
struct Detour
{
	Fn original = nullptr;
	lm_address_t target = LM_ADDRESS_BAD;
	lm_address_t trampoline = LM_ADDRESS_BAD;
	lm_size_t size = 0;
};

constexpr std::uint32_t kAppInfoOrConfig = 0x0002;

LibraryRemovalPolicy::Queue g_queue(HotReloadInputs::kMaxSnapshotIds);
std::atomic<bool> g_ready{false};
std::atomic<void*> g_appChangeSource{nullptr};
GetAppByIDFn g_getAppByID = nullptr;
std::size_t g_ownershipFlagsOffset = 0;
Detour<MarkAppChangeFn> g_markAppChange;
Detour<RunFrameFn> g_runFrame;

// The ownership-flag offset is recovered from the instruction FillInAppOverview
// uses to read it (`8B /r` mod=01 on i386, `[REX] 8B /r` into edx on x86-64),
// never from a layout constant: CSteamApp widens with the pointer size (0x18 on
// i386, 0x1c on x86-64).  The detours only touch that one field and call Steam
// through its own entries, so no Steam protobuf layout is assumed on either.
//
// i386 entries open with a get_pc_thunk call that the trampoline must repair;
// x86-64 code is RIP-relative and has no such thunk, so repair is skipped there
// (fixPICThunkCall reports "no thunk" as failure).
#if defined(__x86_64__)
constexpr bool kRepairPicThunk = false;
#else
constexpr bool kRepairPicThunk = true;
#endif

template<typename Fn>
void uninstall(Detour<Fn>& detour) noexcept
{
	if (detour.size != 0 && detour.target != LM_ADDRESS_BAD &&
		detour.trampoline != LM_ADDRESS_BAD)
	{
		LM_UnhookCode(detour.target, detour.trampoline, detour.size);
	}
	detour = {};
}

template<typename Fn>
bool install(
	Detour<Fn>& detour,
	const Pattern_t& pattern,
	lm_address_t hook,
	bool repairPicThunk)
{
	detour.target = pattern.address;
	detour.size = LM_HookCode(
		detour.target, hook, &detour.trampoline);
	if (detour.size == 0 || detour.trampoline == LM_ADDRESS_BAD)
	{
		detour = {};
		return false;
	}

	detour.original = reinterpret_cast<Fn>(detour.trampoline);
	if (repairPicThunk &&
		!MemHlp::fixPICThunkCall(
			pattern.name.c_str(), detour.target, detour.trampoline))
	{
		uninstall(detour);
		return false;
	}
	return true;
}

void processOneRemoval(void* controller)
{
	const auto appId = g_queue.drainOne();
	if (!appId)
		return;

	void* const source = g_appChangeSource.load(std::memory_order_acquire);
	const MarkAppChangeFn mark = g_markAppChange.original;
	if (controller == nullptr || source == nullptr || g_getAppByID == nullptr ||
		mark == nullptr)
	{
		g_queue.retry(*appId);
		return;
	}

	if (!g_queue.begin(*appId))
		return;
	struct FinishGuard
	{
		LibraryRemovalPolicy::Queue& queue;
		LibraryRemovalPolicy::Work work;
		~FinishGuard() { queue.finish(work); }
	} finishGuard{g_queue, *appId};

	if (appId->action == LibraryRemovalPolicy::Action::Remove)
	{
		if (void* const app = g_getAppByID(controller, appId->appId, false))
		{
			auto* const flags = reinterpret_cast<std::uint32_t*>(
				static_cast<std::uint8_t*>(app) + g_ownershipFlagsOffset);
			*flags = LibraryRemovalPolicy::hiddenOwnershipFlags(*flags);
		}
	}

	// MarkAppChange is invoked through its original trampoline. This avoids a
	// recursive capture. Queue::begin already published the effective state, so
	// a synchronous complete-change callback observes the right removed list.
	mark(source, appId->appId, kAppInfoOrConfig);

	if (g_pLog != nullptr)
		g_pLog->info(
			"LibraryRemoval: app=%u %s published to the live library\n",
			appId->appId,
			appId->action == LibraryRemovalPolicy::Action::Remove
				? "removal" : "restoration");
}

void SLS_CDECL hkMarkAppChange(
	void* source,
	std::uint32_t appId,
	std::uint32_t flags)
{
	g_appChangeSource.store(source, std::memory_order_release);
	g_markAppChange.original(source, appId, flags);
}

void* SLS_CDECL hkRunFrame(void* controller)
{
	if (g_ready.load(std::memory_order_acquire))
	{
		try
		{
			processOneRemoval(controller);
		}
		catch (...)
		{
			if (g_pLog != nullptr)
				g_pLog->warn("LibraryRemoval: UI queue drain failed; retry deferred\n");
		}
	}

	// The original UI frame is called exactly once on every hook path.
	return g_runFrame.original(controller);
}

bool deriveOwnershipOffset(std::size_t& offset) noexcept
{
#if defined(__x86_64__)
	std::array<std::uint8_t, 4> instruction{};
#else
	std::array<std::uint8_t, 3> instruction{};
#endif
	if (LM_ReadMemory(
		Patterns::SteamUI::OwnershipFlagsReference.address,
		instruction.data(), instruction.size()) != instruction.size())
	{
		return false;
	}

	const std::span<const std::uint8_t> bytes(instruction);
#if defined(__x86_64__)
	const auto decoded = LibraryRemovalPolicy::deriveOwnershipOffset64(bytes);
	if (!decoded)
		return false;
	offset = decoded->offset;
#else
	const auto decoded = LibraryRemovalPolicy::deriveOwnershipOffset(bytes);
	if (!decoded)
		return false;
	offset = *decoded;
#endif
	return true;
}

void rollback() noexcept
{
	g_ready.store(false, std::memory_order_release);
	g_queue.close();
	uninstall(g_runFrame);
	uninstall(g_markAppChange);
	g_getAppByID = nullptr;
	g_ownershipFlagsOffset = 0;
	g_appChangeSource.store(nullptr, std::memory_order_release);
}
} // namespace

namespace LibraryRemoval
{
bool setup() noexcept
{
	try
	{
		if (g_ready.load(std::memory_order_acquire))
			return true;
		g_queue.reopen();
		g_ownershipFlagsOffset = 0;

		HotReloadCapabilities::Matrix capabilities;
		capabilities.uiRunFrame =
			Patterns::SteamUI::AppControllerRunFrame.address != LM_ADDRESS_BAD;
		capabilities.uiGetAppById =
			Patterns::SteamUI::GetAppByID.address != LM_ADDRESS_BAD;
		capabilities.uiMarkAppChange =
			Patterns::SteamUI::MarkAppChange.address != LM_ADDRESS_BAD;
		capabilities.uiOwnershipLayout =
			Patterns::SteamUI::OwnershipFlagsReference.address != LM_ADDRESS_BAD &&
			deriveOwnershipOffset(g_ownershipFlagsOffset);

		if (!capabilities.canRemoveVisually())
		{
			g_queue.close();
			if (g_pLog != nullptr)
				g_pLog->once(
					"LibraryRemoval: optional SteamUI capability unavailable; "
					"visual removals will complete after a natural refresh or restart\n");
			return false;
		}

		g_getAppByID = reinterpret_cast<GetAppByIDFn>(
			Patterns::SteamUI::GetAppByID.address);

		// RunFrame is installed last, so no queued work can execute until both
		// support detours have valid original trampolines.
		if (!install(g_markAppChange, Patterns::SteamUI::MarkAppChange,
				reinterpret_cast<lm_address_t>(&hkMarkAppChange), kRepairPicThunk) ||
			!install(g_runFrame, Patterns::SteamUI::AppControllerRunFrame,
				reinterpret_cast<lm_address_t>(&hkRunFrame), kRepairPicThunk))
		{
			rollback();
			if (g_pLog != nullptr)
				g_pLog->once(
					"LibraryRemoval: optional SteamUI hooks could not be installed; "
					"visual removals will complete after a natural refresh or restart\n");
			return false;
		}

		g_ready.store(true, std::memory_order_release);
		if (g_pLog != nullptr)
			g_pLog->info(
				"LibraryRemoval: SteamUI queue ready (ownership offset=0x%zx)\n",
				g_ownershipFlagsOffset);
		return true;
	}
	catch (...)
	{
		rollback();
		if (g_pLog != nullptr)
			g_pLog->once(
				"LibraryRemoval: optional SteamUI setup failed; visual removals "
				"will complete after a natural refresh or restart\n");
		return false;
	}
}

void remove() noexcept
{
	rollback();
}

bool ready() noexcept
{
	return g_ready.load(std::memory_order_acquire);
}

void queue(std::uint32_t appId) noexcept
{
	if (!ready())
		return;
	try
	{
		if (!g_queue.push(appId) && g_pLog != nullptr &&
			!g_queue.desiredRemoved(appId))
		{
			g_pLog->warn(
				"LibraryRemoval: removal queue full; app=%u deferred to restart\n",
				appId);
		}
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"LibraryRemoval: could not queue app=%u; removal deferred to restart\n",
				appId);
	}
}

void cancel(std::uint32_t appId) noexcept
{
	try
	{
		g_queue.cancel(appId);
	}
	catch (...)
	{
		// Cancellation is allocation-free in normal operation. The package and
		// license refresh still makes the newly active app authoritative.
	}
}

void restore(std::uint32_t appId) noexcept
{
	try
	{
		g_queue.readyToRestore(appId);
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn(
				"LibraryRemoval: could not restore app=%u presentation; "
				"natural refresh remains available\n",
				appId);
	}
}

} // namespace LibraryRemoval
