#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

# Ensure the devkitPro toolchain dirs are on PATH so callers only need to set
# DEVKITPRO. Covers aarch64-none-elf-pkg-config (portlibs), the devkitA64
# compilers, and tools like elf2nro. Exported so it applies to every recipe
# shell (e.g. the backtick pkg-config calls in CFLAGS/LIBS) below.
#
# NOTE: GNU Make's parse-time $(shell ...) does NOT see this exported PATH, so
# parse-time tool lookups (UV_CFLAGS/UV_LIBS) use $(PKG_CONFIG) with an explicit
# path instead.
export PATH := $(DEVKITPRO)/portlibs/switch/bin:$(DEVKITPRO)/devkitA64/bin:$(DEVKITPRO)/tools/bin:$(PATH)
PKG_CONFIG := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# EXEFS_SRC is the optional input directory containing data copied into exefs, if anything this normally should only contain "main.npdm".
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#
# NO_ICON: if set to anything, do not use icon.
# NO_NACP: if set to anything, no .nacp file is generated.
# APP_TITLE is the name of the app stored in the .nacp file (Optional)
# APP_AUTHOR is the author of the app stored in the .nacp file (Optional)
# APP_VERSION is the version of the app stored in the .nacp file (Optional)
# APP_TITLEID is the titleID of the app stored in the .nacp file (Optional)
# ICON is the filename of the icon (.jpg), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.jpg
#     - icon.jpg
#     - <libnx folder>/default_icon.jpg
#---------------------------------------------------------------------------------

APP_TITLE   :=  nx.js
APP_TITLEID :=  016e782e6a730000 # 6e782e6a73 is "nx.js" in hex 😉
APP_AUTHOR  :=  TooTallNate
APP_VERSION :=  `jq -r .version < ../packages/runtime/package.json`
LIBNX_VERSION := $(shell $(DEVKITPRO)/pacman/bin/pacman -Q libnx 2>/dev/null | cut -d' ' -f2 | cut -d'-' -f1)
LIBTURBOJPEG_VERSION := $(shell $(DEVKITPRO)/pacman/bin/pacman -Q switch-libjpeg-turbo 2>/dev/null | cut -d' ' -f2 | cut -d'-' -f1)

TARGET		:=	nxjs
BUILD		:=	build
# `source/brotli` is a vendored copy of google/brotli 1.2.0's DECODER
# (c/common + c/dec + c/include, MIT — see source/brotli/LICENSE). devkitPro
# ships no brotli portlib and FreeType here is built without
# FT_CONFIG_OPTION_USE_BROTLI, so WOFF2 — which every modern web font ships as
# — could not be decoded at all. Vendored rather than packaged so the repo
# stays self-contained and no extra pacman package is needed to build it.
SOURCES		:=	source source/brotli/common source/brotli/dec
DATA		:=	data
# extern/libpeer/build/dist/include carries libpeer's mbedtls 3.4.0 headers;
# it MUST come before the portlibs include path (mbedtls 2.28) -- see the
# VERSION MATTERS note next to LIBPEER_DIR.
INCLUDES	:=	include source/brotli/include extern/libpeer/build/dist/include
EXEFS_SRC	:=	exefs_src
ROMFS		:=	romfs
CONFIG_JSON	:=	npdm.json

