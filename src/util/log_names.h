#pragma once

#include <cstdint>
#include <cstring>

// Dive-log filename grammar, split out of logging.cpp so it can be exercised
// on the host without an Arduino/LittleFS stub -- see tools/lognames_test/.
//
// A log is /logs/YYYYMMDD-NNN.csv (LOW) or /logs/YYYYMMDD-NNN.bin (MID/HIGH,
// see log_record.h). Both extensions share one sequence under a date, so NNN is
// unique across them and still gives the order the files were opened in. Date
// first so that lexicographic order
// equals chronological order (logging.cpp's cleanupOldLogs() prunes the
// lexicographically first name and must not prune a newer file), and so that a
// name is not reused after the directory is emptied. Reuse was the real
// hazard: net/log_sync.cpp remembers uploads by name, and the old bare
// NNN.csv counter restarted at 001 whenever /logs was cleared, which would
// hand a fresh log a deleted log's "already uploaded" mark.
namespace log_names {

static constexpr size_t PREFIX_LEN = 8;   // "YYYYMMDD"
// Buffer size for a basename: "YYYYMMDD-NNN.csv" (or .bin) + NUL. Not NAME_MAX --
// that is a POSIX macro, and the host tests include <cstdio>.
static constexpr size_t NAME_BUF_LEN = 20;

static constexpr const char* EXT_CSV = ".csv";
static constexpr const char* EXT_BIN = ".bin";

// True for a name ending in EXT_BIN -- a binary MID/HIGH log.
inline bool isBinary(const char* name) {
    if (!name) return false;
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, EXT_BIN) == 0;
}

// True for either log format. Both upload (net/log_sync.cpp, web_server.cpp):
// dive-map's track processor decodes the binary records itself, which is how
// a MID dive's magnetometer data reaches the server.
inline bool isLog(const char* name) {
    if (!name) return false;
    size_t len = strlen(name);
    return isBinary(name) || (len >= 4 && strcmp(name + len - 4, EXT_CSV) == 0);
}

// Split a basename into its date prefix and sequence number. False for
// anything that isn't this grammar -- notably the pre-dating "NNN.csv" logs,
// which still sort (below every dated name, which is correct: they are older
// than all of them) but never supply a sequence number.
inline bool parse(const char* name, char prefixOut[PREFIX_LEN + 1], uint16_t& seqOut) {
    if (!name) return false;
    for (size_t i = 0; i < PREFIX_LEN; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    if (name[PREFIX_LEN] != '-') return false;

    uint32_t seq    = 0;
    int      digits = 0;
    const char* p = name + PREFIX_LEN + 1;
    while (*p >= '0' && *p <= '9') {
        seq = seq * 10 + (uint32_t)(*p - '0');
        p++;
        digits++;
        if (seq > 65535u) return false;
    }
    if (digits == 0 || (strcmp(p, EXT_CSV) != 0 && strcmp(p, EXT_BIN) != 0)) return false;

    memcpy(prefixOut, name, PREFIX_LEN);
    prefixOut[PREFIX_LEN] = '\0';
    seqOut = (uint16_t)seq;
    return true;
}

// Is `a` an older log than `b`, by name alone? This is the ordering
// logging.cpp's cleanupOldLogs() prunes by, so getting it wrong deletes a
// dive rather than an old file.
//
// Two classes of name exist and they do NOT interleave correctly under a plain
// string compare: every pre-dating "NNN.csv" log predates the dated scheme, so
// it is older than any dated name -- but "001.csv" sorts *after*
// "00000000-001.csv" (the never-had-a-clock fallback), because '1' > '0' at
// the third character. Comparing by class first, then lexicographically within
// a class, is what makes name order equal age order.
inline bool olderThan(const char* a, const char* b) {
    char     pa[PREFIX_LEN + 1], pb[PREFIX_LEN + 1];
    uint16_t sa, sb;
    bool datedA = parse(a, pa, sa);
    bool datedB = parse(b, pb, sb);
    if (datedA != datedB) return !datedA;  // undated is always the older class
    if (!datedA) return strcmp(a, b) < 0;
    // Dated: by date, then sequence. Not a whole-name compare -- the extension
    // must not get a vote, and it would the moment a sequence outgrew %03u.
    int c = strcmp(pa, pb);
    if (c != 0) return c < 0;
    return sa < sb;
}

}  // namespace log_names
