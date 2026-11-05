#include "gekko_yomi/ocr_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace gekko::yomi {
namespace {

double box_area(const Rect& box) {
    if (box.width <= 0 || box.height <= 0) {
        return 0;
    }
    return box.width * box.height;
}

double overlap_ratio(const Rect& left, const Rect& right) {
    const double left_area = box_area(left);
    const double right_area = box_area(right);
    const double smaller = std::min(left_area, right_area);
    if (smaller <= 0) {
        return 0;
    }
    const double x1 = std::max(left.x, right.x);
    const double y1 = std::max(left.y, right.y);
    const double x2 = std::min(left.x + left.width, right.x + right.width);
    const double y2 = std::min(left.y + left.height, right.y + right.height);
    if (x2 <= x1 || y2 <= y1) {
        return 0;
    }
    return ((x2 - x1) * (y2 - y1)) / smaller;
}

Rect unite_box(const Rect& left, const Rect& right) {
    if (box_area(left) <= 0) {
        return right;
    }
    if (box_area(right) <= 0) {
        return left;
    }
    const double x1 = std::min(left.x, right.x);
    const double y1 = std::min(left.y, right.y);
    const double x2 = std::max(left.x + left.width, right.x + right.width);
    const double y2 = std::max(left.y + left.height, right.y + right.height);
    return Rect{x1, y1, x2 - x1, y2 - y1};
}

std::size_t codepoints(std::string_view text) {
    std::size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

}  // namespace

OcrBuffer::OcrBuffer(OcrBufferOptions options) : options_(options) {
    if (options_.promote_after < 1) {
        options_.promote_after = 1;
    }
    if (options_.history < 1) {
        options_.history = 1;
    }
    if (options_.ttl_scans < 0) {
        options_.ttl_scans = 0;
    }
}

std::vector<TextRun> OcrBuffer::observe(const std::vector<TextRun>& runs, const std::vector<TextRun>* present) {
    ++scan_;

    struct Item {
        bool fresh{false};
        Rect box;
        std::string text;
        int last_seen{0};
        std::string shown;
        std::vector<Reading> history;
        Reading incoming;
    };

    std::vector<Item> items;
    items.reserve(tracks_.size() + runs.size());
    for (const auto& track : tracks_) {
        Item item;
        item.box = track.region;
        item.last_seen = track.last_seen;
        item.shown = track.shown;
        item.history = track.history;
        if (!track.history.empty()) {
            item.text = track.history.back().text;
        }
        items.push_back(std::move(item));
    }
    for (const auto& run : runs) {
        if (run.text.empty()) {
            continue;
        }
        Item item;
        item.fresh = true;
        item.box = run.box;
        item.text = run.text;
        item.last_seen = scan_;
        item.incoming.text = run.text;
        item.incoming.box = run.box;
        item.incoming.vertical = run.vertical;
        item.incoming.confidence = run.confidence;
        item.incoming.characters = run.characters;
        item.incoming.scan = scan_;
        item.incoming.order = next_order_++;
        items.push_back(std::move(item));
    }

    const int count = static_cast<int>(items.size());
    std::vector<int> parent(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        parent[static_cast<std::size_t>(i)] = i;
    }
    auto find = [&parent](int index) {
        int root = index;
        while (parent[static_cast<std::size_t>(root)] != root) {
            root = parent[static_cast<std::size_t>(root)];
        }
        while (parent[static_cast<std::size_t>(index)] != root) {
            const int next = parent[static_cast<std::size_t>(index)];
            parent[static_cast<std::size_t>(index)] = root;
            index = next;
        }
        return root;
    };
    auto unite = [&parent, &find](int left, int right) {
        const int left_root = find(left);
        const int right_root = find(right);
        if (left_root != right_root) {
            parent[static_cast<std::size_t>(right_root)] = left_root;
        }
    };

    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const Item& left = items[static_cast<std::size_t>(i)];
            const Item& right = items[static_cast<std::size_t>(j)];
            const bool spatial = overlap_ratio(left.box, right.box) >= options_.overlap;
            const bool both_unplaced = box_area(left.box) <= 0 && box_area(right.box) <= 0 && !left.text.empty() &&
                                        left.text == right.text;
            if (spatial || both_unplaced) {
                unite(i, j);
            }
        }
    }

    std::vector<Track> next;
    std::vector<int> cluster_of(static_cast<std::size_t>(count), -1);
    for (int i = 0; i < count; ++i) {
        const int root = find(i);
        if (cluster_of[static_cast<std::size_t>(root)] < 0) {
            cluster_of[static_cast<std::size_t>(root)] = static_cast<int>(next.size());
            next.push_back(Track{});
        }
        cluster_of[static_cast<std::size_t>(i)] = cluster_of[static_cast<std::size_t>(root)];
        Track& track = next[static_cast<std::size_t>(cluster_of[static_cast<std::size_t>(i)])];
        const Item& item = items[static_cast<std::size_t>(i)];
        if (item.fresh) {
            track.history.push_back(item.incoming);
            // A caller that passes present is feeding remembered lines. Those
            // lines vote, but they are not a new sighting of the box.
            if (present == nullptr) {
                track.last_seen = scan_;
            }
        } else {
            track.history.insert(track.history.end(), item.history.begin(), item.history.end());
            if (item.last_seen >= track.last_seen) {
                track.last_seen = item.last_seen;
                if (!item.shown.empty()) {
                    track.shown = item.shown;
                }
            } else if (track.shown.empty()) {
                track.shown = item.shown;
            }
        }
    }

    std::vector<Track> kept;
    kept.reserve(next.size());
    for (auto& track : next) {
        if (track.history.empty()) {
            continue;
        }
        track.region = Rect{};
        for (const auto& reading : track.history) {
            track.region = unite_box(track.region, reading.box);
        }
        if (present != nullptr) {
            const bool seen = std::any_of(present->begin(), present->end(), [&](const TextRun& run) {
                if (run.text.empty()) {
                    return false;
                }
                if (overlap_ratio(track.region, run.box) >= options_.overlap) {
                    return true;
                }
                if (box_area(track.region) > 0 || box_area(run.box) > 0) {
                    return false;
                }
                return std::any_of(track.history.begin(), track.history.end(),
                                   [&](const Reading& reading) { return reading.text == run.text; });
            });
            if (seen) {
                track.last_seen = scan_;
            }
        }
        if (scan_ - track.last_seen > options_.ttl_scans) {
            continue;
        }
        std::sort(track.history.begin(), track.history.end(), [](const Reading& left, const Reading& right) {
            if (left.scan != right.scan) {
                return left.scan < right.scan;
            }
            return left.order < right.order;
        });
        if (static_cast<int>(track.history.size()) > options_.history) {
            track.history.erase(track.history.begin(),
                                track.history.end() - static_cast<std::ptrdiff_t>(options_.history));
        }
        choose_shown(&track);
        kept.push_back(std::move(track));
    }
    tracks_ = std::move(kept);
    return snapshot();
}

