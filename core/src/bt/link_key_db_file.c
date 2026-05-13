// link_key_db_file — see link_key_db_file.h.

#include "bt/link_key_db_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "btstack_util.h"
#include "btstack_debug.h"

// ── Storage ────────────────────────────────────────────────────────────
// In-memory cache; persisted to disk on every put/delete. We keep this
// small (16 slots) — a typical user has 1-3 paired devices, and we'd
// rather refuse new pairings than silently evict an old one.
#define MAX_ENTRIES 16

typedef struct {
    bool            in_use;
    bd_addr_t       bd_addr;
    link_key_t      link_key;
    link_key_type_t type;
} entry_t;

static entry_t   entries[MAX_ENTRIES];
static char      file_path[MAX_PATH] = "<unresolved>";
static bd_addr_t local_addr_storage;  // unused for now, kept for parity

// ── Path resolution ────────────────────────────────────────────────────
// Pin the storage file next to the exe. Same convention as the firmware/
// finder, except link keys don't traverse upwards — we want the file to
// live alongside the running binary so each install has its own state.
static bool resolve_file_path(void) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return false;
    char* sep = strrchr(exe, '\\');
    if (!sep) return false;
    *sep = '\0';
    int n = snprintf(file_path, sizeof(file_path),
                     "%s\\win-ldac-keys.db", exe);
    return n > 0 && n < (int)sizeof(file_path);
}

// ── Hex codec ──────────────────────────────────────────────────────────
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a 32-char hex string into a 16-byte link key. Returns true on
// success.
static bool parse_link_key_hex(const char* hex, link_key_t out) {
    if (!hex) return false;
    for (int i = 0; i < LINK_KEY_LEN; ++i) {
        int hi = hex_nibble(hex[i * 2 + 0]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    // Must be exactly 32 chars; reject longer.
    return hex[LINK_KEY_LEN * 2] == '\0' ||
           hex[LINK_KEY_LEN * 2] == ' '  ||
           hex[LINK_KEY_LEN * 2] == '\t';
}

static void format_link_key_hex(const link_key_t key, char* out_33) {
    static const char* nibbles = "0123456789ABCDEF";
    for (int i = 0; i < LINK_KEY_LEN; ++i) {
        out_33[i * 2 + 0] = nibbles[(key[i] >> 4) & 0x0F];
        out_33[i * 2 + 1] = nibbles[(key[i] >> 0) & 0x0F];
    }
    out_33[LINK_KEY_LEN * 2] = '\0';
}

// ── File I/O ───────────────────────────────────────────────────────────
static void load_from_file(void) {
    memset(entries, 0, sizeof(entries));

    FILE* f = fopen(file_path, "r");
    if (!f) {
        // First run: no file is fine, just start empty.
        return;
    }
    char line[256];
    int line_no = 0;
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        // Strip trailing newline / cr.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') continue;

        // Format: <bd_addr> <link_key_hex> <type>
        char addr_str[32], key_str[64];
        int  type_int = 0;
        int  consumed = 0;
        int  matched  = sscanf(line, "%31s %63s %d%n",
                               addr_str, key_str, &type_int, &consumed);
        if (matched != 3) {
            log_info("link_key_db: skip malformed line %d", line_no);
            continue;
        }
        bd_addr_t addr;
        if (!sscanf_bd_addr(addr_str, addr)) {
            log_info("link_key_db: skip bad bd_addr on line %d", line_no);
            continue;
        }
        link_key_t key;
        if (!parse_link_key_hex(key_str, key)) {
            log_info("link_key_db: skip bad link_key on line %d", line_no);
            continue;
        }
        if (loaded >= MAX_ENTRIES) {
            log_info("link_key_db: hit MAX_ENTRIES=%d, ignoring rest", MAX_ENTRIES);
            break;
        }
        entries[loaded].in_use = true;
        memcpy(entries[loaded].bd_addr, addr, 6);
        memcpy(entries[loaded].link_key, key, LINK_KEY_LEN);
        entries[loaded].type = (link_key_type_t)type_int;
        loaded++;
    }
    fclose(f);
    log_info("link_key_db: loaded %d entr%s from %s",
             loaded, loaded == 1 ? "y" : "ies", file_path);
}

// Persist all entries atomically: write to a temp file, then MoveFileEx
// with REPLACE_EXISTING. Crash mid-write leaves the old file intact.
static void save_to_file(void) {
    char tmp_path[MAX_PATH];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", file_path);
    if (n <= 0 || n >= (int)sizeof(tmp_path)) {
        log_error("link_key_db: file_path too long to make tmp");
        return;
    }

    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        log_error("link_key_db: cannot open %s for write", tmp_path);
        return;
    }
    fprintf(f, "# win-ldac link key store v1\n");
    fprintf(f, "# format: <bd_addr> <link_key_hex> <type>\n");
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (!entries[i].in_use) continue;
        char key_hex[LINK_KEY_STR_LEN + 1];
        format_link_key_hex(entries[i].link_key, key_hex);
        fprintf(f, "%s %s %d\n",
                bd_addr_to_str(entries[i].bd_addr),
                key_hex,
                (int)entries[i].type);
    }
    fflush(f);
    fclose(f);

    if (!MoveFileExA(tmp_path, file_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_error("link_key_db: MoveFileEx failed err=%lu", GetLastError());
        DeleteFileA(tmp_path);
    }
}

