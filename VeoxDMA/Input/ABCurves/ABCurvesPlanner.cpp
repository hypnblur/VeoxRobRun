#include "pch.h"
#include "ABCurvesPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Base::ABCurves {

namespace {

constexpr double kVelocityScale = 1.1982087601807259;
constexpr double kPositionScale = 81.73350722609987;
constexpr int kHistorySamples = 640;
constexpr int kWarmHistorySamples = 160;
constexpr int kCommitSamples = 32;
constexpr int kSampleUs = 1000;

constexpr double kEventScales[22] = {
    10.0, 0.1, 0.1, 0.1, 1.0, 0.1, 0.1, 0.1, 0.1, 0.1, 0.01, 10.0,
    128.0, 1.0, 1.0, 1.0, 128.0, 10.0, 10.0, 0.1, 1.0, 10.0
};

constexpr int kCoarseIndex[70] = {
    15, 31, 47, 63, 79, 95, 111, 127, 143, 159,
    175, 191, 207, 223, 239, 255, 271, 287, 303, 319,
    335, 351, 367, 383, 399, 415, 431, 447, 463, 479,
    483, 487, 491, 495, 499, 503, 507, 511, 515, 519,
    523, 527, 531, 535, 539, 543, 547, 551, 555, 559,
    563, 567, 571, 575, 579, 583, 587, 591, 595, 599,
    603, 607, 611, 615, 619, 623, 627, 631, 635, 639
};

struct ContinuousWeights {
    // Motor weights (transposed where helpful for contiguous inner loops)
    float motorInputW_T[9 * 96]{};          // (9, 96)
    float motorInputB[96]{};                // (96)
    float motorIhW_T[96 * 288]{};           // (96, 288)
    float motorIhB[288]{};                  // (288)
    float motorHhW_T[96 * 288]{};           // (96, 288)
    float motorHhB[288]{};                  // (288)
    float motorTrunkW[96 * 158]{};          // (96, 158)
    float motorTrunkB[96]{};                // (96)
    float motorOutW[672 * 96]{};            // (672, 96)
    float motorOutB[672]{};                 // (672)

    double velocityBasis[128 * 21]{};       // (128, 21)
    double carryVelocity[128]{};            // (128)
    double meanBasis[16 * 21]{};            // (16, 21)
    double meanCarry[16]{};                 // (16)

    // Selection (choice) weights
    float unaryLatentW[64 * 96]{};          // (64, 96)
    float unaryGeomW_T[16 * 64]{};          // (16, 64)
    float unaryB[64]{};                     // (64)
    float unaryOutW[64]{};                  // (64)
    float unaryOutB = 0.f;                  // (1)
    float ctxW[16 * 96]{};                  // (16, 96)
    float ctxB[16]{};                       // (16)
    float pairCtxW[32 * 16]{};              // (32, 16)
    float pairGeomW_T[20 * 32]{};           // (20, 32)
    float pairB[32]{};                      // (32)
    float pairOutW[32]{};                   // (32)
    float pairOutB = 0.f;                   // (1)

    // Hazard weights
    float hazard0W[64 * 32]{};              // (64, 32)
    float hazard0B[64]{};                   // (64)
    float hazard2W[32 * 64]{};              // (32, 64)
    float hazard2B[32]{};                   // (32)
    float hazard4W[2 * 32]{};               // (2, 32)
    float hazard4B[2]{};                    // (2)

    // Brake weights
    float brakeEnc0W[96 * 32]{};            // (96, 32)
    float brakeEnc0B[96]{};                 // (96)
    float brakeEnc2W[96 * 96]{};            // (96, 96)
    float brakeEnc2B[96]{};                 // (96)
    float brakeW[176 * 96]{};               // (176, 96)
    float brakeB[176]{};                    // (176)
    float freqW[16 * 96]{};                 // (16, 96)
    float freqB[16]{};                      // (16)
};

struct SharedWeightsState {
    std::mutex mutex;
    std::unique_ptr<ContinuousWeights> weights;
    bool attempted = false;
};

SharedWeightsState& SharedWeights()
{
    static SharedWeightsState s;
    return s;
}

template <typename T>
bool ReadRaw(std::ifstream& f, T* dst, std::size_t count)
{
    f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(count * sizeof(T)));
    return f.good();
}

void Transpose(const float* src, float* dst, int rows, int cols)
{
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            dst[c * rows + r] = src[r * cols + c];
}

bool LoadWeightsFromFile(const std::filesystem::path& path, ContinuousWeights& w)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return false;

    char magic[4]{};
    std::uint32_t version = 0;
    std::uint32_t tensorCount = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&tensorCount), 4);
    if (!f.good() || std::memcmp(magic, "ABCC", 4) != 0 || version != 1 || tensorCount != 36)
        return false;

    std::vector<float> tmp;
    auto readTransposed = [&](float* dst, int rows, int cols) -> bool {
        tmp.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
        if (!ReadRaw(f, tmp.data(), tmp.size()))
            return false;
        Transpose(tmp.data(), dst, rows, cols);
        return true;
    };

    if (!readTransposed(w.motorInputW_T, 96, 9)) return false;
    if (!ReadRaw(f, w.motorInputB, 96)) return false;
    if (!readTransposed(w.motorIhW_T, 288, 96)) return false;
    if (!ReadRaw(f, w.motorIhB, 288)) return false;
    if (!readTransposed(w.motorHhW_T, 288, 96)) return false;
    if (!ReadRaw(f, w.motorHhB, 288)) return false;
    if (!ReadRaw(f, w.motorTrunkW, 96 * 158)) return false;
    if (!ReadRaw(f, w.motorTrunkB, 96)) return false;
    if (!ReadRaw(f, w.motorOutW, 672 * 96)) return false;
    if (!ReadRaw(f, w.motorOutB, 672)) return false;
    if (!ReadRaw(f, w.velocityBasis, 128 * 21)) return false;
    if (!ReadRaw(f, w.carryVelocity, 128)) return false;

    // motor.selection.unary.0.weight (64, 112) -> split [:96] and [96:]^T
    {
        tmp.resize(64 * 112);
        if (!ReadRaw(f, tmp.data(), tmp.size())) return false;
        for (int r = 0; r < 64; ++r) {
            std::memcpy(&w.unaryLatentW[r * 96], &tmp[r * 112], 96 * sizeof(float));
            for (int c = 0; c < 16; ++c)
                w.unaryGeomW_T[c * 64 + r] = tmp[r * 112 + 96 + c];
        }
    }
    if (!ReadRaw(f, w.unaryB, 64)) return false;
    if (!ReadRaw(f, w.unaryOutW, 64)) return false;
    if (!ReadRaw(f, &w.unaryOutB, 1)) return false;
    if (!ReadRaw(f, w.ctxW, 16 * 96)) return false;
    if (!ReadRaw(f, w.ctxB, 16)) return false;

    // motor.selection.pair.0.weight (32, 36) -> split [:20]^T and [20:]
    {
        tmp.resize(32 * 36);
        if (!ReadRaw(f, tmp.data(), tmp.size())) return false;
        for (int r = 0; r < 32; ++r) {
            for (int c = 0; c < 20; ++c)
                w.pairGeomW_T[c * 32 + r] = tmp[r * 36 + c];
            std::memcpy(&w.pairCtxW[r * 16], &tmp[r * 36 + 20], 16 * sizeof(float));
        }
    }
    if (!ReadRaw(f, w.pairB, 32)) return false;
    if (!ReadRaw(f, w.pairOutW, 32)) return false;
    if (!ReadRaw(f, &w.pairOutB, 1)) return false;

    if (!ReadRaw(f, w.hazard0W, 64 * 32)) return false;
    if (!ReadRaw(f, w.hazard0B, 64)) return false;
    if (!ReadRaw(f, w.hazard2W, 32 * 64)) return false;
    if (!ReadRaw(f, w.hazard2B, 32)) return false;
    if (!ReadRaw(f, w.hazard4W, 2 * 32)) return false;
    if (!ReadRaw(f, w.hazard4B, 2)) return false;

    if (!ReadRaw(f, w.brakeEnc0W, 96 * 32)) return false;
    if (!ReadRaw(f, w.brakeEnc0B, 96)) return false;
    if (!ReadRaw(f, w.brakeEnc2W, 96 * 96)) return false;
    if (!ReadRaw(f, w.brakeEnc2B, 96)) return false;
    if (!ReadRaw(f, w.brakeW, 176 * 96)) return false;
    if (!ReadRaw(f, w.brakeB, 176)) return false;
    if (!ReadRaw(f, w.freqW, 16 * 96)) return false;
    if (!ReadRaw(f, w.freqB, 16)) return false;

    // Precompute meanBasis (16, 21) and meanCarry (16) from first 64 rows in 4 ms groups
    for (int g = 0; g < 16; ++g) {
        double csum = 0.0;
        for (int j = 0; j < 21; ++j) {
            double bsum = 0.0;
            for (int k = 0; k < 4; ++k)
                bsum += w.velocityBasis[(g * 4 + k) * 21 + j];
            w.meanBasis[g * 21 + j] = bsum * 0.25;
        }
        for (int k = 0; k < 4; ++k)
            csum += w.carryVelocity[g * 4 + k];
        w.meanCarry[g] = csum * 0.25;
    }

    return true;
}

