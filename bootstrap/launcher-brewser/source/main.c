// brewser launcher (NRO flavor).
//
// `sdmc:/switch/brewser.nro` — the only icon in hbmenu, and the only file that
// must never be rewritten. Its whole job:
//
//   1. scan `sdmc:/switch/.brewser/` for `shell-<version>.nro` candidates
//      (the leading "." makes hbmenu skip the directory entirely);
//   2. sort them by semver, highest first;
//   3. for each, verify it is a plausible NRO (size + "NRO0" magic) — because a
//      missing/corrupt next NRO makes hbloader diagAbort() into a fatal screen;
//   4. envSetNextLoad() the first one that passes and `return 0` ⇒ hbloader
//      unmaps this process and maps the shell in place (no trip back to hbmenu);
//   5. if every candidate fails, show an ASCII error screen (never crash).
//
// argv[0] for the next process MUST be the shell's own path: the shell's libnx
// runtime calls romfsMountSelf(), which resolves RomFS through `__system_argv[0]`
// (libnx romfs_dev.c:266-268). So the arg string is just `"<shell path>"`, which
// also makes `Switch.argv[0]` — what the deployer uses to tell "which shell is
// running" — report `sdmc:/switch/.brewser/shell-<ver>.nro`.
//
// Deliberately NOT here: fsInitialize()/fsdevMountSdmc(). hbloader already
// mounted `sdmc:` for homebrew NROs and neither the existing nx.js bootstrap
// launcher nor the engine mounts it themselves; mounting again would leak a
// mount into the chainloaded shell (its own fsdevMountSdmc would then fail).
//
// Kept in sync with `../source/resolve.c` nx_resolve_runtime() (same dir scan,
// prefix/suffix parsing and semver ordering) — only the directory, the name
// scheme and the extra validation/fallback differ.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "match.h"

// ★ 2026-10-27: shell 目录从 sdmc:/switch/.brewser 挪到 sdmc:/brewser。
//   原因：hbmenu 会把 sdmc:/switch/** 里的 .nro 全部当图标显示（连点开头的
//   目录、_trash 都不放过）⇒ 回退档/旧壳一放进去就多一个「一模一样」的 brewser
//   图标，用户根本分不清点的是哪个。sdmc:/brewser 在 switch 之外，hbmenu 完全
//   不扫 ⇒ 图标永远只有 launcher 这一个。
#define SHELL_DIR "sdmc:/brewser"
#define SHELL_PREFIX "shell-"
#define SHELL_SUFFIX ".nro"

// A real shell is ~79 MB; 1 MiB is a "definitely not a truncated download"
// floor. Anything smaller is a half-written file or an HTML error page.
#define SHELL_MIN_BYTES (1024 * 1024)

// hbloader stores the next NRO path in a 512-byte array (libnx env.c:194-210);
// going past it corrupts memory. Keep a margin.
#define MAX_NEXT_PATH 480

#define MAX_CANDIDATES 32
#define VERSION_MAX 64

typedef struct {
	char version[VERSION_MAX];
	char path[FS_MAX_PATH];
} candidate_t;

/* ------------------------------------------------------------------------- */
/*  error screen (ASCII only — the libnx console font has no CJK glyphs)     */
/* ------------------------------------------------------------------------- */

static void wait_for_plus_and_exit(void) {
	PadState pad;
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	printf("\n  Press \x1b[1m+\x1b[0m to exit.\n");
	consoleUpdate(NULL);
	while (appletMainLoop()) {
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
}

static void fail_screen(const candidate_t *list, int n, const char *extra) {
	consoleInit(NULL);
	printf("\n  \x1b[1m\x1b[36mbrewser\x1b[0m launcher\n");
	printf("  --------------\n\n");
	printf("  \x1b[33mNo usable shell was found.\x1b[0m\n\n");
	if (extra && extra[0])
		printf("  %s\n\n", extra);
	printf("  looked in  %s/%s<version>%s\n", SHELL_DIR, SHELL_PREFIX,
	       SHELL_SUFFIX);
	printf("  a shell must be > 1 MiB and start with an NRO0 header\n\n");
	printf("  candidates:\n");
	if (n == 0) {
		printf("    (none)\n");
	} else {
		for (int i = 0; i < n; i++) {
			struct stat st;
			if (stat(list[i].path, &st) != 0) {
				printf("    - shell-%s.nro  (cannot stat)\n", list[i].version);
			} else if (st.st_size < SHELL_MIN_BYTES) {
				printf("    - shell-%s.nro  (too small: %lld bytes)\n",
				       list[i].version, (long long)st.st_size);
			} else {
				printf("    - shell-%s.nro  (no NRO0 header)\n", list[i].version);
			}
		}
	}
	printf("\n  Install a shell with the deployer app, i.e. put\n");
	printf("  \x1b[36mshell-<version>.nro\x1b[0m into \x1b[1m%s/\x1b[0m\n",
	       SHELL_DIR);
	wait_for_plus_and_exit();
}

/* ------------------------------------------------------------------------- */
/*  candidate discovery + ordering (mirrors source/resolve.c:36-76)          */
/* ------------------------------------------------------------------------- */

// "0.1.131", "1.2.0-beta.1" ⇒ plausible. Rejects "..", ".deploytmp" leftovers,
// "shell-0.1.131.nro.deploytmp" (doesn't match the suffix anyway) etc.
static bool version_is_plausible(const char *v) {
	if (v[0] < '0' || v[0] > '9')
		return false;
	bool dot = false;
	for (const char *p = v; *p; p++) {
		char c = *p;
		if (c == '.')
			dot = true;
		else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		           (c >= 'A' && c <= 'Z') || c == '-' || c == '+'))
			return false;
	}
	return dot;
}

