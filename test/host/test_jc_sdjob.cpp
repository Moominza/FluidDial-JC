// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the SD file browser and job management (Instruction 010) against the SIMULATED FluidNC.
// Built with -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_FULL_CONTROL_TEST=1.

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCommandSystem.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcJobState.hpp"
#include "../../src/jc8048w550/JcSdBrowser.hpp"
#include "../../src/jc8048w550/JcSimNc.hpp"
#include "../../src/jc8048w550/JcStatusFrame.hpp"
#include "../../src/jc8048w550/JcToolInfo.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tol) do { ++g_run; if (fabsf(static_cast<float>(a) - static_cast<float>(b)) > (tol)) { ++g_fail; printf("FAIL %s:%d  %s=%g vs %g\n", __FILE__, __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); } } while (0)

struct Nc : JcSimNc {
    std::vector<std::string> wire;
    static bool send(void* self, const char* bytes, size_t n) {
        Nc* s = static_cast<Nc*>(self);
        s->wire.emplace_back(bytes, n);
        return s->JcSimNc::send(bytes, n);
    }
    int count(const char* prefix) const {
        int n = 0;
        for (const std::string& s : wire) if (s.rfind(prefix, 0) == 0) ++n;
        return n;
    }
};

// ---- SD browser rig ------------------------------------------------------------------------------------------

struct SdRig {
    Nc nc;
    JcSdBrowser br;
    std::vector<std::string> queries;
    bool link_ok = true;
    static bool query(void* u, const char* line) {
        SdRig* r = static_cast<SdRig*>(u);
        if (!r->link_ok || !jc_query_allowed(line)) return false;
        r->queries.push_back(line);
        const std::string l = std::string(line) + "\n";
        r->nc.JcSimNc::send(l.data(), l.size());
        r->nc.wire.push_back(l);
        return true;
    }
    SdRig() { br.set_query(&SdRig::query, this); }
    void run(uint32_t ms) {
        const uint32_t end = nc.now + ms;
        while (nc.now < end) {
            nc.update(nc.now + 10);
            char reply[160];
            while (nc.pop_line(reply, sizeof(reply))) br.on_line(reply, nc.now);
            br.service(nc.now);
        }
    }
    void refresh_and_wait(uint32_t ms = 3000) { CHECK(br.refresh(nc.now)); run(ms); }
};

static void test_listing() {
    SdRig r;
    r.nc.add_file("/a.nc", 1234);
    r.nc.add_file("/jobs/b.nc", 2048);
    r.nc.add_file("/jobs/deep/c.gcode", 5u * 1024u * 1024u);
    r.nc.add_file("/Zed.nc", 12);
    r.refresh_and_wait();
    CHECK(r.br.state() == JcSdState::Ready);
    CHECK(r.queries.size() == 2 && r.queries[0] == "$SD/Status" && r.queries[1] == "$SD/List");
    CHECK(strstr(r.br.status_text(), "SD card detected") != nullptr);
    CHECK(r.br.status_supported());
    // root: the directory first, then the files by name
    CHECK(r.br.visible_count() == 3);
    CHECK(r.br.visible(0)->dir && strcmp(r.br.visible(0)->path, "/jobs") == 0);
    CHECK(strcmp(r.br.visible(1)->path, "/Zed.nc") == 0 && r.br.visible(1)->size == 12);
    CHECK(strcmp(r.br.visible(2)->path, "/a.nc") == 0 && r.br.visible(2)->size == 1234);
    CHECK(r.br.total_entries() == 6);                          // 4 files + inferred /jobs and /jobs/deep
}