std::vector<std::filesystem::path> CandidateWeightPaths(const char* explicitPath)
{
    std::vector<std::filesystem::path> paths;
    if (explicitPath && explicitPath[0] != '\0')
        paths.emplace_back(explicitPath);

#ifdef _WIN32
    wchar_t exeBuf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
        std::filesystem::path dir = std::filesystem::path(exeBuf).parent_path();
        paths.push_back(dir / L"abcurves_continuous.bin");
        paths.push_back(dir / L"tools" / L"abcurves_continuous.bin");
        paths.push_back(dir / L"models" / L"abcurves_continuous.bin");
        paths.push_back(dir.parent_path() / L"tools" / L"abcurves_continuous.bin");
    }
#endif
    paths.emplace_back("abcurves_continuous.bin");
    paths.emplace_back("tools/abcurves_continuous.bin");
    paths.emplace_back("models/abcurves_continuous.bin");
    return paths;
}

// --- Math helpers matching neural_native.py ---------------------------------

inline float TanhPade9(float value)
{
    const float x = std::clamp(value, -7.0f, 7.0f);
    const float z = x * x;
    const float num = x * (34459425.0f + z * (4729725.0f + z * (135135.0f + z * (990.0f + z))));
    const float den = 34459425.0f + z * (16216200.0f + z * (945945.0f + z * (13860.0f + 45.0f * z)));
    return std::clamp(num / den, -1.0f, 1.0f);
}

inline float SigmoidPade9(float value)
{
    return 0.5f + 0.5f * TanhPade9(0.5f * value);
}

inline float SigmoidExact(float value)
{
    const float z = std::exp(-std::fabs(value));
    const float den = 1.0f + z;
    return value >= 0.0f ? (1.0f / den) : (z / den);
}

inline float SiluExact(float value)
{
    return value * SigmoidExact(value);
}

// --- RandomStream matching rng.py -------------------------------------------

struct RandomStream {
    std::uint64_t seed = 7;
    std::mt19937 rng{ 7u };

    void Reset(std::uint64_t s)
    {
        seed = s;
        rng.seed(static_cast<std::uint32_t>(s & 0xFFFFFFFFull));
    }

    float Uniform()
    {
        const std::uint32_t w = rng();
        return static_cast<float>(w & 0xFFFFFFu) * (1.0f / 16777216.0f);
    }

    float Exponential()
    {
        const std::uint64_t hi = static_cast<std::uint64_t>(rng());
        const std::uint64_t lo = static_cast<std::uint64_t>(rng());
        const std::uint64_t bits = (hi << 32) | lo;
        const double u = static_cast<double>(bits & ((1ull << 53) - 1ull)) * (1.0 / 9007199254740992.0);
        return static_cast<float>(-std::log1p(-u));
    }

    int Categorical16(const float probs[16])
    {
        int best = 0;
        float bestScore = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 16; ++i) {
            const float e = Exponential();
            const float score = (e > 0.0f) ? (probs[i] / e)
                : (probs[i] > 0.0f ? std::numeric_limits<float>::infinity() : 0.0f);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    void DiscardCategorical16()
    {
        for (int i = 0; i < 32; ++i)
            (void)rng();
    }
};

void Softmax16(const float logits[16], float probs[16])
{
    float m = logits[0];
    for (int i = 1; i < 16; ++i)
        if (logits[i] > m) m = logits[i];
    float sum = 0.0f;
    for (int i = 0; i < 16; ++i) {
        probs[i] = std::exp(logits[i] - m);
        sum += probs[i];
    }
    const float inv = (sum > 0.0f && std::isfinite(sum)) ? (1.0f / sum) : (1.0f / 16.0f);
    for (int i = 0; i < 16; ++i)
        probs[i] *= inv;
}

} // namespace

// --- ContinuousStream::Impl -------------------------------------------------

struct ContinuousStream::Impl {
    const ContinuousWeights* w = nullptr;

    // Mirrored 640-sample ring buffers (length 1280 for contiguous slice [cursor .. cursor+640])
    double history[2 * kHistorySamples][2]{};
    double targetHist[2 * kHistorySamples][2]{};
    std::int64_t available[2 * kHistorySamples]{};
    bool valid[2 * kHistorySamples]{};
    bool motionKnown[2 * kHistorySamples]{};

    double position[2]{};
    double latestTarget[2]{};
    bool hasTarget = false;
    std::int64_t receiptUs = 0;
    std::int64_t nowUs = 0;
    double fracUs = 0.0;
    int cursor = 0;
    bool observed = false;

    double actions[kCommitSamples][2]{};
    int pendingIndex = kCommitSamples;

    // Planner policy state
    RandomStream motorRng;
    RandomStream selectorRng;
    RandomStream brakeRng;
    int mode = -1; // -1 = uninit, 0 = move, 1 = brake, 2 = hold
    double age = 160.0;
    double duration = 64.0;
    double brakeV[2]{};
    double brakeA[2]{};
    double brakeCoeff[5][2]{};
    double holdGoal[2]{};
    double holdPos[2]{};
    double holdError = 0.0;
    bool changed = false;
    double innovAt = std::numeric_limits<double>::quiet_NaN();
    double resumeIntegral = 0.0;
    double stopIntegral = 0.0;
    float resumeBudget = 1.0f;
    float stopBudget = 1.0f;

