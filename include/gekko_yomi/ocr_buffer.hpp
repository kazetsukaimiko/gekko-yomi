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
    // Missed scans to tolerate. A static screen keeps hitting the track, so
    // the count never advances. Wall-clock time is not used.
    int ttl_scans{8};
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

    std::vector<TextRun> observe(const std::vector<TextRun>& runs);
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
class OcrConsensus {
public:
    OcrConsensus();

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
