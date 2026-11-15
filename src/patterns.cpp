#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"
#include "feats/ipcframe.hpp"
#include "pattern_catalog.hpp"
#include "pattern_cache.hpp"
#include "runtime_attestation.hpp"
#include "config.hpp"
#include "utils.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

#include <sys/stat.h>


// The IClient*::RunIPCFrame dispatchers all open with the same saved-register
// chain.  PrologueUpwards searches BACKWARDS from the match, so the vector is
// stored in reverse memory order: i386 is push ebp / mov ebp,esp / push esi /
// push edi, read backwards.  x86-64 saves rbp plus the r12-r15 run instead.
#if defined(__x86_64__)
static const std::vector<int16_t> kIpcPrologue { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41, 0xE5, 0x89, 0x48, 0x55 };
#else
static const std::vector<int16_t> kIpcPrologue { 0x56, 0x57, 0xe5, 0x89, 0x55 };
#endif

namespace
{
	std::optional<PatternCatalog::Catalog> g_steamClientCatalog;
	std::optional<PatternCatalog::Catalog> g_steamUiCatalog;

	struct LocalCatalogState
	{
		std::optional<PatternCache::ModuleIdentity> identity;
		std::optional<PatternCache::Catalog> catalog;
		bool writeEligible = false;
		bool dirty = false;
	};

	LocalCatalogState g_steamClientLocal;
	LocalCatalogState g_steamUiLocal;

	std::optional<std::filesystem::path> patternRoot();

	bool localCacheEnabled()
	{
		return PatternCache::enabled(
			g_config.patternCache.get(), std::getenv("SLSSTEAM_PATTERN_CACHE")
		);
	}

	std::optional<PatternCache::ModuleIdentity> moduleIdentity(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		struct stat value {};
		if (stat(module.path, &value) != 0 || !S_ISREG(value.st_mode) || value.st_size <= 0)
			return std::nullopt;
		const std::string buildId = Utils::getBuildId(module.path);
		if (buildId.empty())
			return std::nullopt;
		return PatternCache::ModuleIdentity
		{
			component,
			moduleName,
			buildId,
			static_cast<std::uint64_t>(value.st_size),
			static_cast<std::int64_t>(value.st_mtim.tv_sec),
			static_cast<std::int64_t>(value.st_mtim.tv_nsec),
		};
	}

	std::filesystem::path localCatalogPath(
		const std::filesystem::path& root,
		const PatternCache::ModuleIdentity& identity
	)
	{
		const std::string key = identity.gnuBuildId + "-"
			+ std::to_string(identity.size) + "-"
			+ std::to_string(identity.mtimeSeconds) + "-"
			+ std::to_string(identity.mtimeNanoseconds) + ".cache";
		return root / "local" / identity.component / key;
	}

	LocalCatalogState loadLocalCatalog(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		LocalCatalogState state;
		if (!localCacheEnabled())
			return state;
		state.identity = moduleIdentity(component, moduleName, module);
		const auto root = patternRoot();
		if (!state.identity || !root)
			return state;

		std::string error;
		state.catalog = PatternCache::load(
			localCatalogPath(*root, *state.identity), *state.identity, &error
		);
		if (!state.catalog)
			state.writeEligible = true;
		return state;
	}

	LocalCatalogState& localStateFor(const Pattern_t& pattern)
	{
		return pattern.module == &g_modSteamUI ? g_steamUiLocal : g_steamClientLocal;
	}

	const PatternCatalog::Catalog* catalogFor(const Pattern_t& pattern)
	{
		const auto& selected = pattern.module == &g_modSteamUI
			? g_steamUiCatalog
			: g_steamClientCatalog;
		return selected ? &*selected : nullptr;
	}

	const char* followModeName(MemHlp::SigFollowMode mode)
	{
		switch (mode)
		{
			case MemHlp::SigFollowMode::Relative: return "Relative";
			case MemHlp::SigFollowMode::PrologueUpwards: return "PrologueUpwards";
			default: return "None";
		}
	}

	std::optional<lm_address_t> localCatalogAddress(
		const Pattern_t& pattern,
		bool logInvalid,
		lm_address_t* matchAddressOut
	)
	{
		LocalCatalogState& state = localStateFor(pattern);
		if (!state.catalog)
			return std::nullopt;

		const auto reject = [&](const char* message) -> std::optional<lm_address_t>
		{
			// The structural IPC-root probe runs before the embedded signature is
			// normalized.  A failed non-logging probe is therefore provisional:
			// let autoResolveIpcFrameRoots adjust the root and recheck the local
			// catalog during the definitive Pattern_t::find() pass.  Normal
			// resolving (logInvalid=true) remains transactional.
			if (logInvalid)
			{
				state.catalog.reset();
				state.writeEligible = true;
				state.dirty = true;
			}
			if (logInvalid)
				g_pLog->warn("Local pattern cache for '%s' rejected: %s; using embedded resolver\n",
				             pattern.name.c_str(), message);
			return std::nullopt;
		};
		const PatternCache::Locator* entry = state.catalog->entry(pattern.symbol);
		if (entry == nullptr)
		{
			// Optional patterns may legitimately be absent from a catalog when
			// they were unresolved during the cold pass.  Keep the required
			// cached hits usable and let this optional feature fall back alone.
			if (pattern.optional)
				return std::nullopt;
			return reject("locator is missing from the complete cache policy");
		}
		if (entry->required != !pattern.optional)
			return reject("compiled policy mismatch");
		if (entry->followMode != followModeName(pattern.followMode)
		    || entry->signature != pattern.pattern)
			return reject("compiled resolver metadata mismatch");

		const lm_module_t& module = pattern.module ? *pattern.module : g_modSteamClient;
		if (entry->targetRva >= module.size
		    || entry->targetRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return reject("target RVA is outside the module");
		const lm_address_t candidate = module.base
			+ static_cast<lm_address_t>(entry->targetRva);
		lm_segment_t targetSegment {};
		const bool targetInside = candidate >= module.base && candidate < module.end;
		const bool targetExecutable = targetInside && LM_FindSegment(candidate, &targetSegment)
			&& (targetSegment.prot & LM_PROT_XR) == LM_PROT_XR;
		if (!targetExecutable)
			return reject("target RVA is not executable");

		const auto signatureBytes = PatternCache::signatureSize(entry->signature);
		if (!signatureBytes || entry->matchRva >= module.size
		    || *signatureBytes > module.size - entry->matchRva
		    || entry->matchRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return reject("match RVA is outside the module");
		const lm_address_t match = module.base
			+ static_cast<lm_address_t>(entry->matchRva);
		lm_segment_t matchSegment {};
		if (!LM_FindSegment(match, &matchSegment)
		    || (matchSegment.prot & LM_PROT_XR) != LM_PROT_XR
		    || match < matchSegment.base
		    || match >= matchSegment.end
		    || matchSegment.end - match < *signatureBytes)
			return reject("signature range is not executable");
		if (!PatternCache::signatureMatches(
			entry->signature,
			std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(match), *signatureBytes
			)))
			return reject("signature bytes changed");

		lm_address_t derivedTarget = match;
		switch (pattern.followMode)
		{
			case MemHlp::SigFollowMode::Relative:
				derivedTarget = MemHlp::getJmpTarget(match);
				break;
			case MemHlp::SigFollowMode::PrologueUpwards:
				derivedTarget = MemHlp::findPrologue(
					match,
					MemHlp::prologueLowerBound(module.base, matchSegment.base),
					pattern.prologue.empty() ? nullptr : pattern.prologue.data(),
					pattern.prologue.size()
				);
				break;
			case MemHlp::SigFollowMode::None:
				break;
		}
		if (derivedTarget == LM_ADDRESS_BAD
		    || derivedTarget < module.base || derivedTarget >= module.end)
			return reject("followed target is outside the module");
		const auto derivedTargetRva = static_cast<std::uint64_t>(
			derivedTarget - module.base
		);
		if (!PatternCache::targetRvaMatches(
			entry->followMode, entry->matchRva, entry->targetRva,
			derivedTargetRva
		))
			return reject("target RVA does not match the matched signature");

		if (matchAddressOut != nullptr)
			*matchAddressOut = match;
		return candidate;
	}

	std::optional<lm_address_t> catalogAddress(
		const Pattern_t& pattern,
		bool logInvalid,
		lm_address_t* matchAddressOut = nullptr
	)
	{
		if (const auto local = localCatalogAddress(pattern, logInvalid, matchAddressOut))
			return local;
		const PatternCatalog::Catalog* catalog = catalogFor(pattern);
		if (catalog == nullptr)
			return std::nullopt;
		const PatternCatalog::Locator* entry = catalog->entry(pattern.symbol);
		if (entry == nullptr)
			return std::nullopt;
		if (entry->required != !pattern.optional)
		{
			if (logInvalid)
				g_pLog->warn("Pattern catalog policy mismatch for '%s'; using embedded resolver\n",
				             pattern.name.c_str());
			return std::nullopt;
		}
		if (entry->signature != pattern.pattern ||
			entry->followMode != followModeName(pattern.followMode))
		{
			if (logInvalid)
				g_pLog->warn(
					"Pattern catalog resolver mismatch for '%s'; using embedded resolver\n",
					pattern.name.c_str());
			return std::nullopt;
		}

		const lm_module_t& module = pattern.module ? *pattern.module : g_modSteamClient;
		const lm_address_t rva = static_cast<lm_address_t>(entry->targetRva);
		if (rva >= module.size || module.base > LM_ADDRESS_BAD - rva)
			return std::nullopt;
		const lm_address_t candidate = module.base + rva;
		const bool inside = candidate >= module.base && candidate < module.end;
		lm_segment_t segment {};
		const bool executable = inside && LM_FindSegment(candidate, &segment)
		                     && (segment.prot & LM_PROT_XR) == LM_PROT_XR;
		if (!inside || !executable)
		{
			if (logInvalid)
				g_pLog->warn("Pattern catalog RVA for '%s' is not executable; using embedded resolver\n",
				             pattern.name.c_str());
			return std::nullopt;
		}
		return candidate;
	}

	std::optional<std::filesystem::path> patternRoot()
	{
		if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0')
		{
			std::filesystem::path root(xdg);
			if (root.is_absolute())
				return root / "SLSsteam" / "patterns";
			return std::nullopt;
		}
		if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
		{
			std::filesystem::path root(home);
			if (root.is_absolute())
				return root / ".config" / "SLSsteam" / "patterns";
		}
		return std::nullopt;
	}

	std::optional<PatternCatalog::Catalog> loadCatalog(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		const auto root = patternRoot();
		if (!root)
			return std::nullopt;
		const std::filesystem::path componentRoot = *root / component;
		std::error_code error;
		if (!std::filesystem::is_directory(componentRoot, error) || error)
			return std::nullopt;

		try
		{
			const std::string sha256 = Utils::getFileSHA256(module.path);
			const std::string buildId = Utils::getBuildId(module.path);
			if (sha256.size() != 64 || buildId.empty())
				return std::nullopt;
			const auto moduleSize = std::filesystem::file_size(module.path, error);
			if (error || moduleSize == 0)
				return std::nullopt;
			const std::filesystem::path path = componentRoot / (sha256 + ".toml");
			const auto status = std::filesystem::symlink_status(path, error);
			if (error || status.type() != std::filesystem::file_type::regular)
				return std::nullopt;
			const auto bodySize = std::filesystem::file_size(path, error);
			if (error || bodySize == 0 || bodySize > PatternCatalog::kMaximumBodySize)
				return std::nullopt;
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return std::nullopt;
			std::string body(static_cast<std::size_t>(bodySize), '\0');
			stream.read(body.data(), static_cast<std::streamsize>(body.size()));
			if (!stream || stream.gcount() != static_cast<std::streamsize>(body.size()))
				return std::nullopt;

			std::string parseError;
			auto catalog = PatternCatalog::parseCanonical(
				body,
				{
					component,
					moduleName,
					sha256,
					moduleSize,
					buildId,
				},
				&parseError
			);
			if (!catalog)
			{
				g_pLog->warn("Pattern catalog for %s rejected: %s\n",
				             component, parseError.c_str());
				return std::nullopt;
			}
			return catalog;
		}
		catch (const std::exception&)
		{
			g_pLog->warn("Pattern catalog for %s could not be read; using embedded resolvers\n",
			             component);
			return std::nullopt;
		}
	}

	std::vector<PatternCatalog::CompiledLocator> compiledPolicy(bool steamUi)
	{
		std::vector<PatternCatalog::CompiledLocator> result;
		for (const Pattern_t* pattern : Patterns::patterns())
		{
			if ((pattern->module == &g_modSteamUI) != steamUi)
				continue;
			result.push_back({
				pattern->symbol,
				!pattern->optional,
				pattern->pattern,
				followModeName(pattern->followMode),
			});
		}
		return result;
	}

	std::vector<PatternCache::CompiledLocator> compiledLocalPolicy(bool steamUi)
	{
		std::vector<PatternCache::CompiledLocator> result;
		for (const Pattern_t* pattern : Patterns::patterns())
		{
			if ((pattern->module == &g_modSteamUI) != steamUi)
				continue;
			result.push_back({
				pattern->symbol,
				!pattern->optional,
				pattern->pattern,
				followModeName(pattern->followMode),
			});
		}
		return result;
	}

	void loadActiveCatalogs()
	{
		g_steamClientLocal = loadLocalCatalog(
			"steamclient", "steamclient.so", g_modSteamClient
		);
		g_steamUiLocal = loadLocalCatalog("steamui", "steamui.so", g_modSteamUI);
		g_steamClientCatalog.reset();
		g_steamUiCatalog.reset();

		const auto clientPolicy = compiledPolicy(false);
		const auto localClientPolicy = compiledLocalPolicy(false);
		if (g_steamClientLocal.catalog
		    && !PatternCache::policyMatches(g_steamClientLocal.catalog.value(), localClientPolicy))
		{
			g_pLog->warn("Local pattern cache for steamclient changed compiled locator policy; ignoring it\n");
			g_steamClientLocal.catalog.reset();
			g_steamClientLocal.writeEligible = true;
			g_steamClientLocal.dirty = true;
		}
		const auto uiPolicy = compiledPolicy(true);
		const auto localUiPolicy = compiledLocalPolicy(true);
		if (g_steamUiLocal.catalog
		    && !PatternCache::policyMatches(g_steamUiLocal.catalog.value(), localUiPolicy))
		{
			g_pLog->warn("Local pattern cache for steamui changed compiled locator policy; ignoring it\n");
			g_steamUiLocal.catalog.reset();
			g_steamUiLocal.writeEligible = true;
			g_steamUiLocal.dirty = true;
		}

		// The local catalog is attempted before SHA-256 attestation.  If it is
		// absent or disabled, retain the existing signed remote catalog path;
		// otherwise a warm boot does no full-module hashing at all.
		if (!g_steamClientLocal.catalog)
			g_steamClientCatalog = loadCatalog("steamclient", "steamclient.so", g_modSteamClient);
		if (!g_steamUiLocal.catalog)
			g_steamUiCatalog = loadCatalog("steamui", "steamui.so", g_modSteamUI);

		if (g_steamClientCatalog && !g_steamClientCatalog->validatePolicy(clientPolicy))
		{
			g_pLog->warn("Pattern catalog for steamclient changed compiled locator policy; ignoring it\n");
			g_steamClientCatalog.reset();
		}
		if (g_steamUiCatalog && !g_steamUiCatalog->validatePolicy(uiPolicy))
		{
			g_pLog->warn("Pattern catalog for steamui changed compiled locator policy; ignoring it\n");
			g_steamUiCatalog.reset();
		}
		if (g_steamClientLocal.catalog)
			g_pLog->info("Pattern cache: loaded steamclient local catalog\n");
		if (g_steamUiLocal.catalog)
			g_pLog->info("Pattern cache: loaded steamui local catalog\n");
		if (g_steamClientCatalog)
			g_pLog->info("Pattern catalog: loaded steamclient revision %llu\n",
			             static_cast<unsigned long long>(g_steamClientCatalog->revision()));
		if (g_steamUiCatalog)
			g_pLog->info("Pattern catalog: loaded steamui revision %llu\n",
			             static_cast<unsigned long long>(g_steamUiCatalog->revision()));
	}

	std::optional<PatternCache::Catalog> buildLocalCatalog(bool steamUi)
	{
		LocalCatalogState& state = steamUi ? g_steamUiLocal : g_steamClientLocal;
		if (!state.identity)
			return std::nullopt;
		const lm_module_t& module = steamUi ? g_modSteamUI : g_modSteamClient;
		PatternCache::Catalog result {*state.identity, {}};
		for (const Pattern_t* pattern : Patterns::patterns())
		{
			if ((pattern->module == &g_modSteamUI) != steamUi)
				continue;
			if (pattern->address == LM_ADDRESS_BAD || pattern->matchAddress == LM_ADDRESS_BAD)
			{
				if (!pattern->optional)
					return std::nullopt;
				continue;
			}
			if (pattern->address < module.base || pattern->matchAddress < module.base
			    || pattern->address >= module.end || pattern->matchAddress >= module.end)
				return std::nullopt;
			const lm_address_t targetRva = pattern->address - module.base;
			const lm_address_t matchRva = pattern->matchAddress - module.base;
			if (targetRva >= module.size || matchRva >= module.size)
				return std::nullopt;
			result.locators.push_back(
				{
					pattern->symbol,
					static_cast<std::uint64_t>(targetRva),
					static_cast<std::uint64_t>(matchRva),
					pattern->pattern,
					followModeName(pattern->followMode),
					!pattern->optional,
				}
			);
		}
		return result.locators.empty() ? std::nullopt : std::optional<PatternCache::Catalog>(std::move(result));
	}

	void writeLocalCatalog(bool steamUi)
	{
		LocalCatalogState& state = steamUi ? g_steamUiLocal : g_steamClientLocal;
		if ((!state.writeEligible && !state.dirty)
		    || (steamUi ? g_steamUiCatalog.has_value() : g_steamClientCatalog.has_value()))
			return;
		const auto root = patternRoot();
		const auto catalog = buildLocalCatalog(steamUi);
		if (!root || !catalog)
			return;
		const std::string body = PatternCache::serialize(*catalog);
		if (body.empty())
			return;
		if (PatternCache::writeAtomic(
			localCatalogPath(*root, catalog->identity), body
		))
		{
			g_pLog->info("Pattern cache: wrote %s local catalog (%zu locators)\n",
			             steamUi ? "steamui" : "steamclient", catalog->locators.size());
			state.writeEligible = false;
			state.dirty = false;
		}
	}

	void writeLocalCatalogs()
	{
		writeLocalCatalog(false);
		writeLocalCatalog(true);
	}
}


Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, lm_module_t* module, const char* symbol)
	:
	Pattern_t(name, pattern, followMode, std::vector<int16_t>(), module, symbol)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue,
	lm_module_t* module, const char* symbol)
	:
	name(name),
	symbol(symbol != nullptr ? symbol : std::string("Patterns::") + name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue.begin(), prologue.end()),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, std::vector<int16_t> prologue,
	lm_module_t* module, const char* symbol)
	:
	name(name),
	symbol(symbol != nullptr ? symbol : std::string("Patterns::") + name),
	pattern(pattern),
	followMode(followMode),
	prologue(std::move(prologue)),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

