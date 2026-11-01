// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Files and jobs (Instruction 010). The list is the CONTROLLER's SD card, read with the read-only queries
// `$SD/Status` and `$SD/List`; the pendant's own flash is never listed. The job state and percentage are the
// controller's own SD report. Instruction 010A: this page is MONITORING ONLY. It has no START JOB and no CYCLE START / RESUME; programs are
// uploaded and started from the PC. Only FEED HOLD and a confirmed RESET remain. Nothing restarts a job, ever.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kStatusText { 16, 72, 530, 30 };
constexpr ui::Rect kRefresh { 560, 68, 224, 40 };
constexpr ui::Rect kUp { 16, 110, 86, 38 };
constexpr ui::Rect kPathText { 112, 117, 672, 26 };
constexpr int16_t kListTop = 152;
constexpr int16_t kRowH = 40;
constexpr uint8_t kRows = 4;
constexpr ui::Rect kPgUp { 724, 152, 60, 78 };
constexpr ui::Rect kPgDn { 724, 234, 60, 78 };
constexpr ui::Rect kListArea { 16, 152, 768, 160 };
constexpr ui::Rect kDetail { 16, 318, 768, 44 };
constexpr ui::Rect kJobText { 16, 366, 768, 26 };
constexpr ui::Rect kBar { 16, 394, 768, 14 };
// Monitoring page: there is no START and no CYCLE START / RESUME (Instruction 010A).
constexpr ui::Rect kHold { 16, 412, 376, 60 };
constexpr ui::Rect kReset { 408, 412, 376, 60 };

ui::Rect row_rect(uint8_t i) { return { 16, static_cast<int16_t>(kListTop + i * kRowH), 700, 38 }; }

uint32_t hash_text(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    return h;
}

}  // namespace

uint32_t JcFilesScreen::list_key() const {
    const JcSdEntry* e = sv.sd.selected();
    return (static_cast<uint32_t>(sv.sd.state()) << 28) ^ (static_cast<uint32_t>(sv.sd.total_entries()) << 14)
           ^ (static_cast<uint32_t>(sv.sd.visible_count()) << 4) ^ (static_cast<uint32_t>(_scroll) << 20)
           ^ hash_text(sv.sd.cwd()) ^ (e != nullptr ? hash_text(e->path) * 3u : 0u);
}

void JcFilesScreen::on_enter() {
    _scroll = 0;
    _drawn_key = 0xFFFFFFFFu;
}

void JcFilesScreen::draw_status() {
    char line[120];
    uint16_t color = ui::kMuted;
    switch (sv.sd.state()) {
        case JcSdState::Idle:
            snprintf(line, sizeof(line), "SD CARD: not read yet. REFRESH asks the controller (read-only).");
            break;
        case JcSdState::Status:
        case JcSdState::Listing:
            snprintf(line, sizeof(line), "READING THE CONTROLLER'S SD CARD ...");
            color = ui::kInfo;
            break;
        case JcSdState::Ready: {
            char space[40] = "";
            const char* f = strstr(sv.sd.summary(), "Free:");   // "/sd Free:14.57 GB Used:25.72 MB Total:14.59 GB"
            if (f != nullptr) {
                const char* e = strstr(f, " Used:");
                const size_t n = e != nullptr ? static_cast<size_t>(e - (f + 5)) : strlen(f + 5);
                snprintf(space, sizeof(space), ", %.*s free", static_cast<int>(n < 24 ? n : 24), f + 5);
            }
            if (sv.sd.message()[0]) snprintf(line, sizeof(line), "%s", sv.sd.message());
            else snprintf(line, sizeof(line), "CONTROLLER SD CARD: %u files%s", static_cast<unsigned>(sv.sd.files_seen()), space);
            color = sv.sd.overflow() ? ui::kWarn : ui::kGood;
            break;
        }
        case JcSdState::Empty:
        case JcSdState::NoCard:
        case JcSdState::Error:
            snprintf(line, sizeof(line), "%s", sv.sd.message());
            color = sv.sd.state() == JcSdState::Empty ? ui::kWarn : RED;
            break;
    }
    _status.draw(sv.display, kStatusText, line, FontId::Body16, color, ui::kBg);
}

