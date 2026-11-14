# Build glue for slsteam-moon.
#
# Common invocations:
#   make             Build bin/SLSsteam.so + bin/library-inject.so on the host.
#                    Use scripts/build.sh --portable for a release-grade
#                    binary with broader glibc compatibility.
#   make clean       Remove all build artefacts.
#   make install     Install onto the host (delegates to setup.sh install).
#   make release     Build portable + package dist/slsteam-moon-linux-<ver>.zip.
#                    Same as scripts/release.sh.
#
# -MMD/-MP keep dependency files in sync so header edits trigger
# recompilation, see https://stackoverflow.com/q/52034997.

# Force g++; clang miscompiles a few hooks.
CXX := g++
.DEFAULT_GOAL := all

# Target architecture selection.
# Default to 32-bit x86 (i386) so bare `make` keeps targeting Steam's 32-bit
# ubuntu12_32 client on x86_64 hosts; pass ARCH=x86_64 (or CROSS_HOST=X86_64)
# for 64-bit steamrt64, or ARCH=aarch64 (or CROSS_HOST=AArch64) for AArch64.
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
	ARCH ?= aarch64
else
	ARCH ?= i386
endif

ifeq ($(shell echo $$CROSS_HOST),X86_64)
	CXX := x86_64-linux-gnu-g++
	ARCH := x86_64
endif
ifeq ($(shell echo $$CROSS_HOST),AArch64)
	CXX := aarch64-linux-gnu-g++
	ARCH := aarch64
endif

MULTILIB :=
ARCH_DEFINES :=
ifneq (,$(filter i386 i486 i586 i686 x86 x86_32,$(ARCH)))
	MULTILIB := -m32
else ifeq ($(ARCH),x86_64)
	ARCH_DEFINES := -DX86_64
else ifeq ($(ARCH),aarch64)
	ARCH_DEFINES := -DAARCH64
endif

ifeq ($(ARCH),x86_64)
	STEAMCLIENT ?= $(HOME)/.local/share/Steam/ubuntu12_64/steamclient.so
	STEAMUI ?= $(HOME)/.local/share/Steam/ubuntu12_64/steamui.so
else
	STEAMCLIENT ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamclient.so
	STEAMUI ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamui.so
endif