static void test_listing_counts() {
    SdRig r;
    r.nc.add_file("/a.nc", 1);
    r.nc.add_file("/jobs/b.nc", 2);
    r.nc.add_file("/jobs/deep/c.gcode", 3);
    r.refresh_and_wait();
    CHECK(r.br.total_entries() == 5 && r.br.files_seen() == 3);   // 3 files + /jobs + /jobs/deep
    CHECK(r.br.enter(0) && strcmp(r.br.cwd(), "/jobs") == 0);
    CHECK(r.br.visible_count() == 2 && r.br.visible(0)->dir && strcmp(r.br.visible(0)->path, "/jobs/deep") == 0);
    CHECK(strcmp(r.br.visible(1)->path, "/jobs/b.nc") == 0);
    CHECK(r.br.select(1) && r.br.selected() != nullptr && strcmp(r.br.selected()->path, "/jobs/b.nc") == 0);
    CHECK(r.br.selected_runnable());
    CHECK(!r.br.select(0));                                     // a directory is entered, never selected
    CHECK(r.br.enter(0) && r.br.visible_count() == 1 && strcmp(r.br.visible(0)->path, "/jobs/deep/c.gcode") == 0);
    CHECK(r.br.up() && strcmp(r.br.cwd(), "/jobs") == 0);
    CHECK(r.br.up() && strcmp(r.br.cwd(), "/") == 0);
    CHECK(!r.br.up());
    CHECK(r.br.selected() == nullptr);                          // navigating clears the selection
}

static void test_formats() {
    SdRig r;                                                     // "[FILE: /x|SIZE:n]" and explicit [DIR:..]
    r.nc.list_full_paths = true;                                 // tolerance: the older full-path entries
    r.nc.list_space_after_colon = true;
    r.nc.list_dir_lines = true;
    r.nc.add_file("/x.nc", 77);
    r.refresh_and_wait();
    CHECK(r.br.state() == JcSdState::Ready);
    bool have_dir = false, have_file = false;
    for (uint16_t i = 0; i < r.br.visible_count(); ++i) {
        if (r.br.visible(i)->dir && strcmp(r.br.visible(i)->path, "/sim_dir") == 0) have_dir = true;
        if (!r.br.visible(i)->dir && strcmp(r.br.visible(i)->path, "/x.nc") == 0 && r.br.visible(i)->size == 77) have_file = true;
    }
    CHECK(have_dir && have_file);

    // malformed entries are dropped, never half-parsed
    SdRig m;
    m.nc.list_never_ends = true;                                 // keep the listing open while lines are injected
    m.br.refresh(m.nc.now);
    m.run(100);                                                  // status + list queries sent
    m.br.on_line("[FILE:/nosize.nc]", m.nc.now);
    m.br.on_line("[FILE:/bad.nc|SIZE:12x]", m.nc.now);
    m.br.on_line("[FILE:/good.nc|SIZE:42]", m.nc.now);
    m.br.on_line("[MSG:something]", m.nc.now);
    m.br.on_line("ok", m.nc.now);
    CHECK(m.br.state() == JcSdState::Ready && m.br.total_entries() == 1 && m.br.visible_count() == 1);
    CHECK(m.br.visible(0)->size == 42);
}