void JcFilesScreen::draw_list() {
    sv.display.fillRect(kListArea.x, kListArea.y, kListArea.w, kListArea.h, ui::kBg);
    // directory bar
    ui::button(sv.display, kUp, "UP", strcmp(sv.sd.cwd(), "/") == 0 ? ui::Btn::Disabled : ui::Btn::Normal);
    char path[96];
    snprintf(path, sizeof(path), "SD:%s", sv.sd.cwd());
    sv.display.fillRect(kPathText.x, kPathText.y, kPathText.w, kPathText.h, ui::kBg);
    ui::text(sv.display, kPathText.x, kPathText.y, path, FontId::Body16, CYAN, ui::kBg);

    const JcSdState st = sv.sd.state();
    if (st != JcSdState::Ready) {
        const char* hint = "";
        if (st == JcSdState::Idle) hint = "Press REFRESH to list the files on the CONTROLLER's SD card. Files on the pendant itself are never shown.";
        else if (st == JcSdState::Empty) hint = "The card is present but holds no files.";
        else if (st == JcSdState::NoCard) hint = "Insert a card in the controller (the pendant has no SD slot involved), then REFRESH.";
        else if (st == JcSdState::Error) hint = "Nothing is shown rather than guessing. REFRESH tries again; nothing was changed.";
        else hint = "Waiting for the controller ...";
        ui::wrap(sv.display, { 24, 164, 692, 120 }, hint, FontId::Body16, ui::kMuted, ui::kBg);
        ui::button(sv.display, kPgUp, "^", ui::Btn::Disabled);
        ui::button(sv.display, kPgDn, "v", ui::Btn::Disabled);
        return;
    }
    const uint16_t n = sv.sd.visible_count();
    if (n == 0) {
        ui::wrap(sv.display, { 24, 164, 692, 120 }, "This folder is empty.", FontId::Body16, ui::kMuted, ui::kBg);
    }
    const JcSdEntry* sel = sv.sd.selected();
    for (uint8_t i = 0; i < kRows; ++i) {
        const uint16_t idx = static_cast<uint16_t>(_scroll + i);
        if (idx >= n) break;
        const JcSdEntry* e = sv.sd.visible(idx);
        const ui::Rect r = row_rect(i);
        const bool selected = sel != nullptr && !e->dir && strcmp(sel->path, e->path) == 0;
        ui::panel(sv.display, r, selected ? ui::kGoodDark : ui::kPanel);
        char name[64];
        char shown[64];
        snprintf(name, sizeof(name), "%s%s", e->dir ? "[DIR]  " : "", JcSdBrowser::base_name(e->path));
        JcSdBrowser::ellipsize(name, 50, shown, sizeof(shown));
        ui::text(sv.display, r.x + 10, r.y + 9, shown, FontId::Body16, e->dir ? CYAN : WHITE, selected ? ui::kGoodDark : ui::kPanel);
        if (!e->dir) {
            char sz[24];
            JcSdBrowser::size_text(e->size, sz, sizeof(sz));
            ui::text(sv.display, r.x + r.w - 10 - text_width(FontId::Body16, sz), r.y + 9, sz, FontId::Body16, ui::kMuted,
                     selected ? ui::kGoodDark : ui::kPanel);
        }
    }
    ui::button(sv.display, kPgUp, "^", _scroll == 0 ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kPgDn, "v", _scroll + kRows >= n ? ui::Btn::Disabled : ui::Btn::Normal);
}

void JcFilesScreen::draw_detail() {
    ui::panel(sv.display, kDetail);
    const JcSdEntry* e = sv.sd.selected();
    char l1[110], l2[110], shown[96];
    if (e == nullptr) {
        snprintf(l1, sizeof(l1), "No file selected. Tap a file to select it.");
        l2[0] = '\0';
    } else {
        JcSdBrowser::ellipsize(e->path, 84, shown, sizeof(shown));
        snprintf(l1, sizeof(l1), "%s", shown);
        char sz[24];
        JcSdBrowser::size_text(e->size, sz, sizeof(sz));
        snprintf(l2, sizeof(l2), "%s   Programs are started from the PC, not from this pendant.", sz);
    }
    sv.display.fillRect(kDetail.x + 2, kDetail.y + 2, kDetail.w - 4, kDetail.h - 4, ui::kPanel);
    ui::text(sv.display, kDetail.x + 10, kDetail.y + 3, l1, FontId::Body16, e != nullptr ? WHITE : ui::kMuted, ui::kPanel);
    ui::text(sv.display, kDetail.x + 10, kDetail.y + 23, l2, FontId::Body16, ui::kMuted, ui::kPanel);
}

void JcFilesScreen::draw_buttons() {
    ui::button(sv.display, kRefresh, sv.sd.busy() ? "READING ..." : "REFRESH SD LIST", sv.sd.busy() ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kHold, "FEED HOLD (PAUSE)", sv.job.active() ? ui::Btn::Normal : ui::Btn::Disabled);
    ui::button(sv.display, kReset, "RESET (ENDS THE JOB)", ui::Btn::Danger);
}

