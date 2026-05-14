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
static void draw_device_card(const engine_status_t* st, const UiResources& ui) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
    ImGui::BeginChild("device_card", ImVec2(0, 56),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();

    ImU32  disabled_col = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

    // Build right-column strings up front so we can right-align them.
    char addr[18];
    std::snprintf(addr, sizeof(addr),
                  "%02X:%02X:%02X:%02X:%02X:%02X",
                  st->target_addr[0], st->target_addr[1], st->target_addr[2],
                  st->target_addr[3], st->target_addr[4], st->target_addr[5]);

    char big_value[32];
    char small_value[48];
    bool streaming = (st->state == ENGINE_STATE_STREAMING && !st->idle_paused);
    if (streaming) {
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
        ImGui::TextUnformatted("WH-1000XM5");
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
static void draw_chart_card(StatusSamples* samples, const UiResources& ui) {
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

// Module-level flag: set by the How2Use? button, consumed by the popup
// renderer once per frame.
static bool g_open_how2use_popup = false;

// ── Mode segmented toggle row + How2Use? button ────────────────────────
static void draw_mode_row(const engine_status_t* st, StatusSamples* samples,
                          const UiResources& ui) {
    const float inner_gap = 4;     // between Fixed and Adaptive
    const float outer_gap = 8;     // between toggle group and How2Use?
    const float btn_h     = 30;

    // Measure How2Use? button width at the toggle font size.
    ImGui::PushFont(ui.font, 13.0f);
    float how_w = ImGui::CalcTextSize("How2Use?").x +
                  ImGui::GetStyle().FramePadding.x * 2 + 6;
    ImGui::PopFont();

    const float total_w     = ImGui::GetContentRegionAvail().x;
    const float toggle_w    = total_w - how_w - outer_gap;
    const float toggle_each = (toggle_w - inner_gap) / 2.0f;

    bool fixed_active = (st->bitrate_mode != ENGINE_BITRATE_ADAPTIVE);

    if (segmented_button("Fixed (990 kbps)", fixed_active,
                         ImVec2(toggle_each, btn_h), ui.font)) {
        if (!fixed_active) {
            engine_post_set_bitrate_mode(ENGINE_BITRATE_FIXED_HQ);
            samples->reset();
        }
    }
    ImGui::SameLine(0, inner_gap);
    if (segmented_button("Adaptive", !fixed_active,
                         ImVec2(toggle_each, btn_h), ui.font)) {
        if (fixed_active) {
            engine_post_set_bitrate_mode(ENGINE_BITRATE_ADAPTIVE);
            samples->reset();
        }
    }

    ImGui::SameLine(0, outer_gap);
    // How2Use? — neutral secondary button with border.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.97f, 0.99f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.92f, 0.94f, 0.97f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.86f, 0.88f, 1.00f));
    ImGui::PushFont(ui.font, 13.0f);
    if (ImGui::Button("How2Use?", ImVec2(how_w, btn_h))) {
        g_open_how2use_popup = true;
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

// ── How2Use? popup ─────────────────────────────────────────────────────
static void draw_how2use_popup(const UiResources& ui) {
    if (g_open_how2use_popup) {
        ImGui::OpenPopup("how2use_modal");
        g_open_how2use_popup = false;
    }

    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 240), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("how2use_modal", nullptr,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(ui.font, 16.0f);
        ImGui::TextUnformatted("How to use win-ldac");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(ui.font, 13.0f);
        ImGui::TextWrapped("(Tutorial content to be written.)");
        ImGui::PopFont();

        // Push the Close button to the bottom-right.
        float btn_h = 30, btn_w = 100;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - btn_h - 12);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btn_w - 12);
        ImGui::PushFont(ui.font, 13.0f);
        if (ImGui::Button("Close", ImVec2(btn_w, btn_h))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopFont();

        ImGui::EndPopup();
    }
}

// ── Bottom action row (compact) ────────────────────────────────────────
static bool draw_action_row(const engine_status_t* st, const UiResources& ui) {
    bool quit = false;

    const float gap = 6;
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float btn_w = (total_w - 2 * gap) / 3.0f;
    const float btn_h = 26;

    ImGui::PushFont(ui.font, 12.0f);

    // Primary blue Reconnect
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

    ImGui::BeginDisabled(true);
    ImGui::Button("Settings?", ImVec2(btn_w, btn_h));
    ImGui::EndDisabled();

    ImGui::SameLine(0, gap);
    if (ImGui::Button("Quit", ImVec2(btn_w, btn_h))) quit = true;

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::PopFont();

    return quit;
}

// ── Main entry ─────────────────────────────────────────────────────────
bool draw_status_window(const engine_status_t* st, StatusSamples* samples,
                        const UiResources& ui) {
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
    draw_how2use_popup(ui);

    ImGui::PopFont();
    ImGui::End();
    return quit_requested;
}

}  // namespace win_ldac