    bool previousValid = false;
    bool hasOrigin = false;
    float previousHead[16][2]{};
    double origin[2]{};

    // Scratch workspaces (no per-step heap allocations)
    float coarse[70][9]{};
    float fine[54]{};
    float dynamics[8]{};
    float tokens[70][96]{};
    float projected[70][288]{};
    float hidden[96]{};
    float recurrent[288]{};
    float combined[158]{};
    float encoded[96]{};
    float motorCoeffs[16][21][2]{};
    float headsGeom[16][16][2]{};
    float unaryGeom[16][16]{};
    float pairFeat[16][20]{};
    float choiceLogits[16]{};

    // Scratch feature buffers (kept in heap Impl rather than on worker stack)
    bool featKnown[kHistorySamples]{};
    bool featKnownCtx[kHistorySamples]{};
    double featMot[kHistorySamples][2]{};
    double featGoal[kHistorySamples][2]{};
    double featTv[kHistorySamples][2]{};
    double featRel[kHistorySamples][2]{};

    void Reset(std::uint64_t seed)
    {
        motorRng.Reset(seed);
        selectorRng.Reset(seed ^ 0x5397A1ull);
        brakeRng.Reset(seed ^ 0x1763B4ull);

        mode = -1;
        previousValid = false;
        hasOrigin = false;
        origin[0] = origin[1] = 0.0;

        position[0] = position[1] = 0.0;
        latestTarget[0] = latestTarget[1] = 0.0;
        hasTarget = false;
        receiptUs = 0;
        nowUs = 0;
        fracUs = 0.0;
        cursor = 0;
        observed = false;
        pendingIndex = kCommitSamples;

        std::memset(history, 0, sizeof(history));
        std::memset(targetHist, 0, sizeof(targetHist));
        std::memset(available, 0, sizeof(available));
        std::memset(valid, 0, sizeof(valid));
        std::memset(motionKnown, 0, sizeof(motionKnown));
        for (int i = kHistorySamples - kWarmHistorySamples; i < kHistorySamples; ++i) {
            motionKnown[i] = true;
            motionKnown[i + kHistorySamples] = true;
        }
    }

    void SetTarget(double tx, double ty)
    {
        latestTarget[0] = tx;
        latestTarget[1] = ty;
        hasTarget = true;
        receiptUs = nowUs;
        const int at = (cursor + kHistorySamples - 1) % kHistorySamples;
        targetHist[at][0] = targetHist[at + kHistorySamples][0] = tx;
        targetHist[at][1] = targetHist[at + kHistorySamples][1] = ty;
        available[at] = available[at + kHistorySamples] = receiptUs;
        valid[at] = valid[at + kHistorySamples] = true;
    }

    void AppendSample(double dx, double dy)
    {
        const int at = cursor;
        const int mirror = at + kHistorySamples;
        history[at][0] = history[mirror][0] = dx;
        history[at][1] = history[mirror][1] = dy;
        if (hasTarget) {
            targetHist[at][0] = targetHist[mirror][0] = latestTarget[0];
            targetHist[at][1] = targetHist[mirror][1] = latestTarget[1];
            available[at] = available[mirror] = receiptUs;
            valid[at] = valid[mirror] = true;
        } else {
            targetHist[at][0] = targetHist[mirror][0] = 0.0;
            targetHist[at][1] = targetHist[mirror][1] = 0.0;
            available[at] = available[mirror] = 0;
            valid[at] = valid[mirror] = false;
        }
        motionKnown[at] = motionKnown[mirror] = true;
        cursor = (at + 1) % kHistorySamples;
    }

    void InitObservation()
    {
        const int olderCount = kHistorySamples - kWarmHistorySamples;
        for (int k = 0; k < olderCount; ++k) {
            const int idx = (cursor + k) % kHistorySamples;
            const int m = idx + kHistorySamples;
            history[idx][0] = history[idx][1] = 0.0;
            history[m][0] = history[m][1] = 0.0;
            targetHist[idx][0] = targetHist[idx][1] = 0.0;
            targetHist[m][0] = targetHist[m][1] = 0.0;
            available[idx] = available[m] = 0;
            valid[idx] = valid[m] = false;
            motionKnown[idx] = motionKnown[m] = false;
        }
        observed = true;
    }