bool Pattern_t::find()
{
	lm_module_t& targetModule = module ? *module : g_modSteamClient;
	matchAddress = LM_ADDRESS_BAD;
	lm_address_t cachedMatch = LM_ADDRESS_BAD;
	if (const auto trusted = catalogAddress(*this, true, &cachedMatch))
	{
		address = *trusted;
		matchAddress = cachedMatch;
	}
	else
	{
		const auto resolved = MemHlp::searchSignatureDetailed
		(
			name.c_str(), pattern.c_str(), targetModule, followMode,
			prologue.empty() ? nullptr : prologue.data(), prologue.size()
		);
		matchAddress = resolved.match;
		address = resolved.target;

		// The hooked address is usually NOT the matched one: Relative follows a
		// call/jmp operand and PrologueUpwards walks backwards, so the derived
		// address carries none of the scan's guarantees.  Prove it lands in this
		// module's executable memory before anyone detours it.  The cache and
		// catalog paths already do this on their own inputs; this closes the
		// embedded resolver, which is the path every unknown client build takes.
		if (address != LM_ADDRESS_BAD)
		{
			lm_segment_t targetSegment {};
			const bool inside = address >= targetModule.base
			                 && address < targetModule.base + targetModule.size;
			const bool executable = LM_FindSegment(address, &targetSegment)
			                     && (targetSegment.prot & LM_PROT_XR) == LM_PROT_XR;
			if (!inside || !executable)
			{
				g_pLog->warn(
					"Resolved target for '%s' is not executable module memory; "
					"refusing to use it\n", name.c_str());
				address = LM_ADDRESS_BAD;
				matchAddress = LM_ADDRESS_BAD;
			}
		}
	}
	const bool resolved = address != LM_ADDRESS_BAD;

	if (RuntimeAttestation::enabled())
	{
		const char* moduleName = module == &g_modSteamUI ? "steamui" : "steamclient";
		if (!resolved)
		{
			RuntimeAttestation::emit
			(
				"locator-missing",
				{
					RuntimeAttestation::Field::text("locator", name),
					RuntimeAttestation::Field::text("module", moduleName),
					RuntimeAttestation::Field::boolean("optional", optional),
				}
			);
		}
		else
		{
			lm_segment_t segment {};
			const bool executable = LM_FindSegment(address, &segment)
			                     && (segment.prot & LM_PROT_XR) == LM_PROT_XR;
			const bool insideModule = address >= targetModule.base
			                       && address < targetModule.base + targetModule.size;
			RuntimeAttestation::emit
			(
				"locator-resolved",
				{
					RuntimeAttestation::Field::text("locator", name),
					RuntimeAttestation::Field::text("module", moduleName),
					RuntimeAttestation::Field::number
					(
						"target_rva", insideModule ? address - targetModule.base : 0
					),
					RuntimeAttestation::Field::boolean("inside_module", insideModule),
					RuntimeAttestation::Field::boolean("executable", executable),
				}
			);
		}
	}

	return resolved;
}

// Re-derive the volatile dispatch-tree root of every IClient*::RunIPCFrame in
// one pass over the steamclient module's executable segments, then point each
// pattern's seed at the nearest live root.  Pure decision logic lives in
// feats/ipcframe.hpp (host-unit-tested); this only walks live memory.
static void autoResolveIpcFrameRoots()
{
	const std::array<Pattern_t*, 6> targets =
	{
		&Patterns::IClientApps::RunIPCFrame,
		&Patterns::IClientAppManager::RunIPCFrame,
		&Patterns::IClientRemoteStorage::RunIPCFrame,
		&Patterns::IClientUGC::RunIPCFrame,
		&Patterns::IClientUserStats::RunIPCFrame,
		&Patterns::IClientUser::RunIPCFrame,
	};
	std::array<bool, targets.size()> catalogResolved {};
	for (size_t i = 0; i < targets.size(); ++i)
		catalogResolved[i] = catalogAddress(*targets[i], false).has_value();

	struct Ctx
	{
		std::vector<IpcFrame::Cand> cands;
	} ctx;

	// Collect candidates from executable segments belonging to steamclient.
	// IpcFrame::scan bounds every read by the segment size, so this never
	// touches a guard page (same safety contract as MemHlp::patternScan).
	const auto enumSegments = [](lm_segment_t* seg, lm_void_t* arg) -> lm_bool_t
	{
		auto* c = reinterpret_cast<Ctx*>(arg);
		if ((seg->prot & LM_PROT_XR) != LM_PROT_XR)
			return LM_TRUE;
		// Restrict to the steamclient module's address range.
		if (seg->base + seg->size <= g_modSteamClient.base)
			return LM_TRUE;
		if (seg->base >= g_modSteamClient.base + g_modSteamClient.size)
			return LM_TRUE;

		const auto local = IpcFrame::scan(reinterpret_cast<const uint8_t*>(seg->base), seg->size);
		for (const auto& cand : local)
			c->cands.push_back({
				static_cast<size_t>(seg->base) + cand.offset,
				cand.root,
				cand.available,
			});
		return LM_TRUE;
	};
	if (!IpcFrame::scanWhenCatalogIncomplete(catalogResolved, [&]
	{
		LM_EnumSegments(enumSegments, &ctx);
	}))
	{
		return;
	}

	if (ctx.cands.empty())
	{
		g_pLog->debug("IpcFrame: no RunIPCFrame candidates found; keeping embedded roots\n");
		return;
	}

	for (size_t i = 0; i < targets.size(); ++i)
	{
		Pattern_t* p = targets[i];
		// A validated exact-SHA catalog is authoritative for this locator.  Do
		// not mutate or scan the embedded fallback unless the catalog entry is
		// absent or fails executable-range validation.
		if (catalogResolved[i])
			continue;
		const uint32_t seed = IpcFrame::parseTrailingRoot(p->pattern);
		size_t idx = IpcFrame::resolveConfident(ctx.cands, seed, IpcFrame::kMaxRootDrift);

		// Guarded structural fallback for an interface whose dispatch median
		// jumped beyond kMaxRootDrift.  RemoteStorage did this earlier; a later
		// beta made UserStats do it too (root 0x876D658x -> 0x85E5D66B, ~1.9M
		// away, so the nearest-root band cannot reach it).  Each such interface
		// is identified by a set of high-24-bit cmp-id pivots: the top three
		// bytes of several of its dispatch ids stay stable across builds even
		// when the low byte and the median drift.  Require >= minMatches pivots
		// present in EXACTLY ONE candidate; none or more than one keeps the
		// embedded root (find() then fails loud rather than mis-hooking).
		const auto resolveByPivots =
			[&](const uint32_t* pivots, size_t count, size_t minMatches) -> size_t
		{
			size_t hit = SIZE_MAX;
			size_t qualifying = 0;
			for (size_t i = 0; i < ctx.cands.size(); ++i)
			{
				const auto& cand = ctx.cands[i];
				if (IpcFrame::countHighPivots(
					reinterpret_cast<const uint8_t*>(cand.offset), cand.available,
					pivots, count) >= minMatches)
				{
					hit = i;
					++qualifying;
				}
			}
			return (qualifying == 1) ? hit : SIZE_MAX;
		};

		if (idx == SIZE_MAX && p == &Patterns::IClientRemoteStorage::RunIPCFrame)
		{
			static constexpr uint32_t remoteStoragePivots[] =
			{
				0x396376, 0x5DB472, 0x84692E, 0x8694E9, 0x8712DD,
				0xC0F5C7, 0xDB2410, 0xE82BD7, 0xF5841E, 0xFD3CA9,
			};
			idx = resolveByPivots(
				remoteStoragePivots, std::size(remoteStoragePivots), 4);
		}
		if (idx == SIZE_MAX && p == &Patterns::IClientUserStats::RunIPCFrame)
		{
			static constexpr uint32_t userStatsPivots[] =
			{
				0x84EDDC, 0x85DE33, 0x85E5D6, 0xF7452C,
				0xF991AC, 0xF9B9E3, 0xFF7DFB, 0xFF94F2,
			};
			idx = resolveByPivots(
				userStatsPivots, std::size(userStatsPivots), 4);
		}
		if (idx == SIZE_MAX)
		{
			// No confident match: drift too large, or an ambiguous neighbour.
			// Keep the embedded root; find() then either still matches it or
			// fails loudly as a required pattern (no silent mis-hook).
			continue;
		}

		const uint32_t found = ctx.cands[idx].root;
		if (found != seed)
		{
			// Auto-resolving root drift is the designed steady state on new
			// client builds, not a problem — keep it out of the normal log and
			// surface it only when ExtendedLogging is explicitly enabled.
			if (g_config.extendedLogging.get())
				g_pLog->debug(
					"IpcFrame: %s root drifted 0x%08X -> 0x%08X; auto-resolved\n",
					p->name.c_str(), seed, found);
			IpcFrame::setTrailingRoot(p->pattern, found);
		}
	}
}


