#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile extern/libpeer (the native WebRTC backend) for the Switch.
#
# libpeer is a pure-C WebRTC library. It has no build system of its own for
# the Switch: its CMakeLists.txt cross-compiles its third-party dependencies
# (mbedtls / libsrtp / usrsctp / cJSON) with ExternalProject and installs
# them into <build>/dist.
#
# The sources are NOT vendored in this repository.  They are fetched from a
# PINNED upstream commit and verified by sha256, so that every checkout builds
# the very same libpeer.a -- and so that a fresh clone can build at all.
#
#   upstream : OpenCloudGaming/OpenNOW-Switch @ e148ccda999e58dae4d2067a2e53b723c1bb7bec
#   subtree  : extern/libpeer/  (libpeer itself is MIT, see
#              extern/libpeer/LICENSE; the Switch specific delta is part of
#              that same MIT tree)
#   tarball  : https://codeload.github.com/OpenCloudGaming/OpenNOW-Switch/tar.gz/e148ccda999e58dae4d2067a2e53b723c1bb7bec
#   sha256   : 3c189eb45a5e33d627c3391e4d974c1301db8b5e9b4c4dcb6523874f31a5cc5f
#
# The anchor that matters most is extern/libpeer/src/rtp.c -- that is the file
# the H.265 depacketizer work (C-01) patches:
#
#   src/rtp.c  303449692a4821cffc70ae6629ace50de8671eba71a7cb6516a28ae7e161a432
#
# Usage:
#   tools/build-libpeer.sh                  # fetch (if needed) + build
#   tools/build-libpeer.sh --fetch-only     # only put the sources in place
#   tools/build-libpeer.sh --force-fetch    # re-download even if in place
#   LIBPEER_BUILD_DIR=/tmp/x tools/build-libpeer.sh
#
# Environment:
#   DEVKITPRO        devkitPro root (required for the build; NOT needed for
#                    --fetch-only)
#   LIBPEER_SRC      a local libpeer source tree to copy into extern/libpeer
#                    instead of downloading it (offline / air-gapped builds)
#   LIBPEER_BUILD_DIR  build tree (default: extern/libpeer/build)
#   PYTHON_DIR       a directory with a python3 interpreter to use for
#                    mbedtls' code generators (default: python3 from PATH)
#   LIBPEER_REAL_MAKE  the real GNU make, when tools/make-shim is used
#
# Requires: devkitPro (DEVKITPRO) with the Switch toolchain + cmake.
# ---------------------------------------------------------------------------
set -euo pipefail

# ---------------------------------------------------------------------------
# Arguments.
# ---------------------------------------------------------------------------
# The original arguments are kept: the delegation below hands them over to
# the engine tree's copy verbatim.
ORIG_ARGS=("$@")
FETCH_ONLY=0
FORCE_FETCH=0

while [ $# -gt 0 ]; do
	case "$1" in
		--fetch-only)
			FETCH_ONLY=1
			;;
		--force-fetch)
			FORCE_FETCH=1
			;;
		-h | --help)
			sed -n '2,43p' "$0" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*)
			echo "error: unknown argument: $1" >&2
			echo "       try: $0 --help" >&2
			exit 1
			;;
	esac
	shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------------------------------------------------------------------------
# Where is the engine tree?
#
# This script exists in two places, with the very same content:
#
#   * <repo>/tools/build-libpeer.sh              -- this copy (repository root)
#   * <repo>/engine/tools/build-libpeer.sh      -- the copy the engine build
#                                                 uses (installed by
#                                                 patches/0015-libpeer-source-fetch.patch)
#
# so that the repository can hand out the sources without a build, and the
# engine tree stays self contained (it may be built on its own).
# ---------------------------------------------------------------------------
if [ -f "${ROOT_DIR}/source/main.cc" ] && [ -d "${ROOT_DIR}/packages/runtime" ]; then
	ENGINE_DIR="${ROOT_DIR}"
elif [ -f "${ROOT_DIR}/engine/source/main.cc" ]; then
	ENGINE_DIR="${ROOT_DIR}/engine"
else
	echo "error: cannot locate the engine tree." >&2
	echo "       expected ${ROOT_DIR} or ${ROOT_DIR}/engine to be the nx.js engine." >&2
	exit 1
fi