// The exact text FluidNC v4.1.1 returned from the real controller (read-only HTTP query, 2026-10-30), shortened.
static void test_real_controller_transcript() {
    SdRig r;
    r.nc.list_never_ends = true;                                 // lines are injected below
    r.br.refresh(r.nc.now);
    r.run(100);
    const char* lines[] = {
        "[DIR:System Volume Information]", "[FILE:  IndexerVolumeGuid|SIZE:76]", "[FILE:  WPSettings.dat|SIZE:12]",
        "[FILE: Test.nc|SIZE:43039]", "[FILE: clamp squares.nc|SIZE:31097]", "[DIR:SIMATIC.S7S]", "[FILE:  S001|SIZE:23473]",
        "[FILE:  S002|SIZE:919]", "[FILE: S7_JOB.S7S|SIZE:6]", "[FILE: HartV2.nc|SIZE:9372688]",
        "[/sd Free:14.57 GB Used:25.72 MB Total:14.59 GB]", "ok" };
    for (const char* l : lines) r.br.on_line(l, r.nc.now);
    CHECK(r.br.state() == JcSdState::Ready);
    CHECK(strstr(r.br.summary(), "Free:14.57 GB") != nullptr && strstr(r.br.summary(), "Total:14.59 GB") != nullptr);
    CHECK(r.br.files_seen() == 8);
    // root: both folders first, then the root files by name
    CHECK(r.br.visible_count() == 6);
    CHECK(r.br.visible(0)->dir && strcmp(r.br.visible(0)->path, "/SIMATIC.S7S") == 0);
    CHECK(r.br.visible(1)->dir && strcmp(r.br.visible(1)->path, "/System Volume Information") == 0);
    bool test_nc = false, squares = false, hart = false;
    for (uint16_t i = 2; i < r.br.visible_count(); ++i) {
        const JcSdEntry* e = r.br.visible(i);
        if (strcmp(e->path, "/Test.nc") == 0 && e->size == 43039) test_nc = true;
        if (strcmp(e->path, "/clamp squares.nc") == 0) squares = true;
        if (strcmp(e->path, "/HartV2.nc") == 0 && e->size == 9372688) hart = true;
    }
    CHECK(test_nc && squares && hart);
    // the files INSIDE a folder are not in the root, and the folder's children keep their relative names
    CHECK(r.br.enter(0) && strcmp(r.br.cwd(), "/SIMATIC.S7S") == 0 && r.br.visible_count() == 2);
    CHECK(strcmp(r.br.visible(0)->path, "/SIMATIC.S7S/S001") == 0 && r.br.visible(0)->size == 23473);
    CHECK(strcmp(r.br.visible(1)->path, "/SIMATIC.S7S/S002") == 0);
    CHECK(r.br.up() && r.br.enter(1) && r.br.visible_count() == 2 && strcmp(r.br.visible(0)->path, "/System Volume Information/IndexerVolumeGuid") == 0);
    CHECK(r.br.up());
    // a file with a space in its name is listed but cannot be started from the pendant (the one command policy)
    for (uint16_t i = 0; i < r.br.visible_count(); ++i) if (strcmp(r.br.visible(i)->path, "/clamp squares.nc") == 0) r.br.select(i);
    CHECK(r.br.selected() != nullptr && !r.br.selected_runnable());
    for (uint16_t i = 0; i < r.br.visible_count(); ++i) if (strcmp(r.br.visible(i)->path, "/Test.nc") == 0) r.br.select(i);
    CHECK(r.br.selected_runnable());
    char sz[24];
    JcSdBrowser::size_text(9372688, sz, sizeof(sz));
    CHECK(strcmp(sz, "8.9 MB") == 0);
    // the status text is plain text, not a bracketed record
    SdRig s2;
    s2.br.refresh(s2.nc.now);
    s2.run(60);
    CHECK(strcmp(s2.br.status_text(), "SD card detected") == 0 && s2.br.status_supported());
}

static void test_paths_and_names() {
    SdRig r;
    r.nc.add_file("/odd#name.nc", 10);
    r.nc.add_file("/ok_name-1.nc", 11);
    std::string longp = "/";
    for (int i = 0; i < 12; ++i) longp += "verylongfoldername/";
    longp += "x.nc";
    r.nc.add_file(longp.c_str(), 12);                            // > 95 characters
    r.refresh_and_wait();
    CHECK(r.br.state() == JcSdState::Ready);
    for (uint16_t i = 0; i < r.br.visible_count(); ++i) {
        if (strcmp(r.br.visible(i)->path, "/odd#name.nc") == 0) { r.br.select(i); CHECK(!r.br.selected_runnable()); }
        if (strcmp(r.br.visible(i)->path, "/ok_name-1.nc") == 0) { r.br.select(i); CHECK(r.br.selected_runnable()); }
    }
    CHECK(!jc_sd_path_runnable("/a/../b.nc") && !jc_sd_path_runnable("a.nc") && !jc_sd_path_runnable("/a#b.nc") && !jc_sd_path_runnable("/a;b.nc") && !jc_sd_path_runnable("/a b.nc") && jc_sd_path_runnable("/a/b_c-1.nc"));
    char buf[40];
    JcSdBrowser::ellipsize("a_really_long_file_name_for_a_part_program.nc", 20, buf, sizeof(buf));
    CHECK(strlen(buf) <= 20 && strstr(buf, "..") != nullptr && strcmp(buf + strlen(buf) - 3, ".nc") == 0);
    JcSdBrowser::ellipsize("short.nc", 20, buf, sizeof(buf));
    CHECK(strcmp(buf, "short.nc") == 0);
    JcSdBrowser::size_text(512, buf, sizeof(buf)); CHECK(strcmp(buf, "512 B") == 0);
    JcSdBrowser::size_text(2048, buf, sizeof(buf)); CHECK(strcmp(buf, "2.0 KB") == 0);
    JcSdBrowser::size_text(3u * 1024u * 1024u, buf, sizeof(buf)); CHECK(strcmp(buf, "3.0 MB") == 0);
    CHECK(strcmp(JcSdBrowser::base_name("/a/b/c.nc"), "c.nc") == 0);
}

