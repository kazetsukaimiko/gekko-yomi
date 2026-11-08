#pragma once

#include "gekko_yomi/ocr.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

// How a buffer decides that two boxes are the same place on the frame.
// overlap is intersection divided by the smaller box. A fragment inside a
// line scores near 1, so the two share a track. A symmetric overlap score
// would miss that fragment.
struct OcrBufferOptions {
    double overlap{0.5};
    // How many scans a reading may vote, including the scan it was seen on.
    // 8 keeps that scan and the seven before it. 1 counts only the scan that
    // just finished, so a line that was replaced in the same box can win
    // without waiting out the older votes. Wall-clock time is not used.
    // The shown text still waits for promote_after agreeing scans.
    int ttl_scans{8};
    // Consecutive scans with no recognition in this box before the group is
    // dropped. 2 keeps the group through one empty scan and drops it on the
    // second. A hit from any overlapping recognition resets the count.
    int positional_invalidation_scans{2};
    // Recent hits kept for the vote. Older hits fall out.
    int history{8};
    // Consecutive non-fragment hits before the shown text changes, including
    // the first time a track shows anything. A one-scan fragment stays hidden.
    int promote_after{2};
};

// One engine's memory of where text has been on the frame.
// observe() is one finished scan. An empty scan still ages every track.
// shown() is the text the buffer will admit. Length breaks a tie. A reading
// that is only a piece of a longer reading in the same track does not vote.
class OcrBuffer {
public:
    explicit OcrBuffer(OcrBufferOptions options = {});
    void set_options(OcrBufferOptions options);

    // runs are the readings to remember. When present is null, those readings
    // also mark the box as seen. When present is set, only those runs mark
    // the box; a remembered line passed in runs does not keep the track alive.
    std::vector<TextRun> observe(const std::vector<TextRun>& runs,
                                 const std::vector<TextRun>* present = nullptr);
    std::vector<TextRun> shown() const;
    // History is present and nothing has been promoted yet.
    bool warming() const;
    void clear();
    int scan_count() const { return scan_; }

private:
    struct Reading {
        std::string text;
        Rect box;
        bool vertical{false};
        float confidence{0};
        std::vector<CharBox> characters;
        int scan{0};
        int order{0};
    };

    struct Track {
        Rect region;
        int last_seen{0};
        std::string shown;
        std::vector<Reading> history;
    };

    void choose_shown(Track* track) const;
    std::vector<TextRun> snapshot() const;

    OcrBufferOptions options_;
    std::vector<Track> tracks_;
    int scan_{0};
    int next_order_{0};
};

// One finished pass from a single engine. An engine that failed still
// contributes an empty run list so its buffer ages.
struct EngineScan {
    std::string engine_id;
    std::vector<TextRun> runs;
};

// One buffer per engine, plus a buffer that watches their shown text.
// With one engine the consensus buffer repeats that engine's shown runs,
// and it does not add another two-scan wait (its promote_after is 1).
// With several engines, overlapping shown runs become one track and vote.
// A track's box is seen when any engine recognizes text there on this scan.
// positional_invalidation_scans empty scans drop the group. The default, 2,
// keeps the group through one empty scan and drops it on the second.
// ttl_scans is how long a reading may vote after that. It does not, by
// itself, delete the group.
class OcrConsensus {
public:
    explicit OcrConsensus(OcrBufferOptions options = {});
    void set_options(OcrBufferOptions options);

    std::vector<TextRun> observe(const std::vector<EngineScan>& engines);
    std::vector<TextRun> shown() const { return consensus_.shown(); }
    bool settling() const;
    void clear();

    const OcrBuffer* engine(std::string_view engine_id) const;

private:
    OcrBufferOptions engine_options_;
    OcrBuffer consensus_;
    std::vector<std::pair<std::string, OcrBuffer>> engines_;
};

}  // namespace gekko::yomi