# When this copy runs from the repository root and the engine tree already
# carries its own copy (i.e. the patches are applied), hand over to it: one
# implementation, one set of anchors.
if [ "${SCRIPT_DIR}" != "${ENGINE_DIR}/tools" ] && [ -f "${ENGINE_DIR}/tools/build-libpeer.sh" ]; then
	exec "${BASH:-bash}" "${ENGINE_DIR}/tools/build-libpeer.sh" ${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}
fi

LIBPEER_DIR="${ENGINE_DIR}/extern/libpeer"
BUILD_DIR="${LIBPEER_BUILD_DIR:-${LIBPEER_DIR}/build}"

# ---------------------------------------------------------------------------
# The pinned upstream.
# ---------------------------------------------------------------------------
LIBPEER_UPSTREAM_REPO="OpenCloudGaming/OpenNOW-Switch"
LIBPEER_UPSTREAM_COMMIT="e148ccda999e58dae4d2067a2e53b723c1bb7bec"
LIBPEER_TARBALL_URL="https://codeload.github.com/${LIBPEER_UPSTREAM_REPO}/tar.gz/${LIBPEER_UPSTREAM_COMMIT}"
LIBPEER_TARBALL_SHA256="3c189eb45a5e33d627c3391e4d974c1301db8b5e9b4c4dcb6523874f31a5cc5f"

# sha256 anchors inside the fetched tree.  src/rtp.c is the one that must
# never move: it is the file the H.265 depacketizer patch (C-01) edits.
LIBPEER_ANCHOR_RTP_C="303449692a4821cffc70ae6629ace50de8671eba71a7cb6516a28ae7e161a432"
LIBPEER_ANCHOR_CMAKE="8ca4b7f9e4ccc53fc7cb31d640d971bae0df3d9b6658b043f2c8f9f8a3b02c68"
LIBPEER_ANCHOR_CONFIG_H="f4361a2a24c5e1bf950f8f9962fa3cd3554b24d616b4fcc31cf953854e97775a"

WORK_DIR=""
cleanup() {
	if [ -n "${WORK_DIR}" ] && [ -d "${WORK_DIR}" ]; then
		rm -rf "${WORK_DIR}"
	fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# sha256 of a file -- sha256sum (GNU), shasum (BSD/macOS) or openssl.
# ---------------------------------------------------------------------------
sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	elif command -v openssl >/dev/null 2>&1; then
		openssl dgst -sha256 "$1" | sed 's/.*= *//'
	else
		echo "error: no sha256 tool found (need sha256sum, shasum or openssl)." >&2
		return 1
	fi
}

# ---------------------------------------------------------------------------
# Download <url> to <file>.  curl and wget are used when available; on a
# Windows host without either, PowerShell's Invoke-WebRequest is the
# fallback.  Proxy settings come from the usual environment variables
# (https_proxy / HTTPS_PROXY), which curl and wget both honour.
# ---------------------------------------------------------------------------
download_to_file() {
	_url="$1"
	_out="$2"
	if command -v curl >/dev/null 2>&1; then
		curl -fL --retry 3 --retry-delay 2 -o "$_out" "$_url"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$_out" "$_url"
	elif command -v curl.exe >/dev/null 2>&1; then
		curl.exe -fL --retry 3 -o "$_out" "$_url"
	elif command -v powershell.exe >/dev/null 2>&1; then
		powershell.exe -NoProfile -Command "Invoke-WebRequest -Uri '$_url' -OutFile '$_out'"
	else
		echo "error: need curl, wget or powershell to download the libpeer sources." >&2
		echo "       (or place the tree by hand and export LIBPEER_SRC)" >&2
		return 1
	fi
}

# ---------------------------------------------------------------------------
# Copy a directory tree.  cp -a is not available everywhere, so fall back to
# a tar pipe.
# ---------------------------------------------------------------------------
copy_tree() {
	_src="$1"
	_dst="$2"
	mkdir -p "$_dst"
	if cp -a "${_src}/." "${_dst}/" 2>/dev/null; then
		return 0
	fi
	(cd "${_src}" && tar -cf - .) | (cd "${_dst}" && tar -xf -)
}

# ---------------------------------------------------------------------------
# Check one file against a pinned sha256.  Hard failure: the whole point of
# pinning is that a mismatch must stop the build instead of silently
# producing a different libpeer.a.
# ---------------------------------------------------------------------------
verify_anchor() {
	_file="$1"
	_expected="$2"
	_label="$3"
	if [ ! -f "${_file}" ]; then
		echo "error: ${_label} is missing (${_file})." >&2
		return 1
	fi
	_actual="$(sha256_of "${_file}")"
	if [ "${_actual}" != "${_expected}" ]; then
		echo "error: ${_label} does not match the pinned sha256." >&2
		echo "       expected ${_expected}" >&2
		echo "       actual   ${_actual}" >&2
		return 1
	fi
	echo "    ok   ${_label}: ${_actual}"
	return 0
}

