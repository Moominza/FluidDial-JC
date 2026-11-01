// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// SD card browser (Instruction 010). Lists files that live on the CONTROLLER's SD card by asking FluidNC with
// the read-only queries `$SD/Status` and `$SD/List`. The local ESP32 filesystem is never listed here: lines are
// accepted only while a request that this browser itself sent is outstanding, and `$LocalFS/...` is never asked.
//
// Parsing is tolerant by design because the exact text could not be checked against this controller:
//   [FILE:/dir/name.nc|SIZE:1234]     [FILE: /dir/name.nc|SIZE:1234]     (optional space after the colon)
//   [DIR:/dir]                                                            (explicit directory line, if sent)
// FluidNC lists the whole card recursively with full paths, so directories are also inferred from the paths.
// `$SD/Status` is only used to show the controller's own words; the listing result is the authority (error:60
// = no card). If `$SD/Status` is refused (error:3) it is reported as "not supported" and the listing continues.
//
// Pure: no I/O. The caller supplies a query function and feeds every controller line to on_line().

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "JcQuery.hpp"

namespace jc8048w550 {


// FluidNC filesystem error numbers (from its error table; shown together with the raw number).
inline const char* jc_sd_error_text(int code) {
    switch (code) {
        case 60: return "SD card not mounted / not present";
        case 61: return "SD read failed";
        case 62: return "could not open the directory";
        case 63: return "directory not found";
        case 64: return "file is empty";
        case 65: return "file not found";
        case 66: return "could not open the file";
        case 67: return "SD card busy (a job is running?)";
        case 3: return "command not supported by this controller";
        case 8: return "controller not idle";
        case 9: return "controller locked (alarm)";
        default: return "controller error";
    }
}

// Read-only browsing (Instruction 010A): the pendant lists the controller's SD card; it never starts a file.
struct JcSdEntry {
    char     path[96];     // full path as reported (truncated if longer)
    uint32_t size;         // bytes; 0 for directories
    bool     dir;
    bool     truncated;    // the controller's path did not fit
};

enum class JcSdState : uint8_t {
    Idle,        // nothing asked yet
    Status,      // waiting for $SD/Status
    Listing,     // waiting for / receiving $SD/List
    Ready,       // listing complete, at least one entry
    Empty,       // listing complete, no entries
    NoCard,      // controller says there is no (mounted) card
    Error,       // another controller error, timeout or refusal; see message()
};

constexpr uint16_t kJcSdMaxEntries = 160;

class JcSdBrowser {
public:
    using QueryFn = bool (*)(void* user, const char* line);

    void set_query(QueryFn fn, void* user) { _fn = fn; _user = user; }

    // REFRESH: discards everything shown so far and asks again. Refused while a request is outstanding.
    bool refresh(uint32_t now) {
        if (busy()) return false;
        clear_listing();
        _status_text[0] = '\0';
        _status_supported = true;
        _msg[0] = '\0';
        _state = JcSdState::Status;
        _sent = false;
        _started = _last_rx = now;
        _cwd[0] = '/'; _cwd[1] = '\0';
        _selected = -1;
        return true;
    }

    // Called from the main loop.
    void service(uint32_t now) {
        if (_state == JcSdState::Status || _state == JcSdState::Listing) {
            if (!_sent) {
                const char* q = _state == JcSdState::Status ? kJcQuerySdStatusLine : kJcQuerySdListLine;
                if (_fn == nullptr || !_fn(_user, q)) { fail("could not send the query (no live link or another operation is busy)"); return; }
                _sent = true;
                _last_rx = now;
                ++_queries;
            } else if (now - _last_rx > (_state == JcSdState::Status ? 4000u : 8000u)) {
                if (_state == JcSdState::Status) {
                    // The listing is what matters: a silent $SD/Status must not block it.
                    _status_supported = false;
                    snprintf(_status_text, sizeof(_status_text), "no reply to $SD/Status");
                    _state = JcSdState::Listing;
                    _sent = false;
                } else {
                    fail("no reply from the controller (timeout)");
                }
            }
        }
    }

