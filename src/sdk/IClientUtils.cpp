#include "IClientUtils.hpp"

#include "instr.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"
#include "steam.hpp"


HSteamPipe IClientUtils::getCurrentSteamPipe()
{
	//Offset found in IClientUtils::GetAppId
	// Signed 4-byte instruction displacement, not a pointer-sized read (see
	// CSteamEngine.cpp:readDisp32).  This particular member is NEGATIVE: the
	// encoder emits -0xc8c on i386 and -0x1028 on x86_64, because the pipe
	// index lives before the IClientUtils sub-object the caller holds.
	std::int32_t raw = 0;
	if (LM_ReadMemory(Patterns::IClientUtils::Offset_GetPipeIndex.address + kDispAfterOpcodeModrm,
	                  reinterpret_cast<lm_byte_t*>(&raw), sizeof(raw)) != sizeof(raw))
	{
		return 0;
	}
	const static auto offset = raw;
	return *reinterpret_cast<HSteamPipe*>(
		reinterpret_cast<std::uintptr_t>(this)
		+ static_cast<std::uintptr_t>(static_cast<std::intptr_t>(offset)));
}


uint32_t IClientUtils::getAppId()
{
	return Hooks::IClientUtils_GetAppId.originalFn.fn(this);
}

IClientUtils* g_pClientUtils;