    void ComputeMotorFeatures(int s)
    {
        bool* known = featKnown;
        auto& mot = featMot;
        auto& goal = featGoal;
        auto& tv = featTv;
        auto& rel = featRel;
        std::memset(tv, 0, sizeof(featTv));
        std::memset(rel, 0, sizeof(featRel));

        double sumMotX = 0.0, sumMotY = 0.0;
        for (int i = 0; i < kHistorySamples; ++i) {
            const std::int64_t slotTime = nowUs + static_cast<std::int64_t>(i - 639) * 1000LL;
            known[i] = valid[s + i] && (available[s + i] <= slotTime);
            const bool mk = motionKnown[s + i];
            mot[i][0] = mk ? history[s + i][0] : 0.0;
            mot[i][1] = mk ? history[s + i][1] : 0.0;
            goal[i][0] = known[i] ? targetHist[s + i][0] : 0.0;
            goal[i][1] = known[i] ? targetHist[s + i][1] : 0.0;
            sumMotX += mot[i][0];
            sumMotY += mot[i][1];
        }
        for (int i = 32; i < kHistorySamples; ++i) {
            if (known[i] && known[i - 32]) {
                tv[i][0] = (goal[i][0] - goal[i - 32][0]) / 32.0;
                tv[i][1] = (goal[i][1] - goal[i - 32][1]) / 32.0;
            }
        }
        double cumX = 0.0, cumY = 0.0;
        for (int i = 0; i < kHistorySamples; ++i) {
            cumX += mot[i][0];
            cumY += mot[i][1];
            if (known[i] && motionKnown[s + i]) {
                const double px = position[0] + cumX - sumMotX;
                const double py = position[1] + cumY - sumMotY;
                rel[i][0] = std::asinh((goal[i][0] - px) / kPositionScale);
                rel[i][1] = std::asinh((goal[i][1] - py) / kPositionScale);
            }
        }

        // Coarse tokens [0..29]: 16 ms bins over [0..480)
        for (int b = 0; b < 30; ++b) {
            double mx = 0.0, my = 0.0, mkSum = 0.0;
            const int base = b * 16;
            for (int k = 0; k < 16; ++k) {
                mx += mot[base + k][0];
                my += mot[base + k][1];
                mkSum += motionKnown[s + base + k] ? 1.0 : 0.0;
            }
            const int idx = kCoarseIndex[b];
            coarse[b][0] = static_cast<float>((mx / 16.0) / kVelocityScale);
            coarse[b][1] = static_cast<float>((my / 16.0) / kVelocityScale);
            coarse[b][2] = static_cast<float>(rel[idx][0]);
            coarse[b][3] = static_cast<float>(rel[idx][1]);
            coarse[b][4] = static_cast<float>(tv[idx][0] / kVelocityScale);
            coarse[b][5] = static_cast<float>(tv[idx][1] / kVelocityScale);
            coarse[b][6] = known[idx] ? 1.0f : 0.0f;
            coarse[b][7] = static_cast<float>(mkSum / 16.0);
            coarse[b][8] = 1.0f;
        }
        // Coarse tokens [30..69]: 4 ms bins over [480..640)
        for (int b = 0; b < 40; ++b) {
            double mx = 0.0, my = 0.0, mkSum = 0.0;
            const int base = 480 + b * 4;
            for (int k = 0; k < 4; ++k) {
                mx += mot[base + k][0];
                my += mot[base + k][1];
                mkSum += motionKnown[s + base + k] ? 1.0 : 0.0;
            }
            const int row = 30 + b;
            const int idx = kCoarseIndex[row];
            coarse[row][0] = static_cast<float>((mx / 4.0) / kVelocityScale);
            coarse[row][1] = static_cast<float>((my / 4.0) / kVelocityScale);
            coarse[row][2] = static_cast<float>(rel[idx][0]);
            coarse[row][3] = static_cast<float>(rel[idx][1]);
            coarse[row][4] = static_cast<float>(tv[idx][0] / kVelocityScale);
            coarse[row][5] = static_cast<float>(tv[idx][1] / kVelocityScale);
            coarse[row][6] = known[idx] ? 1.0f : 0.0f;
            coarse[row][7] = static_cast<float>(mkSum / 4.0);
            coarse[row][8] = 0.25f;
        }

        // Fine [54]
        double mkTotal = 0.0;
        for (int i = 0; i < kHistorySamples; ++i)
            mkTotal += motionKnown[s + i] ? 1.0 : 0.0;

        for (int k = 0; k < 16; ++k) {
            const int src = kHistorySamples - 16 + k;
            fine[2 * k] = static_cast<float>(mot[src][0] / kVelocityScale);
            fine[2 * k + 1] = static_cast<float>(mot[src][1] / kVelocityScale);
            fine[32 + k] = motionKnown[s + src] ? 1.0f : 0.0f;
        }
        if (known[639]) {
            fine[48] = static_cast<float>(std::asinh((goal[639][0] - position[0]) / kPositionScale));
            fine[49] = static_cast<float>(std::asinh((goal[639][1] - position[1]) / kPositionScale));
        } else {
            fine[48] = fine[49] = 0.0f;
        }
        fine[50] = static_cast<float>(tv[639][0] / kVelocityScale);
        fine[51] = static_cast<float>(tv[639][1] / kVelocityScale);
        fine[52] = known[639] ? 1.0f : 0.0f;
        fine[53] = static_cast<float>(mkTotal / static_cast<double>(kHistorySamples));

        // Dynamics [8]
        const float curX = fine[30];
        const float curY = fine[31];
        float m1x = 0.f, m1y = 0.f, m0x = 0.f, m0y = 0.f;
        for (int k = 12; k < 16; ++k) { m1x += fine[2 * k]; m1y += fine[2 * k + 1]; }
        for (int k = 8; k < 12; ++k) { m0x += fine[2 * k]; m0y += fine[2 * k + 1]; }
        float accX = (m1x * 0.25f - m0x * 0.25f) * static_cast<float>(kVelocityScale) * 0.25f;
        float accY = (m1y * 0.25f - m0y * 0.25f) * static_cast<float>(kVelocityScale) * 0.25f;
        bool allKnown8 = true;
        for (int k = 40; k < 48; ++k)
            if (fine[k] <= 0.5f) { allKnown8 = false; break; }
        if (!allKnown8) { accX = 0.f; accY = 0.f; }
        accX /= 0.25f;
        accY /= 0.25f;

        const double ex = std::sinh(static_cast<double>(fine[48])) * kPositionScale;
        const double ey = std::sinh(static_cast<double>(fine[49])) * kPositionScale;
        const double invNorm = 1.0 / std::sqrt(ex * ex + ey * ey + 100.0);
        float dirX = static_cast<float>(ex * invNorm);
        float dirY = static_cast<float>(ey * invNorm);
        if (fine[52] <= 0.5f) { dirX = 0.f; dirY = 0.f; }
        const float normX = -dirY;
        const float normY = dirX;
        const float relX = curX - fine[50];
        const float relY = curY - fine[51];

        dynamics[0] = curX;
        dynamics[1] = curY;
        dynamics[2] = accX;
        dynamics[3] = accY;
        dynamics[4] = relX * dirX + relY * dirY;
        dynamics[5] = relX * normX + relY * normY;
        dynamics[6] = accX * dirX + accY * dirY;
        dynamics[7] = accX * normX + accY * normY;
    }

    void RunMotorNet()
    {
        // 1. coarse (70, 9) @ motorInputW_T (9, 96) + bias -> SiLU (Pade9)
        for (int r = 0; r < 70; ++r) {
            float* out = tokens[r];
            for (int c = 0; c < 96; ++c) out[c] = w->motorInputB[c];
            for (int k = 0; k < 9; ++k) {
                const float v = coarse[r][k];
                const float* wRow = &w->motorInputW_T[k * 96];
                for (int c = 0; c < 96; ++c)
                    out[c] += v * wRow[c];
            }
            for (int c = 0; c < 96; ++c)
                out[c] = out[c] * SigmoidPade9(out[c]);
        }

        // 2. tokens (70, 96) @ motorIhW_T (96, 288) + bias -> projected (70, 288)
        for (int r = 0; r < 70; ++r) {
            float* out = projected[r];
            std::memcpy(out, w->motorIhB, 288 * sizeof(float));
            const float* tok = tokens[r];
            for (int k = 0; k < 96; ++k) {
                const float v = tok[k];
                const float* wRow = &w->motorIhW_T[k * 288];
                for (int c = 0; c < 288; ++c)
                    out[c] += v * wRow[c];
            }
        }

        // 3. GRU recurrence over 70 steps
        std::memset(hidden, 0, sizeof(hidden));
        for (int r = 0; r < 70; ++r) {
            std::memcpy(recurrent, w->motorHhB, 288 * sizeof(float));
            for (int k = 0; k < 96; ++k) {
                const float v = hidden[k];
                const float* wRow = &w->motorHhW_T[k * 288];
                for (int c = 0; c < 288; ++c)
                    recurrent[c] += v * wRow[c];
            }
            const float* proj = projected[r];
            for (int c = 0; c < 96; ++c) {
                const float reset = SigmoidPade9(proj[c] + recurrent[c]);
                const float update = SigmoidPade9(proj[96 + c] + recurrent[96 + c]);
                const float cand = TanhPade9(proj[192 + c] + reset * recurrent[192 + c]);
                hidden[c] = cand + (hidden[c] - cand) * update;
            }
        }

        // 4. Trunk: concat(hidden[96], fine[54], dynamics[8]) -> 96 -> SiLU (Pade9)
        std::memcpy(combined, hidden, 96 * sizeof(float));
        std::memcpy(combined + 96, fine, 54 * sizeof(float));
        std::memcpy(combined + 150, dynamics, 8 * sizeof(float));

        for (int r = 0; r < 96; ++r) {
            float acc = w->motorTrunkB[r];
            const float* wRow = &w->motorTrunkW[r * 158];
            for (int k = 0; k < 158; ++k)
                acc += wRow[k] * combined[k];
            encoded[r] = acc * SigmoidPade9(acc);
        }

        // 5. Output: encoded (96) -> 672 -> motorCoeffs[16][21][2]
        float* coeffFlat = &motorCoeffs[0][0][0];
        for (int r = 0; r < 672; ++r) {
            float acc = w->motorOutB[r];
            const float* wRow = &w->motorOutW[r * 96];
            for (int k = 0; k < 96; ++k)
                acc += wRow[k] * encoded[k];
            coeffFlat[r] = acc;
        }
    }

