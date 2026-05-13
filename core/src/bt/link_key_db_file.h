// link_key_db_file — Windows-friendly persistent BTstack link key store.
//
// Reuses BTstack's btstack_link_key_db_t interface. Drops in via:
//
//     hci_init(...);
//     hci_set_link_key_db(win_ldac_link_key_db_instance());
//     ...
//     hci_power_control(HCI_POWER_ON);
//
// Storage: a single text file `win-ldac-keys.db` placed next to the exe
// (resolved from GetModuleFileNameA). One line per remote device:
//
//     <bd_addr> <link_key_hex> <type>
//
// e.g.
//
//     # win-ldac link key store v1
//     88:C9:E8:F7:D5:F3 1A2B3C4D5E6F70718283849596A7B8C9 4
//
// Why not JSON: link keys are a fixed-shape, small record. A line-based
// format keeps us off any JSON dependency, is trivially inspectable,
// and survives hand-editing if anyone wants to revoke a key. We can
// migrate to nlohmann/json later if we ever store richer per-device
// state (codec prefs, last-seen RSSI, etc.).
//
// Threading: BTstack calls into this from the run loop thread only.

#ifndef WIN_LDAC_LINK_KEY_DB_FILE_H
#define WIN_LDAC_LINK_KEY_DB_FILE_H

#include "classic/btstack_link_key_db.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the BTstack link key DB instance backed by a text file in
// the exe directory. Safe to call from main() before hci_init().
const btstack_link_key_db_t* win_ldac_link_key_db_instance(void);

// For diagnostics. Returns the absolute path to the backing file (or
// "<unresolved>" if the path couldn't be determined yet).
const char* win_ldac_link_key_db_path(void);

#ifdef __cplusplus
}
#endif

#endif  // WIN_LDAC_LINK_KEY_DB_FILE_H