static void test_missing_empty_errors() {
    {   // missing SD card: the controller says so twice, the pendant does not invent a listing
        SdRig r;
        r.nc.sd_present = false;
        r.nc.add_file("/ghost.nc", 1);
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::NoCard && r.br.error_code() == 60);
        CHECK(strstr(r.br.message(), "error:60") != nullptr && r.br.visible_count() == 0 && r.br.total_entries() == 0);
    }
    {   // empty card
        SdRig r;
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::Empty && r.br.visible_count() == 0);
    }
    {   // $SD/Status not supported: the listing still decides
        SdRig r;
        r.nc.sd_status_supported = false;
        r.nc.add_file("/a.nc", 5);
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::Ready && !r.br.status_supported() && strstr(r.br.status_text(), "error:3") != nullptr);
    }
    {   // other controller errors are reported with their number
        SdRig r;
        r.nc.add_file("/a.nc", 5);
        r.nc.sd_list_error = 62;
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::Error && r.br.error_code() == 62 && strstr(r.br.message(), "error:62") != nullptr);
        CHECK(r.br.visible_count() == 0);
    }
    {   // refresh after a failure works and replaces the message
        SdRig r;
        r.nc.sd_list_error = 61;
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::Error);
        r.nc.sd_list_error = 0;
        r.nc.add_file("/a.nc", 5);
        r.refresh_and_wait();
        CHECK(r.br.state() == JcSdState::Ready && r.br.message()[0] == '\0');
    }
}

static void test_large_and_timeouts() {
    {   // large listing: bounded, flagged, still usable
        SdRig r;
        for (int i = 0; i < 210; ++i) { char p[48]; snprintf(p, sizeof(p), "/f%03d.nc", i); r.nc.add_file(p, static_cast<uint32_t>(i)); }
        r.refresh_and_wait(20000);
        CHECK(r.br.state() == JcSdState::Ready && r.br.overflow() && r.br.total_entries() == kJcSdMaxEntries);
        CHECK(strstr(r.br.message(), "TRUNCATED") != nullptr && r.br.visible_count() > 100);
    }
    {   // a listing that never ends: idle timeout, no endless wait
        SdRig r;
        r.nc.list_never_ends = true;
        r.nc.add_file("/a.nc", 1);
        r.refresh_and_wait(15000);
        CHECK(r.br.state() == JcSdState::Error && strstr(r.br.message(), "timeout") != nullptr);
    }
    {   // a silent controller: Status times out, then the listing times out; the query count stays 2 (no retries)
        SdRig r;
        r.nc.silent = true;
        r.refresh_and_wait(30000);
        CHECK(r.br.state() == JcSdState::Error && r.queries.size() == 2);
    }
    {   // no link: nothing is sent, the failure is reported
        SdRig r;
        r.link_ok = false;
        r.refresh_and_wait(500);
        CHECK(r.br.state() == JcSdState::Error && r.queries.empty());
    }
}

static void test_isolation_and_link_loss() {
    SdRig r;
    // lines from anywhere else (the local filesystem, an old reply) are never adopted
    r.br.on_line("[FILE:/localfs/config.yaml|SIZE:99]", 1);
    CHECK(r.br.total_entries() == 0);
    r.nc.add_file("/a.nc", 1);
    r.refresh_and_wait();
    CHECK(r.br.state() == JcSdState::Ready);
    r.br.on_line("[FILE:/late.nc|SIZE:5]", r.nc.now);           // after completion
    CHECK(r.br.total_entries() == 1);
    for (const std::string& q : r.queries) CHECK(q == "$SD/Status" || q == "$SD/List");   // never $LocalFS
    for (const std::string& w : r.nc.wire) CHECK(w.find("LocalFS") == std::string::npos && w.find("Run") == std::string::npos && w.find("Delete") == std::string::npos);

    // connection lost during a listing: reported, nothing re-asked
    SdRig l;
    l.nc.add_file("/a.nc", 1);
    CHECK(l.br.refresh(l.nc.now));
    l.run(30);
    CHECK(l.br.busy());
    CHECK(!l.br.refresh(l.nc.now));                              // refused while a request is outstanding
    const size_t asked = l.queries.size();
    l.br.on_link_lost();
    CHECK(l.br.state() == JcSdState::Error && strstr(l.br.message(), "connection lost") != nullptr);
    l.run(5000);
    CHECK(l.queries.size() == asked);                            // not retried
    l.br.invalidate();
    CHECK(l.br.state() == JcSdState::Idle && l.br.total_entries() == 0 && l.br.selected() == nullptr);
}

