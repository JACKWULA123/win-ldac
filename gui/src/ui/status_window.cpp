#include "ui/status_window.h"

#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "implot.h"

namespace win_ldac {

using ::engine_status_t;
using ::engine_state_t;

// ── Sample ring ────────────────────────────────────────────────────────
void StatusSamples::reset() {
    count = 0;
    last_sample_ms = 0;
}

void StatusSamples::maybe_push(int effective_kbps, uint64_t now_ms) {
    constexpr uint64_t kIntervalMs = 1000 / kPointsPerSecond;
    if (last_sample_ms != 0 && (now_ms - last_sample_ms) < kIntervalMs) return;
    last_sample_ms = now_ms;

    if (count < kCapacity) {
        kbps[count] = static_cast<float>(effective_kbps);
        ++count;
    } else {
        std::memmove(&kbps[0], &kbps[1], sizeof(float) * (kCapacity - 1));
        kbps[kCapacity - 1] = static_cast<float>(effective_kbps);
    }
    for (int i = 0; i < count; ++i) {
        xs[i] = static_cast<float>(-(count - 1 - i)) / kPointsPerSecond;
    }
}

// ── Helpers ────────────────────────────────────────────────────────────
static const char* state_label(engine_state_t s) {
    switch (s) {
    case ENGINE_STATE_IDLE:         return "Idle";
    case ENGINE_STATE_HCI_INIT:     return "Starting...";
    case ENGINE_STATE_DISCONNECTED: return "Disconnected";
    case ENGINE_STATE_CONNECTING:   return "Connecting...";
    case ENGINE_STATE_NEGOTIATING:  return "Negotiating...";
    case ENGINE_STATE_STREAMING:    return "Connected";
    case ENGINE_STATE_STOPPING:     return "Shutting down...";
    default:                        return "?";
    }
}

static ImU32 state_color(engine_state_t s) {
    switch (s) {
    case ENGINE_STATE_STREAMING:
        return IM_COL32(64, 192, 96, 255);
    case ENGINE_STATE_CONNECTING:
    case ENGINE_STATE_NEGOTIATING:
    case ENGINE_STATE_HCI_INIT:
        return IM_COL32(220, 170, 40, 255);
    default:
        return IM_COL32(200, 80, 80, 255);
    }
}

static ImVec4 state_color_vec(engine_state_t s) {
    return ImGui::ColorConvertU32ToFloat4(state_color(s));
}

static const char* eqmid_label(int eqmid) {
    switch (eqmid) {
    case 0: return "HQ";
    case 1: return "SQ";
    case 2: return "MQ";
    default: return "?";
    }
}

// Right-align text within the current item width. Pushes/pops font and
// optional text colour.
static void right_aligned_text(ImFont* font, float size,
                               const char* text, ImU32 col = 0) {
    ImGui::PushFont(font, size);
    float text_w  = ImGui::CalcTextSize(text).x;
    float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w > text_w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - text_w));
    }
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    if (col) ImGui::PopStyleColor();
    ImGui::PopFont();
}