    void RunChoiceNet()
    {
        float shared[64];
        for (int r = 0; r < 64; ++r) {
            float acc = w->unaryB[r];
            const float* wRow = &w->unaryLatentW[r * 96];
            for (int k = 0; k < 96; ++k)
                acc += wRow[k] * encoded[k];
            shared[r] = acc;
        }

        for (int h = 0; h < 16; ++h) {
            float u[64];
            std::memcpy(u, shared, sizeof(u));
            const float* g = unaryGeom[h];
            for (int k = 0; k < 16; ++k) {
                const float v = g[k];
                const float* wRow = &w->unaryGeomW_T[k * 64];
                for (int c = 0; c < 64; ++c)
                    u[c] += v * wRow[c];
            }
            float logit = w->unaryOutB;
            for (int c = 0; c < 64; ++c)
                logit += SiluExact(u[c]) * w->unaryOutW[c];
            choiceLogits[h] = logit;
        }

        if (previousValid) {
            float ctx[16];
            for (int r = 0; r < 16; ++r) {
                float acc = w->ctxB[r];
                const float* wRow = &w->ctxW[r * 96];
                for (int k = 0; k < 96; ++k)
                    acc += wRow[k] * encoded[k];
                ctx[r] = SiluExact(acc);
            }
            float sharedPair[32];
            for (int r = 0; r < 32; ++r) {
                float acc = w->pairB[r];
                const float* wRow = &w->pairCtxW[r * 16];
                for (int k = 0; k < 16; ++k)
                    acc += wRow[k] * ctx[k];
                sharedPair[r] = acc;
            }
            for (int h = 0; h < 16; ++h) {
                float p[32];
                std::memcpy(p, sharedPair, sizeof(p));
                const float* pf = pairFeat[h];
                for (int k = 0; k < 20; ++k) {
                    const float v = pf[k];
                    const float* wRow = &w->pairGeomW_T[k * 32];
                    for (int c = 0; c < 32; ++c)
                        p[c] += v * wRow[c];
                }
                float tr = w->pairOutB;
                for (int c = 0; c < 32; ++c)
                    tr += SiluExact(p[c]) * w->pairOutW[c];
                choiceLogits[h] += tr;
            }
        }
    }

    void RunHazardNet(const float ctx[32], float out[2]) const
    {
        float h1[64];
        for (int r = 0; r < 64; ++r) {
            float acc = w->hazard0B[r];
            const float* wRow = &w->hazard0W[r * 32];
            for (int k = 0; k < 32; ++k) acc += wRow[k] * ctx[k];
            h1[r] = SiluExact(acc);
        }
        float h2[32];
        for (int r = 0; r < 32; ++r) {
            float acc = w->hazard2B[r];
            const float* wRow = &w->hazard2W[r * 64];
            for (int k = 0; k < 64; ++k) acc += wRow[k] * h1[k];
            h2[r] = SiluExact(acc);
        }
        for (int r = 0; r < 2; ++r) {
            float acc = w->hazard4B[r];
            const float* wRow = &w->hazard4W[r * 32];
            for (int k = 0; k < 32; ++k) acc += wRow[k] * h2[k];
            out[r] = acc;
        }
    }

    void RunBrakeNet(const float ctx[32], float durOut[16], float coeffOut[16][5][2], float freqOut[16]) const
    {
        float h1[96];
        for (int r = 0; r < 96; ++r) {
            float acc = w->brakeEnc0B[r];
            const float* wRow = &w->brakeEnc0W[r * 32];
            for (int k = 0; k < 32; ++k) acc += wRow[k] * ctx[k];
            h1[r] = SiluExact(acc);
        }
        float enc[96];
        for (int r = 0; r < 96; ++r) {
            float acc = w->brakeEnc2B[r];
            const float* wRow = &w->brakeEnc2W[r * 96];
            for (int k = 0; k < 96; ++k) acc += wRow[k] * h1[k];
            enc[r] = SiluExact(acc);
        }
        float params[176];
        for (int r = 0; r < 176; ++r) {
            float acc = w->brakeB[r];
            const float* wRow = &w->brakeW[r * 96];
            for (int k = 0; k < 96; ++k) acc += wRow[k] * enc[k];
            params[r] = acc;
        }
        for (int h = 0; h < 16; ++h) {
            const float logDur = std::clamp(params[11 * h], 1.3862943611198906f, 5.2574953720277815f);
            durOut[h] = std::exp(logDur);
            for (int c = 0; c < 10; ++c)
                coeffOut[h][c / 2][c % 2] = 64.0f * params[11 * h + 1 + c];
        }
        for (int h = 0; h < 16; ++h) {
            float acc = w->freqB[h];
            const float* wRow = &w->freqW[h * 96];
            for (int k = 0; k < 96; ++k) acc += wRow[k] * enc[k];
            freqOut[h] = acc;
        }
    }

