# core: win-ldac-core.exe (the headless engine) + standalone tools.
#
# In M1 only the encoder test tool exists. The main.cpp is a placeholder
# that will grow as later milestones add WASAPI capture, BTstack glue, etc.

add_executable(win-ldac-core
    src/main.cpp
)
target_link_libraries(win-ldac-core PRIVATE ldac_enc ldac_abr)

# ── Tools ────────────────────────────────────────────────────
# M1: encode a synthetic PCM signal with libldac and verify ~990 kbps.
add_executable(ldac_encode_test
    src/tools/ldac_encode_test.cpp
)
target_link_libraries(ldac_encode_test PRIVATE ldac_enc)

# M2: BTstack HCI on the windows-winusb transport. Power up, do GAP
# Classic Inquiry, request remote names, print results. Acceptance:
# WH-1000XM5 appears in the output.
if(HAVE_BTSTACK)
    add_executable(hci_scan_test
        src/tools/hci_scan_test.c
    )
    target_link_libraries(hci_scan_test PRIVATE btstack)
endif()