// ── Top device card (now also carries codec / sr / bitrate) ────────────
static void draw_device_card(const engine_status_t* st, UiResources& ui) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
    ImGui::BeginChild("device_card", ImVec2(0, 56),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();

    ImU32  disabled_col = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

    // Build right-column strings up front so we can right-align them.
    char addr[24];
    if (st->has_target) {
        std::snprintf(addr, sizeof(addr),
                      "%02X:%02X:%02X:%02X:%02X:%02X",
                      st->target_addr[0], st->target_addr[1], st->target_addr[2],
                      st->target_addr[3], st->target_addr[4], st->target_addr[5]);
    } else {
        std::snprintf(addr, sizeof(addr), "Click Settings to pair");
    }

    char big_value[32];
    char small_value[48];
    bool streaming = (st->state == ENGINE_STATE_STREAMING && !st->idle_paused);
    if (!st->has_target) {
        std::snprintf(big_value,   sizeof(big_value),   "--");
        std::snprintf(small_value, sizeof(small_value), "(no target)");
    } else if (streaming) {
        std::snprintf(big_value, sizeof(big_value),
                      "%d kbps", st->effective_kbps);
        std::snprintf(small_value, sizeof(small_value),
                      "LDAC %s  %d Hz  %d-bit",
                      eqmid_label(st->eqmid),
                      st->sample_rate_hz, st->wasapi_bit_depth);
    } else if (st->idle_paused) {
        std::snprintf(big_value, sizeof(big_value), "Idle");
        std::snprintf(small_value, sizeof(small_value),
                      "LDAC  %d Hz  %d-bit",
                      st->sample_rate_hz, st->wasapi_bit_depth);
    } else {
        std::snprintf(big_value, sizeof(big_value), "--");
        std::snprintf(small_value, sizeof(small_value),
                      "(not streaming)");
    }

    if (ImGui::BeginTable("hdr", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch, 0.45f);

        // ── Row 1: name + pill | big bitrate ─────────────────────────
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(ui.font, 17.0f);
        float big_h = ImGui::GetTextLineHeight();
        const char* device_label = ui.target_name[0] ? ui.target_name
                                   : (st->has_target ? "Paired device"
                                                     : "Not paired");
        ImGui::TextUnformatted(device_label);
        ImGui::SameLine(0, 10);
        ImGui::PopFont();
        {
            // Centre the small pill vertically inside the big-text line.
            ImGui::PushFont(ui.font, 13.0f);
            float  small_h  = ImGui::GetTextLineHeight();
            float  offset_y = (big_h - small_h) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset_y);

            ImVec2 cs    = ImGui::GetCursorScreenPos();
            float  dot_r = 4.5f;
            float  dot_cy = cs.y + small_h * 0.5f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cs.x + dot_r, dot_cy),
                dot_r, state_color(st->state), 16);
            ImGui::Dummy(ImVec2(dot_r * 2 + 6, 0));
            ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, state_color_vec(st->state));
            ImGui::TextUnformatted(state_label(st->state));
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        ImGui::TableSetColumnIndex(1);
        right_aligned_text(ui.font, 17.0f, big_value);

        // ── Row 2: BD_ADDR | codec · SR · bit-depth ──────────────────
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(ui.font, 11.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, disabled_col);
        ImGui::TextUnformatted(addr);
        if (st->reconnect_attempts > 0 &&
            st->state != ENGINE_STATE_STREAMING) {
            ImGui::SameLine(0, 10);
            ImGui::Text("retry %d", st->reconnect_attempts);
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::TableSetColumnIndex(1);
        right_aligned_text(ui.font, 11.5f, small_value, disabled_col);

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

// ── Chart card (monkey watermark + ImPlot) ─────────────────────────────
static void draw_chart_card(StatusSamples* samples, UiResources& ui) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::BeginChild("chart_card", ImVec2(0, 196),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();

    ImVec2 plot_pos  = ImGui::GetCursorScreenPos();
    ImVec2 avail     = ImGui::GetContentRegionAvail();
    ImVec2 plot_size = ImVec2(avail.x, avail.y);

    if (ui.monkey_tex) {
        // CSS object-fit: cover — fill the whole plot rect, preserve
        // aspect, crop the protruding dimension via centred UVs.
        float chart_aspect = plot_size.x / plot_size.y;
        float img_aspect   = ui.monkey_aspect;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (img_aspect > chart_aspect) {
            float uv_w = chart_aspect / img_aspect;
            uv0.x = (1.0f - uv_w) * 0.5f;
            uv1.x = (1.0f + uv_w) * 0.5f;
        } else {
            float uv_h = img_aspect / chart_aspect;
            uv0.y = (1.0f - uv_h) * 0.5f;
            uv1.y = (1.0f + uv_h) * 0.5f;
        }
        ImGui::GetWindowDrawList()->AddImage(
            ui.monkey_tex,
            plot_pos,
            ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y),
            uv0, uv1, IM_COL32(255, 255, 255, 50));
    }

    ImPlot::PushStyleColor(ImPlotCol_FrameBg,    IM_COL32(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_PlotBg,     IM_COL32(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, IM_COL32(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_AxisText,   IM_COL32(120, 130, 140, 255));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid,   IM_COL32(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_AxisTick,   IM_COL32(200, 205, 210, 200));

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(6, 6));

    if (ImPlot::BeginPlot("##bitrate", plot_size,
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend |
                          ImPlotFlags_NoMouseText | ImPlotFlags_NoFrame)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_NoGridLines |
                          ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_NoGridLines |
                          ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisLimits(ImAxis_X1,
                                -static_cast<double>(StatusSamples::kWindowSeconds),
                                0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1050, ImPlotCond_Always);

        static const double y_refs[3] = { 330.0, 660.0, 990.0 };
        ImPlot::SetupAxisTicks(ImAxis_Y1, y_refs, 3, nullptr, false);

        ImPlotSpec ref_spec;
        ref_spec.LineColor  = ImVec4(0.74f, 0.78f, 0.84f, 0.45f);
        ref_spec.LineWeight = 1.0f;
        for (double y : y_refs) {
            double xs[2] = { -static_cast<double>(StatusSamples::kWindowSeconds), 0 };
            double ys[2] = { y, y };
            ImPlot::PlotLine("##ref", xs, ys, 2, ref_spec);
        }

        if (samples->count > 0) {
            ImPlotSpec line_spec;
            line_spec.LineColor  = ImVec4(0.27f, 0.49f, 0.96f, 1.0f);
            line_spec.LineWeight = 1.8f;
            ImPlot::PlotLine("kbps",
                             samples->xs, samples->kbps, samples->count,
                             line_spec);
        }
        ImPlot::EndPlot();
    }

    ImPlot::PopStyleVar();
    ImPlot::PopStyleColor(6);

    ImGui::EndChild();
}

// Helper: render one half of the segmented mode toggle.
static bool segmented_button(const char* label, bool active, ImVec2 size,
                             ImFont* font) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.27f, 0.49f, 0.96f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.44f, 0.92f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.40f, 0.88f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.96f, 0.96f, 0.97f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.93f, 0.95f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.88f, 0.89f, 0.92f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.30f, 0.34f, 0.40f, 1.00f));
    }
    ImGui::PushFont(font, 13.0f);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed;
}