#---------------------------------------------------------------------------------
# Optional app entrypoint (switch-cloud-browser patch 0004)
#---------------------------------------------------------------------------------
# The NRO this Makefile builds is a *runtime*, not an application: it ships no
# JavaScript entry point of its own. On a standalone launch (no argv[1])
# resolve_entrypoint() (source/main.cc:1545) tries exactly two places:
#   1. "romfs:/main.js"     -- the NRO's own RomFS          (main.cc:1598-1603)
#   2. "<argv[0] with .js>" -- a sibling file on the SD card (main.cc:1604-1611)
# A stock build has neither: the RomFS only holds runtime.js.map and
# GeistMono.ttf (the two rules further down), so the engine boots V8, finds no
# entry point, and dies with "no such file or directory sdmc:/.../<name>.js".
#
# This block makes the *build* bake one app's bundled main.js into the RomFS
# under the name "main.js" -- exactly what step 1 looks for -- so the NRO
# becomes self-contained and starts on its own.
#
# Usage (from the engine directory, inside devkitPro's MSYS2):
#   make APP=gamepads                  # apps/<name>/romfs/main.js
#   make APP=hello-world
#   make APP_MAIN=path/to/main.js      # any bundle, relative to this directory
#
# The bundle is produced by the app's own esbuild step (see the app's
# package.json "build" script), e.g.
#   pnpm --filter gamepads build
# This Makefile only copies the finished bundle; it never runs pnpm/esbuild, so
# the engine build stays independent of the JS toolchain.
APP	?=
APP_DIR	?=	$(if $(strip $(APP)),apps/$(strip $(APP)),)
APP_MAIN	?=	$(if $(strip $(APP_DIR)),$(APP_DIR)/romfs/main.js,)

# The bundle path, resolved against the engine directory, so that one value is
# valid in the top-level make (TOPDIR = the engine directory) and in the
# sub-make that packs the NRO (that one runs with CURDIR = <engine>/build).
# Absolute paths ("/..." or "C:/...") are taken as-is.
ifneq ($(strip $(APP_MAIN)),)
APP_MAIN_PATH := $(if $(findstring :,$(APP_MAIN)),$(APP_MAIN),$(if $(filter /%,$(APP_MAIN)),$(APP_MAIN),$(TOPDIR)/$(APP_MAIN)))
else
APP_MAIN_PATH :=
endif