ifneq ($(MULTILIB),)
	LIBDIR := $(if $(wildcard lib/$(ARCH)/*.a),lib/$(ARCH),lib)
else
	LIBDIR := lib/$(ARCH)
endif

LIBPROTOBUF := $(LIBDIR)/libprotobuf-lite.a
LIBYAML := $(LIBDIR)/libyaml-cpp.a
libs := $(sort $(wildcard $(LIBDIR)/*.a) $(if $(filter x86_64,$(ARCH)),$(LIBPROTOBUF)))

OBJDIR := obj/$(ARCH)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=$(OBJDIR)/%.o)
deps := $(objs:%.o=%.d)

# -fno-reorder-blocks-and-partition: keep exception landing pads in the hot
# partition. With block partitioning (implied at -O2+), a throw's handler can
# land in the ".cold" section and the call-site table fails to route to it, so
# even catch(...) is bypassed and the client aborts (see config.hpp / the
# FakeWalletBalance + malformed-config boot loops). We avoid throwing across
# this boundary in the config path regardless, but pinning the landing pads
# makes every remaining try/catch a reliable backstop.
CXXFLAGS := -O3 -flto=auto -fPIC $(MULTILIB) -std=c++20 \
            -fno-reorder-blocks-and-partition \
            -Wall -Wextra -Wpedantic -Wno-error=format-security \
            -D_GLIBCXX_USE_CXX11_ABI=0 $(ARCH_DEFINES)

LDFLAGS := -shared -Wl,--no-undefined -lpthread -ldl

ifeq ($(shell echo $$NATIVE),1)
	CXXFLAGS += -march=native
endif

# Optional speed-ups picked up if installed.
ifeq ($(shell type ccache &> /dev/null && echo "found"),found)
	export PATH := /usr/lib/ccache/bin:$(PATH)
endif
ifeq ($(shell type mold &> /dev/null && echo "found"),found)
	LDFLAGS += -fuse-ld=mold
endif

.PHONY: all build rebuild clean install release test-ascii test-cmwire test-cmclient-loader test-clientcompat test-dlcids test-dlc-scope test-dlc-metadata test-config-path test-config-discovery test-filewatcher-burst test-provision-notice test-mtvar-contains test-synthmark test-appinfostate test-pattern-catalog test-pattern-cache test-pattern-refresh test-process-lock test-process-runtime test-atomic-file test-cache-pair test-appinfo-transaction test-appinfo-reload test-audit-symbols test-audit-policy test-memhlp-target test-memhlp-prologue test-memhlp-pic test-utils-sha test-provision-cache test-provision-refresh test-provision-result test-provision-terminal test-pending-proton test-provision-schedule test-provision-pass test-runtime-dependencies test-thread-start test-steamstub-warmup test-steamstub-launchopt test-boundedexecutor test-steamless-prewarm test-depotkey-scope test-curl-timeout test-contentserverdirectory test-manifest-index test-manifestselection test-manifeststore-io test-hotreload-inputs test-hotreload-package test-ownerqueue test-hotreload-capabilities test-libraryremoval test-pics test-prewarm-backoff test-yaml-runtime test-manifestpin-patterns test-optional-locators test-patternscan test-manifest-zip test-fakeappid-presence test-fakeappid-wiring test-ticket test-familyshare test-familyshare-wiring
.NOTPARALLEL: clean rebuild
.PHONY: test-library-dates test-library-dates-hook
.PHONY: test-achievements test-achievement-scope test-stats-audit
.PHONY: test-stats-provenance
.PHONY: test-manifestdonor test-manifestfetch
.PHONY: test-reconcilepin-scan
.PHONY: test-depotkey-recv-policy

test-manifestdonor:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_manifestdonor.cpp -o /tmp/test_manifestdonor
	/tmp/test_manifestdonor

test-manifestfetch:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_manifestfetch.cpp -o /tmp/test_manifestfetch
	/tmp/test_manifestfetch

test-stats-provenance:
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_stats_provenance.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_stats_provenance
	/tmp/test_stats_provenance

test-familyshare: $(OBJDIR)/sdk/protobufs/steammessages_base.pb.o $(LIBPROTOBUF)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_familyshare.cpp \
		src/sdk/CNetPacket.cpp $^ \
		-Wl,--gc-sections -pthread -o /tmp/test_familyshare
	/tmp/test_familyshare

test-familyshare-wiring:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_familyshare_wiring.cpp -o /tmp/test_familyshare_wiring
	/tmp/test_familyshare_wiring

test-manifest-zip:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_manifest_zip.cpp src/utils/manifest_zip.cpp -ldl \
		-o /tmp/test_manifest_zip
	/tmp/test_manifest_zip

test-achievements:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread tools/test_achievements.cpp -o /tmp/test_achievements
	/tmp/test_achievements

test-achievement-scope: $(OBJDIR)/sdk/protobufs/steammessages_clientserver_userstats.pb.o $(OBJDIR)/sdk/protobufs/steammessages_base.pb.o $(LIBPROTOBUF)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		tools/test_achievement_scope.cpp src/feats/achievements.cpp src/feats/stats_policy.cpp \
		$^ -pthread -o /tmp/test_achievement_scope
	/tmp/test_achievement_scope

test-fakeappid-presence: $(OBJDIR)/sdk/protobufs/encrypted_app_ticket.pb.o \
                          $(OBJDIR)/sdk/protobufs/steammessages_base.pb.o \
                          $(OBJDIR)/sdk/protobufs/steammessages_clientserver.pb.o \
                          $(OBJDIR)/sdk/protobufs/steammessages_clientserver_2.pb.o $(LIBPROTOBUF)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_fakeappid_presence.cpp \
		src/feats/fakeappid.cpp src/sdk/CNetPacket.cpp $^ \
		-Wl,--gc-sections -pthread -o /tmp/test_fakeappid_presence
	/tmp/test_fakeappid_presence

test-fakeappid-wiring:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_fakeappid_wiring.cpp -o /tmp/test_fakeappid_wiring
	/tmp/test_fakeappid_wiring

test-ticket:
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -Werror -I include -I src \
		tools/test_ticket.cpp -o /tmp/test_ticket
	/tmp/test_ticket

test-process-runtime:
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic \
		-ffunction-sections -fdata-sections -I include -I src \
		tools/test_process_runtime.cpp src/process.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_process_runtime
	/tmp/test_process_runtime

test-stats-audit: bin/SLSsteam.so
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -rdynamic tools/test_stats_audit.cpp -ldl -o /tmp/test_stats_audit
	/tmp/test_stats_audit
	LD_AUDIT=$(CURDIR)/bin/SLSsteam.so /tmp/test_stats_audit audit

all: build
build: bin/SLSsteam.so bin/library-inject.so bin/pattern-refresh
rebuild: clean build

# Native x86_64 build of protobuf-lite from the 29 vendored lite-runtime
# sources in include/google/protobuf/ (matching 3.15.3 / pre-C++11 ABI).
PROTOBUF_LITE_SRCS := \
	include/google/protobuf/any_lite.cc \
	include/google/protobuf/arena.cc \
	include/google/protobuf/arenastring.cc \
	include/google/protobuf/extension_set.cc \
	include/google/protobuf/generated_enum_util.cc \
	include/google/protobuf/generated_message_table_driven_lite.cc \
	include/google/protobuf/generated_message_util.cc \
	include/google/protobuf/implicit_weak_message.cc \
	include/google/protobuf/io/coded_stream.cc \
	include/google/protobuf/io/io_win32.cc \
	include/google/protobuf/io/strtod.cc \
	include/google/protobuf/io/zero_copy_stream.cc \
	include/google/protobuf/io/zero_copy_stream_impl.cc \
	include/google/protobuf/io/zero_copy_stream_impl_lite.cc \
	include/google/protobuf/map.cc \
	include/google/protobuf/message_lite.cc \
	include/google/protobuf/parse_context.cc \
	include/google/protobuf/repeated_field.cc \
	include/google/protobuf/stubs/bytestream.cc \
	include/google/protobuf/stubs/common.cc \
	include/google/protobuf/stubs/int128.cc \
	include/google/protobuf/stubs/status.cc \
	include/google/protobuf/stubs/statusor.cc \
	include/google/protobuf/stubs/stringpiece.cc \
	include/google/protobuf/stubs/stringprintf.cc \
	include/google/protobuf/stubs/structurally_valid.cc \
	include/google/protobuf/stubs/strutil.cc \
	include/google/protobuf/stubs/time.cc \
	include/google/protobuf/wire_format_lite.cc

PROTOBUF_LITE_OBJS := $(PROTOBUF_LITE_SRCS:include/google/protobuf/%.cc=$(OBJDIR)/protobuf-lite/%.o)
PROTOBUF_LITE_CXXFLAGS := -O2 -fPIC $(MULTILIB) -std=c++20 \
	-D_GLIBCXX_USE_CXX11_ABI=0 -DHAVE_PTHREAD $(ARCH_DEFINES) -isystem include

$(OBJDIR)/protobuf-lite/%.o: include/google/protobuf/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(PROTOBUF_LITE_CXXFLAGS) -c $< -o $@

lib/x86_64/libprotobuf-lite.a: $(PROTOBUF_LITE_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# bin/ is shared by both architectures, so a plain timestamp check would happily
# keep an i386 SLSsteam.so after `make ARCH=x86_64` (and vice versa).  Every
# released artifact therefore depends on a stamp naming the arch it was built
# for; switching ARCH invalidates the stamp and forces a relink.  The stamp
# lives under obj/$(ARCH) so `make clean` still removes everything.
ARCH_STAMP := $(OBJDIR)/.arch
$(ARCH_STAMP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s' '$(ARCH)' | cmp -s - $@ || printf '%s' '$(ARCH)' > $@

bin/SLSsteam.so: $(objs) $(libs) $(ARCH_STAMP)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(objs) $(libs) -o $@ $(LDFLAGS)

# Audit-side helper that redirects libcurl loading to a system copy.
# Loaded ahead of SLSsteam.so via $LD_AUDIT.
bin/library-inject.so: tools/library-inject/main.cpp $(ARCH_STAMP)
	@mkdir -p bin
	$(CXX) -O3 $(MULTILIB) $(ARCH_DEFINES) -fPIC -shared -std=c++20 $< -o $@

.PHONY: FORCE
FORCE:

PATTERN_PUBLIC_KEY_HEX = $(shell tr -d '[:space:]' < res/pattern-public-key.hex 2>/dev/null)

# Pre-launch metadata refresher. This is a native host executable, not a
# Steam-loaded i386 object: HTTPS, Ed25519, and cache I/O finish before Steam
# starts and never run in LD_AUDIT preinit.  It is deliberately NOT stamped by
# ARCH -- it always builds for the host, so both arch builds may share it.
bin/pattern-refresh: tools/pattern-refresh/main.cpp \
                     tools/pattern-refresh/catalog.cpp \
                     tools/pattern-refresh/catalog.hpp \
                     src/pattern_catalog.cpp src/pattern_catalog.hpp \
                     res/pattern-public-key.hex
	@test -n "$(PATTERN_PUBLIC_KEY_HEX)" || { echo "missing pattern public key" >&2; exit 1; }
	@mkdir -p bin
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Wpedantic \
		-I src -I tools/pattern-refresh \
		-DPATTERN_PUBLIC_KEY_HEX='"$(PATTERN_PUBLIC_KEY_HEX)"' \
		tools/pattern-refresh/main.cpp tools/pattern-refresh/catalog.cpp \
		src/pattern_catalog.cpp -lcurl -lcrypto -lpthread -o $@

-include $(deps)
$(OBJDIR)/update.o: src/update.cpp res/version.txt
	$(shell ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
$(OBJDIR)/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
$(OBJDIR)/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean:
	rm -rf obj/ bin/ dist/

install:
	sh setup.sh install

# Unit test for cmwire.hpp's protobuf-backed helpers (Task 3).  Links the
# already-compiled protobuf objects + libprotobuf-lite.a, matching the
# main build's 32-bit + pre-C++11 ABI so the static lib is compatible.
# The pure framing tests (Tasks 1-2) also build standalone without this:
#   g++ -std=c++20 -I include tools/test_cmwire.cpp -o /tmp/test_cmwire
test-cmwire: $(OBJDIR)/sdk/protobufs/steammessages_base.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver_login.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver_2.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver_friends.pb.o \
             $(OBJDIR)/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
             $(OBJDIR)/sdk/protobufs/encrypted_app_ticket.pb.o $(LIBPROTOBUF)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -DCMWIRE_PROTOBUF \
		-I include tools/test_cmwire.cpp $^ \
		-lpthread -o /tmp/test_cmwire
	/tmp/test_cmwire

test-cmclient-loader:
	$(CXX) -std=c++20 -ffunction-sections -fdata-sections -I include -I src \
		tools/test_cmclient_loader.cpp src/feats/cmclient.cpp \
		-Wl,--gc-sections -ldl -pthread -o /tmp/test_cmclient_loader
	/tmp/test_cmclient_loader

test-clientcompat:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_clientcompat.cpp src/sdk/IClientCompat.cpp \
		-o /tmp/test_clientcompat
	/tmp/test_clientcompat

test-dlcids:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_dlcids.cpp -o /tmp/test_dlcids
	/tmp/test_dlcids

test-ascii:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_ascii.cpp -o /tmp/test_ascii
	/tmp/test_ascii

test-dlc-scope:
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -I src -I include \
		tools/test_dlc_scope.cpp src/feats/dlc.cpp \
		src/feats/managed_dlc_scope.cpp -pthread \
		-o /tmp/test_dlc_scope
	/tmp/test_dlc_scope

test-dlc-metadata: $(LIBYAML)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -I include -I src \
		tools/test_dlc_metadata.cpp src/feats/dlc_metadata.cpp \
		$(LIBYAML) -o /tmp/test_dlc_metadata
	/tmp/test_dlc_metadata

test-config-path:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_config_path.cpp -o /tmp/test_config_path
	/tmp/test_config_path

test-config-discovery:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_config_discovery.cpp -o /tmp/test_config_discovery
	/tmp/test_config_discovery
test-reconcilepin-scan:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_reconcilepin_scan.cpp -o /tmp/test_reconcilepin_scan
	/tmp/test_reconcilepin_scan

test-filewatcher-burst:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src \
		tools/test_filewatcher_burst.cpp -o /tmp/test_filewatcher_burst
	/tmp/test_filewatcher_burst

test-provision-notice:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_provision_notice.cpp -o /tmp/test_provision_notice
	/tmp/test_provision_notice

test-mtvar-contains:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src \
		tools/test_mtvar_contains.cpp -o /tmp/test_mtvar_contains
	/tmp/test_mtvar_contains

test-synthmark:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_synthmark.cpp -o /tmp/test_synthmark
	/tmp/test_synthmark

test-appinfostate:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src -I include \
		tools/test_appinfostate.cpp -o /tmp/test_appinfostate
	/tmp/test_appinfostate

test-pattern-catalog:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_pattern_catalog.cpp src/pattern_catalog.cpp \
		-o /tmp/test_pattern_catalog
	/tmp/test_pattern_catalog

test-pattern-cache:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_pattern_cache.cpp src/pattern_cache.cpp \
		-o /tmp/test_pattern_cache
	/tmp/test_pattern_cache

test-appinfo-reload:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_appinforeload.cpp -o /tmp/test_appinforeload
	/tmp/test_appinforeload

test-pattern-refresh:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I tools -I src \
		tools/test_pattern_refresh.cpp tools/pattern-refresh/catalog.cpp \
		src/pattern_catalog.cpp -lcurl -lcrypto -lpthread \
		-o /tmp/test_pattern_refresh
	/tmp/test_pattern_refresh

test-audit-symbols:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_audit_symbols.cpp -o /tmp/test_audit_symbols
	/tmp/test_audit_symbols

test-audit-policy:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_audit_policy.cpp -o /tmp/test_audit_policy
	/tmp/test_audit_policy

test-depotkey-scope:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_depotkey_scope.cpp -pthread -o /tmp/test_depotkey_scope
	/tmp/test_depotkey_scope

test-depotkey-recv-policy:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_depotkey_recv_policy.cpp -o /tmp/test_depotkey_recv_policy
	/tmp/test_depotkey_recv_policy

test-memhlp-target:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_target.cpp -o /tmp/test_memhlp_target
	/tmp/test_memhlp_target

test-memhlp-prologue:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_prologue.cpp -o /tmp/test_memhlp_prologue
	/tmp/test_memhlp_prologue

test-memhlp-pic:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_pic.cpp -o /tmp/test_memhlp_pic
	/tmp/test_memhlp_pic

test-utils-sha:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_utils_sha.cpp src/utils.cpp -o /tmp/test_utils_sha
	/tmp/test_utils_sha

test-provision-cache:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_cache.cpp -o /tmp/test_provision_cache
	/tmp/test_provision_cache

test-provision-refresh:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_refresh.cpp -o /tmp/test_provision_refresh
	/tmp/test_provision_refresh

test-provision-result:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_result.cpp -o /tmp/test_provision_result
	/tmp/test_provision_result

test-provision-terminal:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_terminal.cpp src/feats/provision_terminal.cpp \
		-o /tmp/test_provision_terminal
	/tmp/test_provision_terminal

test-pending-proton:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_pending_proton.cpp -o /tmp/test_pending_proton
	/tmp/test_pending_proton

test-provision-schedule:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_schedule.cpp -o /tmp/test_provision_schedule
	/tmp/test_provision_schedule

test-provision-pass:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_pass.cpp -pthread \
		-o /tmp/test_provision_pass
	/tmp/test_provision_pass

test-runtime-dependencies:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -I src \
		-c src/runtime_dependencies.cpp -o /tmp/runtime_dependencies.o
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -I src \
		tools/test_runtime_dependencies.cpp /tmp/runtime_dependencies.o \
		-Wl,--gc-sections -o /tmp/test_runtime_dependencies
	/tmp/test_runtime_dependencies

test-thread-start:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_thread_start.cpp -o /tmp/test_thread_start
	/tmp/test_thread_start

test-steamstub-warmup:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_steamstub_warmup.cpp -o /tmp/test_steamstub_warmup
	/tmp/test_steamstub_warmup

test-steamstub-launchopt:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_steamstub_launchopt.cpp -o /tmp/test_steamstub_launchopt
	/tmp/test_steamstub_launchopt

test-boundedexecutor:
	$(CXX) -std=c++20 -pthread -Wall -Wextra -Wpedantic -Werror \
		tools/test_boundedexecutor.cpp -o /tmp/test_boundedexecutor
	/tmp/test_boundedexecutor

test-steamless-prewarm:
	bash scripts/test-steamless-prewarm.sh

test-curl-timeout:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_curl_timeout.cpp src/curl.cpp -ldl -pthread \
		-o /tmp/test_curl_timeout
	/tmp/test_curl_timeout

test-contentserverdirectory: $(LIBYAML)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -Werror -I include -I src \
		tools/test_contentserverdirectory.cpp src/utils/contentserverdirectory.cpp \
		$(LIBYAML) -o /tmp/test_contentserverdirectory
	/tmp/test_contentserverdirectory

test-manifest-index:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_manifest_index.cpp -o /tmp/test_manifest_index
	/tmp/test_manifest_index

test-manifestselection:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_manifestselection.cpp -o /tmp/test_manifestselection
	/tmp/test_manifestselection

test-manifeststore-io:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_manifeststore_io.cpp -o /tmp/test_manifeststore_io
	/tmp/test_manifeststore_io

test-hotreload-inputs:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_hotreload_inputs.cpp src/feats/provision_terminal.cpp \
		-o /tmp/test_hotreload_inputs
	/tmp/test_hotreload_inputs

test-hotreload-package:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic \
		tools/test_hotreload_package.cpp -o /tmp/test_hotreload_package
	/tmp/test_hotreload_package

test-ownerqueue:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread \
		tools/test_ownerqueue.cpp -o /tmp/test_ownerqueue
	/tmp/test_ownerqueue

test-hotreload-capabilities:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_hotreload_capabilities.cpp -o /tmp/test_hotreload_capabilities
	/tmp/test_hotreload_capabilities

test-libraryremoval: $(OBJDIR)/feats/libraryremoval.o
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_libraryremoval.cpp -o /tmp/test_libraryremoval
	/tmp/test_libraryremoval
	@if nm -C $(OBJDIR)/feats/libraryremoval.o | grep -E 'google::protobuf|RepeatedField'; then \
		echo "library removal must not mutate Steam protobufs across the audit namespace" >&2; \
		exit 1; \
	fi

test-library-dates:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_library_dates.cpp src/feats/librarydates.cpp -pthread \
		-o /tmp/test_library_dates
	/tmp/test_library_dates

test-library-dates-hook:
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_library_dates_hook.cpp src/feats/apps.cpp src/feats/librarydates.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_library_dates_hook
	/tmp/test_library_dates_hook

test-pics:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_pics.cpp src/feats/provision_terminal.cpp -o /tmp/test_pics
	/tmp/test_pics

test-prewarm-backoff:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_prewarm_backoff.cpp -o /tmp/test_prewarm_backoff
	/tmp/test_prewarm_backoff

test-process-lock:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_process_lock.cpp -o /tmp/test_process_lock
	/tmp/test_process_lock

test-atomic-file:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -DATOMIC_FILE_TESTING -I src \
		tools/test_atomic_file.cpp -o /tmp/test_atomic_file
	/tmp/test_atomic_file

test-cache-pair:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_cache_pair.cpp -o /tmp/test_cache_pair
	/tmp/test_cache_pair

test-yaml-runtime: $(LIBYAML)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -I include \
		tools/test_yaml_runtime.cpp $(LIBYAML) -o /tmp/test_yaml_runtime
	/tmp/test_yaml_runtime

# Non-LTO 32-bit object so the host linker can link the transaction test
# without choking on LTO bytecode from a different toolchain version.
$(OBJDIR)/feats/appinfo_vdf_test.o: src/feats/appinfo_vdf.cpp $(deps_early)
	@mkdir -p $(OBJDIR)/feats
	$(CXX) -O2 -fno-lto -fPIC $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -fno-reorder-blocks-and-partition \
		-Wall -Wextra -Wpedantic -Wno-error=format-security \
		-D_GLIBCXX_USE_CXX11_ABI=0 -DAPPINFO_VDF_TESTING \
		-I include -isysteminclude -MMD -MP \
		-c src/feats/appinfo_vdf.cpp -o $(OBJDIR)/feats/appinfo_vdf_test.o

test-appinfo-transaction: $(OBJDIR)/feats/appinfo_vdf_test.o $(OBJDIR)/log.o $(OBJDIR)/config.o \
		$(OBJDIR)/globals.o $(OBJDIR)/filewatcher.o $(OBJDIR)/update.o $(OBJDIR)/api.o \
		$(OBJDIR)/feats/depotkey.o $(OBJDIR)/feats/manifestid.o $(OBJDIR)/feats/librarydates.o \
		$(OBJDIR)/ownerwork.o $(OBJDIR)/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_base.pb.o $(LIBYAML) $(LIBPROTOBUF)
	$(CXX) $(MULTILIB) $(ARCH_DEFINES) -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -DAPPINFO_VDF_TESTING \
		-I include -isysteminclude tools/test_appinfo_transaction.cpp \
		$(OBJDIR)/feats/appinfo_vdf_test.o \
		$(filter-out $(OBJDIR)/feats/appinfo_vdf_test.o,$(filter $(OBJDIR)/%.o,$^)) \
		$(LIBYAML) $(LIBPROTOBUF) -lcrypto -lpthread -ldl \
		-o /tmp/test_appinfo_transaction
	/tmp/test_appinfo_transaction

# Live integration harness for the native CM product-info client (talks
# to real Valve CMs — NOT a unit test).  Links cmclient + its deps.
test-cmclient-live: build
	$(CXX) $(CXXFLAGS) -DCMWIRE_PROTOBUF \
		-I include -isysteminclude tools/test_cmclient_live.cpp \
		$(OBJDIR)/feats/cmclient.o $(OBJDIR)/log.o $(OBJDIR)/config.o $(OBJDIR)/globals.o $(OBJDIR)/update.o $(OBJDIR)/filewatcher.o \
		$(OBJDIR)/feats/librarydates.o \
		$(OBJDIR)/sdk/protobufs/steammessages_base.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver_login.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver_2.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver_friends.pb.o \
		$(OBJDIR)/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
		$(OBJDIR)/sdk/protobufs/encrypted_app_ticket.pb.o \
		$(LIBPROTOBUF) $(LIBYAML) \
		-lpthread -ldl -lcurl -o /tmp/test_cmclient_live

test-manifestpin-patterns:
	$(CXX) -std=c++20 tools/test_manifestpin_patterns.cpp \
		-o /tmp/test_manifestpin_patterns
	/tmp/test_manifestpin_patterns "$(STEAMCLIENT)"

# The two optional locators whose failure only degrades a feature, so a drift
# after a client update would otherwise pass unnoticed.
#
# Point PREV_STEAMCLIENT/PREV_STEAMUI at the previous client's modules to also
# gate the transport: a relaxed signature can be unique on the new build and
# ambiguous (or land on a neighbouring field) on the one users still run, which
# a single-build check cannot see.
test-patternscan:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_patternscan.cpp -o /tmp/test_patternscan
	/tmp/test_patternscan

test-optional-locators:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_optional_locators.cpp -o /tmp/test_optional_locators
	/tmp/test_optional_locators "$(STEAMCLIENT)" "$(STEAMUI)" src/patterns.cpp
ifneq ($(strip $(PREV_STEAMCLIENT)$(PREV_STEAMUI)),)
	@test -n "$(strip $(PREV_STEAMCLIENT))" && test -n "$(strip $(PREV_STEAMUI))" \
		|| { echo "set both PREV_STEAMCLIENT and PREV_STEAMUI" >&2; exit 2; }
	@echo "==> previous client transport check"
	/tmp/test_optional_locators "$(PREV_STEAMCLIENT)" "$(PREV_STEAMUI)" src/patterns.cpp
endif

release:
	bash scripts/release.sh