// ── Mode segmented toggle row ──────────────────────────────────────────
static void draw_mode_row(const engine_status_t* st, StatusSamples* samples,
                          UiResources& ui) {
    const float inner_gap = 4;
    const float btn_h     = 30;

    const float total_w     = ImGui::GetContentRegionAvail().x;
    const float toggle_each = (total_w - inner_gap) / 2.0f;

    bool fixed_active = (st->bitrate_mode != ENGINE_BITRATE_ADAPTIVE);

    if (segmented_button("Fixed (990 kbps)", fixed_active,
                         ImVec2(toggle_each, btn_h), ui.font)) {
        if (!fixed_active) {
            engine_post_set_bitrate_mode(ENGINE_BITRATE_FIXED_HQ);
            samples->reset();
            if (ui.on_bitrate_persist) ui.on_bitrate_persist(false);
        }
    }
    ImGui::SameLine(0, inner_gap);
    if (segmented_button("Adaptive", !fixed_active,
                         ImVec2(toggle_each, btn_h), ui.font)) {
        if (fixed_active) {
            engine_post_set_bitrate_mode(ENGINE_BITRATE_ADAPTIVE);
            samples->reset();
            if (ui.on_bitrate_persist) ui.on_bitrate_persist(true);
        }
    }
}

// ── Unsupported-rate error popup ──────────────────────────────────────
// Shown when the engine reports the Windows mixer is at a rate LDAC
// can't accept (e.g. 192 kHz). Caches the rate that was last acked so
// the popup doesn't reopen every frame after the user dismisses it,
// but does reopen if the rate changes to a different unsupported value.
static int g_acked_unsupported_rate = 0;