# Fail early with an actionable message when the requested bundle is missing;
# plain `make` would stop with a bare "No rule to make target ...". Only the
# top-level make checks this (the sub-make runs from <engine>/build, where a
# relative path would not resolve), and `clean` is exempt: it needs no bundle.
ifneq ($(BUILD),$(notdir $(CURDIR)))
ifneq ($(strip $(APP_MAIN)),)
ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
ifneq ($(shell test -f "$(APP_MAIN)" && echo yes),yes)
$(error APP bundle not found: '$(APP_MAIN)'. Build the app first (see the app's package.json "build" script) or point APP_MAIN at an existing bundle.)
endif
endif
endif
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# V8 must NOT see V8_COMPRESS_POINTERS defined (checked by presence). libuv's
# pkg-config force-includes nx-prelude.h and adds _GNU_SOURCE / SSIZE_MAX.
UV_CFLAGS	:=	$(shell $(PKG_CONFIG) --cflags libuv)
UV_LIBS		:=	$(shell $(PKG_CONFIG) --libs libuv)

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES) -DNXJS_VERSION="\"${APP_VERSION}\"" \
			-DLIBNX_VERSION="\"$(LIBNX_VERSION)\"" \
			-DLIBTURBOJPEG_VERSION="\"$(LIBTURBOJPEG_VERSION)\""

# NVTEGRA (Tegra X1 NVDEC) hardware decode in media-decoder.cc.  Enable only
# when the portlibs ffmpeg is the nvtegra-patched build (the switch-ffmpeg
# 7.1 package published with wiliwili); a stock ffmpeg has neither
# AV_PIX_FMT_NVTEGRA nor libavutil/hwcontext_nvtegra.h.
#
# The host nxjs-test build does NOT define it: it links a stock distro
# ffmpeg, so the hardware path in source/media-decoder.cc is compiled out
# there.
CFLAGS	+=	-DNX_ENABLE_NVTEGRA

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -D_DEFAULT_SOURCE $(UV_CFLAGS) \
			`$(PKG_CONFIG) freetype2 --cflags`

# ---------------------------------------------------------------------------
# libpeer (native WebRTC backend) -- extern/libpeer is a git submodule
# (see .gitmodules). It is a pure-C library; its own CMake build
# cross-compiles mbedtls / libsrtp / usrsctp / cJSON for the Switch
# toolchain into $(LIBPEER_DIST).
#
# Build it first:  make libpeer      (or: tools/build-libpeer.sh)
#
# !! mbedtls: libpeer needs a mbedtls with MBEDTLS_SSL_DTLS_SRTP, which the
# !! devkitPro portlibs copy does NOT have. So we link libpeer's OWN mbedtls
# !! (absolute paths) INSTEAD of the portlibs one. The engine's own crypto.cc /
# !! tls.cc must therefore be satisfied by the SAME archive -- check that
# !! libpeer's mbedtls config keeps MBEDTLS_NET_C and a real entropy source
# !! (OpenNOW's libpeer override sets MBEDTLS_TEST_NULL_ENTROPY, which would
# !! break the engine's mbedtls_ctr_drbg_seed()).
# !! See docs/17-native绑定设计.md §8 (risks).
# !! VERSION MATTERS: the devkitPro portlibs mbedtls is 2.28.x while libpeer
# !! ships mbedtls 3.4.0. Mixing the two is NOT an option: the public structs
# !! (mbedtls_x509_crt / mbedtls_ssl_config / mbedtls_entropy_context /
# !! mbedtls_ctr_drbg_context) have different layouts in 2.x and 3.x, so an
# !! object compiled against one and linked against the other corrupts memory.
# !! The engine therefore has to be COMPILED against libpeer's 3.4.0 headers
# !! as well -- $(LIBPEER_DIST)/include is put in FRONT of the portlibs include
# !! path (see INCLUDES below). crypto.cc / tls.cc already carry
# !! "MBEDTLS_VERSION_MAJOR >= 3" branches, so they build against both.
LIBPEER_DIR	:=	extern/libpeer
# TOPDIR, not CURDIR: the link runs in a RECURSIVE make that is
# started with "make -C $(BUILD)", so CURDIR is <engine>/build there while
# TOPDIR is the engine directory (TOPDIR ?= $(CURDIR) at the top, exported to
# the sub-make).  With CURDIR the linker looked for
# build/extern/libpeer/build/dist/lib/libpeer.a and failed with "cannot find".
LIBPEER_DIST	:=	$(TOPDIR)/$(LIBPEER_DIR)/build/dist
LIBPEER_INC	:=	-I$(TOPDIR)/$(LIBPEER_DIR)/include -I$(LIBPEER_DIST)/include
LIBPEER_LIBS	:=	$(LIBPEER_DIST)/lib/libpeer.a \
					$(LIBPEER_DIST)/lib/libsrtp2.a \
					$(LIBPEER_DIST)/lib/libusrsctp.a \
					$(LIBPEER_DIST)/lib/libcjson.a \
					$(LIBPEER_DIST)/lib/libmbedtls.a \
					$(LIBPEER_DIST)/lib/libmbedx509.a \
					$(LIBPEER_DIST)/lib/libmbedcrypto.a

CFLAGS	+=	$(LIBPEER_INC)

# Skia (Ganesh GL). Source-root include dir; SK_GL selects the GL backend.
SKIA_CFLAGS	:=	-DSK_GL -DSK_BUILD_FOR_UNIX -I$(PORTLIBS)/include/skia

# NVDEC wiring diagnostics (patch 0006): 1 = on (default), 0 = off.
#
# The [nvd] lines the decoder prints land in the engine's stderr, which the
# Switch runtime redirects into nxjs-debug.log on the SD card.  They answer
# one question on real hardware: "did this playback really go through the
# Tegra NVDEC, at what resolution, how many frames?".  Build without them:
#
#     make NX_NVDEC_DIAG=0
#
# (Kept out of the CFLAGS block above on purpose: patches 0001 and 0003
#  both use that neighbourhood as their apply context.)
NX_NVDEC_DIAG ?= 1
CFLAGS	+=	-DNX_NVDEC_DIAG=$(NX_NVDEC_DIAG)

# V8's API is C++; nx.js native modules are C++ (.cc), no exceptions/RTTI, C++20.
# Third-party header noise we can't fix in our code:
#   -Wno-comment: V8 headers (v8-internal.h) contain ASCII-art block comments.
#   -Wno-attributes: Skia's SkTemplates.h uses the clang-only `clang::reinitializes`
#     scoped attribute, which devkitPro GCC doesn't recognize.
#   -Wno-strict-aliasing: V8's v8-function-callback.h PropertyCallbackInfo::
#     GetIsolate() does a reinterpret_cast type-pun in the engine headers.
CXXFLAGS	:=	$(CFLAGS) $(SKIA_CFLAGS) -fno-rtti -fno-exceptions -std=c++20 \
			-Wno-comment -Wno-attributes -Wno-strict-aliasing

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=${DEVKITPRO}/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# V8 static libs must be linked in a --start-group (circular refs). The patched
# archive in $(BUILD) (libc-horizon symbols weakened) takes precedence via -L.
V8_LIBS	:=	-Wl,--start-group -lv8_monolith -labsl -lchrome_zlib -lcompression_utils_portable -Wl,--end-group

# Skia (Ganesh GL) link stack: the horizon port object, then the GL-suffixed
# Skia archives in a group, then the EGL/GLES/Mesa stack. Matches the validated
# trifecta link recipe (MIGRATION-NOTES §269-438).
SKIA_LIBS	:=	$(PORTLIBS)/lib/skia-horizon-port.o \
			-Wl,--start-group -lskia-gl -lskcms-gl -lskshaper-gl \
			-lskunicode_core-gl -lskunicode_libgrapheme-gl -Wl,--end-group
GL_LIBS		:=	-lEGL -lGLESv2 -lglapi -ldrm_nouveau

# Link order (proven): Skia -> V8 -> libuv -> GL -> freetype/harfbuzz -> codecs.
# -lharfbuzz (system) satisfies switch-freetype's autohinter; Skia bundles its
# own ICU-free HarfBuzz internally. png/z are explicit (were formerly pulled via
# cairo). Cairo and pixman are fully removed in Phase 2.1 (Skia is the sole
# rendering backend).
# ffmpeg (media decode for the Video element + decodeAudioData): demux/codec/
# scale/resample only (no avfilter/avdevice). dav1d backs ffmpeg's AV1 decode.
FFMPEG_LIBS	:=	-lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d

# mbedtls comes from libpeer's dist (DTLS-SRTP enabled), NOT from portlibs --
# see the mbedtls note above. Exactly ONE mbedtls must be linked.
LIBS	:=  $(SKIA_LIBS) $(V8_LIBS) $(UV_LIBS) $(GL_LIBS) \
			$(FFMPEG_LIBS) \
			$(LIBPEER_LIBS) \
			-Wl,--start-group -lfreetype -lharfbuzz -Wl,--end-group \
			-lturbojpeg -lwebp -lwebpdemux -ljpeg -lpng -lbz2 -lz -lm -lzstd \
			-lada \
			-L$(DEVKITPRO)/libnx/lib -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
CCFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cc)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# Add `runtime_js.c` (embedded runtime.js) to CFILES if necessary (i.e. on CI)
ifneq ($(filter runtime_js.c,$(CFILES)),runtime_js.c)
	CFILES := $(CFILES) runtime_js.c