    void on_line(const char* line, uint32_t now) {
        if (!busy() || !_sent || line == nullptr) return;   // not ours: never adopt unrelated lines
        _last_rx = now;
        while (*line == ' ') ++line;
        const size_t n = strlen(line);
        if (n == 0) return;
        if (strcmp(line, "ok") == 0) {
            if (_state == JcSdState::Status) { _state = JcSdState::Listing; _sent = false; return; }
            finish_listing();
            return;
        }
        if (strncmp(line, "error:", 6) == 0) {
            const int code = atoi(line + 6);
            if (_state == JcSdState::Status) {
                // Unknown to this firmware or no card: keep going, the listing decides.
                _status_supported = false;
                snprintf(_status_text, sizeof(_status_text), "$SD/Status refused (error:%d)", code);
                _state = JcSdState::Listing;
                _sent = false;
                return;
            }
            _code = code;
            if (code == 60) {
                _state = JcSdState::NoCard;
                snprintf(_msg, sizeof(_msg), "NO SD CARD (error:60): %s", jc_sd_error_text(60));
            } else {
                _state = JcSdState::Error;
                snprintf(_msg, sizeof(_msg), "SD LIST FAILED (error:%d): %s", code, jc_sd_error_text(code));
            }
            return;
        }
        if (strncmp(line, "ALARM:", 6) == 0) {
            fail("controller ALARM during the SD query");
            return;
        }
        if (_state == JcSdState::Status) {
            // Free text: keep the controller's own words, never interpret them as "mounted".
            if (_status_text[0] == '\0') snprintf(_status_text, sizeof(_status_text), "%.70s", line);
            if (strstr(line, "No SD") != nullptr || strstr(line, "no SD") != nullptr || strstr(line, "not mounted") != nullptr
                || strstr(line, "not detected") != nullptr || strstr(line, "No card") != nullptr || strstr(line, "Failed") != nullptr) {
                _status_says_absent = true;
            }
            return;
        }
        parse_listing_line(line, n);
    }

    // The connection went away: nothing shown may be trusted, nothing is re-asked.
    void on_link_lost() {
        if (busy()) fail("connection lost during the SD query; nothing was retried");
    }
    void invalidate() {
        clear_listing();
        _state = JcSdState::Idle;
        _msg[0] = '\0';
        _selected = -1;
    }

    // ---- navigation ----
    // Entries directly inside the current directory (files and directories, directories first, then by name).
    uint16_t visible_count() const { return _nvis; }
    const JcSdEntry* visible(uint16_t i) const { return i < _nvis ? &_entries[_vis[i]] : nullptr; }
    bool enter(uint16_t vis_index) {
        const JcSdEntry* e = visible(vis_index);
        if (e == nullptr || !e->dir) return false;
        snprintf(_cwd, sizeof(_cwd), "%s", e->path);
        rebuild_visible();
        _selected = -1;
        return true;
    }
    bool up() {
        if (strcmp(_cwd, "/") == 0) return false;
        char* slash = strrchr(_cwd, '/');
        if (slash == nullptr) return false;
        if (slash == _cwd) _cwd[1] = '\0'; else *slash = '\0';
        rebuild_visible();
        _selected = -1;
        return true;
    }
    const char* cwd() const { return _cwd; }

    // ---- selection ----
    bool select(uint16_t vis_index) {
        const JcSdEntry* e = visible(vis_index);
        if (e == nullptr || e->dir) return false;
        _selected = static_cast<int16_t>(_vis[vis_index]);
        return true;
    }
    void clear_selection() { _selected = -1; }
    const JcSdEntry* selected() const { return _selected >= 0 ? &_entries[_selected] : nullptr; }

    // ---- observers ----
    JcSdState state() const { return _state; }
    bool busy() const { return _state == JcSdState::Status || _state == JcSdState::Listing; }
    const char* message() const { return _msg; }
    int error_code() const { return _code; }
    const char* status_text() const { return _status_text; }
    // "/sd Free:14.57 GB Used:25.72 MB Total:14.59 GB" as the controller printed it after the listing (empty if absent).
    const char* summary() const { return _summary; }
    bool status_supported() const { return _status_supported; }
    uint16_t total_entries() const { return _n; }
    uint16_t files_seen() const { return _files_seen; }
    bool overflow() const { return _overflow; }
    uint32_t queries_sent() const { return _queries; }