# ---------------------------------------------------------------------------
# Informational anchor report -- used when the tree is already in place, so
# a local modification (the Switch fixes, or C-01's H.265 work) is reported
# instead of being treated as a broken download.
# ---------------------------------------------------------------------------
report_anchors() {
	_dir="$1"
	_rtp="${_dir}/src/rtp.c"
	if [ ! -f "${_rtp}" ]; then
		echo "    warn ${_dir}/src/rtp.c is missing." >&2
		return 0
	fi
	_actual="$(sha256_of "${_rtp}")"
	if [ "${_actual}" = "${LIBPEER_ANCHOR_RTP_C}" ]; then
		echo "    ok   src/rtp.c matches the pinned upstream sha256 (${_actual})"
	else
		echo "    note src/rtp.c differs from the pinned upstream sha256:"
		echo "         upstream ${LIBPEER_ANCHOR_RTP_C}"
		echo "         local    ${_actual}"
		echo "         (expected: the Switch fixes or a local patch changed it)"
	fi
}

# ---------------------------------------------------------------------------
# Extract extern/libpeer/ out of the pinned tarball.
#
# Only that subtree is extracted -- the whole OpenNOW-Switch tree is 113 MB
# and contains an unrelated ffmpeg copy.  The member prefix is discovered
# from the archive itself (and cross-checked against the pinned commit), so
# a renamed archive fails loudly instead of extracting the wrong thing.
# ---------------------------------------------------------------------------
extract_libpeer_subtree() {
	_tarball="$1"
	_outdir="$2"

	_prefix="$(tar -tzf "${_tarball}" 2>/dev/null | sed -n 's|^\([^/]*/extern/libpeer\)/.*|\1|p' | sed -n '1p' || true)"
	if [ -z "${_prefix}" ]; then
		echo "error: ${_tarball} does not contain an extern/libpeer/ subtree." >&2
		return 1
	fi
	case "${_prefix}" in
		*"${LIBPEER_UPSTREAM_COMMIT}"*) ;;
		*)
			echo "error: unexpected tarball layout: ${_prefix}" >&2
			echo "       expected a path ending in ${LIBPEER_UPSTREAM_COMMIT}/extern/libpeer" >&2
			return 1
			;;
	esac

	mkdir -p "${_outdir}"
	# GNU tar and bsdtar spell the two useful forms differently: naming the
	# directory member extracts the whole subtree, and the wildcard form
	# does the same on hosts whose tar does not descend into a named
	# directory.  Try both, then fall back to extracting everything (the
	# archive is only 113 MB).
	_ok=0
	for _pat in "${_prefix}" "${_prefix}/*"; do
		rm -rf "${_outdir:?}/libpeer"
		mkdir -p "${_outdir}"
		if tar -xzf "${_tarball}" -C "${_outdir}" --strip-components=2 "${_pat}" 2>/dev/null &&
			[ -f "${_outdir}/libpeer/src/rtp.c" ]; then
			_ok=1
			break
		fi
	done
	if [ "${_ok}" -ne 1 ]; then
		echo "    note falling back to a full extraction of the archive"
		rm -rf "${_outdir:?}/libpeer"
		mkdir -p "${_outdir}"
		tar -xzf "${_tarball}" -C "${_outdir}" --strip-components=2
		if [ ! -f "${_outdir}/libpeer/src/rtp.c" ]; then
			echo "error: could not extract extern/libpeer out of ${_tarball}." >&2
			return 1
		fi
	fi
	return 0
}