bool Patterns::init()
{
	bool found = true;

	// Establish immutable compiled policy before reading any remote-derived
	// metadata.  A catalog whose required flags or symbols disagree with this
	// registry is rejected in full.

	// CAPIJob::GetPlayerStats is a pure observation hook: hkCAPIJob_GetPlayerStats
	// calls the original and emits a debug line, returning the result unchanged
	// (it modifies nothing).  It was previously required, so a client that
	// restructured this function aborted the ENTIRE injection and left Steam
	// unmodified.  A later beta rewrote the function body (prologue, member
	// layout and control flow all changed), which is exactly that failure.
	// Reclassify it optional to match every other observational hook here: its
	// absence is a genuine safe no-op (only the debug log is lost), so a drift
	// disables just this log instead of every hook.
	CAPIJob::GetPlayerStats.optional = true;

	CUser::MarkLicenseAsChanged.optional = true;
	CUser::ProcessPendingLicenseUpdates.optional = true;
	CUser::NotifyLicensesUpdated.optional = true;
	CUser::Offset_CompatManager.optional = true;
	CCMInterface::RecvPkt.optional = true;
	CAppInfoCache::GetOrAddAppData.optional = true;
	CAppInfoCache::ThreadedReadFromDisk.optional = true;
	CAppInfoCache::SkipFlagReference.optional = true;
	CAppInfoCache::ShaReference.optional = true;
	CDepotDownloadMgr::ProcessDepotManifest.optional = true;
	CDepotDownloadMgr::PrepareDepotDownload.optional = true;
	CDepotDownloadMgr::BuildDepotDependency.optional = true;
	CDepotDownloadMgr::EvaluateConfigChanges.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedStack.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedReg.optional = true;
	ParentalSignatureCheck.optional = true;
	ParentalSettingsReceived.optional = true;
	SteamUI::AppControllerRunFrame.optional = true;
	SteamUI::GetAppByID.optional = true;
	SteamUI::MarkAppChange.optional = true;
	SteamUI::BuildCompleteAppOverviewChange.optional = true;
	SteamUI::OwnershipFlagsReference.optional = true;

	loadActiveCatalogs();

	// Self-heal the IClient*::RunIPCFrame signatures before scanning.  Their
	// only volatile byte is the dispatch-tree root message id, which drifts
	// when Steam adds/removes interface methods (the 2026-06-23 update broke
	// all four this way).  Re-derive each root structurally so a constant-only
	// drift no longer needs a code change; on any failure the embedded root is
	// kept verbatim (no regression).
	autoResolveIpcFrameRoots();
	for(auto& pattern : patterns())
	{
		if (!pattern->find())
		{
			if (pattern->optional)
			{
				// Optional patterns degrade to a safe no-op in their
				// dependent feature; don't fail the whole load.
				g_pLog->warn
				(
					"Optional pattern '%s' not found; dependent feature disabled\n",
					pattern->name.c_str()
				);
				continue;
			}
			// Required pattern missing: log WHICH one so a Steam-client
			// update that drifts a signature is diagnosable from the log
			// instead of just "Failed to find all patterns".
			g_pLog->warn
			(
				"Required pattern '%s' not found\n",
				pattern->name.c_str()
			);
			found = false;
		}
	}

	if (found)
		writeLocalCatalogs();
	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	// Parental::setup flips the jne at +2.  The failure path pushes a member
	// for its log line (`push [eax+disp32]`, 0x182 -> 0x166 on the 2026-10
	// client); it is location-only, so its low bytes are masked.  The jne
	// distance (0x27) keeps the sibling check (jne 0x63) out: one match.
	Pattern_t ParentalSignatureCheck
	{
		"ParentalSignatureCheck",
#if defined(__x86_64__)
		// Measured against steamrt64/steamclient.so (build a57ea463).
		// Proof 1: Immediately follows `call 0x24e3ec0` (RSA signature verification,
		// shared with ParentalSettingsReceived 0x186db40 and matching i386 0x26e4840)
		// inside 0x186d710 ("Validating parental settings", "ParentalSettings\\Signature").
		// Proof 2: Exact `84 C0 75 24` (`test al, al; jne +0x24`) guarding the log
		// "Parental settings signature did not verify for account ID %u" with
		// [rbx+0x242] (CUser SteamID account ID), matching i386 0x1a3cefc (`84 C0 75 27`).
		// Unique at 5/35 tokens.
		"84 C0 75 24 8B 93 ? ? 00 00 48 8D AC 24 ? ? 00 00 31 C0 48 8D 35 ? ? ? ? 48 89 EF E8 ? ? ? ?",
#else
		"84 C0 75 27 8B 85 ? ? ? ? 8D 9D ? ? ? ? 83 EC 04 FF B0 ? ? 00 00 8D 86 ? ? ? ? 50 53 E8 ? ? ? ?",
#endif
		SigFollowMode::None
	};
	Pattern_t ParentalSettingsReceived
	{
		"ParentalSettingsReceived",
#if defined(__x86_64__)
		// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x186db40).
		// Proof 1: Verifies RSA signature via 0x24e3ec0 (shared with
		// ParentalSignatureCheck 0x186d995), parses serialized ParentalSettings
		// protobuf (0x28ba75e), and reads CUser SteamID account ID [rbp+0x242]
		// (same +0x242 offset as ParentalSignatureCheck).
		// Proof 2: Exact 4 external callers (0x119cc90, 0x186df70, 0x186dfd0,
		// 0x186e370), matching i386 0x1a3d520's 4 callers (0x12eb680, 0x1a3d110,
		// 0x1a3db40, 0x1a3dd90), taking (instance, settings, settingsLen,
		// signature, signatureLen, a6, a7) in (rdi, rsi, edx, rcx, r8d, r9, [rsp+8]).
		// Unique at 20/47 tokens.
		"41 57 41 56 45 89 C6 41 55 49 89 F5 41 54 41 89 D4 55 48 89 FD 53 48 89 CB 48 81 EC ? ? 00 00 48 8D 15 ? ? ? ? 48 8B 84 24 ? ? 00 00",
#else
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 00 02 00 00 8B 45 08 8B 55 1C 8B 7D 0C",
#endif
		SigFollowMode::None
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
#if defined(__x86_64__)
		// x86-64: `mov rdi,rdx; shr eax,3` then the store pair.  PrologueUpwards
		// because the anchor is mid-function; the saved-register chain gives the
		// entry.  Verified unique in steamrt64/steamclient.so.
		"48 89 D7 C1 E8",
		SigFollowMode::PrologueUpwards,
		std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
		"E8 ? ? ? ? 83 C4 10 85 FF 74 ? 8B 07 83 EC 04 FF B5 ? ? ? ? FF B5 ? ? ? ? 57 FF 10 83 C4 10 8D 45 ? 83 EC 04 89 F3 6A 04 50 FF 75",
		SigFollowMode::Relative,
#endif
	};

	namespace SteamUI
	{
		// Linux i386 steamui.so build
		// 38adc592f0ab97349639b297203739db70b6547f (SHA-256
		// 833914b45fcb407e50631d1f62558a28c2f74dc7431b0f0aebd6d25e45b4cd72).
		// x86-64 arms: steamrt64/steamui.so build
		// 57d58821dd9c285beae2688cc85d64f7c2c5f308 (SHA-256
		// d4376171a7ffe96bab76f6203b4c65a2f6bff277a9212922b3ca106bf79a3b43).
		// x86-64 has no PIC thunk, so every x86-64 arm anchors on the plain
		// SysV entry (rdi, esi, edx carry the same arguments as the cdecl
		// stack slots).  Each complete signature below has exactly one .text
		// match.  They are optional as a group: live visual removal must fail
		// closed without affecting package refresh when Steam moves any UI
		// implementation.
		//
		// x86-64: slot 3 of the 7-slot primary CSteamUIAppController vtable
		// (RTTI 21CSteamUIAppController), the same slot that holds the i386
		// body; both bodies carry the same CUtlHashMap::RemoveAt / bitstring /
		// refcount asserts.  Unique from the Plat_FloatTime call + xmm convert.
		Pattern_t AppControllerRunFrame
		{
			"CSteamUIAppController::RunFrame",
#if defined(__x86_64__)
			"41 57 41 56 41 55 41 54 55 53 48 83 EC 38 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 28 48 89 FB E8 ? ? ? ? 66 0F EF C9 66 0F EF D2 F2 0F 5A C0 F3 0F 5A D0",
#else
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 7C E8 ? ? ? ? 8B 84 24 90 00 00 00 D9 5C 24 0C D9 44 24 0C",
#endif
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::AppControllerRunFrame"
		};
		// Direct cdecl entry: (controller, appid, create).  The opening PIC
		// thunk is part of the entry and must be repaired in a trampoline only
		// if this locator is detoured in the future; LibraryRemoval calls it.
		//
		// x86-64: two functions reference "CSteamUIAppController::GetAppByID"
		// and "... - Create" on both arches.  The real entry has 182 caller
		// functions (i386 0x1974ca0: 181) and spills both esi and edx (appid,
		// create); the other is a 49-caller FillInAppOverview helper on both.
		Pattern_t GetAppByID
		{
			"CSteamUIAppController::GetAppByID",
#if defined(__x86_64__)
			"41 57 41 56 49 89 FE 41 55 41 54 55 53 48 83 EC 38 89 74 24 0C 89 54 24 08 64 48 8B 04 25 28 00 00 00 48 89 44 24 28",
#else
			// The trailing `mov eax,[eax+disp32]` reads a controller member near
			// 0x9E0 that a later client moved (~0xC38); it is location-only for
			// this locator, so both low displacement bytes are masked while the
			// high bytes stay 00 00 and the prologue keeps one steamui match.
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 5D 10 8B 75 08 89 45 C8 8B 80 ? ? 00 00",
#endif
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::GetAppByID"
		};
		// Direct cdecl entry: (source, appid, change_flags).  Hooking this
		// captures the otherwise private source receiver used by RunFrame.
		//
		// x86-64: the only function referencing "CUpdateManager::MarkAppChange";
		// it also carries "AppChangeNotification" and the utlfixedlinkedlist
		// assert, the same set as the i386 body.
		Pattern_t MarkAppChange
		{
			"CUpdateManager::MarkAppChange",
#if defined(__x86_64__)
			"41 57 41 89 F7 41 56 41 55 41 54 55 48 89 FD 53 48 83 EC 28 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00",
#else
			// Same controller member load as GetAppByID (near 0x9E0 -> ~0xC38 on
			// a later client); location-only, so mask its low displacement bytes
			// (high bytes stay 00 00) while the distinct frame keeps one match.
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 3C 8B 75 0C 8B 5D 10 89 45 D4 8B 80 ? ? 00 00",
#endif
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::MarkAppChange"
		};
		// Direct cdecl entry: (controller, change, optional_callback).  The
		// detour only publishes an atomic reassert request after the original.
		//
		// x86-64: the only function referencing all three of
		// "BuildCompleteAppOverviewChange", "...BuildCompleteAppOverviewChange
		// - app" and "Constructed empty appoverview message in ...".
		Pattern_t BuildCompleteAppOverviewChange
		{
			"BuildCompleteAppOverviewChange",
#if defined(__x86_64__)
			"41 57 41 56 41 55 41 54 55 48 89 FD 53 48 89 F3 48 81 EC D8 00 00 00 48 89 54 24 40",
#else
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC 8C 00 00 00 8B 7D 08 89 5D A4 E8 ? ? ? ? 84 C0",
#endif
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::BuildCompleteAppOverviewChange"
		};
		// FillInAppOverview reads CSteamApp::OwnershipFlags through `mov
		// eax,[ecx+disp8]` immediately before serializing eAppOwnershipFlags.
		// Keep the instruction address so LibraryRemoval derives and validates
		// the Linux field offset instead of importing the unrelated Windows
		// layout constant.
		//
		// FillInAppOverview emits a run of fields that all share this shape
		// (type check -> `mov eax,[app+disp8]` -> push value, push descriptor,
		// push sink, call emitter), so the field displacement is what tells them
		// apart and MUST stay pinned: ownership is 0x18 on both the 2026-08-03
		// and the 2026-08-16 client, and the block right after it reads 0x50
		// through a different emitter.  What actually drifted is only the
		// register Steam parked the app pointer in (ecx -> edx), so the modrm
		// byte is the wildcard.
		//
		// Do NOT wildcard the displacement instead.  That also yields exactly
		// one match on the newer client, but it is the neighbouring field, and
		// LibraryRemoval would then flip hidden-ownership bits inside it.
		//
		// x86-64: `41 8B 56 1C` (mov edx,[r14+0x1c]) loads the field straight
		// into the emitter's third argument, then `lea rsi,[eAppOwnershipFlags]`
		// and the call.  0x1c is confirmed by CSteamApp vtable slot 13 (`mov
		// eax,[rdi+0x1c]; ret`), the slot holding the i386 `[+0x18]` getter.
		// Pinned 1C has one match; a wildcard displacement has two, the second
		// being eAppUpdateError at 0x5c.  Only the modrm (base register) and the
		// frame slot holding the sink are masked.
		Pattern_t OwnershipFlagsReference
		{
			"CSteamApp::OwnershipFlagsReference",
#if defined(__x86_64__)
			"41 8B ? 1C 48 8B BD ? ? ? ? 48 8D 35 ? ? ? ? E8 ? ? ? ?",
#else
			"8B ? 18 8B 9D ? ? ? ? 83 EC 04 50 8D 83 ? ? ? ? 50 FF B5 ? ? ? ? E8 ? ? ? ?",
#endif
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::OwnershipFlagsReference"
		};
	}

	namespace CAPIJob
	{
		// Direct cdecl entry of the CAPIJobRequestUserStats job body (the
		// function carrying "Player.GetUserStats#1"; one caller passes the
		// job as its only argument).  This was a call-site + Relative follow,
		// but the 2026-10 client rewrote that caller.  The entry is the same
		// function: PIC-thunk prologue, `this` from [ebp+8], stack cookie.  The
		// frame size and frame-relative spills are masked; one .text match.
		Pattern_t GetPlayerStats
		{
			"CAPIJob::GetPlayerStats",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Only function (0x1956510) referencing "Player.GetUserStats#1".
			// Proof 2: Single external caller (0x1956f00, matching i386 0x1b32c10), which
			// carries "Bad GameID passed to RequestUserStats()", "17GSStatsReceived_t",
			// "19UserStatsReceived_t", "UserStatsLock", and "[AppID %u] CAPIJobRequestUserStats
			// - no connection to Steam", passing `this` directly to the job body.
			// Masks frame size, canary spill, stack object offsets, calls, and [rbx+0x1a0].
			"41 57 31 F6 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 64 48 8B 1C 25 28 00 00 00 48 89 9C 24 ? ? 00 00 48 89 FB 48 8D 44 24 ? 48 89 C7 48 89 04 24 E8 ? ? ? ? 48 8D 84 24 ? ? 00 00 31 F6 48 89 C7 48 89 44 24 08 E8 ? ? ? ? 48 8B AB ? ? 00 00",
#else
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC ? ? 00 00 8B 75 08 89 B5 ? ? FF FF 89 FB 65 A1 14 00 00 00 89 45 E4 31 C0 8D 85 ? ? FF FF 6A 00 50",
#endif
			SigFollowMode::None
		};
	}

	namespace CCMInterface
	{
		Pattern_t RecvPkt
		{
			"CCMInterface::RecvPkt",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Only function (0x11a0440) referencing "CCMInterface::RecvPkt",
			// "../clientdll/cminterface.cpp", and "CCMInterface - received heartbeat
			// response, connection still good".
			// Proof 2: Calls CProtoBufMsgBase::InitFromPacket (0x25eba90),
			// CJobMgr::BRouteMsgToJob (0x2582660), CSteamEngine::PostCallback (0x1071880),
			// and CNetMsg factory 0x25d67d0 (17 callers, matching i386 0x27e3370's 17).
			// Masks frame size (0x518), GOT/global RIP-rel disp, and canary spill offset.
			// Unique at 26/49 tokens.
			"41 57 41 56 41 55 41 54 49 89 FC 55 53 48 89 F3 48 81 EC ? ? 00 00 48 8B 2D ? ? ? ? 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0",
#else
			// The stack frame grew on a later beta (sub esp,0x4cc -> larger), so
			// the frame size and the frame-relative [ebp-disp] spill of the saved
			// argument both drift together.  Both are local-frame layout, not
			// values the hook reads, so mask the frame immediate and the spill
			// displacement (their high bytes 00 00 / FF FF are kept); the gs
			// cookie load and the rest keep exactly one .text match.
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC ? ? 00 00 8B 45 08 8B 7D 0C 89 85 ? ? FF FF 65 A1 14 00 00 00 89 45 E4 31 C0 8B 86",
#endif
			SigFollowMode::None,
			nullptr,
			"Patterns::CCMInterface::RecvPkt"
		};
	}

	namespace CProtoBufMsgBase
	{
		Pattern_t InitFromPacket
		{
			"CProtoBufMsgBase::InitFromPacket",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Contains assert string "CProtoBufMsg::InitFromPacket( IMsgNetPacket )".
			// Proof 2: Exact 166 callers (matching i386 0x27f78a0's 166 callers) and
			// identical body (checks [rbx+0x19], zeroes [rbx+0x38], decrements m_iRefs).
			// Unique at 24/43 tokens.
			"41 55 41 54 55 53 48 89 FB 48 83 EC 48 48 8B 05 ? ? ? ? "
			"64 4C 8B 24 25 28 00 00 00 4C 89 64 24 38 49 89 F4 8B 10 31 C0 85 D2",
			SigFollowMode::None
#else
			"E8 ? ? ? ? 58 8B 45 ? 8B 8D",
			SigFollowMode::Relative
#endif
		};
		Pattern_t Send
		{
			"CProtoBufMsgBase::Send",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Exact 2 callers (0x1130e60 and 0x25ec4d0, matching i386 0x27f7d50's
			// 2 callers), warns at ../common/msgprotobuf.cpp line 0x640 ("Sending msg of
			// type '%s' with timeout=0...").
			// Proof 2: Stores ByteSize in [rbx+0x2c], calls [rax+0x48] on [rbx+0x38],
			// and returns `lea eax, [rax + rbp + 8]`.  Unique at 17/47 tokens.
			"55 53 48 89 FB 48 83 EC 08 80 3D ? ? ? ? 00 48 8B 7F 30 75 0A "
			"8B 87 C0 00 00 00 85 C0 74 30 E8 ? ? ? ? 48 8B 7B 38 48 89 C5 89 43 2C",
			SigFollowMode::None
#else
			"E8 ? ? ? ? 59 5A 50 56 E8 ? ? ? ? 83 C4 0C",
			SigFollowMode::Relative
#endif
		};
	};

	namespace CSteamEngine
	{
		Pattern_t GetServerPipe
		{
			"CSteamEngine::GetServerPipe",
#if defined(__x86_64__)
			"0F B7 CE 31 D2",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
			"0F B7 CA 31 C0 3B 4B",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x53, 0x56, 0x57 },
#endif
			nullptr,
			"Patterns::CSteamEngine::GetServerPipe"
		};
		Pattern_t Init
		{
			"CSteamEngine::Init",
#if defined(__x86_64__)
			// Entry, not a call site: on x86-64 this is reached directly, so
			// Relative has nothing to follow.  The window stops before the first
			// `mov eax,[rdi+disp32]`, whose displacement is the build-specific
			// m_bInitialized offset (0x1d2c here).  What is kept is stack-frame
			// setup plus the %fs:0x28 canary spill: without the canary pair the
			// prefix matches twice, with it exactly once.
			"41 54 55 53 48 81 EC 10 01 00 00 64 48 8B 04 25 28 00 00 00 "
			"48 89 84 24 08 01 00 00 8B 87 2C 1D 00 00",
			SigFollowMode::None
#else
			"E8 ? ? ? ? 83 C4 10 8D 83 ? ? ? ? 83 EC 0C 89 AB",
			SigFollowMode::Relative
#endif
		};
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
#if defined(__x86_64__)
			"0F 43 C6 83 FB",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
			nullptr,
			"Patterns::CSteamEngine::SetAppIdForCurrentPipe"
#else
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? 8B 85 ? ? ? ? 83 EC 08 FF B5",
			SigFollowMode::Relative
#endif
		};
		Pattern_t ProcessIPCFrame
		{
			"CSteamEngine::ProcessIPCFrame",
#if defined(__x86_64__)
			"48 89 DA 44 89 E6 5B",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
			"5E C3 FF 74 24",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xC3, 0x81, -1, -1, -1, -1, 0xE8, 0x53, 0x56 },
#endif
			nullptr,
			"Patterns::CSteamEngine::ProcessIPCFrame"
		};
		Pattern_t Offset_ClientUtils
		{
			"CSteamEngine::m_ClientUtils",
#if defined(__x86_64__)
			// The constructor publishes the primary vptr and then the embedded
			// sub-object table: `mov [this],vptr; add rax,<vtable delta>;
			// mov [this+disp32],rax`.  That store is where m_ClientUtils lives.
			//
			// The locator must START at the store: the decoder reads the disp32
			// at +3, after `48 89 83`, so anchoring one instruction earlier (on
			// the `lea vptr`) would decode that instruction's RIP displacement
			// instead and produce a nonsense member offset.  The tail is what
			// makes the store itself unique: the same `mov [this+d],rax` shape
			// appears in thousands of constructors.
			"48 89 83 ? ? ? ? 48 8D 83 ? ? ? ? 48 89 C7 48 89 84 24 ? ? ? ? "
			"E8 ? ? ? ? 48 8D 83 ? ? ? ? C7 83",
			SigFollowMode::None,
			nullptr,
			"Patterns::CSteamEngine::Offset_ClientUtils"
#else
			"89 86 ? ? ? ? 8D 86 ? ? ? ? 89 44 24 ? 50 E8 ? ? ? ? 83 C4",
			SigFollowMode::None,
			nullptr,
			"Patterns::CSteamEngine::Offset_ClientUtils"
#endif
		};
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: CSteamEngine::CSteamEngine (0x17c0000) initializes m_ClientUtils
			// at +0x1118 and this CUtlVector<UserEntry> at +0x11c0 (m_Size at +0x11d0).
			// Proof 2: CSteamEngine::BMainLoop (0x17c4930, "CSteamEngine::BMainLoop")
			// and sibling vtable slots [18], [21], [31], [33] scale index<<4 (16-byte
			// pairs) and add [this+0x11c0] before dereferencing [rax+8] as CUser*.
			// Anchored on `48 03 83 C0 11 00 00` (disp32 at +3 = kDispAfterRexOpcodeModrm);
			// unique at 17 tokens (full 27-token tail pins the [rax+0x138] virtual call).
			"48 03 83 ? ? 00 00 48 8B 78 08 48 8B 07 FF 90 38 01 00 00 83 C5 01 41 83 C4 01",
			SigFollowMode::None,