    void ComputeEventFeatures(int s, double innovationAge, int curMode,
        float rawOut[22], float xOut[32], double basisOut[2][2])
    {
        bool* knownCtx = featKnownCtx;
        bool* known = featKnown;
        for (int i = 0; i < kHistorySamples; ++i) {
            const std::int64_t slotTime = nowUs + static_cast<std::int64_t>(i - 639) * 1000LL;
            knownCtx[i] = valid[s + i] && (available[s + i] <= slotTime);
            known[i] = knownCtx[i] && std::isfinite(targetHist[s + i][0]) && std::isfinite(targetHist[s + i][1]);
        }
        const bool latest = known[639];
        const double gx = latest ? targetHist[s + 639][0] : position[0];
        const double gy = latest ? targetHist[s + 639][1] : position[1];
        const double errX = gx - position[0];
        const double errY = gy - position[1];
        const double dist = std::hypot(errX, errY);
        const double invDist = 1.0 / (std::max)(dist, 1e-8);
        const double axX = errX * invDist;
        const double axY = errY * invDist;

        const double v1x = history[s + 639][0];
        const double v1y = history[s + 639][1];
        double v8x = 0.0, v8y = 0.0, v16x = 0.0, v16y = 0.0, v32x = 0.0, v32y = 0.0;
        for (int i = 632; i < 640; ++i) { v8x += history[s + i][0]; v8y += history[s + i][1]; }
        for (int i = 624; i < 632; ++i) { v16x += history[s + i][0]; v16y += history[s + i][1]; }
        for (int i = 608; i < 640; ++i) { v32x += history[s + i][0]; v32y += history[s + i][1]; }
        const double sum32x = v32x, sum32y = v32y;
        v8x /= 8.0; v8y /= 8.0;
        v16x /= 8.0; v16y /= 8.0;
        v32x /= 32.0; v32y /= 32.0;

        const double s1 = std::hypot(v1x, v1y);
        const double s8 = std::hypot(v8x, v8y);
        const double s32 = std::hypot(v32x, v32y);

        auto tvAt = [&](int k, double& outX, double& outY) {
            if (latest && known[639 - k]) {
                outX = (gx - targetHist[s + 639 - k][0]) / static_cast<double>(k);
                outY = (gy - targetHist[s + 639 - k][1]) / static_cast<double>(k);
            } else {
                outX = outY = 0.0;
            }
        };
        double tv8x, tv8y, tv32x, tv32y, tv128x, tv128y;
        tvAt(8, tv8x, tv8y);
        tvAt(32, tv32x, tv32y);
        tvAt(128, tv128x, tv128y);
        const double ts8 = std::hypot(tv8x, tv8y);
        const double ts32 = std::hypot(tv32x, tv32y);
        const double ts128 = std::hypot(tv128x, tv128y);

        const double prevPosX = position[0] - sum32x;
        const double prevPosY = position[1] - sum32y;
        const double growth = (latest && known[607])
            ? (dist - std::hypot(targetHist[s + 607][0] - prevPosX, targetHist[s + 607][1] - prevPosY)) / 32.0
            : 0.0;
        const double accX = (v8x - v16x) / 8.0;
        const double accY = (v8y - v16y) / 8.0;

        int lastChangeIdx = -1;
        double excursion = 0.0;
        double known128Sum = 0.0;
        for (int k = 0; k < 128; ++k) {
            const int idx = 512 + k;
            const double ex = std::hypot(targetHist[s + idx][0] - gx, targetHist[s + idx][1] - gy);
            if (known[idx]) {
                known128Sum += 1.0;
                if (ex > excursion) excursion = ex;
            }
            if (ex > 1e-8 || !known[idx])
                lastChangeIdx = k;
        }
        const double targetAge = (lastChangeIdx >= 0) ? static_cast<double>(127 - lastChangeIdx) : 128.0;
        double mkMean = 0.0;
        for (int i = 0; i < kHistorySamples; ++i)
            mkMean += motionKnown[s + i] ? 1.0 : 0.0;
        mkMean /= static_cast<double>(kHistorySamples);

        rawOut[0] = static_cast<float>(dist);
        rawOut[1] = static_cast<float>(s1);
        rawOut[2] = static_cast<float>(s8);
        rawOut[3] = static_cast<float>(s32);
        rawOut[4] = static_cast<float>(s8 / (std::max)(s32, 0.005));
        rawOut[5] = static_cast<float>(ts32);
        rawOut[6] = static_cast<float>(ts128);
        rawOut[7] = static_cast<float>(v8x * axX + v8y * axY);
        rawOut[8] = static_cast<float>(std::fabs(v8x * axY - v8y * axX));
        rawOut[9] = static_cast<float>(growth);
        rawOut[10] = static_cast<float>(-(accX * v8x + accY * v8y) / (std::max)(s8, 0.005));
        rawOut[11] = static_cast<float>(excursion);
        rawOut[12] = static_cast<float>(targetAge);
        rawOut[13] = static_cast<float>(known128Sum / 128.0);
        rawOut[14] = latest ? 1.0f : 0.0f;
        rawOut[15] = static_cast<float>(mkMean);
        rawOut[16] = static_cast<float>((std::min)(age, 4096.0));
        rawOut[17] = static_cast<float>(std::hypot(gx - holdGoal[0], gy - holdGoal[1]));
        rawOut[18] = static_cast<float>(std::hypot(position[0] - holdPos[0], position[1] - holdPos[1]));
        rawOut[19] = static_cast<float>(ts8);
        rawOut[20] = static_cast<float>((v8x * tv32x + v8y * tv32y) / (std::max)(s8 * ts32, 1e-8));
        rawOut[21] = static_cast<float>(holdError);
        if (curMode == 0) {
            rawOut[17] = rawOut[18] = rawOut[21] = 0.0f;
        }

        float ctxRaw[22];
        std::memcpy(ctxRaw, rawOut, sizeof(ctxRaw));
        ctxRaw[16] = (std::min)(ctxRaw[16], 256.0f);

        const double cgx = knownCtx[639] ? targetHist[s + 639][0] : position[0];
        const double cgy = knownCtx[639] ? targetHist[s + 639][1] : position[1];
        const double ceX = cgx - position[0];
        const double ceY = cgy - position[1];
        const double cDist = std::hypot(ceX, ceY);
        double dirX = 1.0, dirY = 0.0;
        if (cDist > 1e-6) {
            dirX = ceX / (std::max)(cDist, 1e-9);
            dirY = ceY / (std::max)(cDist, 1e-9);
        } else if (s8 > 1e-6) {
            dirX = v8x / (std::max)(s8, 1e-9);
            dirY = v8y / (std::max)(s8, 1e-9);
        }
        basisOut[0][0] = dirX;  basisOut[0][1] = -dirY;
        basisOut[1][0] = dirY;  basisOut[1][1] = dirX;

        auto projBasis = [&](double vx, double vy, double& bx, double& by) {
            bx = vx * basisOut[0][0] + vy * basisOut[1][0];
            by = vx * basisOut[0][1] + vy * basisOut[1][1];
        };

        double ctv32x = 0.0, ctv32y = 0.0, ctv128x = 0.0, ctv128y = 0.0;
        if (knownCtx[639] && knownCtx[607]) {
            ctv32x = (cgx - targetHist[s + 607][0]) / 32.0;
            ctv32y = (cgy - targetHist[s + 607][1]) / 32.0;
        }
        if (knownCtx[639] && knownCtx[511]) {
            ctv128x = (cgx - targetHist[s + 511][0]) / 128.0;
            ctv128y = (cgy - targetHist[s + 511][1]) / 128.0;
        }

        double extra[10];
        projBasis(v1x, v1y, extra[0], extra[1]);
        projBasis(accX * 8.0, accY * 8.0, extra[2], extra[3]);
        projBasis(ctv32x, ctv32y, extra[4], extra[5]);
        projBasis(ctv128x, ctv128y, extra[6], extra[7]);
        double v8bx = 0.0, v8by = 0.0;
        projBasis(v8x, v8y, v8bx, v8by);
        extra[8] = v8by;
        extra[9] = (std::min)(2048.0, innovationAge) / 128.0;

        for (int i = 0; i < 22; ++i)
            xOut[i] = static_cast<float>(std::asinh(static_cast<double>(ctxRaw[i]) / kEventScales[i]));
        for (int i = 0; i < 10; ++i)
            xOut[22 + i] = static_cast<float>(std::asinh(extra[i]));
    }

