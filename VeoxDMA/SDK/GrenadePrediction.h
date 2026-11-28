#pragma once

#include "CollisionBVH.h"
#include "WorldEntities.h"
#include <optional>
#include <unordered_map>

namespace RobRunGrenades {

struct Path {
    std::vector<CollisionBVH::Float3> points;
    bool hitsSurface = false;
    bool estimatedImpact = false;
};

struct Trajectory {
    Path flight;
    Path rebound;
    float effectRadius = 0.f;
};

struct Preview {
    static constexpr std::uint64_t MaxAgeMs = 150;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t stampMs = 0;
    std::vector<Trajectory> trajectories;
};

class Predictor {
public:
    std::vector<Trajectory> Update(std::uint64_t stampMs,
        const std::vector<RobRunWorld::GrenadeSample>& samples, const CollisionBVH::Scene* scene,
        const std::optional<CollisionBVH::Float3>& groundReference = std::nullopt);
    void Reset() { tracks.clear(); rebounds.clear(); }

private:
    using Point = CollisionBVH::Float3;

    struct PositionSample {
        double time = 0.0;
        Point position{};
    };

    struct Motion {
        Point position{};
        Point velocity{};
        float accelerationY = 0.f;
        Point PositionAt(float time) const;
        Point VelocityAt(float time) const;
    };

    struct Impact {
        double time = 0.0;
        CollisionBVH::SegmentHit hit;
        Point incomingVelocity{};
    };

    struct Track {
        std::uintptr_t model = 0;
        std::uintptr_t transform = 0;
        std::uintptr_t description = 0;
        float effectRadius = 0.f;
        std::uint64_t lastSeen = 0;
        std::vector<PositionSample> samples;
        std::optional<Point> ground;
        std::optional<Impact> upcomingImpact;
        std::optional<Impact> observedImpact;
        bool moving = false;
    };

    struct BounceModel {
        float normalRetention = 0.f;
        float tangentRetention = 0.f;
        int observations = 0;
    };

    static std::optional<Motion> FitMotion(const std::vector<PositionSample>& samples);
    static std::optional<Motion> EstimateDirection(const std::vector<PositionSample>& samples);
    static Path Trace(const Motion& motion, const CollisionBVH::Scene* scene,
        double time, float horizon, std::optional<Impact>& impact,
        const std::optional<Point>& ground = std::nullopt,
        const std::optional<Point>& gameContact = std::nullopt);
    static void AddSample(Track& track, const PositionSample& sample);

    void LearnRebound(Track& track, const Motion& motion, double time);
    static unsigned SurfaceKey(const CollisionBVH::SegmentHit& hit);

    std::unordered_map<int, Track> tracks;
    std::unordered_map<std::uintptr_t, std::unordered_map<unsigned, BounceModel>> rebounds;
};

} // namespace RobRunGrenades
