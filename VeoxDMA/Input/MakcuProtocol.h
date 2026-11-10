#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <string_view>
#include "CtlState.h"

namespace Makcu {
// MAK_API canonical IDs, independent of Xbox/Sony report layouts.
inline uint16_t ControllerButton(unsigned id) {
    constexpr uint16_t masks[] = {CTL_BTN_A, CTL_BTN_B, CTL_BTN_X, CTL_BTN_Y,
        CTL_BTN_DPAD_UP, CTL_BTN_DPAD_DOWN, CTL_BTN_DPAD_LEFT, CTL_BTN_DPAD_RIGHT,
        CTL_BTN_LB, CTL_BTN_RB, 0, 0, 0, 0, 0, 0, CTL_BTN_L3, CTL_BTN_R3,
        CTL_BTN_BACK, CTL_BTN_START, CTL_BTN_GUIDE};
    return id < std::size(masks) ? masks[id] : 0;
}
inline uint16_t ControllerButtons(uint32_t bits) {
    uint16_t result = 0;
    for (unsigned id = 0; id <= 20; ++id) if (bits & (1u << id)) result |= ControllerButton(id);
    return result;
}
inline uint8_t Trigger(uint16_t value) { return uint8_t((value * 255u + 511u) / 1023u); }
struct ControllerState {
    uint32_t low = 0, high = 0;
    uint16_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
inline uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] | unsigned(p[1]) << 8); }
inline uint32_t Read32(const uint8_t* p) { return uint32_t(Read16(p)) | uint32_t(Read16(p + 2)) << 16; }
inline bool DecodePhysical(const uint8_t* p, size_t n, ControllerState& state) {
    if (n != 32 || Read16(p + 8) > 1023 || Read16(p + 10) > 1023) return false;
    state = {Read32(p), Read32(p + 4), Read16(p + 8), Read16(p + 10),
        int16_t(Read16(p + 12)), int16_t(Read16(p + 14)), int16_t(Read16(p + 16)), int16_t(Read16(p + 18))};
    return true;
}
inline bool ParsePhysical(std::string_view text, ControllerState& state) {
    while (text.substr(0, 4) == ">>> ") text.remove_prefix(4);
    constexpr std::string_view prefix = "km.controller_physical(";
    if (text.substr(0, prefix.size()) == prefix) {
        if (text.empty() || text.back() != ')') return false;
        text.remove_prefix(prefix.size()); text.remove_suffix(1);
    }
    int64_t values[12]{};
    for (unsigned i = 0; i < 12; ++i) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), values[i]);
        if (parsed.ec != std::errc{}) return false;
        text.remove_prefix(size_t(parsed.ptr - text.data()));
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        if (i < 11) { if (text.empty() || text.front() != ',') return false; text.remove_prefix(1); }
    }
    if (!text.empty()) return false;
    for (unsigned i : {0u, 1u, 8u, 9u}) if (values[i] < 0 || values[i] > UINT32_MAX) return false;
    for (unsigned i : {2u, 3u}) if (values[i] < 0 || values[i] > 1023) return false;
    for (unsigned i = 4; i < 8; ++i) if (values[i] < -32768 || values[i] > 32767) return false;
    for (unsigned i : {10u, 11u}) if (values[i] < 0 || values[i] > 65535) return false;
    state = {uint32_t(values[0]), uint32_t(values[1]), uint16_t(values[2]), uint16_t(values[3]),
        int16_t(values[4]), int16_t(values[5]), int16_t(values[6]), int16_t(values[7])};
    return true;
}
// Firmware frames are "km." followed by a binary five-button mask. Text
// replies share the stream; never interpret an echoed command as a button.
struct Parser {
    std::array<char, 256> line{};
    size_t size = 0;
    bool overflow = false;
    uint8_t mask = 0;
    ControllerState controller;
    std::array<uint8_t, 512> payload{};
    unsigned frameStage = 0, frameLength = 0, frameSize = 0;
    uint8_t opcode = 0;
    enum class Event { None, Buttons, Version, Frame, Physical };
    Event Feed(uint8_t byte) {
        // Binary records and text replies can be adjacent or fragmented across reads.
        if (frameStage) {
            if (frameStage == 1) {
                if (byte == 0xAD) { frameStage = 2; return Event::None; }
                frameStage = byte == 0xDE ? 1 : 0; return Event::None;
            }
            if (frameStage == 2) { frameLength = byte; frameStage = 3; return Event::None; }
            if (frameStage == 3) { frameLength |= unsigned(byte) << 8; frameStage = 4; return Event::None; }
            if (frameStage == 4) {
                opcode = byte; frameSize = 0;
                frameStage = frameLength ? 5 : 0;
                return frameLength ? Event::None : Event::Frame;
            }
            if (frameSize < payload.size()) payload[frameSize] = byte;
            if (++frameSize == frameLength) {
                frameStage = 0;
                return frameLength <= payload.size() ? Event::Frame : Event::None;
            }
            return Event::None;
        }
        if (byte == 0xDE) { frameStage = 1; return Event::None; }
        if (!overflow && size == 3 && line[0] == 'k' && line[1] == 'm'
            && line[2] == '.' && byte < 32) {
            mask = byte; size = 0;
            return Event::Buttons;
        }
        if (byte == '\r' || byte == '\n') {
            line[size] = 0;
            const bool version = !overflow && std::strstr(line.data(), "km.MAKCU") != nullptr;
            const bool physical = !overflow && ParsePhysical({line.data(), size}, controller);
            size = 0; overflow = false;
            return version ? Event::Version : physical ? Event::Physical : Event::None;
        }
        if (size + 1 < line.size()) {
            line[size++] = static_cast<char>(byte);
            if (size == 4 && std::memcmp(line.data(), ">>> ", 4) == 0) size = 0;
        }
        else overflow = true;
        return Event::None;
    }
};
inline std::array<uint8_t, 9> BaudCommand(uint32_t baud) {
    return {0xDE, 0xAD, 5, 0, 0xA5, static_cast<uint8_t>(baud),
        static_cast<uint8_t>(baud >> 8), static_cast<uint8_t>(baud >> 16), static_cast<uint8_t>(baud >> 24)};
}
}