# ---------------------------------------------------------------------------
# 0. Put the libpeer sources in place.
#
# Three ways, in this order:
#
#   a) the tree is already there      -> nothing to do (idempotent)
#   b) $LIBPEER_SRC points at a copy  -> copy it (offline / air-gapped)
#   c) download the pinned tarball    -> verify sha256 -> extract the subtree
#
# The submodule route (git submodule update) is NOT used any more: the URL in
# .gitmodules points at a fork that does not exist (404), and the submodule
# is not registered in the engine's index anyway.  It is kept as a last
# resort in case that fork shows up one day.
# ---------------------------------------------------------------------------
fetch_libpeer_sources() {
	if [ "${FORCE_FETCH}" -eq 1 ] && [ -d "${LIBPEER_DIR}" ]; then
		echo "==> --force-fetch: removing ${LIBPEER_DIR}"
		report_anchors "${LIBPEER_DIR}"
		rm -rf "${LIBPEER_DIR}"
	fi

	if [ -f "${LIBPEER_DIR}/CMakeLists.txt" ]; then
		echo "==> extern/libpeer is already in place -- no download needed"
		report_anchors "${LIBPEER_DIR}"
		return 0
	fi

	if [ -n "${LIBPEER_SRC:-}" ]; then
		if [ ! -f "${LIBPEER_SRC}/CMakeLists.txt" ]; then
			echo "error: LIBPEER_SRC=${LIBPEER_SRC} does not look like a libpeer tree." >&2
			echo "       (expected ${LIBPEER_SRC}/CMakeLists.txt to exist)" >&2
			return 1
		fi
		echo "==> populating extern/libpeer from LIBPEER_SRC=${LIBPEER_SRC}"
		copy_tree "${LIBPEER_SRC}" "${LIBPEER_DIR}"
		report_anchors "${LIBPEER_DIR}"
		return 0
	fi

	echo "==> fetching the libpeer sources from the pinned upstream"
	echo "    repo    : ${LIBPEER_UPSTREAM_REPO}"
	echo "    commit  : ${LIBPEER_UPSTREAM_COMMIT}"
	echo "    tarball : ${LIBPEER_TARBALL_URL}"

	WORK_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t libpeer)"
	_tarball="${WORK_DIR}/libpeer.tar.gz"

	if ! download_to_file "${LIBPEER_TARBALL_URL}" "${_tarball}"; then
		echo "error: could not download the libpeer tarball." >&2
		echo "       url: ${LIBPEER_TARBALL_URL}" >&2
		echo "       Offline?  Put the tree in place and export LIBPEER_SRC," >&2
		echo "       e.g. from a checkout of ${LIBPEER_UPSTREAM_REPO}" >&2
		echo "       (commit ${LIBPEER_UPSTREAM_COMMIT}), path extern/libpeer." >&2
		return 1
	fi

	echo "==> verifying the tarball"
	verify_anchor "${_tarball}" "${LIBPEER_TARBALL_SHA256}" "tarball sha256" || {
		echo "       the download is corrupt or the upstream changed -- refusing to use it." >&2
		return 1
	}

	echo "==> extracting extern/libpeer"
	extract_libpeer_subtree "${_tarball}" "${WORK_DIR}/x" || return 1

	echo "==> verifying the extracted tree"
	verify_anchor "${WORK_DIR}/x/libpeer/src/rtp.c" "${LIBPEER_ANCHOR_RTP_C}" "extern/libpeer/src/rtp.c" || return 1
	verify_anchor "${WORK_DIR}/x/libpeer/CMakeLists.txt" "${LIBPEER_ANCHOR_CMAKE}" "extern/libpeer/CMakeLists.txt" || return 1
	verify_anchor "${WORK_DIR}/x/libpeer/src/config.h" "${LIBPEER_ANCHOR_CONFIG_H}" "extern/libpeer/src/config.h" || return 1

	mkdir -p "$(dirname "${LIBPEER_DIR}")"
	rm -rf "${LIBPEER_DIR}"
	mv "${WORK_DIR}/x/libpeer" "${LIBPEER_DIR}"
	rm -rf "${WORK_DIR}"
	WORK_DIR=""

	echo "==> extern/libpeer is in place: ${LIBPEER_DIR}"
	return 0
}

fetch_libpeer_sources

if [ "${FETCH_ONLY}" -eq 1 ]; then
	echo "==> --fetch-only: the sources are in place, stopping here"
	exit 0
fi

# ---------------------------------------------------------------------------
# 0d. GNU Make 4.4 shim (Windows only).
#
# CMake's "Unix Makefiles" generator writes a target's compiler dependency
# list to compiler_depend.make.  For a target defined in a project
# sub-directory (usrsctp's "usrsctplib/" target) CMake glues the
# sub-directory in front of the Windows drive letter:
#
#     usrsctplib/C:/devkitPro/devkitA64/aarch64-none-elf/include/errno.h
#
# GNU Make 4.4 parses such a line as a *pattern rule* and aborts the build
# with "*** multiple target patterns.  Stop.".  tools/make-shim/make rewrites
# those entries to the plain "C:/devkitPro/..." form that make parses
# correctly, before handing over to the real make.
#
# It has to be first on PATH so that find_program(CMAKE_MAKE_PROGRAM) picks
# it up for the top level project AND for every ExternalProject child.
# The shim is optional: on a POSIX builder the problem does not exist.
# ---------------------------------------------------------------------------
MAKE_SHIM_DIR="${SCRIPT_DIR}/make-shim"
if [ -x "${MAKE_SHIM_DIR}/make" ]; then
	export PATH="${MAKE_SHIM_DIR}:${PATH}"
