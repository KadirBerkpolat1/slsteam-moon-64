// SPDX-License-Identifier: AGPL-3.0-only
// Pure policy and instruction decoder for live Steam compatibility mappings.
#pragma once

#include "../sdk/IClientCompat.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace CompatLive
{
inline constexpr int kMappingPriority = 250;
inline constexpr std::size_t kMaxManagerOffset = 1024 * 1024;
inline constexpr std::uint32_t kMaxPollAttempts = 20;

enum class StepStatus : std::uint8_t
{
	Unavailable,
	Ready,
	Requested,
	Waiting,
};

struct StepResult
{
	StepStatus status = StepStatus::Unavailable;
};

// Decodes the `lea ecx,[esi+disp32]` (8D 8E) that the CUser constructor uses
// to pass its embedded CCompatManager.  Only this form is accepted: the older
// `lea ebx` (8D 9E) site belongs to clients whose IClientCompat vtable had
// three more methods, so the current VFTIndexes must never reach that layout.
inline std::optional<std::size_t> decodeManagerOffset(
	std::span<const std::uint8_t> instruction) noexcept
{
	if (instruction.size() < 6 || instruction[0] != 0x8d ||
		instruction[1] != 0x8e)
	{
		return std::nullopt;
	}
	const std::uint32_t offset =
		static_cast<std::uint32_t>(instruction[2]) |
		(static_cast<std::uint32_t>(instruction[3]) << 8) |
		(static_cast<std::uint32_t>(instruction[4]) << 16) |
		(static_cast<std::uint32_t>(instruction[5]) << 24);
	if (offset == 0 || offset > kMaxManagerOffset)
		return std::nullopt;
	return static_cast<std::size_t>(offset);
}

// Decodes the 7-byte x86-64 `lea r64,[base+disp32]` (REX.W 8D mod=10 rm!=100)
// that CUser::CUser uses to pass its embedded CCompatManager (e.g.
// `48 8D 83 58 4A 00 00` = `lea rax,[rbx+0x4a58]`).
inline std::optional<std::size_t> decodeManagerOffset64(
	std::span<const std::uint8_t> instruction) noexcept
{
	if (instruction.size() < 7)
		return std::nullopt;
	const std::uint8_t rex = instruction[0];
	if ((rex & 0xFA) != 0x48 || instruction[1] != 0x8D)
		return std::nullopt;
	const std::uint8_t modrm = instruction[2];
	const std::uint8_t mod = modrm >> 6;
	const std::uint8_t rm = modrm & 0x7;
	if (mod != 0b10 || rm == 0b100)
		return std::nullopt;
	const std::uint32_t raw =
		static_cast<std::uint32_t>(instruction[3]) |
		(static_cast<std::uint32_t>(instruction[4]) << 8) |
		(static_cast<std::uint32_t>(instruction[5]) << 16) |
		(static_cast<std::uint32_t>(instruction[6]) << 24);
	const auto disp = static_cast<std::int32_t>(raw);
	if (disp <= 0 || static_cast<std::size_t>(disp) > kMaxManagerOffset)
		return std::nullopt;
	return static_cast<std::size_t>(disp);
}

inline StepResult step(
	IClientCompat* compat,
	std::uint32_t appId,
	bool requestAlreadySubmitted) noexcept
{
	if (compat == nullptr || appId == 0)
		return {StepStatus::Unavailable};
	try
	{
		const char* current = compat->getCompatToolName(appId);
		if (current != nullptr && current[0] != '\0')
			return {StepStatus::Ready};
		if (requestAlreadySubmitted)
			return {StepStatus::Waiting};

		const char* selected = compat->getCompatToolName(0);
		if (selected == nullptr || selected[0] == '\0')
			selected = "proton_experimental";
		compat->specifyCompatTool(appId, selected, "", kMappingPriority);
		current = compat->getCompatToolName(appId);
		if (current != nullptr && current[0] != '\0')
			return {StepStatus::Ready};
		return {StepStatus::Requested};
	}
	catch (...)
	{
		return {StepStatus::Unavailable};
	}
}
}