void OcrBuffer::choose_shown(Track* track) const {
    auto contained = [&](const Reading& reading) {
        if (reading.text.empty()) {
            return true;
        }
        const std::size_t length = codepoints(reading.text);
        for (const auto& other : track->history) {
            if (codepoints(other.text) <= length) {
                continue;
            }
            if (other.text.find(reading.text) == std::string::npos) {
                continue;
            }
            if (box_area(reading.box) <= 0 || box_area(other.box) <= 0 ||
                overlap_ratio(reading.box, other.box) >= options_.overlap) {
                return true;
            }
        }
        return false;
    };

    struct Candidate {
        std::string text;
        int count{0};
        std::size_t length{0};
        float confidence{0};
        int last_scan{0};
        int last_order{0};
    };
    std::vector<Candidate> candidates;
    for (const auto& reading : track->history) {
        if (contained(reading)) {
            continue;
        }
        Candidate* slot = nullptr;
        for (auto& candidate : candidates) {
            if (candidate.text == reading.text) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            candidates.push_back(Candidate{});
            slot = &candidates.back();
            slot->text = reading.text;
            slot->length = codepoints(reading.text);
        }
        slot->count += 1;
        float confidence = reading.confidence;
        if (!reading.characters.empty()) {
            float sum = 0;
            for (const auto& character : reading.characters) {
                sum += character.confidence;
            }
            confidence = sum / static_cast<float>(reading.characters.size());
        }
        slot->confidence = std::max(slot->confidence, confidence);
        if (reading.scan > slot->last_scan || (reading.scan == slot->last_scan && reading.order > slot->last_order)) {
            slot->last_scan = reading.scan;
            slot->last_order = reading.order;
        }
    }
    if (candidates.empty()) {
        return;
    }
    const Candidate* winner = &candidates.front();
    for (const auto& candidate : candidates) {
        const bool more_votes = candidate.count > winner->count;
        const bool same_votes_longer = candidate.count == winner->count && candidate.length > winner->length;
        const bool same_shape = candidate.count == winner->count && candidate.length == winner->length &&
                                candidate.confidence > winner->confidence + 1e-4F;
        const bool later = candidate.count == winner->count && candidate.length == winner->length &&
                           std::fabs(candidate.confidence - winner->confidence) <= 1e-4F &&
                           (candidate.last_scan > winner->last_scan ||
                            (candidate.last_scan == winner->last_scan && candidate.last_order > winner->last_order));
        if (more_votes || same_votes_longer || same_shape || later) {
            winner = &candidate;
        }
    }
    if (winner->text == track->shown) {
        return;
    }
    // Count scans, not individual hits. One scan can carry a fragment and the
    // full line, or two engines. The scan agrees when the winner is among its
    // readings. A suppressed fragment does not cancel that scan.
    int streak = 0;
    for (int index = static_cast<int>(track->history.size()) - 1; index >= 0;) {
        const int scan = track->history[static_cast<std::size_t>(index)].scan;
        bool has_winner = false;
        bool has_other = false;
        while (index >= 0 && track->history[static_cast<std::size_t>(index)].scan == scan) {
            const Reading& reading = track->history[static_cast<std::size_t>(index)];
            if (!contained(reading)) {
                if (reading.text == winner->text) {
                    has_winner = true;
                } else {
                    has_other = true;
                }
            }
            --index;
        }
        if (has_winner) {
            ++streak;
            continue;
        }
        if (has_other) {
            break;
        }
    }
    if (streak >= options_.promote_after) {
        track->shown = winner->text;
    }
}