void JcFilesScreen::draw_job() {
    char line[140];
    uint16_t color = ui::kMuted;
    const JcJobPhase ph = sv.job.phase();
    const char* sim = sv.host.job_simulated() ? "SIMULATED  " : "";
    if (!sv.model.has_status() || !sv.live_mode) {
        snprintf(line, sizeof(line), "JOB: no live controller status");
    } else if (ph == JcJobPhase::None) {
        snprintf(line, sizeof(line), "JOB: none reported (machine %s)", sv.model.state());
        color = WHITE;
    } else {
        char pct[24] = "";
        if (sv.job.percent_valid()) snprintf(pct, sizeof(pct), "  %.1f%%", static_cast<double>(sv.job.percent()));
        snprintf(line, sizeof(line), "%sJOB %s%s  %s", sim, jc_job_phase_name(ph), pct, sv.job.name());
        color = ph == JcJobPhase::Running ? ui::kGood : (ph == JcJobPhase::Alarm ? RED : (ph == JcJobPhase::Complete ? ui::kGood : ui::kWarn));
    }
    _job.draw(sv.display, kJobText, line, FontId::Button20, color, ui::kBg);
    int16_t bar = 0;
    if (sv.job.percent_valid() && ph != JcJobPhase::None) bar = static_cast<int16_t>(sv.job.percent() * kBar.w / 100.0f);
    if (bar != _last_bar) {
        sv.display.fillRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBg);
        if (bar > 0) sv.display.fillRect(kBar.x, kBar.y, bar, kBar.h, ph == JcJobPhase::Paused ? ui::kWarn : ui::kGood);
        sv.display.drawRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBorder);
        _last_bar = bar;
    }
}

void JcFilesScreen::on_draw() {
    _status.invalidate();
    _job.invalidate();
    _last_bar = -1;
    draw_status();
    draw_list();
    draw_detail();
    draw_buttons();
    draw_job();
    _drawn_key = list_key();
}

void JcFilesScreen::ask_reset() {
    open_custom_confirm("RESET THE CONTROLLER?", "RESET");
    dialog.add_line(kJcJobStopText, ui::kWarn);
    dialog.add_line("The job will NOT continue and cannot be resumed from this pendant.");
    dialog.add_line("Motion stops; an alarm, unlock or re-homing may follow.");
    dialog.add_line("This is different from FEED HOLD, which only pauses.");
    dialog.add_line("It is NOT a replacement for the emergency stop.", RED);
    draw();
}

void JcFilesScreen::on_custom_confirm() {
    const JcCommand c = jc_soft_reset();
    if (!c.ok) { show_message("CANNOT PREPARE COMMAND", jc_cmd_error_name(c.error)); return; }
    // The operator's confirmation arms ONE submission: the arming is dropped again straight away.
    sv.dispatcher.arm_for(millis(), 5000, 0x0F);
    const JcAuthDecision d = sv.dispatcher.submit(c, sv.dispatch_context(true));
    sv.dispatcher.disarm();
    if (d != JcAuthDecision::Allowed) {
        show_message("NOT SENT", jc_auth_name(d), "Nothing was transmitted to the controller.");
    }
    // FluidNC's own answer (ok / error:N) is shown when it arrives.
}

void JcFilesScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kRefresh, x, y)) {
        const char* why = sv.host.sd_refresh();
        if (why != nullptr) show_message("CANNOT LIST THE SD CARD", why, "Read-only query; nothing was changed.");
        else { _scroll = 0; draw(); }
        return;
    }
    if (ui::hit(kUp, x, y)) {
        if (sv.sd.up()) { _scroll = 0; draw(); }
        return;
    }
    if (ui::hit(kPgUp, x, y)) {
        if (_scroll >= kRows) _scroll = static_cast<uint16_t>(_scroll - kRows); else _scroll = 0;
        draw_list();
        _drawn_key = list_key();
        return;
    }
    if (ui::hit(kPgDn, x, y)) {
        if (_scroll + kRows < sv.sd.visible_count()) _scroll = static_cast<uint16_t>(_scroll + kRows);
        draw_list();
        _drawn_key = list_key();
        return;
    }
    if (sv.sd.state() == JcSdState::Ready) {
        for (uint8_t i = 0; i < kRows; ++i) {
            if (!ui::hit(row_rect(i), x, y)) continue;
            const uint16_t idx = static_cast<uint16_t>(_scroll + i);
            const JcSdEntry* e = sv.sd.visible(idx);
            if (e == nullptr) return;
            if (e->dir) { sv.sd.enter(idx); _scroll = 0; }
            else sv.sd.select(idx);
            draw_list();
            draw_detail();
            draw_buttons();
            _drawn_key = list_key();
            return;
        }
    }
    if (ui::hit(kHold, x, y)) {
        // Stopping is immediate: no dialog. FEED HOLD is always available while LIVE.
        const JcAuthDecision d = submit_now(jc_feed_hold());
        if (d != JcAuthDecision::Allowed) show_message("FEED HOLD NOT SENT", jc_auth_name(d));
    } else if (ui::hit(kReset, x, y)) {
#if JC_AUTHORITY
        ask_reset();
#else
        const char* notes[] = { "Ends the running job; it cannot be resumed from here.",
                                "Motion stops; position may be lost; an alarm or re-homing may follow.",
                                "It is NOT a replacement for the E-stop." };
        stage(jc_soft_reset(), "RESET", notes, 3);
#endif
    }
}

void JcFilesScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 300) return;
    _last_tick = now;
    draw_status();
    draw_job();
    if (list_key() != _drawn_key) {
        draw_list();
        draw_detail();
        _drawn_key = list_key();
    }
    draw_buttons();
}

}  // namespace jc8048w550
