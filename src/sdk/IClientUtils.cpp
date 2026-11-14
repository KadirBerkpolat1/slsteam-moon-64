#include "IClientUtils.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"
#include "steam.hpp"


HSteamPipe IClientUtils::getCurrentSteamPipe()
{
	//Offset found in IClientUtils::GetAppId
	// 4-byte instruction displacement, not a pointer-sized read (see
	// CSteamEngine.cpp:readDisp32).
	std::uint32_t raw = 0;
	if (LM_ReadMemory(Patterns::IClientUtils::Offset_GetPipeIndex.address + 0x2,
	                  reinterpret_cast<lm_byte_t*>(&raw), sizeof(raw)) != sizeof(raw))
	{
		return 0;
	}
	const static auto offset = static_cast<lm_address_t>(raw);
	return *reinterpret_cast<HSteamPipe*>(this + offset);
}


uint32_t IClientUtils::getAppId()
{
	return Hooks::IClientUtils_GetAppId.originalFn.fn(this);
}

IClientUtils* g_pClientUtils;