fi

# ---------------------------------------------------------------------------
# 0e. Python for mbedtls' code generators.
#
# mbedtls 3.4.0 generates three sources at build time:
#
#     library/psa_crypto_driver_wrappers.c   (scripts/generate_driver_wrappers.py)
#     library/ssl_debug_helpers_generated.c   (scripts/generate_ssl_debug_helpers.py)
#     library/../include/psa/...               (scripts/generate_psa_constants.py)
#
# The first one needs jinja2 + jsonschema.  A system python3 with those two
# modules is enough; PYTHON_DIR can point at a self-contained interpreter
# (e.g. the official python.org embeddable build) when the host has none.
# ---------------------------------------------------------------------------
if [ -n "${PYTHON_DIR:-}" ]; then
	if [ ! -x "${PYTHON_DIR}/python.exe" ] && [ ! -x "${PYTHON_DIR}/python3" ]; then
		echo "error: PYTHON_DIR has no interpreter: ${PYTHON_DIR}" >&2
		exit 1
	fi
	export PATH="${PYTHON_DIR}:${PATH}"
elif ! command -v python3 >/dev/null 2>&1; then
	echo "warning: no python3 on PATH -- mbedtls' code generators will fail." >&2
	echo "         set PYTHON_DIR=<dir with python3 + jinja2 + jsonschema>" >&2
fi

# ---------------------------------------------------------------------------
# 0f. Toolchain.
#
# The OFFICIAL devkitPro Switch toolchain, wrapped by tools/Switch-libpeer.cmake
# which additionally passes -D__unix__ to the mbedtls sub-project only
# (mbedtls' net_sockets.c / timing.c refuse to compile on anything that is
# not Unix or Windows, while the Switch does provide the needed POSIX API).
# ---------------------------------------------------------------------------
if [ -z "${DEVKITPRO:-}" ]; then
	echo "error: DEVKITPRO is not set" >&2
	echo "       export DEVKITPRO=/opt/devkitpro" >&2
	exit 1
fi

TOOLCHAIN="${SCRIPT_DIR}/Switch-libpeer.cmake"
if [ ! -f "${TOOLCHAIN}" ]; then
	echo "error: Switch toolchain wrapper not found at ${TOOLCHAIN}" >&2
	exit 1
fi
if [ ! -f "${DEVKITPRO}/cmake/Switch.cmake" ]; then
	echo "error: official devkitPro Switch toolchain not found at ${DEVKITPRO}/cmake/Switch.cmake" >&2
	exit 1
fi

# ---------------------------------------------------------------------------
# 1. Check that libpeer's third-party dependencies came along.
#
# They are vendored inside the fetched extern/libpeer tree, so this is only
# a sanity check on the extraction.
# ---------------------------------------------------------------------------
for dep in mbedtls libsrtp usrsctp cJSON coreHTTP coreMQTT; do
	if [ ! -f "${LIBPEER_DIR}/third_party/${dep}/CMakeLists.txt" ] &&
		[ ! -f "${LIBPEER_DIR}/third_party/${dep}/library/CMakeLists.txt" ]; then
		echo "warning: third_party/${dep} looks empty." >&2
		echo "         re-run with --force-fetch to get a pristine tree." >&2
	fi
done

# ---------------------------------------------------------------------------
# 1b. Apply the Switch specific fixes to libpeer.
#
# extern/libpeer is a submodule, so its sources are not edited in place.
# The fixes live in tools/libpeer-switch/*.patch and are applied here, once
# the tree is in place and before it is configured.
#
# The step is idempotent, so a rebuild is fine.
# ---------------------------------------------------------------------------
"${SCRIPT_DIR}/apply-libpeer-switch-patches.sh" "${LIBPEER_DIR}"

