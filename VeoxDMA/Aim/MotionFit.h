#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace AimMotionFit {

inline double LeadMilliseconds(double responseMs, double observationAgeMs,
                               int sampleCount, int minHistory = 3,
                               double maxLeadMs = 120.0)
{
    if (!std::isfinite(responseMs) || !std::isfinite(observationAgeMs) ||
        maxLeadMs <= 0.0) return 0.0;
    const double confidence = std::clamp(
        (static_cast<double>(sampleCount - minHistory + 1) / 6.0), 0.5, 1.0);
    return std::clamp(std::max(0.0, responseMs) * confidence +
        std::clamp(observationAgeMs, 0.0, 80.0), 4.0, maxLeadMs);
}

struct Derivatives {
    double vx = 0.0, vy = 0.0, vz = 0.0;
    double ax = 0.0, ay = 0.0, az = 0.0;
};

template <typename Samples>
bool FitLatest(const Samples& samples, Derivatives& out, double* interceptOut = nullptr)
{
    const std::size_t count = samples.size();
    if (count < 3) return false;

    const std::uint64_t firstMs = samples.front().tickMs;
    const std::uint64_t lastMs = samples.back().tickMs;
    if (lastMs <= firstMs) return false;
    const double spanMs = static_cast<double>(lastMs - firstMs);
    const double spanSeconds = spanMs * 0.001;

    double system[3][6]{};
    std::uint64_t previousMs = 0;
    bool first = true;
    for (const auto& sample : samples) {
        if (!first && sample.tickMs <= previousMs) return false;
        first = false;
        previousMs = sample.tickMs;
        if (sample.tickMs > lastMs) return false;

        const double s = -static_cast<double>(lastMs - sample.tickMs) / spanMs;
        const double basis[3] = {1.0, s, s * s};
        const double coordinates[3] = {sample.position.x, sample.position.y, sample.position.z};
        if (!std::isfinite(coordinates[0]) || !std::isfinite(coordinates[1]) ||
            !std::isfinite(coordinates[2])) return false;

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j)
                system[i][j] += basis[i] * basis[j];
            for (int axis = 0; axis < 3; ++axis)
                system[i][3 + axis] += basis[i] * coordinates[axis];
        }
    }

    for (int col = 0; col < 3; ++col) {
        int pivotRow = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::abs(system[r][col]) > std::abs(system[pivotRow][col]))
                pivotRow = r;
        if (std::abs(system[pivotRow][col]) < 1e-10) return false;
        if (pivotRow != col)
            for (int j = 0; j < 6; ++j)
                std::swap(system[col][j], system[pivotRow][j]);
        const double pivot = system[col][col];
        for (int j = col; j < 6; ++j) system[col][j] /= pivot;
        for (int row = 0; row < 3; ++row) {
            if (row == col) continue;
            const double factor = system[row][col];
            for (int j = col; j < 6; ++j)
                system[row][j] -= factor * system[col][j];
        }
    }

    Derivatives result{};
    const double invSpan = 1.0 / spanSeconds;
    const double accelerationFactor = 2.0 * invSpan * invSpan;
    result.vx = system[1][3] * invSpan;
    result.vy = system[1][4] * invSpan;
    result.vz = system[1][5] * invSpan;
    result.ax = system[2][3] * accelerationFactor;
    result.ay = system[2][4] * accelerationFactor;
    result.az = system[2][5] * accelerationFactor;
    if (!std::isfinite(result.vx) || !std::isfinite(result.vy) ||
        !std::isfinite(result.vz) || !std::isfinite(result.ax) ||
        !std::isfinite(result.ay) || !std::isfinite(result.az)) return false;
    out = result;
    if (interceptOut) {
        interceptOut[0] = system[0][3];
        interceptOut[1] = system[0][4];
        interceptOut[2] = system[0][5];
    }
    return true;
}

template <typename Samples>
bool FitLinearLatest(const Samples& samples, Derivatives& out,
                     double intercept[3])
{
    const std::size_t n = samples.size();
    if (n < 3) return false;
    const auto firstMs = samples.front().tickMs;
    const auto lastMs = samples.back().tickMs;
    if (lastMs <= firstMs) return false;
    double sumT = 0, sumT2 = 0;
    double sumP[3]{}, sumTP[3]{};
    std::uint64_t prev = 0;
    bool first = true;
    for (const auto& sample : samples) {
        if (!first && sample.tickMs <= prev) return false;
        first = false;
        prev = sample.tickMs;
        if (sample.tickMs > lastMs) return false;
        const double t = -static_cast<double>(lastMs - sample.tickMs) * 0.001;
        const double p[3] = {sample.position.x, sample.position.y, sample.position.z};
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return false;
        sumT += t;
        sumT2 += t * t;
        for (int k = 0; k < 3; ++k) {
            sumP[k] += p[k];
            sumTP[k] += t * p[k];
        }
    }
    const double den = static_cast<double>(n) * sumT2 - sumT * sumT;
    if (!(den > 1e-12)) return false;
    double v[3]{};
    for (int k = 0; k < 3; ++k) {
        v[k] = (static_cast<double>(n) * sumTP[k] - sumT * sumP[k]) / den;
        intercept[k] = (sumP[k] - v[k] * sumT) / static_cast<double>(n);
        if (!std::isfinite(v[k]) || !std::isfinite(intercept[k])) return false;
    }
    out = {v[0], v[1], v[2], 0.0, 0.0, 0.0};
    return true;
}

template <typename Samples>
bool FitAdaptive(const Samples& samples, Derivatives& out, bool& quadratic)
{
    quadratic = false;
    double linearIntercept[3]{};
    Derivatives linear{};
    if (!FitLinearLatest(samples, linear, linearIntercept)) return false;

    if (samples.size() < 5) {
        out = linear;
        return true;
    }

    Derivatives fitted{};
    double quadraticIntercept[3]{};
    if (!FitLatest(samples, fitted, quadraticIntercept)) {
        out = linear;
        return true;
    }

    const std::uint64_t lastMs = samples.back().tickMs;
    double sseLinear = 0.0, sseQuadratic = 0.0, roundingFloor = 0.0;
    for (const auto& sample : samples) {
        const double t = -static_cast<double>(lastMs - sample.tickMs) * 0.001;
        const double actual[3] = {
            sample.position.x, sample.position.y, sample.position.z
        };
        const double v[3] = {linear.vx, linear.vy, linear.vz};
        const double qV[3] = {fitted.vx, fitted.vy, fitted.vz};
        const double qA[3] = {fitted.ax, fitted.ay, fitted.az};
        for (int k = 0; k < 3; ++k) {
            const double lr = actual[k] - (linearIntercept[k] + v[k] * t);
            const double qr = actual[k] -
                (quadraticIntercept[k] + qV[k] * t + 0.5 * qA[k] * t * t);
            sseLinear += lr * lr;
            sseQuadratic += qr * qr;
            const double resolution = static_cast<double>(
                std::numeric_limits<float>::epsilon()) *
                std::max(1.0, std::abs(actual[k]));
            roundingFloor += resolution * resolution;
        }
    }
    if (!std::isfinite(sseLinear) || !std::isfinite(sseQuadratic)) {
        out = linear;
        return true;
    }

    const double n = static_cast<double>(samples.size());
    const double improvementRatio = std::pow(3.0 * n, -1.0 / n);
    if (sseLinear > roundingFloor &&
        sseQuadratic + roundingFloor < sseLinear * improvementRatio) {
        out = fitted;
        quadratic = true;
    } else {
        out = linear;
    }
    return true;
}

}
