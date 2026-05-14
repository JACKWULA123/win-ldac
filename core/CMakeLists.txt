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

    # M3.1: A2DP source with SBC codec, sine wave generator → XM5.
    # Hardcodes the XM5 BD_ADDR captured in M2.
    add_executable(a2dp_sine_test
        src/tools/a2dp_sine_test.c
    )
    target_link_libraries(a2dp_sine_test PRIVATE btstack)

    # M3.2: A2DP source with SBC codec, audio from WASAPI loopback.
    # Same plumbing as M3.1 but the audio frames come from whatever
    # Windows is currently rendering on its default endpoint.
    add_library(wasapi_loopback STATIC
        src/audio/wasapi_loopback.cpp
    )
    target_include_directories(wasapi_loopback PUBLIC src)
    target_link_libraries(wasapi_loopback PUBLIC ole32)

    add_executable(a2dp_wasapi_test
        src/tools/a2dp_wasapi_test.c
    )
    target_include_directories(a2dp_wasapi_test PRIVATE src)
    target_link_libraries(a2dp_wasapi_test PRIVATE btstack wasapi_loopback)

    # M4: negotiate LDAC over A2DP (no audio streaming). Connects to XM5,
    # advertises only an LDAC SEP, drives the vendor SET_CONFIGURATION,
    # and reports the negotiated parameters. PASSes if XM5 accepts an
    # LDAC config.
    add_executable(a2dp_ldac_test
        src/tools/a2dp_ldac_test.c
    )
    target_link_libraries(a2dp_ldac_test PRIVATE btstack)

    # M5: full LDAC playback. WASAPI loopback → libldac encoder → AVDTP
    # MEDIA PDUs → XM5. Acceptance test plays whatever Windows is
    # rendering through the headphones, and Sony Headphones Connect
    # reports the codec as LDAC.
    add_library(ldac_wrapper STATIC
        src/ldac/ldac_wrapper.c
    )
    target_include_directories(ldac_wrapper PUBLIC src)
    target_link_libraries(ldac_wrapper PUBLIC ldac_enc ldac_abr)

    add_library(a2dp_ldac_source STATIC
        src/a2dp_ldac/a2dp_ldac_caps.c
        src/a2dp_ldac/a2dp_ldac_source.c
    )
    target_include_directories(a2dp_ldac_source PUBLIC src)
    target_link_libraries(a2dp_ldac_source PUBLIC btstack ldac_wrapper)

    # M6 (phase 1): persistent BTstack link key store, so we don't have
    # to re-pair the XM5 every time we restart the program / reboot.
    add_library(bt_link_key_db STATIC
        src/bt/link_key_db_file.c
    )
    target_include_directories(bt_link_key_db PUBLIC src)
    target_link_libraries(bt_link_key_db PUBLIC btstack)

    add_executable(a2dp_ldac_play_test
        src/tools/a2dp_ldac_play_test.c
    )
    target_include_directories(a2dp_ldac_play_test PRIVATE src)
    target_link_libraries(a2dp_ldac_play_test
        PRIVATE btstack wasapi_loopback a2dp_ldac_source bt_link_key_db)

    # M6.2: auto-reconnect. Same playback path as the M5 tool but the
    # link stays alive — if XM5 powers off / goes out of range, we keep
    # retrying until it comes back. The reconnect supervisor lives in
    # core/src/app/ and is reused later by the real win-ldac-core entry
    # point.
    add_library(reconnect_supervisor STATIC
        src/app/reconnect_supervisor.c
    )
    target_include_directories(reconnect_supervisor PUBLIC src)
    target_link_libraries(reconnect_supervisor PUBLIC btstack)

    add_executable(a2dp_ldac_persistent_test
        src/tools/a2dp_ldac_persistent_test.c
    )
    target_include_directories(a2dp_ldac_persistent_test PRIVATE src)
    target_link_libraries(a2dp_ldac_persistent_test
        PRIVATE btstack wasapi_loopback a2dp_ldac_source bt_link_key_db
                reconnect_supervisor)

    # M7: the headless engine. Bundles all the BTstack/A2DP/WASAPI/LDAC
    # plumbing behind a small API (engine_init/run/stop + a snapshot
    # query + cross-thread command posts). Both the CLI test tool and
    # the upcoming Dear ImGui GUI link this library; the engine itself
    # is GUI-agnostic.
    add_library(core_engine STATIC
        src/engine/engine.c
    )
    target_include_directories(core_engine PUBLIC src)
    target_link_libraries(core_engine
        PUBLIC btstack wasapi_loopback a2dp_ldac_source bt_link_key_db
               reconnect_supervisor)

    # M8: config file persistence. Used by the GUI to read the target
    # device on startup and write it after pairing.
    add_library(config_file STATIC
        src/app/config_file.c
    )
    target_include_directories(config_file PUBLIC src)
    target_link_libraries(config_file PUBLIC btstack)

    add_executable(engine_persistent_test
        src/tools/engine_persistent_test.c
    )
    target_include_directories(engine_persistent_test PRIVATE src)
    target_link_libraries(engine_persistent_test PRIVATE core_engine)
endif()
