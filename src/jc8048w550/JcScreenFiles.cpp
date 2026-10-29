// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kListPanel { 16, 76, 768, 132 };
constexpr ui::Rect kJobPanel { 16, 218, 768, 96 };
constexpr ui::Rect kPathButton { 16, 326, 140, 50 };
constexpr ui::Rect kPathSlot { 166, 326, 618, 50 };
constexpr ui::Rect kStart { 16, 392, 180, 70 };
constexpr ui::Rect kPause { 206, 392, 180, 70 };
constexpr ui::Rect kResume { 396, 392, 180, 70 };
constexpr ui::Rect kCancel { 586, 392, 198, 70 };
constexpr ui::Rect kBar { 28, 284, 744, 20 };

void edit_callback(void* user, bool ok, const char* text) {
    static_cast<JcFilesScreen*>(user)->edit_done(ok, text);
}

}  // namespace

void JcFilesScreen::on_draw() {
    ui::panel(sv.display, kListPanel);
    ui::text(sv.display, 28, 84, "SD CARD BROWSER: UNAVAILABLE", FontId::Button20, ui::kWarn, ui::kPanel);
    ui::wrap(sv.display, { 28, 114, 744, 90 },
             "Listing the controller's SD card needs a file-list command ($Files/ListGCode), but this build "
             "transmits only the '?' status query. No sample or cached files are shown. You can still type a "
             "controller file path below and preview the job commands.",
             FontId::Body16, ui::kMuted, ui::kPanel);
    ui::panel(sv.display, kJobPanel);
    ui::text(sv.display, 28, 224, "JOB STATUS (from controller)", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::button(sv.display, kPathButton, "FILE PATH");
    ui::button(sv.display, kStart, "START JOB");
    ui::button(sv.display, kPause, "PAUSE");
    ui::button(sv.display, kResume, "RESUME");
    ui::button(sv.display, kCancel, "CANCEL JOB", ui::Btn::Danger);
    _job.invalidate();
    _path_slot.invalidate();
    _last_bar = -1;
    sv.display.fillRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBg);
    draw_job();
}

void JcFilesScreen::draw_job() {
    char line[96];
    uint16_t color = ui::kMuted;
    int16_t bar = 0;
    if (!sv.model.has_status()) {
        snprintf(line, sizeof(line), "No live status");
    } else if (sv.model.job_running()) {
        const JcLiveMachineStatus& s = sv.model.status();
        snprintf(line, sizeof(line), "%s  %.1f%%  %s", s.machine_state, static_cast<double>(s.sd_percent), s.sd_name);
        color = sv.model.fresh() ? ui::kGood : ui::kMuted;
        bar = static_cast<int16_t>(s.sd_percent * kBar.w / 100.0f);
    } else {
        snprintf(line, sizeof(line), "No job reported (%s)", sv.model.state());
        color = WHITE;
    }
    _job.draw(sv.display, { 28, 248, 744, 30 }, line, FontId::Button20, color, ui::kPanel);
    if (bar != _last_bar) {
        sv.display.fillRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBg);
        sv.display.fillRect(kBar.x, kBar.y, bar, kBar.h, ui::kGood);
        sv.display.drawRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBorder);
        _last_bar = bar;
    }
    _path_slot.draw(sv.display, kPathSlot, _path[0] ? _path : "(no path entered)", FontId::Button20,
                    _path[0] ? CYAN : ui::kMuted, ui::kBg);
}

void JcFilesScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    strncpy(_path, text, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
}

void JcFilesScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kPathButton, x, y)) {
        JcInputRequest r {};
        r.prompt = "Controller file path, e.g. /sd/job.nc";
        strncpy(r.initial, _path[0] ? _path : "/", sizeof(r.initial) - 1);
        r.numeric = false;
        r.max_len = 60;
        r.done = edit_callback;
        r.user = this;
        sv.host.request_input(r);
    } else if (ui::hit(kStart, x, y)) {
        if (_path[0] == '\0') {
            show_message("NO FILE SELECTED", "Enter the controller file path first (FILE PATH).");
            return;
        }
        const char* notes[] = { "Runs this G-code file on the machine.",
                                "Check the file, work offset, tool and workholding first." };
        stage(jc_run_sd_file(_path), "START JOB", notes, 2);
    } else if (ui::hit(kPause, x, y)) {
        const char* notes[] = { "Feed hold: pauses motion; the job can be resumed." };
        stage(jc_feed_hold(), "PAUSE JOB", notes, 1);
    } else if (ui::hit(kResume, x, y)) {
        const char* notes[] = { "Cycle start: resumes a held job." };
        stage(jc_cycle_resume(), "RESUME JOB", notes, 1);
    } else if (ui::hit(kCancel, x, y)) {
        const char* notes[] = { "Soft reset ends the job and stops motion.", "It is NOT a replacement for the E-stop." };
        stage(jc_soft_reset(), "CANCEL JOB", notes, 2);
    }
}

void JcFilesScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 400) {
        _last_tick = now;
        draw_job();
    }
}

}  // namespace jc8048w550