#else
			"8B 80 ? ? ? ? FF 75 ? 8D 34",
			SigFollowMode::None,
#endif
			nullptr,
			"Patterns::CSteamEngine::Offset_User"
		};
	}

	namespace CSteamMatchmakingServers
	{
		Pattern_t GetServerDetails
		{
			"CSteamMatchmakingServers::GetServerDetails",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: 24CSteamMatchMakingServers RTTI vtable (0x30e5d78, 27 slots)
			// slot 7 = 0x1676900; 34CAdapterSteamMatchmakingServers003[7] forwards via
			// `jmp [rax + 0x38]` (matching i386 slot 7 at 0x183a6c0).
			// Proof 2: Locks [rdi+0x68] and scans the [r13+0x38] list (count [r13+0x48]).
			// Unique at 42/46 tokens.
			"41 55 49 89 FD 41 54 4C 8D 67 68 55 4C 89 E7 89 D5 53 48 89 F3 48 83 EC 08 "
			"E8 ? ? ? ? 49 63 55 48 49 8B 45 38 85 D2 7E 46 48 8D 14 D0",
			SigFollowMode::None
#else
			"89 45 ? 83 C4 10 83 EC 0C 89 F3",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#endif
		};
		Pattern_t RequestInternetServerList
		{
			"CSteamMatchmakingServers::RequestInternetServerList",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: 24CSteamMatchMakingServers RTTI vtable (0x30e5d78, 27 slots)
			// slot 0 = 0x167a670; 34CAdapterSteamMatchmakingServers003[0] forwards via
			// `jmp [rax]` (matching i386 slot 0 at 0x18398f0).
			// Proof 2: Locks [rdi+0x68], allocates 0x410 bytes (0x384 on i386), and
			// passes `xor ecx, ecx; mov r8d, 1` (eInternet=0), distinguishing it from
			// the 4 sibling LAN/Friends/Favorites/History slots; unique at 17/36 tokens.
			"BF 10 04 00 00 E8 ? ? ? ? FF B3 80 01 00 00 31 C9 48 89 DE 48 89 C7 "
			"FF B3 78 01 00 00 41 B8 01 00 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x55, 0x41, 0xCE, 0x89, 0x41, 0x56, 0x41, 0x57, 0x41, 0xE5, 0x89, 0x48, 0x55 }
#else
			"C7 04 24 ? ? 00 00 E8 ? ? ? ? 5A 89 45 ? 59 FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? 6A 01",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x57, 0xe5, 0x89, 0x55 }
#endif
		};
	}

	namespace CUser
	{
		// CCompatManager is an embedded member of the local CUser. Locate the
		// constructor call and retain the LEA so CSteamEngine can decode its
		// displacement instead of pinning a build-specific class offset.
		// 2026-10 client: `lea ecx,[esi+disp32]; pop; push esi; push ecx;
		// spill; call ctor` (was `lea ebx` + `mov ebx,edi`).  Only this form is
		// decoded, because the same update reordered IClientCompat's vtable
		// (see VFTIndexes::IClientCompat).
		Pattern_t Offset_CompatManager
		{
			"CUser::m_CompatManager",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Calls 0x11b9a50, whose constructor writes vtable 0x30b22a8
			// with RTTI type_info 0x30b1dd8 ("14CCompatManager") and whose slots [1]
			// (0x11c34e0) and [4] (0x11a5ba0) match SpecifyCompatTool / GetCompatToolName.
			// Proof 2: Inside CUser::CUser (0x1855ff0), 0x1856c2a loads
			// `lea rax, [rbx + 0x4a58]`, passes rbx (CUser*) in rsi and rax in rdi,
			// and calls 0x11b9a50. Masks member disp low 2 bytes, stack spill offset,
			// and call rel32. Unique at 0x1856c2a.
			"48 8D 83 ? ? 00 00 48 89 DE 48 89 C7 48 89 84 24 ? ? 00 00 E8 ? ? ? ? 66 0F 6F 1D",
#else
			"8D 8E ? ? ? ? 58 56 51 89 8D ? ? ? ? E8 ? ? ? ? 58 5A C7 86 ? ? ? ? FF FF FF FF",
#endif
			SigFollowMode::None,
			nullptr,
			"Patterns::CUser::Offset_CompatManager"
		};
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
#if defined(__x86_64__)
			"48 89 E8 31 ED",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
			"E8 ? ? ? ? 88 45 ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 45 ? 80 7D ? 00",
			SigFollowMode::Relative
#endif
		};
		// Direct cdecl entry.  This was a call-site + Relative follow, but the
		// caller restructured on a later client (the je flipped to a jne with a
		// different body), so the call-site form drifted to zero matches.  The
		// entry itself is stable across builds: the PIC-thunk prologue, then the
		// (appList, size, flags) frame — movzx eax,[ebp+0x14], two null pushes,
		// the [ebp-0x71] byte save, and the movdqa scratch init.  The two movdqa
		// displacements are PIC-relative to a data symbol (location-only) and are
		// masked; the whole form still has exactly one .text match on every
		// inspected build.  The resolved target is the function entry either way,
		// so moving from Relative to None does not change the hook placement.
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: CUser(259) RTTI vtable slot 182 (0x1844c00) rebases `this`
			// by -0x1f88 and tail-jumps to 0x1844840, matching i386 slot 182 -> 0x1a1f630.
			// Proof 2: Same (this, pAppList, size, flags) entry shape: two 16-byte
			// movdqa stack vector inits and signed count check on [rdi+0x24c0].
			// Unique at 15 tokens; 44-token prologue masks only the .rodata RIP disp.
			"41 57 41 56 41 55 41 54 55 53 48 83 EC 78 66 0F 6F 05 ? ? ? ? "
			"48 89 74 24 10 89 54 24 0C 88 4C 24 0B 64 48 8B 04 25 28 00 00 00",
			SigFollowMode::None