static void draw_unsupported_rate_popup(const engine_status_t* st,
                                        UiResources& ui) {
    int bad_rate = st->wasapi_unsupported_rate_hz;
    if (bad_rate != 0 && bad_rate != g_acked_unsupported_rate) {
        ImGui::OpenPopup("unsupported_rate_modal");
    }

    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 220), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("unsupported_rate_modal", nullptr,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::PushFont(ui.font, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.30f, 0.30f, 1.0f));
    ImGui::TextUnformatted("Unsupported sample rate");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(ui.font, 13.0f);
    ImGui::Text("Windows is delivering audio at %d Hz.", bad_rate);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "LDAC only accepts 44100, 48000, 88200, or 96000 Hz. "
        "Open the Sound control panel (Win+R \xe2\x86\x92 mmsys.cpl), "
        "right-click your output device \xe2\x86\x92 Properties \xe2\x86\x92 "
        "Advanced tab, then set Default Format to one of the supported "
        "rates.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "After changing the rate, restart win-ldac. See the tutorial "
        "file in the project root for more details.");
    ImGui::PopFont();

    float btn_h = 30, btn_w = 100;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - btn_h - 12);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btn_w - 12);
    ImGui::PushFont(ui.font, 13.0f);
    if (ImGui::Button("OK", ImVec2(btn_w, btn_h))) {
        g_acked_unsupported_rate = bad_rate;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopFont();

    ImGui::EndPopup();
}

// Module-level flag: set by the Settings button, consumed by the pair
// modal renderer once per frame.
static bool g_open_pair_modal = false;

// ── Bottom action row (compact) ────────────────────────────────────────
static bool draw_action_row(const engine_status_t* st, UiResources& ui) {
    bool quit = false;

    const float gap = 6;
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float btn_w = (total_w - 2 * gap) / 3.0f;
    const float btn_h = 26;

    ImGui::PushFont(ui.font, 12.0f);

    // Primary blue Connection Refresh
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.27f, 0.49f, 0.96f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.44f, 0.92f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.40f, 0.88f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    bool can_disconnect = st->link_up || st->state == ENGINE_STATE_STREAMING;
    ImGui::BeginDisabled(!can_disconnect);
    if (ImGui::Button("Connection Refresh", ImVec2(btn_w, btn_h))) {
        engine_post_disconnect();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0, gap);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.97f, 0.99f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.97f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.86f, 0.88f, 1.00f));

    if (ImGui::Button("Pair new device", ImVec2(btn_w, btn_h))) {
        g_open_pair_modal = true;
    }

    ImGui::SameLine(0, gap);
    if (ImGui::Button("Quit", ImVec2(btn_w, btn_h))) quit = true;

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::PopFont();

    return quit;
}

// ── Pair / settings modal ──────────────────────────────────────────────
// Selected device row index, persists across frames while modal is open.
static int  g_selected_device = -1;
static bool g_filter_audio_only = true;

static bool cod_is_audio(uint32_t cod) {
    // Major Device Class field is bits 8..12; Audio/Video = 0x04.
    return ((cod >> 8) & 0x1F) == 0x04 ||
           // Or Audio Service Class set (bit 21 of services field).
           (cod & 0x200000) != 0;
}