    // Basename for display (the part after the last '/').
    static const char* base_name(const char* path) {
        const char* s = strrchr(path, '/');
        return s != nullptr && s[1] != '\0' ? s + 1 : path;
    }
    // Human size: "812 B", "12.3 KB", "4.2 MB".
    static void size_text(uint32_t bytes, char* out, size_t size) {
        if (bytes < 1024u) snprintf(out, size, "%u B", static_cast<unsigned>(bytes));
        else if (bytes < 1024u * 1024u) snprintf(out, size, "%.1f KB", static_cast<double>(bytes) / 1024.0);
        else snprintf(out, size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    // Shortens `text` to at most `max_chars`, keeping the end (the extension) and marking the cut with "..".
    static void ellipsize(const char* text, size_t max_chars, char* out, size_t size) {
        const size_t n = strlen(text);
        if (n <= max_chars || max_chars < 6 || max_chars + 1 > size) { snprintf(out, size, "%s", text); return; }
        const size_t tail = max_chars / 3;
        const size_t head = max_chars - tail - 2;
        snprintf(out, size, "%.*s..%s", static_cast<int>(head), text, text + n - tail);
    }

private:
    void clear_listing() {
        _n = 0; _nvis = 0; _files_seen = 0; _overflow = false; _code = 0; _status_says_absent = false;
        _stack_n = 0; _summary[0] = '\0';
    }
    void fail(const char* why) {
        _state = JcSdState::Error;
        snprintf(_msg, sizeof(_msg), "%s", why);
    }

    static bool parse_size(const char* s, uint32_t& out) {
        if (*s < '0' || *s > '9') return false;
        uint64_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10u + static_cast<uint32_t>(*s - '0'); if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull; ++s; }
        out = static_cast<uint32_t>(v);
        return *s == '\0';
    }

    // The format of FluidNC v4.1.1 (read from the real controller):
    //   [DIR:System Volume Information]          a folder: its depth is the number of leading spaces
    //   [FILE:  IndexerVolumeGuid|SIZE:76]       a file inside it: depth + 1 leading spaces
    //   [FILE: Test.nc|SIZE:43039]               a file in the root: one leading space
    //   [/sd Free:14.57 GB Used:25.72 MB Total:14.59 GB]   the card summary after the list
    // Names are relative; the folder stack rebuilds the path. Full-path entries ("[FILE:/a/b.nc|SIZE:1]") are also accepted.
    void parse_listing_line(const char* line, size_t n) {
        const bool is_file = strncmp(line, "[FILE:", 6) == 0;
        const bool is_dir = strncmp(line, "[DIR:", 5) == 0;
        if (!is_file && !is_dir) {
            if (line[0] == '[' && line[1] == '/' && strstr(line, "Free:") != nullptr && line[n - 1] == ']') {
                size_t len = n - 2;
                if (len >= sizeof(_summary)) len = sizeof(_summary) - 1;
                memcpy(_summary, line + 1, len);
                _summary[len] = '\0';
            }
            return;
        }
        if (line[n - 1] != ']') { ++_bad_lines; return; }
        const char* p = line + (is_file ? 6 : 5);
        size_t spaces = 0;
        while (*p == ' ') { ++p; ++spaces; }
        char body[160];
        const size_t len = static_cast<size_t>(line + n - 1 - p);
        if (len == 0 || len >= sizeof(body)) { ++_bad_lines; return; }
        memcpy(body, p, len);
        body[len] = '\0';
        uint32_t size = 0;
        if (is_file) {
            char* bar = strstr(body, "|SIZE:");
            if (bar == nullptr) { ++_bad_lines; return; }     // a file entry without a size is not trusted
            *bar = '\0';
            if (!parse_size(bar + 6, size)) { ++_bad_lines; return; }
        }
        char path[200];
        if (body[0] == '/') {                                  // full-path form
            snprintf(path, sizeof(path), "%s", body);
        } else {
            size_t depth = is_file ? (spaces > 0 ? spaces - 1 : 0) : spaces;
            if (depth > _stack_n) depth = _stack_n;            // inconsistent nesting: attach to the deepest known folder
            path[0] = '\0';
            size_t used = 0;
            for (size_t d = 0; d < depth && d < kMaxDepth; ++d) {
                used += static_cast<size_t>(snprintf(path + used, sizeof(path) - used, "/%s", _stack[d]));
                if (used >= sizeof(path)) { used = sizeof(path) - 1; break; }
            }
            snprintf(path + used, sizeof(path) - used, "/%s", body);
            if (is_dir && depth < kMaxDepth) {
                snprintf(_stack[depth], sizeof(_stack[depth]), "%s", body);
                _stack_n = depth + 1;
            }
        }
        // A trailing slash on a folder is not part of its name.
        size_t bl = strlen(path);
        while (bl > 1 && path[bl - 1] == '/') path[--bl] = '\0';
        if (is_file) ++_files_seen;
        add(path, size, is_dir);
        if (is_file) {
            // Every parent folder exists even if its [DIR:] line was not seen.
            char parent[96];
            const size_t pl = strlen(path);
            for (size_t i = 1; i < pl; ++i) {
                if (path[i] == '/' && i < sizeof(parent)) {
                    memcpy(parent, path, i);
                    parent[i] = '\0';
                    add(parent, 0, true);
                }
            }
        }
    }

    void add(const char* path, uint32_t size, bool dir) {
        for (uint16_t i = 0; i < _n; ++i) {
            if (strcmp(_entries[i].path, path) == 0 && _entries[i].dir == dir) return;   // listed twice
        }
        if (_n >= kJcSdMaxEntries) { _overflow = true; return; }
        JcSdEntry& e = _entries[_n++];
        const size_t len = strlen(path);
        e.truncated = len >= sizeof(e.path);
        snprintf(e.path, sizeof(e.path), "%s", path);
        e.size = size;
        e.dir = dir;
    }

    void finish_listing() {
        if (_status_says_absent && _n == 0) {
            _state = JcSdState::NoCard;
            snprintf(_msg, sizeof(_msg), "NO SD CARD: the controller reports \"%s\"", _status_text);
            return;
        }
        if (_n == 0) {
            _state = JcSdState::Empty;
            snprintf(_msg, sizeof(_msg), "SD CARD HAS NO FILES");
            return;
        }
        _state = JcSdState::Ready;
        _msg[0] = '\0';
        if (_overflow) snprintf(_msg, sizeof(_msg), "LIST TRUNCATED: only the first %u entries are kept", static_cast<unsigned>(kJcSdMaxEntries));
        rebuild_visible();
    }

    // Parent of `path`: "/a/b" -> "/a", "/a" -> "/".
    static void parent_of(const char* path, char* out, size_t size) {
        snprintf(out, size, "%s", path);
        char* slash = strrchr(out, '/');
        if (slash == nullptr || slash == out) { snprintf(out, size, "/"); return; }
        *slash = '\0';
    }

    void rebuild_visible() {
        _nvis = 0;
        char parent[96];
        for (int pass = 0; pass < 2; ++pass) {          // directories first
            for (uint16_t i = 0; i < _n; ++i) {
                if (_entries[i].dir != (pass == 0)) continue;
                parent_of(_entries[i].path, parent, sizeof(parent));
                if (strcmp(parent, _cwd) != 0) continue;
                // insertion by name within the group
                uint16_t pos = _nvis;
                const uint16_t group_start = pass == 0 ? 0 : first_file_index();
                while (pos > group_start && strcmp(_entries[_vis[pos - 1]].path, _entries[i].path) > 0) {
                    _vis[pos] = _vis[pos - 1];
                    --pos;
                }
                _vis[pos] = i;
                ++_nvis;
            }
        }
    }
    uint16_t first_file_index() const {
        uint16_t k = 0;
        while (k < _nvis && _entries[_vis[k]].dir) ++k;
        return k;
    }

    QueryFn   _fn = nullptr;
    void*     _user = nullptr;
    JcSdState _state = JcSdState::Idle;
    bool      _sent = false;
    bool      _status_supported = true;
    bool      _status_says_absent = false;
    uint32_t  _started = 0, _last_rx = 0, _queries = 0, _bad_lines = 0;
    int       _code = 0;
    char      _msg[96] = "";
    char      _status_text[72] = "";
    char      _cwd[96] = "/";
    static constexpr size_t kMaxDepth = 6;
    char      _stack[kMaxDepth][40] = {};
    size_t    _stack_n = 0;
    char      _summary[72] = "";
    JcSdEntry _entries[kJcSdMaxEntries];
    uint16_t  _vis[kJcSdMaxEntries];
    uint16_t  _n = 0, _nvis = 0, _files_seen = 0;
    int16_t   _selected = -1;
    bool      _overflow = false;
};

}  // namespace jc8048w550