#else
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 C4 80 0F B6 45 14 6A 00 6A 00 88 45 8F 8D 45 D0 66 0F 6F 86 ? ? ? ? 50 89 45 80 0F 11 45 BC 66 0F 6F 86 ? ? ? ? 89 75 94 0F 29 45 A8",
			SigFollowMode::None
#endif
		};
		Pattern_t PostCallback
		{
			"CSteamEngine::PostCallback",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: All six `mov esi, 0x1f6` (AppOwnershipTicketReceived_t) sites
			// call 0x1071880 with (this, 0x1f6, &cb, 0x1c, 0), forwarding to
			// CSteamEngine::PostCallback (0x17b4e40) and tail-jumping to 0x1071720.
			// Proof 2: 0x1071720 checks [rax+0x14] > 0 and asserts "erverPipes.Element(i) > 0"
			// in ../clientdll/baseuser.cpp (line 0xb6), matching i386 0x11aa6b0->0x11aa710.
			// Sibling 0x1071a00 shares the first 74 bytes; the trailing `E9 52 FE FF FF`
			// jump to 0x1071720 makes it unique at 75/78 tokens.
			"41 55 45 31 C9 41 89 CD 41 54 49 89 D4 55 89 F5 53 48 89 FB 48 83 EC 10 "
			"48 8D 05 ? ? ? ? 8B 77 48 48 8B 38 41 50 41 89 C8 48 89 D1 89 EA E8 ? ? ? ? "
			"48 83 C4 18 44 89 E9 4C 89 E2 89 EE 48 89 DF 5B 5D 41 5C 41 5D E9 52 FE FF FF",
			SigFollowMode::None,
#else
			"E8 ? ? ? ? 8D 86 ? ? ? ? 83 C4 18 68 F6 01 00 00",
			SigFollowMode::Relative,
#endif
			nullptr,
			"Patterns::CUser::PostCallback"
		};
		Pattern_t PostCallbackToAppId
		{
			"CUser::PostCallbackToAppId",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Direct entry 0x1071c60 checks `cmp esi, 7` and asserts
			// "Don't post callbacks to WinUI" in ../clientdll/baseuser.cpp (line 0x15e),
			// identical to i386 0x11aac40.
			// Proof 2: Walks the same 0x18-stride RB-tree at [rbp+0xcc8..0xcf8] and calls
			// CSteamEngine::PostCallbackToPipe (0x17ac590).  Unique at 19/47 tokens.
			"41 57 41 56 41 55 41 54 55 48 89 FD 53 48 83 EC 28 83 FE 07 89 74 24 0C "
			"89 54 24 18 48 89 4C 24 10 44 89 44 24 1C 0F 84 ? ? ? ? 4C 8D 3D",
			SigFollowMode::None
#else
			"84 C0 0F 45 F8 89 F8",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xE8, 0x53, 0x56, 0x57, 0x55 }
#endif
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"IClientUser::UpdateAppOwnershipTicket",
#if defined(__x86_64__)
			"41 89 CD 83 FD",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? ? 8D 45 ? 89 45 ? EB",
			SigFollowMode::Relative,
#endif
			nullptr,
			"Patterns::CUser::UpdateAppOwnershipTicket"
		};
		// Current Linux i386 entry at RVA 0x0186BDC0.  Callers at
		// 0x0187309x/0x018731ax push this/package-id/bool, and the body hashes
		// the package id into the changed-license table.  This is a direct
		// cdecl entry with a PIC thunk call; the full signature has one match in
		// the inspected steamclient.so.
		Pattern_t MarkLicenseAsChanged
		{
			"CUser::MarkLicenseAsChanged",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x1849fc0).
			// Proof 1: Hashes packageId (esi) with Murmur3 constants 0x85ebca6b and
			// 0xc2b2ae35 into CUser::m_mapChangedLicenses (+0x2378, count +0x23bc,
			// elements +0x23a8), the exact map read by CUser::NotifyLicensesUpdated
			// (0x184a260), then looks up package in 0x1460 and marks apps via 0x102a9a0.
			// Proof 2: Exact 3 external callers (0x1037240, 0x184a980, 0x18614e0),
			// matching i386 0x1a0cd00's 3 external callers (0x115c870, 0x1a13a10,
			// 0x1a14090), and takes (this, packageId, bool) in (rdi, esi, dl).
			// Unique at 10/51 tokens.
			"41 57 48 8D 87 ? 23 00 00 41 89 F7 41 56 41 55 41 54 55 53 48 83 EC 28 88 54 24 ? 8B 97 ? 23 00 00 89 74 24 ? 48 89 44 24 ? 85 D2 0F 84 ? ? ? ?",