static int scan_candidates(candidate_t *out, int max) {
	DIR *dir = opendir(SHELL_DIR);
	if (!dir)
		return 0;

	int n = 0;
	struct dirent *ent;
	size_t plen = strlen(SHELL_PREFIX);
	size_t slen = strlen(SHELL_SUFFIX);
	while ((ent = readdir(dir)) != NULL && n < max) {
		const char *fn = ent->d_name;
		size_t fnlen = strlen(fn);
		if (fnlen <= plen + slen)
			continue;
		if (strncasecmp(fn, SHELL_PREFIX, plen) != 0)
			continue;
		if (strcasecmp(fn + fnlen - slen, SHELL_SUFFIX) != 0)
			continue;
		size_t vlen = fnlen - plen - slen;
		if (vlen >= VERSION_MAX)
			continue;
		char ver[VERSION_MAX];
		memcpy(ver, fn + plen, vlen);
		ver[vlen] = '\0';
		if (!version_is_plausible(ver))
			continue;
		int dup = 0;
		for (int i = 0; i < n; i++)
			if (strcasecmp(out[i].version, ver) == 0)
				dup = 1;
		if (dup)
			continue;
		snprintf(out[n].version, sizeof(out[n].version), "%s", ver);
		snprintf(out[n].path, sizeof(out[n].path), "%s/%s", SHELL_DIR, fn);
		n++;
	}
	closedir(dir);

	// Insertion sort, highest version first (same comparator the nx.js slim
	// launcher uses to pick its runtime).
	for (int i = 1; i < n; i++) {
		candidate_t key = out[i];
		int j = i - 1;
		while (j >= 0 && nx_version_compare(out[j].version, key.version) < 0) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = key;
	}
	return n;
}

/* ------------------------------------------------------------------------- */
/*  validation + hand-off                                                    */
/* ------------------------------------------------------------------------- */

// Returns true if `path` looks like a loadable NRO. On failure sets *reason.
static bool shell_file_usable(const char *path, const char **reason) {
	struct stat st;
	if (stat(path, &st) != 0) {
		*reason = "stat failed";
		return false;
	}
	if ((long long)st.st_size < SHELL_MIN_BYTES) {
		*reason = "file too small";
		return false;
	}

	unsigned char hdr[32];
	FILE *f = fopen(path, "rb");
	if (!f) {
		*reason = "cannot open";
		return false;
	}
	size_t got = fread(hdr, 1, sizeof(hdr), f);
	fclose(f);
	if (got < 20) {
		*reason = "short header read";
		return false;
	}
	// Same criterion as the deployer's nroMagicOk(): scan the first 32 bytes
	// for "NRO0" — the magic sits at offset 0x10 in a normal NRO and at 0 in the
	// alternative layout.
	for (size_t i = 0; i + 4 <= got; i++) {
		if (memcmp(hdr + i, "NRO0", 4) == 0)
			return true;
	}
	*reason = "no NRO0 header";
	return false;
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	// Chainloading only works when hbloader owns this process (i.e. we were
	// started from hbmenu / Sphere). Otherwise there is no next-load slot.
	if (!envHasNextLoad()) {
		fail_screen(NULL, 0,
		            "This app must be launched from the homebrew menu (hbmenu).");
		return 0;
	}

	candidate_t list[MAX_CANDIDATES];
	int n = scan_candidates(list, MAX_CANDIDATES);

	// Walk highest → lowest, so a broken newest shell falls back to the previous
	// one instead of bricking the launcher.
	const char *last_reason = NULL;
	for (int i = 0; i < n; i++) {
		const char *reason = NULL;
		if (!shell_file_usable(list[i].path, &reason)) {
			last_reason = reason;
			continue;
		}
		if (strlen(list[i].path) >= MAX_NEXT_PATH) {
			last_reason = "path too long";
			continue;
		}

		char args[FS_MAX_PATH + 8];
		snprintf(args, sizeof(args), "\"%s\"", list[i].path);

		Result rc = envSetNextLoad(list[i].path, args);
		if (R_SUCCEEDED(rc))
			return 0; // hbloader now maps the shell — no hbmenu round trip

		last_reason = "envSetNextLoad failed";
	}

	fail_screen(list, n, last_reason);
	return 0;
}