# ---------------------------------------------------------------------------
# 2. Reconcile mbedtls config BEFORE configuring/building.
#
# The original patch applies the mbedtls fixup AFTER the build, which is too
# late: libpeer's CMakeLists rewrites mbedtls_config.h at CONFIGURE time and
# mbedtls is compiled during the build, so the post-build sed never reaches
# the archive.  We therefore run the very same fixup up front, via
# tools/fix-mbedtls-config.sh (extracted from section 4 below).
#
# libpeer's CMakeLists only appends its override block ONCE (guarded by the
# "OpenNOW Switch mbedtls overrides" marker), so the fixup survives the
# configure step and reaches the compiled archive.
# ---------------------------------------------------------------------------
"${SCRIPT_DIR}/fix-mbedtls-config.sh" "${LIBPEER_DIR}"

# ---------------------------------------------------------------------------
# 3. Configure + build.
#
# BUILD_SHARED_LIBS=OFF is REQUIRED: libpeer's CMakeLists defaults it to ON,
# which would produce libpeer.so instead of libpeer.a.
# CMAKE_INSTALL_PREFIX + "cmake --install" are required too: the patch never
# installs the peer target itself, so dist/lib/libpeer.a would never exist.
# ---------------------------------------------------------------------------
echo "==> configuring libpeer (${BUILD_DIR})"
# Number of parallel jobs.  nproc is GNU only; macOS has sysctl.
if [ -z "${LIBPEER_JOBS:-}" ]; then
	if command -v nproc >/dev/null 2>&1; then
		LIBPEER_JOBS="$(nproc)"
	elif command -v sysctl >/dev/null 2>&1; then
		LIBPEER_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
	else
		LIBPEER_JOBS=4
	fi
fi
cmake -S "${LIBPEER_DIR}" -B "${BUILD_DIR}" \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DPLATFORM_SWITCH=ON \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/dist"

echo "==> building libpeer"
# Only the "peer" target is built, NOT "all":
#
#   * "peer" carries add_dependencies(peer cjson mbedtls usrsctp srtp2), so
#     the four third-party ExternalProjects are still cross-compiled AND
#     installed into <build>/dist before it.
#   * "all" would additionally build examples/generic/sample.elf, which links
#     the archives WITHOUT the engine.  mbedtls' entropy.c then has no
#     mbedtls_hardware_poll() to call -- the Switch has no /dev/urandom, so
#     the entropy comes from the engine (source/nx_entropy.cc) instead -- and
#     the example fails to link with "undefined reference to
#     mbedtls_hardware_poll".  The example is a sample, not a deliverable.
cmake --build "${BUILD_DIR}" --target peer -j"${LIBPEER_JOBS}"

echo "==> installing libpeer into ${BUILD_DIR}/dist"
cmake --install "${BUILD_DIR}"

# ---------------------------------------------------------------------------
# 4. Keep the INSTALLED mbedtls config in sync with the fixup.
#
# The engine compiles against ${BUILD_DIR}/dist/include/mbedtls/mbedtls_config.h
# and links ${BUILD_DIR}/dist/lib/libmbedcrypto.a.  mbedtls' ExternalProject
# only re-installs its headers when it actually recompiles, so the installed
# header can lag behind the fixup in step 2 -- and a header/archve mismatch
# means the engine is compiled against a different mbedtls than the one it
# links (struct layouts!).  Copy the fixed config over, but only when the
# archive is newer than the fixup (i.e. it really was built with it).
# ---------------------------------------------------------------------------
INSTALLED_CONFIG="${BUILD_DIR}/dist/include/mbedtls/mbedtls_config.h"
# The config that tools/fix-mbedtls-config.sh reconciled in the SOURCE tree.
# It has to be spelled out here: the script runs in its own process, so its
# variable does not survive into this one, and `set -u` would turn the
# missing name into a fatal error right at the end of a successful build.
MBEDTLS_CONFIG="${LIBPEER_DIR}/third_party/mbedtls/include/mbedtls/mbedtls_config.h"
if [ -f "${INSTALLED_CONFIG}" ] && [ -f "${BUILD_DIR}/dist/lib/libmbedcrypto.a" ] &&
	[ "${BUILD_DIR}/dist/lib/libmbedcrypto.a" -nt "${MBEDTLS_CONFIG}" ]; then
	echo "==> syncing installed mbedtls config with the fixup"
	cp -f "${MBEDTLS_CONFIG}" "${INSTALLED_CONFIG}"
fi

echo "==> done: ${BUILD_DIR}/dist/lib"
ls -la "${BUILD_DIR}/dist/lib" 2>/dev/null || true
