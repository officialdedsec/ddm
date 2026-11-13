// How fast a download is going, as a number calm enough to read.
//
// Bytes don't arrive evenly. Eight to thirty-two connections each hand over whatever the network just gave
// them, so the amount that lands in any half second swings wildly even while the download itself is perfectly
// steady. A speed worked out from one such moment flickers; worked out only now and then, it also seems to
// stall and then jump, because whoever draws it reads it on a clock of their own.
//
// So this keeps how much had arrived at each of the last few moments, divides the bytes of the last second and
// a half by that much time, and eases the answer towards that figure. The number then moves the way the
// download really does. Ask for it as often as you like: it is worked out from the clock, not from how often
// you ask, so asking more often only draws the same movement more finely.
//
// One meter belongs to one download, and is used from that download's own thread (or under its lock).
#pragma once
#include <cmath>
#include <cstdint>

namespace ddm {

class SpeedMeter {
public:
    void start(int64_t now) { count_ = 0; head_ = 0; shown_ = 0; shownAt_ = now; mark(now); }
    void add(int64_t bytes) { total_ += bytes; }       // bytes that have just arrived
    void setTotal(int64_t bytes) { total_ = bytes; }   // … or how much has arrived in all, when that is counted elsewhere
    void stop() { count_ = 0; shown_ = 0; }            // paused or finished: there is no speed to show

    // Bytes per second, ready to draw.
    double value(int64_t now) {
        if (count_ == 0) { start(now); return 0; }
        if (now - newest().at >= kStep) mark(now);
        while (count_ > 2 && now - oldest().at > kWindow) forget();
        int64_t span = now - oldest().at;
        // less than a quarter second to go on: keep what was shown rather than guess from almost nothing
        double raw = span >= kLeast ? static_cast<double>(total_ - oldest().total) * 1000.0 / static_cast<double>(span) : shown_;
        int64_t dt = now - shownAt_;
        shownAt_ = now;
        if (dt > 0) shown_ += (raw - shown_) * (1.0 - std::exp(-static_cast<double>(dt) / static_cast<double>(kEase)));
        if (shown_ < 1) shown_ = 0;
        return shown_;
    }

private:
    enum { kRing = 16, kStep = 125, kWindow = 1500, kLeast = 250, kEase = 300 };
    struct Mark { int64_t at; int64_t total; };

    Mark ring_[kRing]{};
    int count_ = 0, head_ = 0;
    int64_t total_ = 0, shownAt_ = 0;
    double shown_ = 0;

    Mark& oldest() { return ring_[head_]; }
    Mark& newest() { return ring_[(head_ + count_ - 1) % kRing]; }
    void mark(int64_t now) {
        Mark m{now, total_};
        if (count_ < kRing) { ring_[(head_ + count_) % kRing] = m; count_++; }
        else { ring_[head_] = m; head_ = (head_ + 1) % kRing; }
    }
    void forget() { head_ = (head_ + 1) % kRing; count_--; }
};

} // namespace ddm