// ---- job rig ---------------------------------------------------------------------------------------------------

struct JobRig {
    Nc nc;
    JcCommandSystem sys;
    JcJogSession jogs;
    JcDispatcher disp;
    JcJobTracker job;
    bool fresh = true;
    bool armed = true;
    JcCommandResult last;
    bool have_last = false;
    char state_buf[20] = "Idle";
    JobRig() : sys(&Nc::send, &nc), jogs(&JcCommandSystem::session_tx, &sys) {
        sys.attach_jogs(&jogs);
        disp.set_sink_ex(&JcCommandSystem::sink, &sys);
        disp.arm();
        nc.add_file("/jobs/a.nc", 100);
        nc.add_file("/odd#name.nc", 5);
    }
    JcAuthDecision submit(const JcCommand& c, bool confirmed = true) {
        JcDispatchContext x;
        x.live = true; x.link_fresh = fresh;
        strncpy(state_buf, nc.state, sizeof(state_buf) - 1);
        x.machine_state = state_buf; x.operator_confirmed = confirmed; x.now_ms = nc.now;
        x.operation_pending = sys.busy() || jogs.unresolved();
        return disp.submit(c, x);
    }
    void run(uint32_t ms) {
        const uint32_t end = nc.now + ms;
        while (nc.now < end) {
            nc.update(nc.now + 10);
            sys.set_clock(nc.now);
            jogs.set_clock(nc.now);
            char reply[96];
            while (nc.pop_line(reply, sizeof(reply))) sys.on_line(reply);
            sys.on_machine_state(nc.state);
            sys.tick();
            JcCommandResult r;
            if (sys.take_result(r)) { last = r; have_last = true; }
            if (nc.now % 100 == 0) {
                char frame[256];
                nc.status_frame(frame, sizeof(frame));
                JcStatusFrame f;
                if (fresh && jc_parse_status_frame(frame, strlen(frame), f)) job.on_status(true, f.state, f.has_sd, f.sd_percent, f.sd_name);
                else if (!fresh) job.on_status(false, nullptr, false, 0, nullptr);
            }
        }
    }
};

static void test_job_run_pause_resume_stop() {
    JobRig r;
    CHECK(r.job.phase() == JcJobPhase::None);
    CHECK(r.submit(jc_run_sd_file("/jobs/a.nc")) == JcAuthDecision::Allowed);
    r.run(300);
    CHECK(r.have_last && r.last.outcome == JcExecOutcome::Acked);   // the acknowledgement says "accepted", nothing more
    CHECK(r.nc.count("$SD/Run=/jobs/a.nc") == 1);
    CHECK(r.job.phase() == JcJobPhase::Running);
    // the percentage is the controller's own SD: field, nothing else
    r.run(2000);
    NEAR(r.job.percent(), r.nc.job_pct, 1.1f);
    CHECK(r.job.percent() > 10.0f && r.job.percent() < 40.0f);
    CHECK(strcmp(r.job.name(), "/jobs/a.nc") == 0);

    // feed hold: the percentage freezes at the controller's value
    CHECK(r.submit(jc_feed_hold()) == JcAuthDecision::Allowed);
    r.run(300);
    CHECK(r.job.phase() == JcJobPhase::Paused && r.job.can_resume());
    const float held = r.job.percent();
    r.run(3000);
    NEAR(r.job.percent(), held, 0.01f);                            // nothing creeps while held
    CHECK(r.job.phase() == JcJobPhase::Paused);

    // cycle start resumes the SAME job
    CHECK(r.submit(jc_cycle_resume()) == JcAuthDecision::Allowed);
    r.run(1500);
    CHECK(r.job.phase() == JcJobPhase::Running && r.job.percent() > held);
    CHECK(r.nc.count("$SD/Run=") == 1);                            // resume is not a restart

    // controller reset ends the job; it is not resumable
    CHECK(r.submit(jc_soft_reset()) == JcAuthDecision::Allowed);
    r.run(500);
    CHECK(r.nc.resets == 1);
    CHECK(r.job.phase() == JcJobPhase::Ended && !r.job.can_resume());
    CHECK(r.submit(jc_cycle_resume()) == JcAuthDecision::Allowed);    // the gate allows the byte; the controller has nothing to resume
    r.run(500);
    CHECK(r.job.phase() == JcJobPhase::Ended);
}