std::vector<TextRun> OcrBuffer::snapshot() const {
    std::vector<TextRun> runs;
    for (const auto& track : tracks_) {
        if (track.shown.empty()) {
            continue;
        }
        const Reading* source = nullptr;
        for (const auto& reading : track.history) {
            if (reading.text == track.shown) {
                source = &reading;
            }
        }
        TextRun run;
        run.text = track.shown;
        if (source != nullptr) {
            run.box = source->box;
            run.vertical = source->vertical;
            run.confidence = source->confidence;
            run.characters = source->characters;
        } else {
            run.box = track.region;
        }
        runs.push_back(std::move(run));
    }
    std::sort(runs.begin(), runs.end(), [](const TextRun& left, const TextRun& right) {
        if (left.box.y != right.box.y) {
            return left.box.y < right.box.y;
        }
        return left.box.x < right.box.x;
    });
    return runs;
}

std::vector<TextRun> OcrBuffer::shown() const { return snapshot(); }

bool OcrBuffer::warming() const {
    bool history = false;
    for (const auto& track : tracks_) {
        if (!track.shown.empty()) {
            return false;
        }
        if (!track.history.empty()) {
            history = true;
        }
    }
    return history;
}

void OcrBuffer::clear() {
    tracks_.clear();
    scan_ = 0;
    next_order_ = 0;
}

OcrConsensus::OcrConsensus() : consensus_(([] {
                                             OcrBufferOptions options;
                                             options.promote_after = 1;
                                             return options;
                                         })()) {}

std::vector<TextRun> OcrConsensus::observe(const std::vector<EngineScan>& engines) {
    std::vector<TextRun> shown_runs;
    std::vector<TextRun> sightings;
    std::vector<std::string> seen;
    seen.reserve(engines.size());
    for (const auto& engine : engines) {
        seen.push_back(engine.engine_id);
        OcrBuffer* buffer = nullptr;
        for (auto& entry : engines_) {
            if (entry.first == engine.engine_id) {
                buffer = &entry.second;
                break;
            }
        }
        if (buffer == nullptr) {
            engines_.emplace_back(engine.engine_id, OcrBuffer{engine_options_});
            buffer = &engines_.back().second;
        }
        const auto shown = buffer->observe(engine.runs);
        shown_runs.insert(shown_runs.end(), shown.begin(), shown.end());
        sightings.insert(sightings.end(), engine.runs.begin(), engine.runs.end());
    }
    for (auto& entry : engines_) {
        bool present = false;
        for (const auto& id : seen) {
            if (id == entry.first) {
                present = true;
                break;
            }
        }
        if (!present) {
            entry.second.observe({});
        }
    }
    return consensus_.observe(shown_runs, &sightings);
}

bool OcrConsensus::settling() const {
    if (!consensus_.shown().empty()) {
        return false;
    }
    if (consensus_.warming()) {
        return true;
    }
    for (const auto& entry : engines_) {
        if (entry.second.warming()) {
            return true;
        }
    }
    return false;
}

void OcrConsensus::clear() {
    engines_.clear();
    consensus_.clear();
}

const OcrBuffer* OcrConsensus::engine(std::string_view engine_id) const {
    for (const auto& entry : engines_) {
        if (entry.first == engine_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

}  // namespace gekko::yomi