static void draw_pair_modal(const engine_status_t* st, UiResources& ui) {
    if (g_open_pair_modal) {
        ImGui::OpenPopup("pair_modal");
        g_open_pair_modal = false;
        g_selected_device = -1;
        engine_post_clear_scan_results();
    }

    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(disp.x - 24, disp.y - 30),
                             ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("pair_modal", nullptr,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::PushFont(ui.font, 16.0f);
    ImGui::TextUnformatted("Pair a Bluetooth device");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    engine_scan_state_t scan{};
    engine_get_scan_state(&scan);

    // Top row: Scan button + status + filter toggle
    ImGui::PushFont(ui.font, 12.0f);
    if (scan.active) {
        ImGui::BeginDisabled(true);
        ImGui::Button("Scanning...", ImVec2(120, 26));
        ImGui::EndDisabled();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.27f, 0.49f, 0.96f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.44f, 0.92f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.40f, 0.88f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
        if (ImGui::Button("Scan now", ImVec2(120, 26))) {
            engine_post_clear_scan_results();
            engine_post_start_scan(8);  // 8 * 1.28s ≈ 10s
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("%d device%s",
                        scan.device_count, scan.device_count == 1 ? "" : "s");

    ImGui::SameLine();
    float right_w = ImGui::CalcTextSize("Show non-audio devices").x + 40;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - right_w);
    bool show_all = !g_filter_audio_only;
    if (ImGui::Checkbox("Show non-audio devices", &show_all)) {
        g_filter_audio_only = !show_all;
        g_selected_device   = -1;  // selection may now point to filtered-out row
    }
    ImGui::PopFont();

    ImGui::Spacing();

    // Device list
    ImGui::PushFont(ui.font, 12.0f);
    float list_h = ImGui::GetContentRegionAvail().y - 50;
    if (list_h < 120) list_h = 120;
    ImGui::BeginChild("device_list", ImVec2(0, list_h),
                      ImGuiChildFlags_Borders);

    int visible = 0;
    for (int i = 0; i < scan.device_count; ++i) {
        const engine_scan_device_t& d = scan.devices[i];
        if (g_filter_audio_only && !cod_is_audio(d.cod)) continue;
        ++visible;

        char addr[24];
        std::snprintf(addr, sizeof(addr),
                      "%02X:%02X:%02X:%02X:%02X:%02X",
                      d.addr[0], d.addr[1], d.addr[2],
                      d.addr[3], d.addr[4], d.addr[5]);

        char label[160];
        std::snprintf(label, sizeof(label),
                      "%s##dev%d",
                      d.have_name ? d.name : "(resolving name...)", i);

        bool selected = (g_selected_device == i);
        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0, 26))) {
            g_selected_device = i;
        }

        ImGui::SameLine(280);
        ImGui::TextDisabled("%s", addr);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 70);
        if (d.have_rssi) {
            ImGui::TextDisabled("%+4d dBm", d.rssi_dbm);
        } else {
            ImGui::TextDisabled("--");
        }
    }
    if (visible == 0) {
        ImGui::TextDisabled(scan.active
            ? "Scanning..."
            : "No devices yet. Put your headphones in pairing mode and click Scan now.");
    }
    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::Spacing();

    // Bottom action buttons: Unpair (if currently paired), Cancel, Pair
    ImGui::PushFont(ui.font, 13.0f);
    float btn_h = 30;

    if (st->has_target) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.97f, 0.99f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.97f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.86f, 0.88f, 1));
        if (ImGui::Button("Unpair current", ImVec2(140, btn_h))) {
            if (ui.on_unpair) ui.on_unpair();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::SameLine();
    }

    // Push Cancel + Pair to the right.
    float right_btns_w = 80 + 8 + 100;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - right_btns_w);

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.97f, 0.99f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.97f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.86f, 0.88f, 1));
    if (ImGui::Button("Cancel", ImVec2(80, btn_h))) {
        if (scan.active) engine_post_stop_scan();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::SameLine(0, 8);
    bool can_pair = (g_selected_device >= 0 &&
                     g_selected_device < scan.device_count);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.27f, 0.49f, 0.96f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.44f, 0.92f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.40f, 0.88f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
    ImGui::BeginDisabled(!can_pair);
    if (ImGui::Button("Pair", ImVec2(100, btn_h)) && can_pair) {
        const engine_scan_device_t& d = scan.devices[g_selected_device];
        if (ui.on_pair) {
            ui.on_pair(d.addr, d.have_name ? d.name : "");
        }
        if (scan.active) engine_post_stop_scan();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);

    ImGui::PopFont();

    ImGui::EndPopup();
}

// ── Main entry ─────────────────────────────────────────────────────────
bool draw_status_window(const engine_status_t* st, StatusSamples* samples,
                        UiResources& ui) {
    bool quit_requested = false;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("win-ldac", nullptr, flags);
    ImGui::PushFont(ui.font, 13.0f);

    draw_device_card(st, ui);
    draw_chart_card(samples, ui);
    draw_mode_row(st, samples, ui);
    if (draw_action_row(st, ui)) quit_requested = true;
    draw_pair_modal(st, ui);
    draw_unsupported_rate_popup(st, ui);

    ImGui::PopFont();
    ImGui::End();
    return quit_requested;
}

}  // namespace win_ldac