static void test_job_completion() {
    JobRig r;
    r.nc.job_total_ms = 20000;
    r.submit(jc_run_sd_file("/jobs/a.nc"));
    r.run(1000);
    CHECK(r.job.phase() == JcJobPhase::Running);
    r.run(20000);
    CHECK(r.job.phase() == JcJobPhase::Complete && r.job.percent() >= 99.0f);
    CHECK(strcmp(JcJobTracker().name(), "") == 0);
}

static void test_job_errors() {
    {   JobRig r; r.nc.run_error = 66;
        r.submit(jc_run_sd_file("/jobs/a.nc")); r.run(400);
        CHECK(r.have_last && r.last.outcome == JcExecOutcome::ControllerError && r.last.code == 66);
        CHECK(r.job.phase() == JcJobPhase::None && !r.nc.job);
    }
    {   JobRig r;                                                   // file removed since the listing
        r.submit(jc_run_sd_file("/jobs/gone.nc")); r.run(400);
        CHECK(r.last.outcome == JcExecOutcome::ControllerError && r.last.code == 65 && r.job.phase() == JcJobPhase::None);
    }
    {   JobRig r; r.nc.sd_present = false;
        r.submit(jc_run_sd_file("/jobs/a.nc")); r.run(400);
        CHECK(r.last.outcome == JcExecOutcome::ControllerError && r.last.code == 60);
    }
    {   JobRig r;                                                   // a second start while one runs is refused by the controller
        r.submit(jc_run_sd_file("/jobs/a.nc")); r.run(300);
        r.submit(jc_run_sd_file("/jobs/a.nc")); r.run(400);
        CHECK(r.last.outcome == JcExecOutcome::ControllerError && r.last.code == 67);
        CHECK(r.job.phase() == JcJobPhase::Running);
    }
    {   JobRig r;                                                   // names with characters the policy does not allow never reach the wire
        const JcCommand c = jc_run_sd_file("/odd#name.nc");
        CHECK(!c.ok);
        r.submit(c); r.run(100);
        CHECK(r.nc.count("$SD/Run") == 0);
        CHECK(!jc_command_line_allowed(JcCommandClass::Job, "$SD/Run=/a;$X") && !jc_command_line_allowed(JcCommandClass::Job, "$SD/Run=/a/../b.nc"));
        CHECK(!jc_run_sd_file("/job one.nc").ok);
    }
    {   JobRig r;                                                   // an alarm mid-job stops everything; only recovery remains
        r.nc.job_alarm_at_pct = 40;
        r.submit(jc_run_sd_file("/jobs/a.nc")); r.run(6000);
        CHECK(r.job.phase() == JcJobPhase::Alarm && strcmp(r.nc.state, "Alarm") == 0);
        CHECK(r.submit(jc_run_sd_file("/jobs/a.nc")) == JcAuthDecision::BlockedAlarm);
        CHECK(r.submit(jc_soft_reset()) == JcAuthDecision::Allowed);
        r.run(300);
        CHECK(r.job.phase() == JcJobPhase::Ended);
    }
    {   JobRig r;                                                   // confirmation is required to start; hold needs none
        CHECK(r.submit(jc_run_sd_file("/jobs/a.nc"), false) == JcAuthDecision::BlockedUnconfirmed);
        CHECK(r.nc.count("$SD/Run") == 0);
        CHECK(r.submit(jc_feed_hold(), false) == JcAuthDecision::Allowed);
    }
}

