#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Apply the Switch specific fixes to extern/libpeer.
#
# extern/libpeer is a git SUBMODULE (patch 0003 brings it in), so its sources
# must not be edited in place: the next `git submodule update` would throw the
# changes away, and the submodule pointer in the engine tree is what the
# repository tracks.
#
# The fixes therefore live as a patch file next to this script
# (tools/libpeer-switch/*.patch) and are applied to the checked out tree
# right before libpeer is built.  That keeps the engine patch self contained:
# one patch file for the engine, one patch file for the submodule.
#
# The patch file is stored with LF line endings (that is what this
# repository's index uses).  The libpeer tree itself may be checked out with
# CRLF, so `git apply --ignore-whitespace` is used: it matches the context
# regardless of the line ending.
#
# The step is idempotent: a patch that is already in the tree is skipped, so
# a rebuild does not fail on the second run.
#
# Usage:
#   tools/apply-libpeer-switch-patches.sh [libpeer-dir]
#
# Environment:
#   LIBPEER_DIR   the libpeer tree to patch
#                 (default: <engine>/extern/libpeer)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOPDIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LIBPEER_DIR="${LIBPEER_DIR:-${1:-${TOPDIR}/extern/libpeer}}"
PATCH_DIR="${SCRIPT_DIR}/libpeer-switch"

if [ ! -f "${LIBPEER_DIR}/src/config.h" ]; then
	echo "error: ${LIBPEER_DIR} does not look like a libpeer tree" >&2
	echo "       (expected ${LIBPEER_DIR}/src/config.h to exist)" >&2
	exit 1
fi

if [ ! -d "${PATCH_DIR}" ]; then
	echo "error: patch directory not found: ${PATCH_DIR}" >&2
	exit 1
fi

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
	echo "==> no libpeer switch patches in ${PATCH_DIR}, nothing to do"
	exit 0
fi

# ---------------------------------------------------------------------------
# Make git resolve the patch paths against the libpeer tree -- ALWAYS.
#
# `git apply` resolves the paths of a patch relative to the ROOT of the
# repository it runs in, NOT relative to the current directory.  extern/libpeer
# usually has no repository of its own (the submodule is not initialised) and
# it lives INSIDE the engine repository, so git resolves "src/config.h"
# against the ENGINE root, does not find it there, and SILENTLY SKIPS every
# patched path -- while still reporting SUCCESS.  The build would then link an
# UNPATCHED libpeer and the only symptom would be a missing symbol at compile
# time.
#
# GIT_CEILING_DIRECTORIES does not fix that: a ceiling stops git from
# *descending into* a directory, but the upward search still finds the
# enclosing repository, so the paths are still resolved against the engine
# root.
#
# The reliable way is to hand git a repository whose WORK TREE is the libpeer
# tree: then "src/config.h" can only ever mean "<libpeer>/src/config.h".
# A throwaway bare repository is used for that -- it is never written to, and
# it makes the result independent of whether extern/libpeer happens to carry
# a .git of its own.
# ---------------------------------------------------------------------------
SANDBOX="$(mktemp -d)"
cleanup() {
	rm -rf "${SANDBOX}"
}
trap cleanup EXIT

GIT_SANDBOX="${SANDBOX}/repo"
SCRATCH="${SANDBOX}/scratch"
mkdir -p "${SCRATCH}"
if ! git init -q --bare "${GIT_SANDBOX}" >/dev/null 2>&1; then
	echo "error: could not create the temporary git repository at ${GIT_SANDBOX}" >&2
	exit 1
fi

# Run `git apply` against a given tree.
#   $1 = the tree, the rest = git apply arguments
git_apply() {
	_tree="$1"
	shift
	GIT_DIR="${GIT_SANDBOX}" GIT_WORK_TREE="${_tree}" git -C "${_tree}" "$@"
}

# ---------------------------------------------------------------------------
# "Is this patch already in the tree?"
#
# `git apply --reverse --check` is the exact answer -- but it is NOT reliable
# once a LATER patch of the same stack touched the same lines: the reverse
# then no longer matches the file.  Concretely, on a fully patched tree
# 0001/0004/0005/0006 all fail their reverse check (0002..0007 rewrote the
# very lines they added), so a naive "reverse == applied" test reports
# "0001 does not apply" on a tree that HAS 0001 -- and the next build fails.
#
# The stack is therefore peeled on a scratch copy of the files it touches:
# undo the patches from the top down and every patch whose reverse matches
# AT THAT POINT is provably in the tree.  The scratch copy holds only the
# touched files, so it is cheap.
# ---------------------------------------------------------------------------
stack_files="$(
	for patch in "${patches[@]}"; do
		sed -n 's|^+++ b/||p' "${patch}"
	done | sort -u
)"

scratch="${SCRATCH}"
for rel in ${stack_files}; do
	if [ -f "${LIBPEER_DIR}/${rel}" ]; then
		mkdir -p "${scratch}/$(dirname "${rel}")"
		cp "${LIBPEER_DIR}/${rel}" "${scratch}/${rel}"
	fi
done

# How many leading patches are already in the tree?  The stack is applied in
# order, so the patches present in the tree are always a prefix: the first
# patch whose forward check succeeds is the first one that is missing.
present=0
while [ "${present}" -lt "${#patches[@]}" ]; do
	if git_apply "${LIBPEER_DIR}" apply --check --ignore-whitespace \
		"${patches[present]}" >/dev/null 2>&1; then
		break
	fi
	present=$((present + 1))
done

# Prove the prefix: peel it on the scratch copy, from the top down.  Every
# reverse that matches there is a proof that the patch is in the tree.
for ((i = present - 1; i >= 0; i--)); do
	patch="${patches[i]}"
	name="$(basename "${patch}")"
	if ! git_apply "${scratch}" apply --reverse --check --ignore-whitespace \
		"${patch}" >/dev/null 2>&1; then
		echo "error: ${name} is expected to be in ${LIBPEER_DIR} but is not." >&2
		echo "       the libpeer tree does not match the patch stack -- reset it" >&2
		exit 1
	fi
	git_apply "${scratch}" apply --reverse --ignore-whitespace "${patch}"
done

for ((i = 0; i < present; i++)); do
	echo "==> $(basename "${patches[i]}"): already applied, skipping"
done

# ---------------------------------------------------------------------------
# Apply the rest of the stack.
# ---------------------------------------------------------------------------
for ((i = present; i < ${#patches[@]}; i++)); do
	patch="${patches[i]}"
	name="$(basename "${patch}")"

	if ! git_apply "${LIBPEER_DIR}" apply --check --ignore-whitespace \
		"${patch}" >/dev/null 2>&1; then
		echo "error: ${name} does not apply to ${LIBPEER_DIR}" >&2
		echo "       the libpeer baseline changed -- rebase the patch" >&2
		exit 1
	fi

	git_apply "${LIBPEER_DIR}" apply --ignore-whitespace "${patch}"

	# The apply must have REALLY landed.  `git apply` can report success while
	# ignoring every path (see the note above), and that would leave the
	# build unpatched without any error at all.  Reversing the patch must
	# now succeed -- that is the proof that the tree carries the fixes.
	if ! git_apply "${LIBPEER_DIR}" apply --reverse --check --ignore-whitespace \
		"${patch}" >/dev/null 2>&1; then
		echo "error: ${name} reported success but is NOT in ${LIBPEER_DIR}" >&2
		echo "       (git apply ignored the patch paths -- refusing to continue)" >&2
		exit 1
	fi

	echo "==> ${name}: applied"
done

echo "==> libpeer switch patches ready in ${LIBPEER_DIR}"
