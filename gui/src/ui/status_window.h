// status_window — the one ImGui panel that fills the win-ldac window.
// Reads engine_get_status_snapshot() each frame, displays connection
// state + codec / sample rate / live bitrate + 60-second bitrate chart,
// and offers Fixed/Adaptive radio + Reconnect / Quit buttons.
//
// Owns a small ring buffer of effective_kbps samples (1 per ~100 ms,
// 600 points = 60 s). The buffer is filled from the main GUI thread on
// a wall-clock timer.

#pragma once

#include <cstdint>

#include "imgui.h"

extern "C" {
#include "engine/engine.h"
}

namespace win_ldac {

struct StatusSamples {
    static constexpr int kPointsPerSecond = 10;
    static constexpr int kWindowSeconds   = 60;
    static constexpr int kCapacity        = kPointsPerSecond * kWindowSeconds;

    float kbps[kCapacity] = {0};   // effective_kbps history, oldest..newest
    float xs  [kCapacity] = {0};   // seconds relative to now (negative)
    int   count           = 0;     // number of valid points (≤ kCapacity)

    uint64_t last_sample_ms = 0;   // wall clock of last push

    void maybe_push(int effective_kbps, uint64_t now_ms);
    void reset();
};

// Resources owned by main.cpp and handed to the draw function each
// frame. Empty / null fields trigger graceful fallback paths in the
// drawing code.
struct UiResources {
    ImFont*       font              = nullptr;  // Maple Mono if loaded, else default
    const char*   font_loaded_name  = nullptr;
    ImTextureID   monkey_tex        = 0;
    float         monkey_aspect     = 1.0f;     // width / height
};

// Draw the full status window. Returns true if the user clicked the
// Quit button.
bool draw_status_window(const ::engine_status_t* st,
                        StatusSamples* samples,
                        const UiResources& ui);

}  // namespace win_ldac
