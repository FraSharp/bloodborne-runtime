// SPDX-FileCopyrightText: Copyright 2026 bloodborne-native contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include "core/libraries/avplayer/playback_clock.h"

struct TestClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<TestClock>;
    static inline std::atomic<std::int64_t> millis{0};
    static time_point now() { return time_point{duration{millis.load()}}; }
};
int main() {
    Libraries::AvPlayer::PlaybackClock<TestClock> clock;
    assert(clock.CurrentTime(false) == 0);
    clock.Start();
    TestClock::millis = 25;
    assert(clock.CurrentTime(false) == 25);
    assert(clock.CurrentTime(true) == 0); // Wait for the first audio anchor.
    clock.NoteAudio(100);
    TestClock::millis = 35;
    assert(clock.CurrentTime(true) == 110);
    clock.Pause();
    TestClock::millis = 2035;
    assert(clock.CurrentTime(true) == 110);
    assert(clock.CurrentTime(false) == 35);
    clock.Resume();
    TestClock::millis = 2045;
    assert(clock.CurrentTime(true) == 120);
    assert(clock.CurrentTime(false) == 45);
    clock.NoteAudio(150);
    TestClock::millis = 2055;
    assert(clock.CurrentTime(true) == 160);
    assert(clock.CurrentTime(false) == 55); // A new audio anchor preserves video pause time.
    clock.Pause();
    TestClock::millis = 2065;
    clock.NoteAudio(0); // An audio request began before pause, then published late.
    assert(clock.CurrentTime(true) == 0); // Negative elapsed cannot wrap to UINT64_MAX.
    clock.Stop();
    assert(clock.CurrentTime(true) == 0);
    clock.Start();
    assert(clock.CurrentTime(true) == 0);
    clock.NoteAudio(std::numeric_limits<std::uint64_t>::max());
    TestClock::millis = 2066;
    assert(clock.CurrentTime(true) == std::numeric_limits<std::uint64_t>::max());
    clock.Stop();
    TestClock::millis = 3000;
    clock.Start();
    clock.NoteAudio(1000);
    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i) clock.NoteAudio(i % 2 ? 1000 : 2000);
    });
    for (int i = 0; i < 20000; ++i) {
        const auto value = clock.CurrentTime(true);
        assert(value == 1000 || value == 2000);
    }
    producer.join();
    std::puts("PASS: audio anchors, pause/resume, restart, unsigned bounds and concurrent snapshots");
}