static void test_job_disconnect_no_replay() {
    JobRig r;
    r.submit(jc_run_sd_file("/jobs/a.nc"));
    r.run(1500);
    CHECK(r.job.phase() == JcJobPhase::Running);
    // the connection drops: the tracker does not guess, the link logic ends the operation, nothing is re-sent
    r.fresh = false;
    r.sys.on_link_lost();
    r.run(500);
    CHECK(r.job.phase() == JcJobPhase::Unknown && r.job.unknown_events() >= 1);
    const size_t wire_before = r.nc.wire.size();
    r.run(5000);
    CHECK(r.nc.wire.size() == wire_before);                           // silence while disconnected
    // reconnect: the controller is still running its job (it never stopped); the pendant only re-learns it
    r.fresh = true;
    r.run(500);
    CHECK(r.job.phase() == JcJobPhase::Running);
    CHECK(r.nc.count("$SD/Run=") == 1 && r.nc.wire.size() == wire_before);
    // a job that ended while the pendant was away is NOT restarted either
    r.fresh = false; r.run(200);
    r.nc.job = false; r.nc.set_state("Idle"); r.nc.job_pct = 30.0f;
    r.fresh = true; r.run(300);
    CHECK(r.job.phase() == JcJobPhase::Ended);
    CHECK(r.nc.count("$SD/Run=") == 1);
    CHECK(strcmp(jc_job_phase_name(JcJobPhase::Unknown), "UNKNOWN (no fresh status)") == 0);
}

static void test_job_blockers() {
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::None, true, true) == nullptr);
    CHECK(jc_job_start_blocker(false, true, "Idle", JcJobPhase::None, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, false, "Idle", JcJobPhase::None, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::None, false, false) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::None, true, false) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Run", JcJobPhase::None, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Alarm", JcJobPhase::None, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::Running, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::Paused, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::Unknown, true, true) != nullptr);
    CHECK(jc_job_start_blocker(true, true, "Idle", JcJobPhase::Complete, true, true) == nullptr);
    // the three controls are described as three different things
    CHECK(strstr(kJcJobPauseText, "FEED HOLD") && strstr(kJcJobResumeText, "CYCLE START") && strstr(kJcJobStopText, "RESET"));
    // the realtime bytes are distinct
    CHECK(jc_feed_hold().text[0] == '!' && jc_cycle_resume().text[0] == '~' && static_cast<uint8_t>(jc_soft_reset().text[0]) == 0x18);
    // SD status field parsing: the percentage comes from the report
    JcStatusFrame f;
    const char* fr = "<Run|WPos:1.000,2.000,3.000,0.000|FS:500,0|SD:42.50,/jobs/a.nc>";
    CHECK(jc_parse_status_frame(fr, strlen(fr), f) && f.has_sd && fabsf(f.sd_percent - 42.5f) < 0.001f && strcmp(f.sd_name, "/jobs/a.nc") == 0);
    const char* fr2 = "<Idle|WPos:1.000,2.000,3.000,0.000|FS:0,0>";
    CHECK(jc_parse_status_frame(fr2, strlen(fr2), f) && !f.has_sd);
}


// ---- tool state (Instruction 010) ----------------------------------------------------------------------------

struct ToolRig {
    Nc nc;
    JcToolReader rd;
    std::vector<std::string> queries;
    bool link_ok = true;
    static bool query(void* u, const char* line) {
        ToolRig* r = static_cast<ToolRig*>(u);
        if (!r->link_ok || !jc_query_allowed(line)) return false;
        r->queries.push_back(line);
        const std::string l = std::string(line) + "\n";
        r->nc.JcSimNc::send(l.data(), l.size());
        return true;
    }
    ToolRig() { rd.set_query(&ToolRig::query, this); }
    void run(uint32_t ms) {
        const uint32_t end = nc.now + ms;
        while (nc.now < end) {
            nc.update(nc.now + 10);
            char reply[160];
            while (nc.pop_line(reply, sizeof(reply))) rd.on_line(reply);
            rd.service(nc.now);
        }
    }
};

