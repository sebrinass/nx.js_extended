# ---------------------------------------------------------------------------
# Switch toolchain wrapper for the libpeer cross build.
#
# It uses the OFFICIAL devkitPro Switch toolchain verbatim:
#
#     ${DEVKITPRO}/cmake/Switch.cmake
#
# and only adds one project-scoped workaround on top of it.
#
# Why the workaround is needed
# ---------------------------
# mbedtls 3.4.0 hard-errors in two platform modules unless the target looks
# like Unix or Windows:
#
#     library/net_sockets.c:34
#         #error "This module only works on Unix and Windows,
#                  see MBEDTLS_NET_C in mbedtls_config.h"
#
#     library/timing.c:28
#         #error "This module only works on Unix and Windows,
#                  see MBEDTLS_TIMING_C in mbedtls_config.h"
#
# (library/entropy_poll.c has the same guard, but that module is not compiled
# at all because MBEDTLS_NO_PLATFORM_ENTROPY stays enabled -- see
# tools/fix-mbedtls-config.sh.)
#
# The Nintendo Switch (newlib + libnx) is neither __unix__ nor _WIN32, but it
# DOES provide the POSIX/BSD API that those modules use:
#
#     * libnx implements socket()/bind()/connect()/send()/recv()/poll()/...,
#       so library/net_sockets.c compiles and works as-is.
#     * newlib provides gmtime_r()/clock_gettime()/nanosleep().
#
# Declaring the POSIX flavour with -D__unix__ lets both modules compile and
# behave correctly.  It is also what makes the mbedtls config fixup
# (MBEDTLS_NET_C / MBEDTLS_TIMING_C on) buildable for the Switch at all.
#
# It is applied ONLY to the mbedtls sub-project (CMAKE_SOURCE_DIR ends in
# "mbedtls"), so libpeer, usrsctp, libsrtp and cJSON are built untouched.
#
# Verified for the other mbedtls files that react to __unix__:
#     platform_util.c -> gmtime_r()      (newlib has it)
#     threading.c     -> nanosleep()     (newlib has it)
#     timing.c        -> clock_gettime() (newlib has it)
#     sha256.c/sha512.c -> only inside the opt-in MBEDTLS_SHA*_USE_A64_CRYPTO_*
#                          macros, which are off by default.
#     libnx headers   -> do not branch on __unix__ at all.
# ---------------------------------------------------------------------------

# The official devkitPro Switch toolchain resolves DEVKITPRO from the
# ENVIRONMENT (see dkp-initialize-path.cmake) - the CMake variable is not
# populated yet while the toolchain file is being processed.
if(DEFINED ENV{DEVKITPRO})
	set(_libpeer_dkp "$ENV{DEVKITPRO}")
else()
	set(_libpeer_dkp "/opt/devkitpro")
endif()

include("${_libpeer_dkp}/cmake/Switch.cmake")
get_filename_component(_libpeer_toolchain_project "${CMAKE_SOURCE_DIR}" NAME)
if(_libpeer_toolchain_project STREQUAL "mbedtls")
	add_compile_definitions(-D__unix__)
endif()

# ---------------------------------------------------------------------------
# 接收侧余量保险：把 libpeer 每帧的接收 drain 上限从 32 提到 64。
#
# 为什么（下面的数字全部来自真机实测，不是估算）：
#
#   * libpeer 的收包是 **poll 式**：接收循环 peer_connection_loop() 由
#     **渲染帧** 驱动（engine/source/webrtc_backend_libpeer.cc:2029-2051）。
#     每个 tick 最多收 AGENT_RECV_DRAIN_MAX 个数据报
#     （engine/extern/libpeer/src/peer_connection.c:1430），socket 干了就 break。
#   * 这个上限是**带 #ifndef 守卫的编译期常量**
#     （engine/extern/libpeer/src/agent.h:179-180）：
#         #ifndef AGENT_RECV_DRAIN_MAX
#         #define AGENT_RECV_DRAIN_MAX 32
#     ⇒ 可以在编译期用 -D 覆盖 ⇒ **不需要改 extern/libpeer/ 下的任何源码**
#       （那里是别人的地盘，而且会被编译脚本重置；改了还会和内层补丁的 hunk 打架）。
#   * 实测渲染 fps：**min 7.8 / median 21.2 / max 27.9**；
#     云端 dispatch 里配的是 bit_rate=5120000（5.12 Mbps）。
#   * 按 1200 B/包估算，32 包/帧的接收天花板：
#         27.9 fps ≈ 8.6 Mbps
#         21.2 fps ≈ 6.5 Mbps
#          7.8 fps ≈ 2.4 Mbps
#     ⇒ fps 一旦掉到 ~17 以下，内核就开始**丢 UDP 包**
#       （SO_RCVBUF 只有 256 KB，扛不住持续超额）
#       ⇒ 相当于把「饿死」换成「花屏」。
#   * 提到 64 包/帧之后：
#         21.2 fps ≈ 13 Mbps
#          7.8 fps ≈ 4.8 Mbps
#     ⇒ 对 5.12 Mbps 的余量明显变好。
#
# 只作用于 libpeer 子工程（CMAKE_SOURCE_DIR 的目录名是 "libpeer"），
# mbedtls / usrsctp / libsrtp / cJSON 保持原样。
# ---------------------------------------------------------------------------
if(_libpeer_toolchain_project STREQUAL "libpeer")
	add_compile_definitions(-DAGENT_RECV_DRAIN_MAX=64)
endif()