#else
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 74 24 40 8B 44 24 48 8B BE ? ? 00 00 88 44 24 1C 8D 86 ? ? 00 00 89 44 24 0C 85 FF 0F 84 ? ? ? ?",
#endif
			SigFollowMode::None
		};
		// Current Linux i386 entry at RVA 0x0186C800.  Its cdecl receiver is
		// read from [esp+0x40] after the saved-register prologue; it walks the
		// pending-license vector, validates CAppData, and drives the downstream
		// callback path containing callback 0xF90BE.  Caller 0x0187A67B passes
		// the same local-user receiver.  The full direct-entry signature has one
		// module match.
		Pattern_t ProcessPendingLicenseUpdates
		{
			"CUser::ProcessPendingLicenseUpdates",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Reads CUser::m_vecPendingLicenses (+0x2588, m_Size at +0x2598,
			// confirmed by "m_vecPendingLicenses" in CUser::ValidateStatics 0x186bdd3),
			// iterates PendingLicense_t entries skipping -1, and calls 0x102e680
			// (CAppInfoCache::GetOrAddAppData) checking [rax+0x18] == 3 (i386 0x1a0d7a0).
			// Proof 2: Single external caller (0x187bca0, matching i386 0x1a1c480) and
			// calls 0x184a260 ("17LicensesUpdated_t" notifier), 0x102e680, 0x10282e0,
			// 0x1850700, 0x18478c0.
			// Masks m_vecPendingLicenses low displacement byte, jle target, and RIP disp32.
			// Unique at 14/54 tokens.
			"41 57 48 89 F8 41 56 41 55 41 54 55 53 48 83 EC 28 8B 97 ? 25 00 00 48 89 7C 24 18 85 D2 0F 8E ? ? ? ? 48 05 ? 25 00 00 48 89 44 24 08 4C 8D 25 ? ? ? ?",
#else
			// The trailing `lea eax,[ebx+disp32]` reaches a data symbol through
			// the PIC base, so its displacement tracks the GOT layout and moves
			// on an unrelated client change (0x3B314 -> 0x3B714 between the
			// 2026-08-03 and 2026-08-16 builds).  Nothing here reads it -- the
			// hook only calls the resolved entry -- so it is location-only and
			// stays wildcarded.  The two CUser member offsets loaded above
			// (count near 0x1C7C, array base near 0x1C70) drifted by 4 on a
			// later beta (0x1C7C->0x1C78, 0x1C70->0x1C6C), the same 4-byte layout
			// shift seen on other members; the hook calls the resolved entry and
			// never reads these values, so only their low byte is masked (the
			// 0x1C high byte stays pinned) -- the relaxed form keeps one module
			// match while self-healing across that layout drift.
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 44 24 40 8B 88 ? 1C 00 00 85 C9 0F 8E ? ? ? ? 05 ? 1C 00 00 89 44 24 18 8D 83 ? ? ? ? 8B 30",
#endif
			SigFollowMode::None
		};
		// CUser::<broadcast LicensesUpdated_t>(CUser* this)
		//
		// The license-update notifier: rebuilds the LicensesUpdated_t
		// callback (callback id 0x7d) from the CUser's own license vector
		// and posts it to every subscriber via the PostCallback dispatch.
		// Used as the post-injection reconcile: after we append our
		// AdditionalApps into package 0's AppIdVec, invoking this on the
		// local CUser forces Steam's ownership/library layer to re-read
		// licenses (and therefore package 0, now containing our appids),
		// which breaks the cold-cache PICS product-info request loop.
		//
		// Positively identified via the RTTI string "17LicensesUpdated_t"
		// referenced just before it posts callback 0x7d.  Single stack arg
		// (`this`, read from [ebp+0x8]); standard cdecl, safe to call by
		// resolved pointer with g_pLocalUser as `this`.
		//
		// Direct prologue match (push ebp / mov ebp,esp / push edi,esi,ebx
		// / get_pc_thunk + add ebx / sub esp,0x1bc / mov edi,[ebp+0x8] /
		// mov edi,[eax+<off>] / mov [ebp-0x1ac],ebx / test edi,edi).  The
		// get_pc_thunk call rel, the PIC add immediate, the frame size, and
		// the [ebp-0x1ac] spill offset are masked so local-frame reshuffles
		// across builds stay compatible.  The member offset (`8B B8 ?? ?? 00
		// 00` = mov edi,[eax+0x1bXX]) is ALSO masked: it is a CUser layout
		// field whose value drifted 0x1b18 -> 0x1b14 on the 2026-06-23 client
		// (a 4-byte shift; RequiresLegacyCDKey moved the same way).  SLSsteam
		// only needs to LOCATE this function (it invokes it by pointer with
		// g_pLocalUser as `this`); the offset value itself is internal to the
		// function, so wildcarding it makes the signature self-heal across that
		// class of layout drift while staying unique.
		//
		// A later beta drifted this further: the field load changed BOTH the
		// destination register and the offset (mov edi,[eax+0x1b14] -> mov
		// esi,[eax+0x1b10]), which also flipped the following `test edi,edi` to
		// `test esi,esi`.  Both the modrm byte and the test are register-choice
		// noise the hook never reads, so mask them (`8B ? ? 1B 00 00`, `85 ?`).
		// To stay unique with the looser field load the signature now extends
		// down to this function's defining act: rebuilding LicensesUpdated_t and
		// posting callback id 0x7d (`... 83 EC 08 6A 7D 50` = sub esp,8; push
		// 0x7d; push eax).  Verified: exactly 1 match on every inspected build.
		Pattern_t NotifyLicensesUpdated
		{
			"CUser::NotifyLicensesUpdated",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x184a260).
			// Proof 1: References RTTI string "17LicensesUpdated_t" (at 0x184a29c) and
			// posts callback id 0x7d (`mov esi, 0x7d`) via CSteamEngine::PostCallback
			// (0x1071880), plus callback 0xf90be (at 0x184a446, matching i386).
			// Proof 2: Called by CUser::ProcessPendingLicenseUpdates (0x1851250 at
			// 0x1851501) and reads CUser::m_mapChangedLicenses (+0x23a8/+0x23bc/+0x23c0,
			// shared with CUser::MarkLicenseAsChanged 0x1849fc0).
			// Unique at 25/100 tokens.
			"41 57 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 44 8B 8F ? 23 00 00 48 89 7C 24 ? 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0 45 85 C9 0F 8E ? ? ? ? 48 8D 3D ? ? ? ? BE 7D 00 00 00 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 5C 24 ? 45 31 C0 B9 01 00 00 00 BE 7D 00 00 00",
#else
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC BC 01 00 00 8B 45 08 8B ? ? 1B 00 00 89 9D 54 FE FF FF 85 ? 0F 8E ? ? ? ? 8D 83 ? ? ? ? 83 EC 08 6A 7D 50",
#endif
			SigFollowMode::None
		};
	}

	namespace CAppInfoCache
	{
		// Current Linux i386 cdecl entry at RVA 0x00FCA900.  The current
		// module's GetOrAddAppData source assertion xref is inside this body;
		// [ebp+8]/[ebp+0xc]/[ebp+0x10] are cache/appid/create and the returned
		// node is consumed as CAppData by ProcessPendingLicenseUpdates.  The
		// direct PIC-thunk prologue is the trampoline entry; one full match.
		// The separate 0x00FD4070 regparm helper is a lookup path, not this
		// cdecl GetOrAddAppData target.  The `mov eax,[eax+disp32]` right after
		// the frame reads a PIC-relative global slot (near 0x8BC) that a later
		// beta relocated to ~0xA90; the slot address is location-only, so both
		// low displacement bytes are masked (high bytes stay 00 00) and the
		// prologue keeps one .text match.
		Pattern_t GetOrAddAppData
		{
			"CAppInfoCache::GetOrAddAppData",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Direct RB-tree lookup + CAppData construction (0x1026280,
			// matching i386 0x115f3a0) on CAppInfoCache+0x50, asserting
			// "nAppID != k_uAppIdInvalid" / "../clientdll/appdatacache.cpp" (0xb47)
			// when appID == 0 (`test ebp,ebp; je 0x102e538`).
			// Proof 2: Called twice inside CUser::ProcessPendingLicenseUpdates
			// (0x1851250 at 0x185132c and 0x18513a9/0x1851547 for appID 7) right
			// after BlockOnInitialization (0x10282e0), returning CAppData* whose
			// SHA ([rax+0x20..+0x34]) and skip flag ([rax+0x14]) are tested.
			// Unique at 20/90 tokens.
			"41 57 41 56 41 55 41 54 49 89 FC 55 89 F5 53 48 83 EC 18 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 08 89 D3 8B 10 31 C0 85 D2 74 14 48 8D 3D ? ? ? ? 48 89 E2 BE 04 00 00 00 E8 ? ? ? ? 85 ED 48 89 04 24 0F 84 ? ? ? ? 49 63 44 24 68 83 F8 FF",
#else
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 2C 8B 75 0C 8B 7D 10 89 45 D0 8B 80 ? ? 00 00 8B 00 85 C0 0F 85 ? ? ? ?",
#endif
			SigFollowMode::None
		};
		// CAppInfoCache::ThreadedReadFromDisk is Steam's own background cache
		// loader.  The current Linux i386 entry is RVA 0x00FC89F0 and takes the
		// CAppInfoCache pointer as its sole cdecl argument.  The 0x110c-byte
		// parser frame plus the saved cache argument makes this prologue unique
		// in the current module (one full match); signed catalogs can replace the
		// optional locator independently when a client update changes the frame.
		Pattern_t ThreadedReadFromDisk
		{
			"CAppInfoCache::ThreadedReadFromDisk",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x102f4a0).
			// Proof 1: Only function referencing "ThreadedReadFromDisk" (0x102f4e6) and
			// "ThreadedReadFromDisk: loading appinfo cache from: %s" (0x102f545),
			// matching i386 0x1162630.
			// Proof 2: Allocates the 0x1188-byte parser stack frame (`sub rsp, 0x1188`),
			// receives CAppInfoCache* in rdi, and is called by the CAppInfoCache reader
			// thread entry 0x102fed0 (matching i386 0x1162630's caller 0x117ee70).
			// Unique at 25/40 tokens.
			"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC 88 11 00 00 48 89 BD ? ? FF FF 64 48 8B 04 25 28 00 00 00 48 89 45 C8",
#else
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 0C 11 00 00 8B 45 08 89 85 0C EF FF FF",
#endif
			SigFollowMode::None
		};
		// Instruction site at RVA 0x0186C929: cmp byte ptr [esi+disp8],0
		// tests the same CAppData returned by GetOrAddAppData.  None is
		// intentional because AppDataLayout decodes this instruction in place;
		// the full reference signature has one module match.
		Pattern_t SkipFlagReference
		{
			"CAppInfoCache::SkipFlagReference",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, insn 0x1851361).
			// Proof 1: Inside CUser::ProcessPendingLicenseUpdates (0x1851250), immediately
			// after GetOrAddAppData (0x102e360 -> CAppData* in rax) and the 20-byte
			// SHA comparison at +0x20..+0x34, reads [rax+0x14] (`movzx eax, byte ptr
			// [rax+0x14]`) and ANDs it into the all-skipped flag (`and [rsp+0x17], al`).
			// Proof 2: CAppData constructor (0x1026280) zero-initializes [rdi+0x14] as a
			// word alongside +0x16, matching i386 CAppData ctor (0x115f3a0) zeroing
			// [esi+0x10] alongside +0x12 (4-byte vptr growth shifts +0x10 -> +0x14).
			// Unique in module.
			"0F B6 40 14 49 83 C6 01 20 44 24 ? 45 39 77 50 7F ?",
#else
			"80 7E 10 00 0F 44 C8 88 4C 24 1E 8B 44 24 10 83 C7 01 3B 7D 44 8B 30 7C 9E",
#endif
			SigFollowMode::None
		};
		// Instruction site at RVA 0x0186C914: lea eax,[eax+disp8], using the
		// GetOrAddAppData return while the preceding mov esi,eax preserves a copy
		// for the later skip test.  A 20-byte SHA comparison follows.  None keeps
		// the instruction address for runtime layout derivation; one full match.
		Pattern_t ShaReference
		{
			"CAppInfoCache::ShaReference",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, insn 0x185133b).
			// Proof 1: Inside CUser::ProcessPendingLicenseUpdates (0x1851250), immediately
			// after GetOrAddAppData (0x102e360 -> CAppData* in rax), compares the
			// 20-byte SHA at [rax+0x20..+0x34] (`cmp [rax+0x20], rcx; cmp [rax+0x28],
			// rsi; cmp [rax+0x30], edi`) before testing the skip flag at [rax+0x14].
			// Proof 2: CAppData constructor (0x1026280) zero-initializes the 20-byte
			// SHA at +0x20..+0x34 (qword +0x20, qword +0x28, dword +0x30), matching
			// i386 CAppData ctor (0x115f3a0) zeroing +0x1c..+0x30.
			// Unique in module.
			"48 39 48 20 75 ? 48 8D 35 ? ? ? ? 48 8B 76 08 48 39 70 28 75 ? 48 8D 3D ? ? ? ? 8B 7F 10 39 78 30 75 ?",
#else
			"8D 40 1C 50 E8 ? ? ? ? 83 C4 10 85 C0 75 10 0F B6 4C 24 1E 80 7E 10 00 0F 44 C8 88 4C 24 1E",
