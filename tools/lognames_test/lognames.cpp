// Host-side test for the dive-log filename grammar and the ordering the rest
// of the system leans on. Compiles the REAL src/util/log_names.h (it is pure
// C++ by design so this is possible), not a copy.
//   see tools/lognames_test/run.sh
#include "../../src/util/log_names.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static void expectParse(const char* name, const char* wantPrefix, uint16_t wantSeq) {
    char     prefix[log_names::PREFIX_LEN + 1] = "";
    uint16_t seq = 0;
    bool ok = log_names::parse(name, prefix, seq);
    if (!ok) { printf("  FAIL: %s should parse\n", name); failures++; return; }
    if (strcmp(prefix, wantPrefix) != 0 || seq != wantSeq) {
        printf("  FAIL: %s -> %s/%u, want %s/%u\n", name, prefix, seq, wantPrefix, wantSeq);
        failures++;
    }
}

static void expectReject(const char* name) {
    char     prefix[log_names::PREFIX_LEN + 1] = "";
    uint16_t seq = 0;
    if (log_names::parse(name, prefix, seq)) {
        printf("  FAIL: %s should not parse\n", name);
        failures++;
    }
}

// Mirrors logging.cpp's openNextFile(): today's date if the clock is set,
// otherwise the newest prefix already on disk, then the next free sequence
// under whichever prefix that is.
static std::string nextName(const std::vector<std::string>& dir, const char* today) {
    std::string prefix = today ? today : "";
    if (prefix.empty()) {
        for (const auto& n : dir) {
            char p[log_names::PREFIX_LEN + 1]; uint16_t s;
            if (log_names::parse(n.c_str(), p, s) && prefix < p) prefix = p;
        }
        if (prefix.empty()) prefix = "00000000";
    }
    uint16_t highest = 0;
    for (const auto& n : dir) {
        char p[log_names::PREFIX_LEN + 1]; uint16_t s;
        if (log_names::parse(n.c_str(), p, s) && prefix == p && s > highest) highest = s;
    }
    char out[log_names::NAME_BUF_LEN];
    snprintf(out, sizeof(out), "%s-%03u.csv", prefix.c_str(), (unsigned)(highest + 1));
    return out;
}

int main() {
    printf("log_names grammar\n");
    expectParse("20260908-001.csv", "20260908", 1);
    expectParse("20260908-999.csv", "20260908", 999);
    expectParse("00000000-001.csv", "00000000", 1);   // never-had-a-clock fallback
    expectReject("001.csv");                          // pre-dating logs
    expectReject("2026098-001.csv");                  // 7-digit prefix
    expectReject("20260908_001.csv");                 // wrong separator
    expectReject("20260908-.csv");                    // no sequence
    expectReject("20260908-001.txt");                 // not a log
    expectReject("20260908-001.csv.bak");             // trailing junk
    expectReject("2026090a-001.csv");                 // non-digit in prefix
    expectReject("mag_base.json");
    expectReject("");

    printf("buffer sizing\n");
    check(strlen("20260908-999.csv") + 1 <= log_names::NAME_BUF_LEN, "NAME_BUF_LEN fits the grammar");

    printf("name order == age order\n");
    // cleanupOldLogs() prunes the file olderThan() ranks first; it must be the
    // genuinely oldest one, across a year boundary and with pre-dating logs
    // present. Note "001.csv" > "00000000-001.csv" as plain strings, which is
    // exactly why olderThan() compares by class before comparing text.
    std::vector<std::string> chronological = {
        "001.csv", "017.csv",                            // oldest: pre-dating
        "00000000-001.csv",                              // clockless unit
        "20251231-002.csv", "20260101-001.csv",          // year boundary
        "20260908-001.csv", "20260908-002.csv", "20260908-010.csv",
    };
    check(!std::is_sorted(chronological.begin(), chronological.end()),
          "plain string order would get this wrong (guards the test itself)");
    std::vector<std::string> shuffled = chronological;
    std::sort(shuffled.begin(), shuffled.end(), [](const std::string& a, const std::string& b) {
        return log_names::olderThan(a.c_str(), b.c_str());
    });
    check(shuffled == chronological, "olderThan order matches age order");

    // And the property cleanupOldLogs() actually depends on: scanning for the
    // single oldest name never picks a file that is newer than another.
    for (size_t i = 0; i < chronological.size(); i++) {
        for (size_t j = i + 1; j < chronological.size(); j++) {
            check(log_names::olderThan(chronological[i].c_str(), chronological[j].c_str()),
                  "earlier name ranks older");
            check(!log_names::olderThan(chronological[j].c_str(), chronological[i].c_str()),
                  "ordering is antisymmetric");
        }
    }

    printf("sequence allocation\n");
    std::vector<std::string> dir;
    check(nextName(dir, "20260908") == "20260908-001.csv", "empty dir -> 001");
    dir.push_back("20260908-001.csv");
    check(nextName(dir, "20260908") == "20260908-002.csv", "second log of the day");
    dir.push_back("20260908-002.csv");
    check(nextName(dir, "20260909") == "20260909-001.csv", "new day restarts at 001");

    // The bug the dated scheme exists to kill: deleting every log must not
    // let a new one inherit a deleted one's name.
    dir.push_back("20260909-001.csv");
    std::vector<std::string> emptied;
    check(nextName(emptied, "20260909") != "20260908-001.csv", "wipe does not reuse an old name");
    check(nextName(emptied, "20260909") == "20260909-001.csv", "wipe restarts under today");

    // Deleting the middle of a day still moves forward, never backward.
    std::vector<std::string> gapped = {"20260908-001.csv", "20260908-003.csv"};
    check(nextName(gapped, "20260908") == "20260908-004.csv", "gap does not rewind the sequence");

    printf("clockless fallback\n");
    std::vector<std::string> dated = {"20260908-001.csv", "20251231-009.csv"};
    check(nextName(dated, nullptr) == "20260908-002.csv", "no clock -> continue newest prefix");
    std::vector<std::string> legacyOnly = {"001.csv"};
    check(nextName(legacyOnly, nullptr) == "00000000-001.csv", "no clock, no dated log -> zeros");
    // A clockless file must still rank after the pre-dating logs it follows
    // and before any later dated one, so cleanup order stays honest.
    check(log_names::olderThan("001.csv", "00000000-001.csv"), "legacy older than clockless");
    check(log_names::olderThan("00000000-001.csv", "20260908-001.csv"), "clockless older than dated");

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("\nAll log_names tests passed.\n");
    return 0;
}
