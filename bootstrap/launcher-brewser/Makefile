#---------------------------------------------------------------------------------
# brewser launcher (NRO) — a tiny (~200 KB) homebrew NRO that chainloads the
# highest installed versioned shell from `sdmc:/switch/.brewser/shell-<ver>.nro`
# via envSetNextLoad. libnx only (no V8/Skia/romfs/ssl), so the output stays
# small and — crucially — this NRO is the ONE file that never has to be written
# again: shell/engine updates land in `.brewser/` (hidden from hbmenu) and this
# launcher picks them up on the next tap.
#
# It reuses the shared semver matcher (../source/match.c + ../source/vendor/
# semver.c) but none of the nx.js runtime resolving / download / UI code, so
# those .c files are excluded from the link (see BOOTSTRAP_EXCLUDE_C).
#---------------------------------------------------------------------------------
# Absolute dir of THIS Makefile (so `include` works from the recursive build/
# sub-make where CURDIR changes).
THIS_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

TARGET		:=	launcher
NXJS_OUTPUT	:=	nro

# ---- NACP metadata (2026-10-27 revision) -------------------------------
# 这里曾经要求「和 79MB 壳一模一样」（同 title/author/version/icon），前提是 hbmenu 里
# 只有这一个 brewser。实际 hbmenu 会把 switch/** 里所有 .nro 都当图标显示（点开头的
# 目录、_trash 都不放过）⇒ 壳/回退档一多就出现一排无法区分的图标，用户被迫瞎猜点错。
# 现在壳挪到 sdmc:/brewser（hbmenu 不扫）⇒ 启动器是唯一入口，title 改成一眼可认的
# 名字，version 用真实值（不再是壳的版本号）。
APP_TITLE	:=	brewser云游戏
APP_AUTHOR	:=	Natureglass
APP_VERSION	:=	0.1.132
APP_TITLEID	:=	01f5905036d20000

# Unused by this launcher (no romfs, no [runtime] requirement), but common.mk
# bakes it into CFLAGS — set it explicitly so jq is not needed at build time.
NXJS_RUNTIME_DEFAULT := *

# icon.jpg lives next to this Makefile (copied from the shell's icon) so
# common.mk's default `*.jpg` wildcard picks it up as the NRO icon.
ICON		:=	icon.jpg

BOOTSTRAP_EXCLUDE_C := download.c ui.c resolve.c

include $(THIS_MK_DIR)/../common.mk
