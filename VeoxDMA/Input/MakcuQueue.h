#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace Makcu {

struct Command {
    std::array<char, 256> text{};
    size_t size = 0;
    uint64_t created = 0;
    bool test = false;
};

struct Outbox {
    static constexpr int kCapacity = 16;
    Command ring[kCapacity]{};
    int head = 0, tail = 0;
    int count = 0;
    bool hasMovement = false;
    bool hasRecoilMovement = false;
    Command movement{};
    Command recoilMovement{};

    bool Push(const Command& c, bool isMovement, bool isRecoil) {
        if (isRecoil) {
            recoilMovement = c;
            hasRecoilMovement = true;
            return true;
        }
        if (isMovement) {
            movement = c;
            hasMovement = true;
            return true;
        }
        if (count >= kCapacity)
            return false;
        ring[tail] = c;
        tail = (tail + 1) % kCapacity;
        ++count;
        return true;
    }

    bool Pop(Command& out, uint64_t now) {
        (void)now;
        if (hasRecoilMovement) {
            out = recoilMovement;
            hasRecoilMovement = false;
            return true;
        }
        if (hasMovement) {
            out = movement;
            hasMovement = false;
            return true;
        }
        if (count <= 0)
            return false;
        out = ring[head];
        head = (head + 1) % kCapacity;
        --count;
        return true;
    }

    void Clear() {
        head = tail = count = 0;
        hasMovement = false;
        hasRecoilMovement = false;
    }
};

} // namespace Makcu
