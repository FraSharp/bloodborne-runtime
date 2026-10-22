// SPDX-FileCopyrightText: Copyright 2026 bloodborne-native contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace Libraries::AvPlayer {

// Audio timestamps and host anchors must be read as one snapshot. Read the monotonic
// clock inside the same lock so a newly published anchor cannot lie after the sampled now.
template <class Clock = std::chrono::steady_clock>
class PlaybackClock {
public:
    void Start() {
        std::scoped_lock lock{mutex};
        start = Clock::now();
        audio.reset();
        paused.reset();
        total_pause = audio_pause = {};
    }
    void Stop() {
        std::scoped_lock lock{mutex};
        start.reset();
        audio.reset();
        paused.reset();
        total_pause = audio_pause = {};
    }
    void NoteAudio(std::uint64_t timestamp) {
        std::scoped_lock lock{mutex};
        if (start) {
            audio = Anchor{timestamp, Clock::now()};
            audio_pause = {};
        }
    }
    void Pause() {
        std::scoped_lock lock{mutex};
        if (start && !paused) paused = Clock::now();
    }
    void Resume() {
        std::scoped_lock lock{mutex};
        if (paused) {
            const auto duration = Clock::now() - *paused;
            total_pause += duration;
            audio_pause += duration;
            paused.reset();
        }
    }
    std::uint64_t CurrentTime(bool has_audio) {
        std::scoped_lock lock{mutex};
        if (!start || (has_audio && !audio)) return 0;
        const auto now = paused ? *paused : Clock::now();
        const auto anchor = has_audio ? audio->time : *start;
        const auto pause = has_audio ? audio_pause : total_pause;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - anchor - pause).count();
        const std::uint64_t base = has_audio ? audio->timestamp : 0;
        // A late audio publication during pause must not become unsigned time wraparound.
        if (elapsed <= 0) return base;
        const auto delta = static_cast<std::uint64_t>(elapsed);
        const auto max = std::numeric_limits<std::uint64_t>::max();
        return delta > max - base ? max : base + delta;
    }

private:
    struct Anchor { std::uint64_t timestamp; typename Clock::time_point time; };
    std::mutex mutex;
    std::optional<typename Clock::time_point> start, paused;
    std::optional<Anchor> audio;
    typename Clock::duration total_pause{}, audio_pause{};
};

} // namespace Libraries::AvPlayer
