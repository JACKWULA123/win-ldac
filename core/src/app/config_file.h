// config_file — load / save user-facing configuration for win-ldac.
//
// Stores what device we're paired with, plus a few preferences. Backed
// by a tiny key=value text file kept next to the exe (same pattern as
// win-ldac-keys.db). The format is human-editable; lines starting with
// '#' are comments and unknown keys are ignored on load so adding new
// fields in future versions is backwards-compatible.
//
// Threading: not internally thread-safe; the GUI thread is responsible
// for serialising save/load with respect to its own engine_post_*
// dispatches.

#ifndef WIN_LDAC_CONFIG_FILE_H
#define WIN_LDAC_CONFIG_FILE_H

#include <stdbool.h>
#include <stdint.h>

#include "bluetooth.h"   // bd_addr_t

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIN_LDAC_CFG_BITRATE_FIXED_HQ = 0,
    WIN_LDAC_CFG_BITRATE_ADAPTIVE = 1,
} win_ldac_cfg_bitrate_t;

typedef struct {
    bool                   has_target;        // true if target_addr is meaningful
    bd_addr_t              target_addr;       // {0,0,0,0,0,0} if unpaired
    char                   target_name[64];   // remote name shown in GUI; may be empty
    win_ldac_cfg_bitrate_t bitrate_mode;      // default FIXED_HQ
} win_ldac_config_t;

// Populate `out` from disk. Returns true if the file exists and was
// parsed; false if the file is missing or malformed. On false, `out`
// is zero-initialised (has_target=false, bitrate_mode=FIXED_HQ).
bool win_ldac_config_load(win_ldac_config_t* out);

// Atomically write `cfg` to disk (writes to .tmp + rename). Returns
// true on success.
bool win_ldac_config_save(const win_ldac_config_t* cfg);

// Resolved absolute path to the config file. Pinned next to the exe.
const char* win_ldac_config_path(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_CONFIG_FILE_H