endif

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)$(CCFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CCFILES:.cc=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

.PHONY: $(BUILD) clean all libpeer

#---------------------------------------------------------------------------------
all: $(BUILD)

# Embed runtime.js as a C byte array (replaces the old qjsc bytecode step).
# V8 evaluates the runtime from source at boot.
# NB: use the literal path, NOT $(SOURCES) — that is a LIST of source dirs
# (source, source/brotli/common, source/brotli/dec), so `$(SOURCES)/runtime_js.c`
# expanded to three bogus targets and none of them was `source/runtime_js.c`.
# The stale file on disk then had no rule, make called it up to date, and every
# NRO silently shipped whatever runtime.js had been embedded last — C++ changes
# landed while the TypeScript half did not. Cost a full debug cycle 2026-09-14.
RUNTIME_JS_C := source/runtime_js.c
$(RUNTIME_JS_C): packages/runtime/runtime.js tools/embed-runtime.mjs
	@node tools/embed-runtime.mjs packages/runtime/runtime.js $@
	@echo "embedded 'packages/runtime/runtime.js' -> '$@'"

$(ROMFS)/runtime.js.map: packages/runtime/runtime.js.map
	@mkdir -p $(ROMFS)
	@cp -v packages/runtime/runtime.js.map $(ROMFS)

# Geist Mono font, used by the canvas-backed console renderer. Extracted at
# build time from the `geist` npm package (a devDependency of @nx.js/runtime)
# into romfs (mounted as `nxjs:/GeistMono.ttf` at runtime) — NOT committed to
# the repo. The pnpm symlink at packages/runtime/node_modules/geist is a stable
# path to the versioned package in the store.
GEIST_MONO_TTF := packages/runtime/node_modules/geist/dist/fonts/geist-mono/GeistMono-Regular.ttf
$(ROMFS)/GeistMono.ttf: $(GEIST_MONO_TTF)
	@mkdir -p $(ROMFS)
	@cp -v $(GEIST_MONO_TTF) $(ROMFS)/GeistMono.ttf

# Native WebRTC backend (libpeer). Cross-compiles extern/libpeer and its
# third-party deps (mbedtls / libsrtp / usrsctp / cJSON) for the Switch.
#
# The recipe hangs off the archive itself instead of a phony target, so an
# already-built extern/libpeer is NOT rebuilt on every `make` (the phony form
# made every single build re-run the whole cross compile). Force a rebuild
# with:  make -B extern/libpeer/build/dist/lib/libpeer.a
libpeer: $(LIBPEER_DIST)/lib/libpeer.a

$(LIBPEER_DIST)/lib/libpeer.a:
	@bash tools/build-libpeer.sh

#---------------------------------------------------------------------------------
# Bundled CJK system font
#---------------------------------------------------------------------------------
# The Switch's shared system font is a pure Latin face (no CJK glyphs), so the
# engine ships its own subset of Noto Sans SC (SIL OFL 1.1) and registers it
# as the system font at runtime — otherwise every page that relies on
# `system-ui` / `sans-serif` renders Chinese as tofu boxes.
#
# The subset lives in the SUPERPROJECT (not in this submodule), because a
# binary blob cannot live in a patch:
#     <superproject>/assets/fonts/NotoSansSC-subset.ttf
# and is copied into romfs at build time — the same pattern as the Geist Mono
# font above (extracted from an npm package into romfs, mounted as `nxjs:`).
#
# Override the source path with `make CJK_FONT_TTF=/path/to/font.ttf`.
CJK_FONT_TTF ?= $(TOPDIR)/../assets/fonts/NotoSansSC-subset.ttf
CJK_FONT_SRC := $(abspath $(CJK_FONT_TTF))
CJK_FONT_ROMFS := $(ROMFS)/NotoSansSC-subset.ttf

# --- 0025: SIL OFL 1.1 requires the licence text to travel with the font ---
# The font alone is NOT a compliant distribution: OFL 1.1 section 2 requires
# that every copy of the Font Software is accompanied by the copyright notice
# and the licence text.  0010 copied only the .ttf into romfs, so the shipped
# .nro carried the font without its licence.
#
# Copy OFL.txt next to the font (mounted as `nxjs:/NotoSansSC-OFL.txt`).
# Override with `make CJK_FONT_OFL=/path/to/OFL.txt`.
CJK_FONT_OFL ?= $(TOPDIR)/../assets/fonts/OFL.txt
CJK_FONT_OFL_SRC := $(abspath $(CJK_FONT_OFL))
CJK_FONT_OFL_ROMFS := $(ROMFS)/NotoSansSC-OFL.txt

# An engine checkout without the superproject's assets/ must still build (it
# just renders Chinese as tofu), so the font only joins the build when the
# source file is actually there.
ifneq ($(wildcard $(CJK_FONT_SRC)),)
CJK_FONT_DEP := $(CJK_FONT_ROMFS)
# A font without its licence text is a licence violation, not a style issue.
# Fail loudly instead of silently shipping an non-compliant .nro.
ifeq ($(wildcard $(CJK_FONT_OFL_SRC)),)
$(error SIL OFL 1.1 requires the licence text to accompany the font, but $(CJK_FONT_OFL_SRC) was not found - set CJK_FONT_OFL=/path/to/OFL.txt)
endif
CJK_FONT_DEP += $(CJK_FONT_OFL_ROMFS)
else
CJK_FONT_DEP :=
endif

$(CJK_FONT_ROMFS): $(CJK_FONT_SRC)
	@mkdir -p $(ROMFS)
	@cp -v $(CJK_FONT_SRC) $(CJK_FONT_ROMFS)

$(CJK_FONT_OFL_ROMFS): $(CJK_FONT_OFL_SRC)
	@mkdir -p $(ROMFS)
	@cp -v $(CJK_FONT_OFL_SRC) $(CJK_FONT_OFL_ROMFS)

$(BUILD): $(RUNTIME_JS_C) romfs/runtime.js.map romfs/GeistMono.ttf $(LIBPEER_DIST)/lib/libpeer.a $(CJK_FONT_DEP)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# `clean` also drops the embedded app bundle (patch 0004): it is a build
# artifact, and a leftover romfs/main.js would silently ship the previous app
# inside the next NRO.
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(SOURCES)/runtime_js.c $(TARGET).pfs0 $(TARGET).nso $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).npdm
	@rm -f $(TOPDIR)/$(ROMFS)/main.js


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	: $(OUTPUT).pfs0 $(OUTPUT).nro