// ── Slot lookup ────────────────────────────────────────────────────────
static int find_slot(const bd_addr_t bd_addr) {
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (entries[i].in_use &&
            memcmp(entries[i].bd_addr, bd_addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        if (!entries[i].in_use) return i;
    }
    return -1;
}

// ── btstack_link_key_db_t implementation ───────────────────────────────
static void db_open(void) {
    if (!resolve_file_path()) {
        log_error("link_key_db: could not resolve file path, "
                  "running with in-memory keys only");
        memset(entries, 0, sizeof(entries));
        return;
    }
    load_from_file();
}

static void db_close(void) {
    // load_from_file is the source of truth; we already write through.
    // Nothing to flush here.
}

static void db_set_local_bd_addr(bd_addr_t bd_addr) {
    memcpy(local_addr_storage, bd_addr, 6);
    // The POSIX FS implementation namespaces files by local addr because
    // a desktop with multiple dongles might switch between them. For
    // our single-dongle case it doesn't matter; keep all entries in one
    // file and let the local-addr field be informational.
}

static int db_get_link_key(bd_addr_t bd_addr, link_key_t link_key,
                           link_key_type_t* type) {
    int i = find_slot(bd_addr);
    if (i < 0) return 0;
    memcpy(link_key, entries[i].link_key, LINK_KEY_LEN);
    *type = entries[i].type;
    log_info("link_key_db: HIT  %s (type=%d)",
             bd_addr_to_str(bd_addr), (int)*type);
    return 1;
}

static void db_put_link_key(bd_addr_t bd_addr, link_key_t link_key,
                            link_key_type_t type) {
    int i = find_slot(bd_addr);
    if (i < 0) {
        i = find_free_slot();
        if (i < 0) {
            log_error("link_key_db: full, refusing to store new key for %s",
                      bd_addr_to_str(bd_addr));
            return;
        }
    }
    entries[i].in_use = true;
    memcpy(entries[i].bd_addr, bd_addr, 6);
    memcpy(entries[i].link_key, link_key, LINK_KEY_LEN);
    entries[i].type = type;
    log_info("link_key_db: PUT  %s (type=%d)",
             bd_addr_to_str(bd_addr), (int)type);
    save_to_file();
}

static void db_delete_link_key(bd_addr_t bd_addr) {
    int i = find_slot(bd_addr);
    if (i < 0) return;
    entries[i].in_use = false;
    memset(entries[i].link_key, 0, LINK_KEY_LEN);
    memset(entries[i].bd_addr,  0, 6);
    log_info("link_key_db: DEL  %s", bd_addr_to_str(bd_addr));
    save_to_file();
}

// Iterator. BTstack walks this during certain bonding flows and during
// the `gap_delete_all_link_keys` housekeeping call. We expose the in-
// memory array via the context pointer (cast to a uintptr_t-sized index).
//
// We pass the cursor as `(void *)(uintptr_t)next_index` to avoid an
// allocation. iterator_done is a no-op.
static int it_init(btstack_link_key_iterator_t* it) {
    it->context = NULL;  // next index = 0
    return 1;
}

static int it_next(btstack_link_key_iterator_t* it, bd_addr_t bd_addr,
                   link_key_t link_key, link_key_type_t* type) {
    int i = (int)(uintptr_t)it->context;
    while (i < MAX_ENTRIES) {
        if (entries[i].in_use) {
            memcpy(bd_addr,  entries[i].bd_addr, 6);
            memcpy(link_key, entries[i].link_key, LINK_KEY_LEN);
            *type = entries[i].type;
            it->context = (void*)(uintptr_t)(i + 1);
            return 1;
        }
        i++;
    }
    it->context = (void*)(uintptr_t)i;
    return 0;
}

static void it_done(btstack_link_key_iterator_t* it) {
    it->context = NULL;
}

static const btstack_link_key_db_t s_db = {
    &db_open,
    &db_set_local_bd_addr,
    &db_close,
    &db_get_link_key,
    &db_put_link_key,
    &db_delete_link_key,
    &it_init,
    &it_next,
    &it_done,
};

const btstack_link_key_db_t* win_ldac_link_key_db_instance(void) {
    return &s_db;
}

const char* win_ldac_link_key_db_path(void) {
    if (file_path[0] == '<') {
        // db_open() hasn't run yet — resolve now so the caller sees a
        // useful path. Cheap, idempotent.
        resolve_file_path();
    }
    return file_path;
}