#endif
			SigFollowMode::None
		};
	}

	namespace IClientAppManager
	{
		Pattern_t RunIPCFrame
		{
			"IClientAppManager::RunIPCFrame",
#if defined(__x86_64__)
			// x86-64 tail: call rel32; mov eax,[rbp+disp32]; cmp eax,imm32.
			// The 32-bit `83 C4 10` (add esp) is absent -- x86-64 callee cleans.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 83 85 0A 7A",
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D B7 85 0A 7A",
#endif
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
		};
		Pattern_t BCanRemotePlayTogether
		{
			"IClientAppManager::BCanRemotePlayTogether",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: 15CUserAppManager RTTI vtable (0x309fab0, 117 slots) slot 73
			// = 0x103c9e0; locks CAppInfoCache, checks RemotePlayTogetherTestingBranches
			// (0x103a4d0) with CUser* [r12+0xf8], and falls back through parent appid
			// (0x1027270, "dlcforappid"), matching i386 0x1185340.
			// Proof 2: Unique at 16/47 tokens; 47-token prefix masks RIP & cache disp.
			"41 55 41 54 49 89 FC 55 89 F5 53 48 83 EC 08 4C 8D 2D ? ? ? ? "
			"49 8B 45 00 48 8D 98 ? ? 00 00 48 89 DF E8 ? ? ? ? 89 EE 48 89 DF E8",
			SigFollowMode::None
#else
			"58 5A FF 74 24 ? 56 E8 ? ? ? ? 83 C4 10 85 C0 74",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x53, 0x56, 0x57 }
#endif
		};
	}

	namespace IClientApps
	{
		Pattern_t RunIPCFrame
		{
			"IClientApps::RunIPCFrame",
#if defined(__x86_64__)
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D CA 9C 88 A6",
			SigFollowMode::PrologueUpwards,
			// This dispatcher spills its first argument between the saved-register
			// groups (entry: 55 48 89 E5 41 57 41 56 49 89 F6 41 55), so the
			// generic r12-r15 prologue does not match it.  The vector is the entry
			// bytes read in the order findPrologue compares them.
			std::vector<int16_t> { 0x89, 0x49, 0x56, 0x41, 0x57, 0x41, 0xE5, 0x89, 0x48, 0x55 }
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 37 9C 88 A6",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#endif
		};
	}

	namespace IClientRemoteStorage
	{
		Pattern_t RunIPCFrame
		{
			"IClientRemoteStorage::RunIPCFrame",
			// Seed root refreshed to a current dispatch median (0x8712DD65) so
			// the nearest-root fast path resolves directly on recent builds; the
			// stale 0x872FE86C only ever resolved via the pivot fallback.  The
			// high-24 pivot fingerprint (autoResolveIpcFrameRoots) remains as the
			// safety net for a future median jump.
#if defined(__x86_64__)
			// x86-64 tail: call rel32; mov eax,[rbp+disp32]; cmp eax,imm32.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 65 DD 12 87",
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 65 DD 12 87",
#endif
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
		};
	}

	namespace IClientUser
	{
		Pattern_t RunIPCFrame
		{
			"IClientUser::RunIPCFrame",
#if defined(__x86_64__)
			// x86-64 tail: call rel32; mov eax,[rbp+disp32]; cmp eax,imm32.
			// The 32-bit `83 C4 10` (add esp) is absent -- x86-64 callee cleans.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 10 A3 86 73",
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 10 A3 86 73",
#endif
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
		};

		Pattern_t BLoggedOn
		{
			"IClientUser::BLoggedOn",
#if defined(__x86_64__)
			// A whole function, eleven bytes: compare the account-state member
			// against 4 and return the flag.  There is no call site to follow, so
			// this is a direct entry match.
			//
			// The instruction alone is not unique -- 22 sites compare some member
			// against 4 and setcc, and one of them is byte-identical.  Identity
			// comes from the vtable: this address is slot 4 of the 259-slot CUser
			// vtable, which is the same index BLoggedOn occupies in the i386
			// IClientUser vtable.  The trailing single-byte nop is what
			// separates it from the identical CGameServer body (which is padded
			// with int3 instead).
			"83 BF ? ? FF FF 04 0F 94 C0 C3 90",
			SigFollowMode::None
#else
			"E9 ? ? ? ? ? ? ? ? ? ? 5B 5E 5F FF E0",
			SigFollowMode::Relative
#endif
		};
		Pattern_t BUpdateAppOwnershipTicket
		{
			"IClientUser::BUpdateAppOwnershipTicket",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: CUser(259) RTTI vtable slot 69 (0x185f860) rebases `this` by
			// -0x1f88 and tail-jumps to 0x185f560 (matching i386 slot 69 at 0x1a628e0).
			// Proof 2: Called directly by CSteamEngine::SetAppIdForCurrentPipe (0x17cc6f9)
			// and preserves the `test dl, dl; cmp dword [rbx+0x1b0], 4` guard.
			// Unique at 21/49 tokens.
			"41 57 41 56 41 55 41 54 55 53 48 89 FB 48 81 EC B8 00 00 00 84 D2 "
			"64 48 8B 2C 25 28 00 00 00 48 89 AC 24 A8 00 00 00 89 F5 0F 85 ? ? ? ? 83 BB",
			SigFollowMode::None
#else
			"83 EC 0C 89 F3 8B 7D ? FF 30 E8 ? ? ? ? 83 C4 10 83 FF 01 77 ? 84 C0 75 ? 80 7D ? 00 74 ? 80 7D ? 00 0F 84 ? ? ? ? 8D 65",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#endif
		};
		Pattern_t GetAppOwnershipTicketExtendedData
		{
			"IClientUser::GetAppOwnershipTicketExtendedData",
#if defined(__x86_64__)
			// Located by vtable index: this i386 method occupies slot 105 of the
			// IClientUser vtable, so slot 105 of the x86_64 CUser vtable is the
			// same method.  Confirmed structurally, not just positionally --
			// both bodies rebase `this` onto the enclosing object before calling
			// through (`sub eax,0x18b0` vs `sub rdi,0x1f88`).
			//
			// The `sub rdi,<layout offset>` constant is part of this build's
			// class layout, the same way the i386 arm's whole shape is.
			"41 57 48 81 EF 88 1F 00 00",
			SigFollowMode::None
#else
			"83 EC 24 FF 74 24 ? 8B 44 24",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
#endif
		};
		Pattern_t IsUserSubscribedAppInTicket
		{
			"IClientUser::IsUserSubscribedAppInTicket",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Called by CUser(259) RTTI vtable slot 118 (0x1832de0) after
			// `lea rdi, [rbx - 0x1f88]`, matching i386 slot 118 (0x1a5fdd0 -> 0x11ad7a0).
			// Proof 2: Contains assert "steamID == GSteamEngine().GetTicketSteamID(...)"
			// in ../clientdll/baseuser.cpp and scans the 0xb8-stride ticket vector.
			// Unique at 16/48 tokens.
			"41 57 41 56 41 55 41 54 55 53 48 83 EC 18 89 4C 24 0C 8B 8F ? ? 00 00 "
			"85 C9 0F 8E ? ? ? ? 49 89 F6 48 89 FD 49 89 D5 31 DB BE 02 00 00 00",
			SigFollowMode::None
#else
			"E8 ? ? ? ? 89 C3 83 C4 20 8B ? ? ? ? ? 8B",
			SigFollowMode::Relative
#endif
		};
		// The tail thunk adjusts the interface `this` to the implementation
		// pointer with `sub eax,<off>` (`2D ?? ?? 00 00`).  That offset is a
		// layout constant that drifted 0x18d8 -> 0x18d4 on the 2026-06-23
		// client; it is only part of the LOCATION signature (SLSsteam hooks the
		// function, it does not use the offset value), so it is masked here to
		// self-heal across that drift.  The long surrounding tail keeps the
		// match unique — verified exactly 1 hit in both the pre- and
		// post-2026-06-23 steamclient.so.
		Pattern_t RequiresLegacyCDKey
		{
			"IClientUser::RequiresLegacyCDKey",
#if defined(__x86_64__)
			// Slot 86 by the same vtable-index correspondence as
			// GetAppOwnershipTicketExtendedData.  An eleven-byte thunk: rebase
			// `this` onto the enclosing object, then tail-call the real body.
			// The trailing `41 57 41 56 41 89 F6` is the entry of the target it
			// jumps to, which keeps the match unique -- the `sub rdi` alone is
			// not.
			"48 81 EF 88 1F 00 00 E9 ? ? ? ? "
			"CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC "
			"41 57 41 56 41 89 F6",
			SigFollowMode::None
#else
			"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D ? ? 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
#endif
		};
	}

	namespace IClientUGC
	{
		Pattern_t RunIPCFrame
		{
			"IClientUGC::RunIPCFrame",
#if defined(__x86_64__)
			// x86-64 tail: call rel32; mov eax,[rbp+disp32]; cmp eax,imm32.
			// The 32-bit `83 C4 10` (add esp) is absent -- x86-64 callee cleans.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 56 0C D2 71",
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 62 0C D2 71",
#endif
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
		};
	}

	namespace IClientUserStats
	{
		Pattern_t RunIPCFrame
		{
			"IClientUserStats::RunIPCFrame",
#if defined(__x86_64__)
			// The dispatch tail, in the same shape every other RunIPCFrame arm
			// uses, so autoResolveIpcFrameRoots keeps working: the last four
			// tokens ARE the volatile root id and nothing else.  (An entry-based
			// signature resolves statically but fails in the live client -- the
			// resolver rewrites the trailing four tokens, which in an entry
			// signature are the frame size, corrupting it.)
			//
			// x86-64 tail: E8 rel32 | 8B 85 disp32 | 3D imm32, root at +12.
			// Identified by dispatch-root count: 55 distinct roots, and
			// IClientUserStatsMap is the only RTTI map with 55 slots.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 6B D6 E5 85",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#else
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 8F 65 6D 87",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#endif
		};
	}

	namespace CPackageInfoCache
	{
		Pattern_t LoadPackage
		{
			"CPackageInfoCache::LoadPackage",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Contains assert "Corrupt data in buffer for package %u, CLN %u,"
			// in ../clientdll/appdatacache.cpp; called by CPackageInfoCache::ReadFromDisk
			// (0x1036c00).
			// Proof 2: Populates PackageInfo (AppIdVec at +0x40, DepotIdVec at +0x58)
			// via CUtlMemory<uint32_t>::Grow (0x102d4f0).  Unique at 20/47 tokens.
			"41 57 41 56 49 89 F6 41 55 41 54 55 53 48 89 FB 48 81 EC 18 01 00 00 "
			"4C 8B 25 ? ? ? ? 89 54 24 14 48 63 69 10 64 4C 8B 3C 25 28 00 00 00",
			SigFollowMode::None
#else
			"E8 ? ? ? ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 95 ? ? FF FF 8B 7A 18 83 FF FF",
			SigFollowMode::Relative
#endif
		};
	}

	namespace CUtlMemory
	{
		Pattern_t Grow
		{
			"CUtlMemory::Grow",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Call site 0x1036491 inside CPackageInfoCache::LoadPackage
			// (0x10361a0) growing PackageInfo::AppIdVec ([rbx+0x40], size at [rbx+0x50]).
			// Proof 2: Target 0x102d4f0 has 101 callers (matching i386 99 callers), uses
			// 4-byte element stride (`mov ecx, 4; lea rsi, [rax*4]`) and asserts at
			// ../public/tier1/utlmemory.h line 0x1fa (identical to i386 0x116eb80).
			// Anchoring at the LoadPackage call site disambiguates the dozens of
			// identical 4-byte CUtlMemory<T>::Grow copies; unique at 12/26 tokens.
			"E8 ? ? ? ? 8B 43 50 8D 70 01 EB 82 66 90 48 8B 6B 30 C7 43 68 00 00 00 00",
			SigFollowMode::Relative
#else
			"E8 ? ? ? ? 8B 85 ? ? FF FF 83 C4 10 8B 40 44 89 85 ? ? FF FF 83 C0 01 E9",
			SigFollowMode::Relative
#endif
		};
	}

	namespace CDepotDownloadMgr
	{
		// Two cooperating hook points in CDepotDownloadMgr, both with the
		// same 7-dword signature (context, ., appId, depotId, uint64 gid, .)
		// and both self-contained PIC functions.
		//
		// (1) ProcessDepotManifest (the manifest-acquisition LEAF, 5 callers):
		//     builds "<root>/depotcache/<depot>_<gid>.manifest" from its gid
		//     arg, checks it on disk, and only calls BYldRequestDepotManifest
		//     when missing.  Redirecting the gid here makes the on-disk check
		//     find the locally-staged (zip) manifest and SKIP the request-code
		//     fetch — this is what lets a providers-down install proceed past
		//     "No internet connection".  PIC get_pc_thunk is the FIRST insn, so
		//     fixPICThunkCall must repair the relocated thunk in the tramp.
		//
		// (2) PrepareDepotDownload (one of the 5 callers, a LATER pipeline
		//     stage): after calling the leaf it looks the depot up in the
		//     per-download table BY the gid it was called with and derefs the
		//     per-manifest state pointer.  If the leaf was redirected to the
		//     zip gid but this frame still looks up the public gid -> miss ->
		//     NULL deref -> SIGSEGV at Reconfiguring (core-dump confirmed).
		//     Redirecting the
		//     gid here too keeps the leaf call and the table lookup consistent.
		//     PIC get_pc_thunk is at +5 (after the 5-byte prologue we relocate)
		//     so fixPICThunkCall is a harmless no-op for this hook point.
		//
		// Both verified: 1 match each in .text.
		Pattern_t ProcessDepotManifest
		{
			"CDepotDownloadMgr::ProcessDepotManifest",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x1059d00).
			// Proof 1: Only function referencing "%s/depotcache/%d_%llu.manifest"
			// (at 0x1059ef3) and calling CDepotDownloadMgr::BYldRequestDepotManifest
			// (0x1398b50), matching i386 0x117b010.
			// Proof 2: Exact 4 external callers (0x105a1e0, 0x10e3d30, 0x11d41c0,
			// 0x11d45a0), matching i386's 4 external callers (0x117b550, 0x12277e0,
			// 0x132c080, 0x132e480), and inserts/looks up manifestId (r8) in
			// [rdi+0x48] RB-tree ([rdi+0x60] root, [rdi+0x78] base).
			// Unique at 20/57 tokens.
			"41 57 41 56 41 89 CE 41 55 41 54 49 89 FC 55 53 4C 89 C3 48 83 EC 38 48 89 34 24 89 54 24 0C 4C 89 4C 24 10 64 48 8B 04 25 28 00 00 00 48 89 44 24 28 48 63 47 60 83 F8 FF",
#else
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 55 1C 89 45 C0 8B 45 18 89 55 CC 89 45 C8",
#endif
			SigFollowMode::None
		};

		Pattern_t PrepareDepotDownload
		{
			"CDepotDownloadMgr::PrepareDepotDownload",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x105a1e0).
			// Proof 1: Directly calls CDepotDownloadMgr::ProcessDepotManifest (0x1059d00
			// at 0x105a20b), then looks up manifestId (r8) in [rbx+0x60]/[rbx+0x78]
			// RB-tree and polls state [rbp+0x14] == 0x16 -> 1, matching i386 0x117b550.
			// Proof 2: Exact 2 external callers (0x108b0b0 and 0x108b440, called by
			// CDepotDownloadMgr::BuildDepotDependency 0x11d6040), matching i386
			// 0x117b550's 2 external callers (0x11bd3b0, 0x11bd720).
			// Unique at 25/55 tokens.
			"41 57 41 56 41 55 41 54 55 53 48 89 FB 48 83 EC 78 48 89 74 24 08 89 4C 24 04 64 4C 8B 24 25 28 00 00 00 4C 89 64 24 68 4D 89 C4 E8 ? ? ? ? 48 63 43 60 83 F8 FF",