    void RunMotorStep(int s, double outAction[kCommitSamples][2])
    {
        ComputeMotorFeatures(s);
        RunMotorNet();

        const double inVx = history[s + 639][0];
        const double inVy = history[s + 639][1];
        for (int h = 0; h < 16; ++h) {
            for (int g = 0; g < 16; ++g) {
                double vx = w->meanCarry[g] * inVx;
                double vy = w->meanCarry[g] * inVy;
                const double* bRow = &w->meanBasis[g * 21];
                for (int j = 0; j < 21; ++j) {
                    vx += bRow[j] * static_cast<double>(motorCoeffs[h][j][0]);
                    vy += bRow[j] * static_cast<double>(motorCoeffs[h][j][1]);
                }
                headsGeom[h][g][0] = static_cast<float>(vx);
                headsGeom[h][g][1] = static_cast<float>(vy);
            }
            for (int g = 0; g < 8; ++g) {
                unaryGeom[h][2 * g] = std::asinh(headsGeom[h][g][0]);
                unaryGeom[h][2 * g + 1] = std::asinh(headsGeom[h][g][1]);
            }
        }

        if (previousValid) {
            const float actX = hasOrigin ? static_cast<float>(position[0] - origin[0]) : 0.f;
            const float actY = hasOrigin ? static_cast<float>(position[1] - origin[1]) : 0.f;
            float prevAllX = 0.f, prevAllY = 0.f, prevFirst8X = 0.f, prevFirst8Y = 0.f;
            for (int g = 0; g < 16; ++g) {
                prevAllX += previousHead[g][0];
                prevAllY += previousHead[g][1];
                if (g < 8) {
                    prevFirst8X += previousHead[g][0];
                    prevFirst8Y += previousHead[g][1];
                }
            }
            const float p18 = std::asinh((prevFirst8X * 4.0f - actX) / 32.0f);
            const float p19 = std::asinh((prevFirst8Y * 4.0f - actY) / 32.0f);

            for (int h = 0; h < 16; ++h) {
                float headFirst8X = 0.f, headFirst8Y = 0.f;
                for (int g = 0; g < 8; ++g) {
                    headFirst8X += headsGeom[h][g][0];
                    headFirst8Y += headsGeom[h][g][1];
                    pairFeat[h][2 * g] = std::asinh(previousHead[8 + g][0] - headsGeom[h][g][0]);
                    pairFeat[h][2 * g + 1] = std::asinh(previousHead[8 + g][1] - headsGeom[h][g][1]);
                }
                pairFeat[h][16] = std::asinh(((prevAllX * 4.0f - actX) - headFirst8X * 4.0f) / 32.0f);
                pairFeat[h][17] = std::asinh(((prevAllY * 4.0f - actY) - headFirst8Y * 4.0f) / 32.0f);
                pairFeat[h][18] = p18;
                pairFeat[h][19] = p19;
            }
        } else {
            std::memset(pairFeat, 0, sizeof(pairFeat));
        }

        RunChoiceNet();
        float probs[16];
        Softmax16(choiceLogits, probs);
        const int chosen = motorRng.Categorical16(probs);
        std::memcpy(previousHead, headsGeom[chosen], sizeof(previousHead));
        origin[0] = position[0];
        origin[1] = position[1];
        hasOrigin = true;
        previousValid = true;

        // Decode selected head for first 32 ms
        for (int t = 0; t < kCommitSamples; ++t) {
            double vx = w->carryVelocity[t] * inVx;
            double vy = w->carryVelocity[t] * inVy;
            const double* bRow = &w->velocityBasis[t * 21];
            for (int j = 0; j < 21; ++j) {
                vx += bRow[j] * static_cast<double>(motorCoeffs[chosen][j][0]);
                vy += bRow[j] * static_cast<double>(motorCoeffs[chosen][j][1]);
            }
            outAction[t][0] = vx;
            outAction[t][1] = vy;
        }
    }

    void PlanNext32Ms()
    {
        if (!observed)
            InitObservation();

        const int s = cursor;
        const bool known = valid[s + 639] && (available[s + 639] <= nowUs);
        const double goalX = known ? targetHist[s + 639][0] : position[0];
        const double goalY = known ? targetHist[s + 639][1] : position[1];

        if (mode < 0) {
            double maxRecent = 0.0;
            for (int i = 608; i < 640; ++i) {
                maxRecent = (std::max)(maxRecent, std::fabs(history[s + i][0]));
                maxRecent = (std::max)(maxRecent, std::fabs(history[s + i][1]));
            }
            mode = (maxRecent <= 1e-12) ? 2 : 0;
            age = 160.0;
            duration = 64.0;
            brakeV[0] = brakeV[1] = 0.0;
            brakeA[0] = brakeA[1] = 0.0;
            std::memset(brakeCoeff, 0, sizeof(brakeCoeff));
            holdGoal[0] = goalX;
            holdGoal[1] = goalY;
            holdPos[0] = position[0];
            holdPos[1] = position[1];
            holdError = std::hypot(goalX - position[0], goalY - position[1]);
            changed = holdError > 1e-7;
            innovAt = changed ? (static_cast<double>(nowUs) * 0.001) : std::numeric_limits<double>::quiet_NaN();
            resumeIntegral = 0.0;
            stopIntegral = 0.0;
            resumeBudget = -std::log((std::max)(selectorRng.Uniform(), 1e-8f));
            stopBudget = -std::log((std::max)(brakeRng.Uniform(), 1e-8f));
        }

        const int oldMode = mode;
        if (oldMode != 0) {
            previousValid = false;
            int firstChange = -1;
            for (int k = 0; k < 32; ++k) {
                const int idx = 608 + k;
                const std::int64_t slotTime = nowUs + static_cast<std::int64_t>(k - 31) * 1000LL;
                if (valid[s + idx] && available[s + idx] <= slotTime) {
                    if (std::hypot(targetHist[s + idx][0] - holdGoal[0], targetHist[s + idx][1] - holdGoal[1]) > 1e-7) {
                        firstChange = k;
                        break;
                    }
                }
            }
            if (firstChange >= 0) {
                changed = true;
                if (std::isnan(innovAt))
                    innovAt = static_cast<double>(nowUs) * 0.001 - 31.0 + static_cast<double>(firstChange);
            }
        }

        const double innovationAge = std::isnan(innovAt)
            ? 0.0
            : (std::max)(0.0, static_cast<double>(nowUs) * 0.001 - innovAt);

        float raw[22];
        float ctx[32];
        double basis[2][2];
        ComputeEventFeatures(s, innovationAge, oldMode, raw, ctx, basis);

        const bool innovation = (oldMode == 2) &&
            (changed || std::hypot(position[0] - holdPos[0], position[1] - holdPos[1]) > 1e-5 || raw[0] > 10.0f);
        const bool needHazard = (oldMode == 0) || innovation;

        float probs[2]{ 0.0f, 0.0f };
        if (needHazard) {
            float hazard[2];
            RunHazardNet(ctx, hazard);
            for (int i = 0; i < 2; ++i) {
                const float h = std::clamp(hazard[i], -40.0f, 40.0f);
                probs[i] = 1.0f / (1.0f + std::exp(-h));
            }
        }

        const float draw0 = selectorRng.Uniform();
        const float draw1 = selectorRng.Uniform();
        bool brakeSampled = false;

        if (oldMode == 0) {
            if (known)
                stopIntegral += -std::log((std::max)(1e-8, 1.0 - static_cast<double>(probs[0])));
            if (stopIntegral >= static_cast<double>(stopBudget) && known) {
                float dur16[16];
                float coeff16[16][5][2];
                float freq16[16];
                float freqProbs[16];
                RunBrakeNet(ctx, dur16, coeff16, freq16);
                Softmax16(freq16, freqProbs);
                const int bHead = brakeRng.Categorical16(freqProbs);
                brakeSampled = true;

                mode = 1;
                age = 0.0;
                brakeV[0] = history[s + 639][0];
                brakeV[1] = history[s + 639][1];
                double m1x = 0.0, m1y = 0.0, m0x = 0.0, m0y = 0.0;
                for (int i = 632; i < 640; ++i) { m1x += history[s + i][0]; m1y += history[s + i][1]; }
                for (int i = 624; i < 632; ++i) { m0x += history[s + i][0]; m0y += history[s + i][1]; }
                brakeA[0] = (m1x / 8.0 - m0x / 8.0) / 8.0;
                brakeA[1] = (m1y / 8.0 - m0y / 8.0) / 8.0;
                duration = static_cast<double>(dur16[bHead]);
                for (int i = 0; i < 5; ++i) {
                    const double c0 = static_cast<double>(coeff16[bHead][i][0]);
                    const double c1 = static_cast<double>(coeff16[bHead][i][1]);
                    brakeCoeff[i][0] = c0 * basis[0][0] + c1 * basis[0][1];
                    brakeCoeff[i][1] = c0 * basis[1][0] + c1 * basis[1][1];
                }
                holdGoal[0] = goalX;
                holdGoal[1] = goalY;
                holdError = static_cast<double>(raw[0]);
                changed = false;
                innovAt = std::numeric_limits<double>::quiet_NaN();
            }
        } else if (oldMode == 2) {
            if (innovation && known)
                resumeIntegral += -std::log((std::max)(1e-8, 1.0 - static_cast<double>(probs[1])));
            if (innovation && resumeIntegral >= static_cast<double>(resumeBudget) && known) {
                mode = 0;
                age = 0.0;
                changed = false;
                innovAt = std::numeric_limits<double>::quiet_NaN();
                stopIntegral = 0.0;
                stopBudget = static_cast<float>(-std::log((std::max)(static_cast<double>(draw0), 1e-8)));
            }
        }

        if (!brakeSampled)
            brakeRng.DiscardCategorical16();

        if (mode == 0) {
            RunMotorStep(s, actions);
            age += 32.0;
        } else {
            motorRng.DiscardCategorical16();
            if (mode == 1) {
                double curve[33][2];
                for (int t = 0; t <= 32; ++t) {
                    const double u = std::clamp((age + static_cast<double>(t)) / (std::max)(duration, 1e-6), 0.0, 1.0);
                    const double z = 2.0 * u - 1.0;
                    const double u2 = u * u;
                    const double u3 = u2 * u;
                    const double om = 1.0 - u;
                    const double om3 = om * om * om;
                    const double env = 64.0 * u3 * om3;
                    const double r0 = env;
                    const double r1 = env * z;
                    const double r2 = env * (3.0 * z * z - 1.0) * 0.5;
                    const double r3 = env * (5.0 * z * z * z - 3.0 * z) * 0.5;
                    const double carry = u - 6.0 * u3 + 8.0 * u3 * u - 3.0 * u3 * u2;
                    const double accB = 0.5 * u2 * om3;
                    const double goalB = 10.0 * u3 - 15.0 * u3 * u + 6.0 * u3 * u2;
                    for (int ax = 0; ax < 2; ++ax) {
                        curve[t][ax] = carry * duration * brakeV[ax]
                            + accB * duration * duration * brakeA[ax]
                            + goalB * brakeCoeff[0][ax]
                            + r0 * brakeCoeff[1][ax]
                            + r1 * brakeCoeff[2][ax]
                            + r2 * brakeCoeff[3][ax]
                            + r3 * brakeCoeff[4][ax];
                    }
                }
                double sumX = 0.0, sumY = 0.0;
                for (int t = 0; t < 32; ++t) {
                    actions[t][0] = curve[t + 1][0] - curve[t][0];
                    actions[t][1] = curve[t + 1][1] - curve[t][1];
                    sumX += actions[t][0];
                    sumY += actions[t][1];
                }
                age += 32.0;
                if (age >= duration) {
                    mode = 2;
                    age = (std::max)(0.0, age - duration);
                    holdPos[0] = position[0] + sumX;
                    holdPos[1] = position[1] + sumY;
                    holdGoal[0] = goalX;
                    holdGoal[1] = goalY;
                    holdError = std::hypot(goalX - holdPos[0], goalY - holdPos[1]);
                    changed = false;
                    innovAt = std::numeric_limits<double>::quiet_NaN();
                    resumeIntegral = 0.0;
                    resumeBudget = static_cast<float>(-std::log((std::max)(static_cast<double>(draw1), 1e-8)));
                }
            } else {
                std::memset(actions, 0, sizeof(actions));
                age += 32.0;
            }
            previousValid = false;
        }

        // Guard against any nonfinite output
        for (int t = 0; t < kCommitSamples; ++t) {
            if (!std::isfinite(actions[t][0]) || !std::isfinite(actions[t][1]) ||
                std::fabs(actions[t][0]) > 1e5 || std::fabs(actions[t][1]) > 1e5) {
                actions[t][0] = actions[t][1] = 0.0;
            }
        }
        pendingIndex = 0;
    }