ifeq ($(strip $(APP_JSON)),)
$(OUTPUT).pfs0	:	$(OUTPUT).nso
else
$(OUTPUT).pfs0	:	$(OUTPUT).nso $(OUTPUT).npdm
endif

$(OUTPUT).nso	:	$(OUTPUT).elf

#---------------------------------------------------------------------------------
# RomFS payload (switch-cloud-browser patch 0004)
#---------------------------------------------------------------------------------
# elf2nro packs the RomFS directory into the NRO, but the .nro rule only
# depends on the ELF and the NACP -- a changed payload would therefore NOT
# trigger a re-pack. Watch the RomFS directory (its mtime moves when a file is
# added or removed) and, when an app is embedded, the bundle copied into it.
NRO_ROMFS_DEP := $(TOPDIR)/$(ROMFS)
ifneq ($(strip $(APP_MAIN)),)
NRO_ROMFS_DEP += $(TOPDIR)/$(ROMFS)/main.js
endif

# Copy the app's bundled entry point into the RomFS as "main.js" -- the exact
# path resolve_entrypoint() looks for first (source/main.cc:1601). Without it
# the NRO carries no entry point at all and exits with "no such file or
# directory" right after V8 has come up.
ifneq ($(strip $(APP_MAIN)),)
$(TOPDIR)/$(ROMFS)/main.js: $(APP_MAIN_PATH)
	@mkdir -p $(TOPDIR)/$(ROMFS)
	@cp -v $(APP_MAIN_PATH) $@
endif

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp $(NRO_ROMFS_DEP)
else
$(OUTPUT).nro	:	$(OUTPUT).elf $(NRO_ROMFS_DEP)
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# devkitA64 base_rules only defines %.o: %.cpp; nx.js native modules are .cc.
#---------------------------------------------------------------------------------
%.o: %.cc
	@echo $(notdir $<)
	$(SILENTCMD)$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@ $(ERROR_FILTER)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
