// SPDX-FileCopyrightText: Copyright 2026 bloodborne-native contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>
#include "core/libraries/avplayer/avplayer_common.h"

using Libraries::AvPlayer::AvPlayerQueue;

int main() {
    AvPlayerQueue<std::unique_ptr<unsigned>> queue;
    assert(!queue.Pop());
    queue.Push(std::make_unique<unsigned>(10));
    queue.Push(std::make_unique<unsigned>(20));
    assert(!queue.PopIf([](const auto& value) { return *value > 10; }));
    assert(queue.Size() == 2);
    auto first = queue.PopIf([](const auto& value) { return *value == 10; });
    assert(first && **first == 10);
    auto second = queue.Pop();
    assert(second && **second == 20);
    assert(!queue.Pop());

    // One decoder, two competing consumers and an EOF-style size observer.
    constexpr unsigned count = 20000;
    std::array<std::atomic<unsigned>, count> seen{};
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (unsigned i = 0; i < count; ++i) queue.Push(std::make_unique<unsigned>(i));
        done.store(true, std::memory_order_release);
    });
    auto consume = [&] {
        while (!done.load(std::memory_order_acquire) || queue.Size() != 0) {
            if (auto value = queue.Pop()) {
                assert(**value < count);
                assert(seen[**value].fetch_add(1) == 0);
            } else {
                std::this_thread::yield();
            }
        }
    };
    std::thread a(consume), b(consume);
    std::thread observer([&] {
        while (!done.load(std::memory_order_acquire)) {
            assert(queue.Size() <= count);
            std::this_thread::yield();
        }
    });
    producer.join();
    a.join();
    b.join();
    observer.join();
    for (const auto& n : seen) assert(n.load() == 1);
    assert(queue.Size() == 0);

    // Clearing and popping must transfer or destroy each owned buffer exactly once.
    struct Buffer {
        std::atomic<unsigned>& destroyed;
        explicit Buffer(std::atomic<unsigned>& destroyed_) : destroyed{destroyed_} {}
        ~Buffer() { ++destroyed; }
    };
    std::atomic<unsigned> destroyed{0};
    AvPlayerQueue<std::unique_ptr<Buffer>> buffers;
    for (unsigned i = 0; i < count; ++i) buffers.Push(std::make_unique<Buffer>(destroyed));
    std::atomic<bool> start{false};
    std::thread popper([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (buffers.Pop()) {}
    });
    start.store(true, std::memory_order_release);
    buffers.Clear();
    popper.join();
    assert(buffers.Size() == 0 && destroyed == count);
    std::puts("PASS: conditional frame selection, concurrent queue access and buffer ownership");
}