static void test_tool_info() {
    bool modal = false;
    CHECK(jc_gc_tool("G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0", &modal) == 0 && !modal);
    CHECK(jc_gc_tool("G0 G55 G17 G21 G90 G94 G43.1 M5 M9 T12 F100 S0", &modal) == 12 && modal);
    CHECK(jc_gc_tool("G0 G54 M5", &modal) == -1 && !modal);
    {   float z = 0.0f;                                              // FluidNC v4 prints one TLO value per axis
        CHECK(jc_parse_tlo_z("0.000,0.000,6.500,0.000", &z) && fabsf(z - 6.5f) < 0.0001f);
        CHECK(jc_parse_tlo_z("2.5", &z) && fabsf(z - 2.5f) < 0.0001f);
        CHECK(!jc_parse_tlo_z("1,2", &z) && !jc_parse_tlo_z("", &z) && !jc_parse_tlo_z("x", &z));
    }
    CHECK(jc_gc_tool("T", nullptr) == -1 && jc_gc_tool("T99999", nullptr) == -1 && jc_gc_tool("TX", nullptr) == -1 && jc_gc_tool(nullptr, nullptr) == -1);
    {   ToolRig r;
        r.nc.tool = 3; r.nc.tlo = 2.5f; r.nc.tlo_active = true;
        CHECK(r.rd.start(r.nc.now));
        CHECK(!r.rd.start(r.nc.now));                                   // not while a read is running
        r.run(500);
        CHECK(r.rd.state() == JcToolReadState::Done);
        CHECK(r.rd.info().tool == 3 && r.rd.info().tlo_modal && r.rd.info().tlo_known && fabsf(r.rd.info().tlo - 2.5f) < 0.0001f);
        CHECK(r.queries.size() == 2 && r.queries[0] == "$G" && r.queries[1] == "$#");   // only the two read-only queries
        r.rd.on_link_lost();
        CHECK(r.rd.state() == JcToolReadState::Idle && !r.rd.info().tlo_known && r.rd.info().tool == -1);   // nothing survives a lost link
    }
    {   ToolRig r; r.link_ok = false;
        r.rd.start(r.nc.now); r.run(100);
        CHECK(r.rd.state() == JcToolReadState::Failed && r.queries.empty());
    }
    {   ToolRig r; r.nc.silent = true;
        r.rd.start(r.nc.now); r.run(6000);
        CHECK(r.rd.state() == JcToolReadState::Failed && strstr(r.rd.reason(), "no reply") != nullptr && r.queries.size() == 1);   // no retry
    }
    {   ToolRig r;                                                      // a controller that does not report [TLO:]
        r.rd.start(r.nc.now); r.run(30);
        r.rd.on_line("[GC:G0 G54 T0]"); r.rd.on_line("ok");
        r.run(30);
        r.rd.on_line("[G54:0.000,0.000,0.000,0.000]"); r.rd.on_line("ok");
        CHECK(r.rd.state() == JcToolReadState::Failed && strstr(r.rd.reason(), "[TLO:]") != nullptr);
    }
    {   ToolRig r;                                                      // a refusal ends the read
        r.rd.start(r.nc.now); r.run(30);
        r.rd.on_line("error:3");
        CHECK(r.rd.state() == JcToolReadState::Failed && strstr(r.rd.reason(), "error:3") != nullptr);
        r.rd.on_line("[GC:G0 T9]");                                     // late lines are not adopted
        CHECK(r.rd.info().tool != 9);
    }
    {   JcToolStore st;
        st.set_ref(-20.0f, 5.0f, -1);
        CHECK(st.has_ref && st.ref_mz == -20.0f);
        st.tlo_verified = true;
        st.invalidate("connection lost");
        CHECK(!st.has_ref && !st.tlo_verified && strstr(st.invalid_reason, "connection lost") != nullptr);
        st.set_ref(-19.0f, 0.0f, 1);
        CHECK(st.has_ref && st.invalid_reason[0] == '\0');
    }
}

int main() {
    test_listing();
    test_listing_counts();
    test_real_controller_transcript();
    test_formats();
    test_paths_and_names();
    test_missing_empty_errors();
    test_large_and_timeouts();
    test_isolation_and_link_loss();
    test_job_run_pause_resume_stop();
    test_job_completion();
    test_job_errors();
    test_job_disconnect_no_replay();
    test_job_blockers();
    test_tool_info();
    printf("sdjob: %d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
