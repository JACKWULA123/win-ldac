// config_file — see config_file.h.

#include "app/config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "btstack_util.h"  // sscanf_bd_addr, bd_addr_to_str

#define CONFIG_FILE_NAME  "win-ldac-config.cfg"

static char g_file_path[MAX_PATH] = "<unresolved>";

static bool resolve_path(void) {
    if (g_file_path[0] != '<') return true;
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return false;
    char* sep = strrchr(exe, '\\');
    if (!sep) return false;
    *sep = '\0';
    int n = snprintf(g_file_path, sizeof(g_file_path),
                     "%s\\" CONFIG_FILE_NAME, exe);
    return n > 0 && n < (int)sizeof(g_file_path);
}

const char* win_ldac_config_path(void) {
    if (g_file_path[0] == '<') resolve_path();
    return g_file_path;
}

// Trim leading/trailing whitespace (including CR/LF) in place. Returns
// the start of the trimmed string within `s`.
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

bool win_ldac_config_load(win_ldac_config_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->bitrate_mode = WIN_LDAC_CFG_BITRATE_FIXED_HQ;

    if (!resolve_path()) return false;
    FILE* f = fopen(g_file_path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(p);
        char* val = trim(eq + 1);

        if (strcmp(key, "target_addr") == 0) {
            bd_addr_t a;
            if (sscanf_bd_addr(val, a)) {
                memcpy(out->target_addr, a, sizeof(bd_addr_t));
                // Treat all-zero address as "not paired" even if a stray
                // file lists it explicitly.
                bool nonzero = false;
                for (int i = 0; i < 6; ++i) if (a[i]) { nonzero = true; break; }
                out->has_target = nonzero;
            }
        } else if (strcmp(key, "target_name") == 0) {
            size_t n = strlen(val);
            if (n >= sizeof(out->target_name)) n = sizeof(out->target_name) - 1;
            memcpy(out->target_name, val, n);
            out->target_name[n] = '\0';
        } else if (strcmp(key, "bitrate_mode") == 0) {
            if (strcmp(val, "adaptive") == 0) {
                out->bitrate_mode = WIN_LDAC_CFG_BITRATE_ADAPTIVE;
            } else {
                out->bitrate_mode = WIN_LDAC_CFG_BITRATE_FIXED_HQ;
            }
        }
        // Unknown keys silently ignored for forward compatibility.
    }
    fclose(f);
    return true;
}

bool win_ldac_config_save(const win_ldac_config_t* cfg) {
    if (!cfg) return false;
    if (!resolve_path()) return false;

    char tmp_path[MAX_PATH];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_file_path);
    if (n <= 0 || n >= (int)sizeof(tmp_path)) return false;

    FILE* f = fopen(tmp_path, "w");
    if (!f) return false;

    fprintf(f, "# win-ldac configuration v1 (auto-generated)\n");
    fprintf(f, "# Edit while the program is not running.\n\n");

    if (cfg->has_target) {
        fprintf(f, "target_addr=%s\n", bd_addr_to_str(cfg->target_addr));
        if (cfg->target_name[0]) {
            fprintf(f, "target_name=%s\n", cfg->target_name);
        }
    } else {
        fprintf(f, "# no target_addr set — run pairing flow first\n");
    }

    fprintf(f, "bitrate_mode=%s\n",
            cfg->bitrate_mode == WIN_LDAC_CFG_BITRATE_ADAPTIVE
                ? "adaptive" : "fixed");

    fflush(f);
    fclose(f);

    // Atomic replace: on Windows, MOVEFILE_REPLACE_EXISTING.
    if (!MoveFileExA(tmp_path, g_file_path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp_path);
        return false;
    }
    return true;
}