    Delta StepTowardError(double errX, double errY, double dtSec)
    {
        Delta out{};
        if (!w || !std::isfinite(errX) || !std::isfinite(errY) || !std::isfinite(dtSec) || dtSec <= 0.0)
            return out;

        SetTarget(position[0] + errX, position[1] + errY);

        const double dtUs = std::clamp(dtSec * 1.0e6, 50.0, 50000.0);
        fracUs += dtUs;
        const int steps = static_cast<int>(std::floor(fracUs / static_cast<double>(kSampleUs)));
        if (steps <= 0)
            return out;
        fracUs -= static_cast<double>(steps) * static_cast<double>(kSampleUs);

        const int runSteps = (std::min)(steps, 32);
        for (int i = 0; i < runSteps; ++i) {
            if (pendingIndex >= kCommitSamples)
                PlanNext32Ms();
            const double dx = actions[pendingIndex][0];
            const double dy = actions[pendingIndex][1];
            ++pendingIndex;
            nowUs += kSampleUs;
            position[0] += dx;
            position[1] += dy;
            AppendSample(dx, dy);
            out.dx += dx;
            out.dy += dy;
        }
        return out;
    }
};

bool ContinuousStream::EnsureLoaded(const char* explicitPath)
{
    try {
        auto& state = SharedWeights();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.weights)
            return true;
        if (state.attempted && (!explicitPath || explicitPath[0] == '\0'))
            return false;
        state.attempted = true;

        for (const auto& candidate : CandidateWeightPaths(explicitPath)) {
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec) || ec)
                continue;
            auto loaded = std::make_unique<ContinuousWeights>();
            if (LoadWeightsFromFile(candidate, *loaded)) {
                state.weights = std::move(loaded);
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool ContinuousStream::Available()
{
    return EnsureLoaded();
}

ContinuousStream::ContinuousStream() = default;
ContinuousStream::~ContinuousStream() = default;
ContinuousStream::ContinuousStream(ContinuousStream&&) noexcept = default;
ContinuousStream& ContinuousStream::operator=(ContinuousStream&&) noexcept = default;

void ContinuousStream::Reset(std::uint64_t seed)
{
    if (!EnsureLoaded())
        return;
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    if (!impl_->w) {
        std::lock_guard<std::mutex> lock(SharedWeights().mutex);
        impl_->w = SharedWeights().weights.get();
    }
    impl_->Reset(seed ? seed : 7ull);
}

ContinuousStream::Delta ContinuousStream::StepTowardError(double errX, double errY, double dtSec)
{
    if (!EnsureLoaded())
        return {};
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
        impl_->Reset(7ull);
    }
    if (!impl_->w) {
        std::lock_guard<std::mutex> lock(SharedWeights().mutex);
        impl_->w = SharedWeights().weights.get();
    }
    return impl_->StepTowardError(errX, errY, dtSec);
}

} // namespace Base::ABCurves