#else
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 60 8B 7D 08 8B 45 18 8B 55 1C FF 75 20 89 45 98 52 50 FF 75 14 89 55 9C FF 75 10 FF 75 0C 57 E8 ? ? ? ? 8B 47 4C 83 C4 20 83 F8 FF",
#endif
			SigFollowMode::None
		};

		// (3) BuildDepotDependency (the install-plan CONSUMER, located via a
		//     runtime stack trace).  The
		//     per-app planner: receives an already-built CUtlVector<DepotEntry>
		//     (arg2 = [ebp+0x10]; count @ +0xc, element base @ +0, stride 0x20)
		//     and, per entry, copies ManifestGid (+0x8) into the context's
		//     planned-gid vector (ctx+0x664) AND drives ProcessDepotManifest /
		//     the shared-depot handler.  Patching depots[i].ManifestGid here
		//     (before the original runs) is the LumaCore manifest-override
		//     point: it mutates the SOURCE the commit reads from, not the
		//     by-value gid the acquisition leaf gets (which the commit ignores).
		//     PIC get_pc_thunk is the FIRST insn (like ProcessDepotManifest) ->
		//     fixPICThunkCall repairs the relocated thunk in the tramp.
		//     Verified: 1 match in .text (entry VA 0x11413e0 on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		//     The 2026-10 client (1791918634) shrank the frame 0x48c -> 0x43c
		//     and reassigned the argument registers; it now copies the vector
		//     before planning, after this hook has filtered it.  Same callers
		//     (four flag-0/flag-1 pairs), same ABI and DepotEntry layout.  The
		//     neighbouring yielding variant kept the old 0x48c frame and is
		//     NOT this function.  One match at entry VA 0x132bb60.
		Pattern_t BuildDepotDependency
		{
			"CDepotDownloadMgr::BuildDepotDependency",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Only function (0x11d6040) carrying "Failed to initialize depot %u,
			// manifest %llu (%s)", "count >= 0", and "../public/tier1/utlvector.h",
			// matching i386 0x132bb00. Copies a 0x20-stride DepotEntry CUtlVector ([rdx+0x10]
			// count, [rdx] base) and marks [rdi+0x1c]=1.
			// Proof 2: Exact 4 callers x 2 call sites = 8 calls (4 flag=0 via xor esi,esi;
			// 4 flag=1 via mov esi,1): 0x10ff6a0, 0x1106180, 0x137c6e0, 0x177e6b0,
			// matching i386 callers 0x1248470, 0x124dab0, 0x14e5d20, 0x194c180.
			// Masks stack frame size (0x448) and canary spill offset. Unique at 18/51 tokens.
			"41 57 41 56 41 55 41 54 41 89 F4 55 48 89 FD 53 48 81 EC ? ? 00 00 64 4C 8B 2C 25 28 00 00 00 4C 89 AC 24 ? ? 00 00 49 89 CD C6 47 1C 01 44 8B 72 10",
#else
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 3C 04 00 00 8B 5D 08 8B 75 14 89 85 ? ? FF FF 8B 45 0C",
#endif
			SigFollowMode::None
		};

		// Structured chunk-completion callbacks. Steam reaches these before it
		// formats the content_log line, with the unpack result still available as
		// the final argument. There are two compiler-generated ABI variants for
		// the same body: ordinary cdecl and regparm(3). Both are optional and
		// independently hooked; a future signature drift disables quarantine
		// detection rather than affecting the download pipeline.
		Pattern_t OnChunkUnpackedStack
		{
			"CDepotDownloadMgr::OnChunkUnpacked[cdecl]",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 7C 04 00 00 8B 45 0C 8B 7D 08 89 85 90 FB FF FF 8B 45 10 89 85 8C FB FF FF",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedStack"
		};

		Pattern_t OnChunkUnpackedReg
		{
			"CDepotDownloadMgr::OnChunkUnpacked[regparm3]",
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 89 C6 53 81 EC 7C 04 00 00 8B 45 0C 89 95 90 FB FF FF 89 8D 8C FB FF FF 89 85 94 FB FF FF",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedReg"
		};

		// (4) EvaluateConfigChanges (the post-commit reconcile, located via
		//     static RE).  Emits the
		//     content_log "AppID %u ...config changed : added/removed/updated
		//     depots %s" lines and decides "Update Required".  It diffs the
		//     app's installed depot vector (ptr @ ctx+0x78, count @ ctx+0x84,
		//     stride 0x20, ManifestGid @ +0x8) against an appinfo-derived
		//     target list, flagging a depot "updated" when the two gids differ
		//     (the movq/pxor compare at VA 0xfe4598).  THIS is what perpetually
		//     re-flags a downgraded (pinned) install whose appinfo still
		//     carries the public gid -> the loop.  Hook target for the (B) fix.
		//     Calling convention: a global anchor/manager pointer comes in EAX
		//     (used as both the manager object and the PIC string anchor); the
		//     three args are on the stack (ctx @ ebp+0x8, appId @ ctx+0x8).
		//     The prologue's first insn is `push ebp` (NOT a get_pc_thunk), so
		//     no fixPICThunkCall is needed; a regparm(1) detour preserves EAX.
		//     Verified: 1 match in .text (entry VA 0xfe425a on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		Pattern_t EvaluateConfigChanges
		{
			"CDepotDownloadMgr::EvaluateConfigChanges",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463, entry 0x108eb70).
			// Proof 1: Only function referencing "AppID %u %sconfig changed : added
			// depots %s", "updated depots %s", and "removed depots %s" (at 0x108f6d5..
			// 0x108f793), matching i386 0x11c460a.
			// Proof 2: Exact 8 external callers (0x1049100, 0x104b160, 0x104b880,
			// 0x108f9f0, 0x108fbf0, 0x1093760, 0x1107860, 0x11c36a0), matching i386's
			// 8 callers; takes (ctx, mgr, a2) in (rdi, rsi, edx), reads ctx->flags
			// (+0x08) and ctx->depots (+0xc0, stride 0x20), and calls the 8-arg
			// target-vector builder 0x1052dc0 at +0x19e.
			// Unique at 30/67 tokens.
			"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 89 95 ? ? FF FF 8B 57 08 48 89 B5 ? ? FF FF 64 48 8B 04 25 28 00 00 00 48 89 45 C8 31 C0 41 89 D4 41 83 E4 01 75 ? 83 BF ? ? 00 00 19",
#else
			"55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04",
#endif
			SigFollowMode::None
		};
	}

	namespace IClientFriends
	{
		Pattern_t GetFriendGamePlayed
		{
			"IClientFriends::GetFriendGamePlayed",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: 17IClientFriendsMap RTTI vtable (0x30df258, 246 slots) slot 48
			// = 0x1440f50.
			// Proof 2: Writes IPC method hash 0x335bf1a2 (`C7 44 24 08 A2 F1 5B 33`)
			// at 0x1440fc7, matching i386 0x1775660 (`C7 45 ? A2 F1 5B 33`).
			// Unique at 6/20 tokens; PrologueUpwards walks to 0x1440f50.
			"C7 44 24 08 A2 F1 5B 33 E8 ? ? ? ? 4C 89 E6 48 89 DF E8",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x55, 0x54, 0x41, 0x00, 0x00, 0x00, 0x20, 0xBA, 0xC9, 0x31, 0x55, 0x41 }
#else
			"C7 45 ? A2 F1 5B 33 6A 04 50 57 E8 ? ? ? ?",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xE5, 0x89, 0x55 }
#endif
		};
	}

	namespace IClientUtils
	{
		Pattern_t RunIPCFrame
		{
			"IClientUtils::RunIPCFrame",
#if defined(__x86_64__)
			// Dispatch tail, same contract as the UserStats arm above: the last
			// four tokens are the volatile root and nothing else, so
			// autoResolveIpcFrameRoots can rewrite them safely.
			//
			// Identified by dispatch-root count: 118 distinct roots, and
			// IClientUtilsMap is the only RTTI map with 118 slots.  The i386 arm
			// has no x86-64 analogue (it anchors mid-body) but the tail shape is
			// the same as every other dispatcher here.
			"E8 ? ? ? ? 8B 85 ? ? ? ? 3D 5E 86 40 82",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#else
			"83 EC 08 89 F3 50 57 E8 ? ? ? ? 58 FF B5 ? ? ? ? E8 ? ? ? ? 58 8D 45",
			SigFollowMode::PrologueUpwards,
			kIpcPrologue
#endif
		};
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
#if defined(__x86_64__)
			// The pipe-index accessor is one of three sibling CSteamEngine helpers
			// that are byte-identical in size (0x55) and shape: index an intrusive
			// red-black tree on a container offset, compare against the pipe index,
			// and return the matched node's field.  They differ only in the
			// container offsets.
			//
			// m_PipeIndex is the only one whose index member sits at a NEGATIVE
			// displacement (-0x1028 here; -0xc8c on i386) because it lives before
			// the sub-object the caller holds.  That sign is the discriminator:
			// keying on the `FF FF` high half of the disp32 selects this
			// instruction alone, where matching the surrounding shape selects all
			// three.  The low two disp bytes stay wildcarded so a class-layout
			// shift does not break it.
			"8B B7 ? ? FF FF 83 F8 FF 74 ? 48 8B 8F",
			SigFollowMode::None,
			nullptr,
			"Patterns::IClientUtils::Offset_GetPipeIndex"
#else
			"8B 91 ? ? ? ? 83 F8 FF 74 ? 8B 89 ? ? ? ? EB ? ? ? ? 8B 00 83 F8 FF 74 ? 8D 04 ? 8D 04 ? 3B 50",
			SigFollowMode::None,
			nullptr,
			"Patterns::IClientUtils::Offset_GetPipeIndex"
#endif
		};
	}

	namespace ISteamMatchmakingPingResponse
	{
		Pattern_t ServerResponded
		{
			"ISteamMatchmakingPingResponse::ServerResponded",
#if defined(__x86_64__)
			// Measured against steamrt64/steamui.so (build 57d58821).
			// Proof 1: 15CGameInfoDialog RTTI vtable (0x28a0690) slot 5 = 0x18c13e0
			// (followed by ServerFailedToRespond at slot 6 = 0x18c0fe0), matching
			// i386 15CGameInfoDialog (0x27b6ef0) slot 5 = 0x189bb90.
			// Proof 2: Checks [rdi+0x58] and server.m_NetAdr.m_unIP ([rbp+4]) equality.
			// Unique at 17/61 tokens.
			"41 55 41 54 55 53 48 89 FB 48 81 EC B8 00 00 00 8B 47 58 "
			"64 48 8B 2C 25 28 00 00 00 48 89 AC 24 A8 00 00 00 48 89 F5 "
			"85 C0 0F 84 ? ? ? ? 8B 45 04 85 C0 0F 84 ? ? ? ? 39 43 58",
			SigFollowMode::None,
			&g_modSteamUI
#else
			"8B 85 ? ? ? ? 8B 40 ? 85 C0 0F 84 ? ? ? ? 39 46",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x57, 0xe5, 0x89, 0x55 },
			&g_modSteamUI
#endif
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
#if defined(__x86_64__)
			"45 31 F6 83 7F",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
#else
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC AC 00 00 00 "
			"8B 45 10 8B 55 08 89 85 ? ? FF FF 89 95 ? ? FF FF "
			"65 A1 14 00 00 00",
			SigFollowMode::None,
#endif
		};
	}

	namespace CRemoteClientManager
	{
		Pattern_t RecvPkt
		{
			"CRemoteClientManager::RecvPkt",
#if defined(__x86_64__)
			// Entry.  r12/rbp/rbx saved, rbp := this (rdi), a 0x10 frame, then the
			// cached-global load and the %fs:0x28 canary pair.  The rip-relative
			// displacement is a separate translation unit's static, so it is
			// wildcarded; the canary pair keeps the match unique (the prefix alone
			// matches twice).
			"41 54 55 48 89 FD 53 48 83 EC 10 48 8B 05 ? ? ? ? "
			"64 4C 8B 24 25 28 00 00 00 4C 89 64 24 08 49 89 F4 8B 10",
			SigFollowMode::None
#else
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 1C "
			// [esi+disp32]: PIC-relative global slot whose displacement drifts
			// between Steam client builds (0x8B0 -> 0x8B4 on 1781041600; the
			// whole low word moved on a later beta).  Wildcard both low
			// displacement bytes so the match survives the shift; the two high
			// bytes stay 00 00 (the slot is well under 0x10000) and the rest of
			// the body keeps the match unique.
			"8B 86 ? ? 00 00 8B 00 85 C0 0F 85 ? ? ? ? "
			"C7 45 E4 00 00 00 00 83 EC 08 89 F3 6A 01 FF 75 0C "
			"E8 ? ? ? ? 89 C7 83 C4 10 85 C0 0F 84 ? ? ? ? 83 EC 0C 50",
			SigFollowMode::None
#endif
		};
	}

	namespace CJobMgr
	{
		Pattern_t BRouteMsgToJob
		{
			"CJobMgr::BRouteMsgToJob",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Contains assert strings "CJobMgr::BRouteMsgToJob() - continue job"
			// and "CJobMgr::BRouteMsgToJob() - job" in ../common/jobmgr.cpp; 7 callers
			// (matching i386 0x278a3a0's 6 callers).
			// Proof 2: Identical 5-arg prologue saving (rdi, rsi, rdx, rcx, r8d) and
			// comparing [pMsg+0x10] against [rax+0x68].  Unique at 24/51 tokens.
			"55 48 89 E5 41 57 41 56 41 55 49 89 D5 41 54 53 48 81 EC 98 00 00 00 "
			"48 89 BD 70 FF FF FF 48 89 B5 68 FF FF FF 48 89 8D 78 FF FF FF 44 89 85 64 FF FF FF",
			SigFollowMode::None
#else
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 7C "
			"8B 45 08 8B 4D 14 89 45 90 8B 45 0C 89 4D A0 89 45 88 "
			"8B 45 10 89 45 A4 65 8B 0D 14 00 00",
			SigFollowMode::None
#endif
		};
	}

	namespace CDepotDownloadMgr
	{
		Pattern_t BYldRequestDepotManifest
		{
			"CDepotDownloadMgr::BYldRequestDepotManifest",
#if defined(__x86_64__)
			// Measured against steamrt64/steamclient.so (build a57ea463).
			// Proof 1: Contains assert strings "CDepotDownloadMgr::BYldRequestDepotManifest:
			// same manifest %llu requested twice" and "...Failed to get manifest request code"
			// in ../clientdll/depotdownloadmgr.cpp.
			// Proof 2: Preserves (pthis, appId, depotId, manifestId, branch, arg20) ABI
			// with `lea rax, [rcx - 1]; cmp rax, -3` manifestId guard.  Unique at 8/50 tokens.
			"41 57 48 8D 41 FF 41 89 D7 41 56 41 55 41 54 4D 89 C4 55 48 89 CD 53 48 89 FB "
			"48 81 EC 98 00 00 00 48 83 F8 FD 89 74 24 0C 64 4C 8B 34 25 28 00 00 00",
			SigFollowMode::None
#else
			"55 B9 FD FF FF FF 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 83 EC 7C 8B 45 14 8B 55 18",
			SigFollowMode::None
#endif
		};
	}

	std::vector<Pattern_t*>& patterns()
	{
		// Function-local static: guaranteed initialized on first call,
		// which happens from the first Pattern_t constructor above —
		// before init() ever iterates it.  Immune to static-init order.
		static std::vector<Pattern_t*> instance;
		return instance;
	}
}
