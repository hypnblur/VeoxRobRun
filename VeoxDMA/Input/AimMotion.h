#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Base::AimMotion {
struct Vec2 { float x = 0, y = 0; };
struct Settings {
    int smoothX = 4, smoothY = 4;
    int pathVariation = 0, speedVariation = 0;
    int pathRandom = 0;
    float curveBend1 = 0.f, curveBend2 = 0.f;
    float curveT1 = 0.33f, curveT2 = 0.67f;
    std::uint64_t variationSalt = 0;
    float countsPerPixelX = 1, countsPerPixelY = 1;
    float deadzonePixels = 0;
    float maxCountsPerSecond = 8000;
    // When > 0, use a millisecond response time instead of the slider curve.
    // The hardware path sets this from the same Smoothness mapping the memory
    // write path uses, so both aim modes converge at the same rate.
    float responseMs = 0;
};
inline float Alpha(int slider, float dt) {
    if (slider <= 1) return 1;
    const float fraction = 1.f / std::pow(float(std::clamp(slider, 2, 100)), 0.85f);
    return -std::expm1(std::log1p(-fraction) * std::clamp(dt, 0.f, 0.05f) * 125.f);
}
inline bool Finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline float ResponseGain(float responseMs, float dt) {
    if (!(responseMs > 0.f) || !std::isfinite(responseMs)) return -1.f;
    const float seconds = (std::max)(0.f, dt);
    return 1.f - std::exp(-seconds * 1000.f / responseMs);
}
inline Vec2 Step(Vec2 error, const Settings& s, float dt) {
    if (!Finite(error) || !std::isfinite(dt) || dt <= 0
        || !std::isfinite(s.countsPerPixelX) || !std::isfinite(s.countsPerPixelY)
        || s.countsPerPixelX <= 0 || s.countsPerPixelY <= 0
        || std::hypot(error.x, error.y) <= s.deadzonePixels) return {};
    const float response = ResponseGain(s.responseMs, dt);
    Vec2 out{error.x * (response >= 0.f ? response : Alpha(s.smoothX, dt)) * s.countsPerPixelX,
             error.y * (response >= 0.f ? response : Alpha(s.smoothY, dt)) * s.countsPerPixelY};
    const float length = std::hypot(out.x, out.y);
    const float limit = (std::max)(0.f, s.maxCountsPerSecond) * (std::min)(dt, 0.05f);
    if (length > limit && length > 0) { out.x *= limit / length; out.y *= limit / length; }
    return out;
}
inline constexpr float kPathBendScale = 0.40f;
inline float PathDeviation(float initialError, int percent) {
    const float strength = std::sqrt(std::clamp(percent, 0, 100) * .01f);
    return (std::max)(0.f, initialError) * kPathBendScale * strength;
}
struct Variation {
    std::uint64_t target = 0;
    int pathPercent = -1, pathRandom = -1;
    float bend1 = 0, bend2 = 0;
    float initialError = 0, lateral1 = 0, lateral2 = 0;
    float speedPhase1 = 0, speedPhase2 = 0;
    float microCenter[3]{}, microHeight[3]{}, microHalf[3]{};
    std::uint64_t salt = 0;
    Vec2 normal{};
    void Reset() { *this = {}; }
    static float Random(std::uint64_t& state) {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return float((state * 0x2545F4914F6CDD1Dull) >> 40) / float(1u << 24);
    }
    void Begin(std::uint64_t key, Vec2 error, int percent,
        float userBend1 = 0.f, float userBend2 = 0.f, int randomPercent = 0,
        std::uint64_t variationSalt = 0) {
        Reset(); target = key; pathPercent = percent; salt = variationSalt;
        bend1 = std::clamp(userBend1, -1.f, 1.f);
        bend2 = std::clamp(userBend2, -1.f, 1.f);
        pathRandom = std::clamp(randomPercent, 0, 100);
        initialError = std::hypot(error.x, error.y);
        if (!(initialError > 0)) return;
        normal = {-error.y / initialError, error.x / initialError};
        std::uint64_t seed = key ^ (salt * 0x9E3779B97F4A7C15ull);
        const float deviation = PathDeviation(initialError, percent);
        lateral1 = bend1 * deviation;
        lateral2 = bend2 * deviation;
        const float micro = pathRandom * .01f * .22f;
        for (int i = 0; i < 3; ++i) {
            float n = Random(seed) * 2.f - 1.f;
            if (std::fabs(n) < .4f) n = n < 0.f ? -.4f : .4f;
            const float half = .08f + Random(seed) * .04f;
            const float center = (i + .35f + (Random(seed) - .5f) * .35f) / 3.f;
            microHalf[i] = half;
            microCenter[i] = std::clamp(center, half + .02f, 1.f - half - .02f);
            microHeight[i] = n * micro;
        }
        constexpr float twoPi = 6.28318530718f;
        speedPhase1 = Random(seed) * twoPi;
        speedPhase2 = Random(seed) * twoPi;
    }
};
inline float SpeedScale(float t, float phase1, float phase2, int speedPercent) {
    t = std::clamp(t, 0.f, 1.f);
    constexpr float pi = 3.14159265359f;
    const float wave = (std::sin(pi * t + phase1) + .5f * std::sin(2.f * pi * t + phase2)) / 1.5f;
    const float speedStrength = std::clamp(speedPercent, 0, 100) * .007f;
    return std::clamp(1.f + wave * speedStrength, 1.f - speedStrength, 1.f + speedStrength);
}
inline float SmoothResponse(int slider) {
    if (slider <= 1) return 1.f;
    return 1.f / std::pow(float(std::clamp(slider, 2, 100)), 0.85f);
}
inline float SmoothingPace(int smooth, float t, float phase1, float phase2, int speedPercent) {
    if (speedPercent <= 0) return 1.f;
    const float pace = SpeedScale(t, phase1, phase2, speedPercent);
    const float cap = 1.f / SmoothResponse((std::max)(smooth, 1));
    return std::clamp(pace, 0.f, cap);
}
inline float TravelSeconds(int smooth) {
    smooth = std::clamp(smooth, 1, 50);
    const float u = float(smooth - 1) / 49.f;
    return 0.38f + 1.9f * std::pow(u, 0.55f);
}
inline constexpr float kCurveTMin = 0.12f;
inline constexpr float kCurveTMax = 0.88f;
inline constexpr float kCurveTGap = 0.16f;
inline void OrderCurveKnots(float& t1, float& t2) {
    if (!std::isfinite(t1)) t1 = 0.33f;
    if (!std::isfinite(t2)) t2 = 0.67f;
    t1 = std::clamp(t1, kCurveTMin, kCurveTMax - kCurveTGap);
    t2 = std::clamp(t2, kCurveTMin + kCurveTGap, kCurveTMax);
    if (t2 < t1 + kCurveTGap) t2 = (std::min)(kCurveTMax, t1 + kCurveTGap);
}
inline float PathBend(float t, float y1, float y2, float t1 = 0.33f, float t2 = 0.67f) {
    t = std::clamp(t, 0.f, 1.f);
    if (!(t > 0.f) || !(t < 1.f)) return 0.f;
    if (!std::isfinite(y1)) y1 = 0.f;
    if (!std::isfinite(y2)) y2 = 0.f;
    OrderCurveKnots(t1, t2);
    const float s1 = t1 * (1.f - t1), s2 = t2 * (1.f - t2);
    const float u1 = y1 / s1, u2 = y2 / s2;
    const float slope = (u2 - u1) / (t2 - t1);
    const float base = u1 - slope * t1;
    return t * (1.f - t) * (base + slope * t);
}
inline float MicroBend(float t, const Variation& v) {
    if (v.pathRandom <= 0) return 0.f;
    t = std::clamp(t, 0.f, 1.f);
    float sum = 0.f;
    for (int i = 0; i < 3; ++i) {
        const float half = v.microHalf[i];
        if (!(half > 1e-4f)) continue;
        const float u = (t - (v.microCenter[i] - half)) / (2.f * half);
        if (u <= 0.f || u >= 1.f) continue;
        sum += v.microHeight[i] * (4.f * u * (1.f - u));
    }
    return sum;
}
inline float ApproachLateral(float lateral, float length, float initial) {
    if (!(length > 0.f) || !std::isfinite(lateral)) return 0.f;
    const float remain = initial > 1.f ? length / initial : 1.f;
    lateral *= std::clamp(remain / 0.15f, 0.f, 1.f);
    const float cap = length * 0.7f;
    if (lateral > cap) return cap;
    if (lateral < -cap) return -cap;
    return lateral;
}
inline Vec2 ApplyVariation(Variation& v, std::uint64_t target, Vec2 error,
    Vec2 nominal, const Settings& settings) {
    const float length = std::hypot(error.x, error.y);
    const float step = std::hypot(nominal.x, nominal.y);
    if (!(length > 0) || !(step > 0)) return {};
    if (settings.pathVariation <= 0 && settings.speedVariation <= 0) { v.Reset(); return nominal; }
    if (v.target != target || v.pathPercent != settings.pathVariation
        || v.pathRandom != settings.pathRandom
        || std::fabs(v.bend1 - settings.curveBend1) > 1e-3f
        || std::fabs(v.bend2 - settings.curveBend2) > 1e-3f
        || v.salt != settings.variationSalt || !(v.initialError > 0))
        v.Begin(target, error, settings.pathVariation,
            settings.curveBend1, settings.curveBend2, settings.pathRandom, settings.variationSalt);
    const float t = std::clamp(1.f - length / v.initialError, 0.f, 1.f);
    const float curveScale = PathDeviation(v.initialError, v.pathPercent);
    const float lateral = ApproachLateral(
        PathBend(t, v.lateral1, v.lateral2, settings.curveT1, settings.curveT2)
            + MicroBend(t, v) * curveScale, length, v.initialError);
    const float paceX = SmoothingPace(settings.smoothX, t, v.speedPhase1, v.speedPhase2, settings.speedVariation);
    const float paceY = SmoothingPace(settings.smoothY, t, v.speedPhase1, v.speedPhase2, settings.speedVariation);
    const Vec2 paced{nominal.x * paceX, nominal.y * paceY};
    if (std::fabs(lateral) <= 1e-3f) return paced;
    float nx = -error.y / length, ny = error.x / length;
    if (v.normal.x * nx + v.normal.y * ny < 0.f) { nx = -nx; ny = -ny; }
    Vec2 steered{error.x + nx * lateral, error.y + ny * lateral};
    const float steeredLength = std::hypot(steered.x, steered.y);
    const float pacedStep = std::hypot(paced.x, paced.y);
    if (!(steeredLength > 0.f) || !(pacedStep > 0.f)) return paced;
    const float variedStep = (std::min)(steeredLength, pacedStep);
    return {steered.x * variedStep / steeredLength, steered.y * variedStep / steeredLength};
}
struct Quantizer {
    Vec2 residual{};
    void Reset() { residual = {}; }
    static int Axis(float value, float& remainder, int limit) {
        if (!std::isfinite(value)) { remainder = 0; return 0; }
        const float total = std::clamp(value + remainder, -float(limit), float(limit));
        const int n = int(total);
        remainder = total - float(n);
        return n;
    }
    Vec2 Round(Vec2 value, int limit = 127) {
        return {float(Axis(value.x, residual.x, limit)), float(Axis(value.y, residual.y, limit))};
    }
};
struct Controller {
    std::uint64_t target = 0, observation = 0;
    Vec2 remaining{};
    Variation variation;
    Quantizer quantizer;
    void Reset() { *this = {}; }
    void Observe(std::uint64_t key, std::uint64_t sample, Vec2 error) {
        if (target != key) { Reset(); target = key; }
        if (observation != sample) { observation = sample; remaining = error; }
    }
    Vec2 Next(const Settings& settings, float dt) {
        if (std::hypot(remaining.x, remaining.y) <= settings.deadzonePixels) {
            quantizer.Reset(); return {};
        }
        return quantizer.Round(ApplyVariation(
            variation, target, remaining, Step(remaining, settings, dt), settings));
    }
    void Written(Vec2 counts, const Settings& settings) {
        const auto consume = [](float error, float count, float gain) {
            const float next = error - count / (std::max)(0.001f, gain);
            return error * next <= 0 ? 0.f : next;
        };
        remaining.x = consume(remaining.x, counts.x, settings.countsPerPixelX);
        remaining.y = consume(remaining.y, counts.y, settings.countsPerPixelY);
    }
};
}
