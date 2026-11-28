#include "pch.h"
#include "GrenadePrediction.h"

namespace RobRunGrenades {
namespace {

using CollisionBVH::Float3;
using CollisionBVH::dot;

constexpr float kHorizon = 2.f;
constexpr float kStep = 1.f / 40.f;
constexpr double kSampleWindow = 0.28;
constexpr double kMaxUnmovedTime = 0.12;
constexpr double kImpactTolerance = 0.13;
constexpr float kContactTolerance = 0.35f;

float LengthSquared(const Float3& v) { return dot(v, v); }

float HorizontalDistanceSquared(const Float3& a, const Float3& b)
{
    const auto delta = a - b;
    return delta.x * delta.x + delta.z * delta.z;
}

} // namespace

Predictor::Point Predictor::Motion::PositionAt(float time) const
{
    Point point = position + velocity * time;
    point.y += 0.5f * accelerationY * time * time;
    return point;
}

Predictor::Point Predictor::Motion::VelocityAt(float time) const
{
    return { velocity.x, velocity.y + accelerationY * time, velocity.z };
}

std::optional<Predictor::Motion> Predictor::FitMotion(const std::vector<PositionSample>& samples)
{
    if (samples.size() < 4 || samples.back().time - samples.front().time < 0.055)
        return std::nullopt;

    // Fit linear horizontal motion and quadratic vertical motion, anchored
    // to the latest observed position. Timestamps are relative to that sample.
    const auto& latest = samples.back();
    Float3 linear{};
    float tt = 0.f, ttt = 0.f, tttt = 0.f, quadraticY = 0.f;
    for (const auto& sample : samples) {
        const auto t = static_cast<float>(sample.time - latest.time);
        const auto delta = sample.position - latest.position;
        const float t2 = t * t;
        linear = linear + delta * t;
        tt += t2;
        ttt += t2 * t;
        tttt += t2 * t2;
        quadraticY += delta.y * t2;
    }
    const float det = tt * tttt - ttt * ttt;
    if (det <= 1e-11f) return std::nullopt;
    const Motion motion{ latest.position,
        { linear.x / tt, (linear.y * tttt - quadraticY * ttt) / det, linear.z / tt },
        2.f * (tt * quadraticY - ttt * linear.y) / det };
    if (motion.accelerationY >= -0.5f || motion.accelerationY < -150.f ||
        LengthSquared(motion.velocity) < 0.04f || LengthSquared(motion.velocity) > 22500.f)
        return std::nullopt;

    for (const auto& sample : samples) {
        const auto expected = motion.PositionAt(static_cast<float>(sample.time - latest.time));
        if (LengthSquared(sample.position - expected) > 0.12f * 0.12f) return std::nullopt;
    }
    return motion;
}

std::optional<Predictor::Motion> Predictor::EstimateDirection(const std::vector<PositionSample>& samples)
{
    if (samples.size() < 2) return std::nullopt;
    const auto& latest = samples.back();
    const auto first = std::lower_bound(samples.begin(), samples.end() - 1, latest.time - 0.12,
        [](const PositionSample& sample, double time) { return sample.time < time; });
    const auto elapsed = static_cast<float>(latest.time - first->time);
    if (elapsed < 0.016f) return std::nullopt;
    const auto velocity = (latest.position - first->position) * (1.f / elapsed);
    const float speedSquared = LengthSquared(velocity);
    if (speedSquared < 0.25f || speedSquared > 22500.f) return std::nullopt;
    return Motion{ latest.position, velocity, 0.f };
}

Path Predictor::Trace(const Motion& motion, const CollisionBVH::Scene* scene,
    double time, float horizon, std::optional<Impact>& impact, const std::optional<Point>& ground,
    const std::optional<Point>& gameContact)
{
    Path path;
    const int steps = static_cast<int>(std::ceil(horizon / kStep));
    path.points.reserve(steps + 1);
    path.points.push_back(motion.position);
    for (int step = 1; step <= steps; ++step) {
        const float to = std::min(step * kStep, horizon);
        const auto next = motion.PositionAt(to);
        const auto& previous = path.points.back();
        CollisionBVH::SegmentHit hit;
        bool intersects = scene && CollisionBVH::TraceSegment(*scene, previous, next, hit);
        if (!intersects && gameContact) {
            const auto segment = next - previous;
            const auto distanceSquared = LengthSquared(segment);
            if (distanceSquared > 1e-6f) {
                const float along = dot(*gameContact - previous, segment) / distanceSquared;
                if (along >= 0.f && along <= 1.f &&
                    LengthSquared(previous + segment * along - *gameContact) < 0.45f * 0.45f) {
                    hit = { *gameContact, {}, along, CollisionBVH::Surface_DefaultOpaque };
                    intersects = true;
                    path.estimatedImpact = true;
                }
            }
        }
        if (!intersects && !scene && ground && previous.y >= ground->y && next.y < ground->y) {
            const float fraction = (previous.y - ground->y) / (previous.y - next.y);
            const auto point = previous + (next - previous) * fraction;
            if (HorizontalDistanceSquared(point, *ground) > 45.f * 45.f) break;
            hit = { point, { 0.f, 1.f, 0.f }, fraction, CollisionBVH::Surface_DefaultOpaque };
            intersects = true;
            path.estimatedImpact = true;
        }
        if (intersects) {
            const float from = (step - 1) * kStep;
            const float contactTime = from + (to - from) * hit.fraction;
            impact = Impact{ time + contactTime, hit, motion.VelocityAt(contactTime) };
            path.points.push_back(hit.point);
            path.hitsSurface = true;
            return path;
        }
        path.points.push_back(next);
    }
    return path;
}

void Predictor::AddSample(Track& track, const PositionSample& sample)
{
    auto& history = track.samples;
    if (!history.empty()) {
        const auto& last = history.back();
        const auto dt = sample.time - last.time;
        if (dt <= 0.0) return;
        const auto travel = sample.position - last.position;
        if (LengthSquared(travel) < 0.000001f) return;

        if (dt > kMaxUnmovedTime || LengthSquared(travel) > 22500.0 * dt * dt) {
            history.clear();
            track.upcomingImpact.reset();
            track.observedImpact.reset();
        } else if (track.upcomingImpact) {
            const auto& expected = *track.upcomingImpact;
            const auto offset = sample.position - expected.hit.point;
            const auto& normal = expected.hit.normal;
            const auto reach = static_cast<float>(dt) * std::sqrt(LengthSquared(expected.incomingVelocity));
            if (std::abs(sample.time - expected.time) <= kImpactTolerance &&
                LengthSquared(offset) <= (kContactTolerance + reach) * (kContactTolerance + reach) &&
                dot(offset, normal) >= -0.12f &&
                dot(expected.incomingVelocity, normal) < -0.75f && dot(travel, normal) > 0.2 * dt) {
                // Fit the outgoing leg using positions from after the rebound.
                track.observedImpact = expected;
                track.upcomingImpact.reset();
                history.clear();
            } else if (sample.time - expected.time > kImpactTolerance) {
                track.upcomingImpact.reset();
            }
        }
    }

    constexpr std::size_t maxHistory = 9;
    while (!history.empty() &&
           (sample.time - history.front().time > kSampleWindow || history.size() >= maxHistory))
        history.erase(history.begin());
    history.push_back(sample);
}

unsigned Predictor::SurfaceKey(const CollisionBVH::SegmentHit& hit)
{
    // Keep floors, ceilings, steep slopes and walls in separate groups.
    const unsigned orientation = hit.normal.y > 0.7f ? 0 : hit.normal.y < -0.7f ? 1
        : std::abs(hit.normal.y) < 0.25f ? 2 : 3;
    return static_cast<unsigned>(hit.surface) * 4 + orientation;
}

void Predictor::LearnRebound(Track& track, const Motion& motion, double time)
{
    if (!track.observedImpact) return;
    const auto impact = *track.observedImpact;
    track.observedImpact.reset();
    if (track.description == 0 || time - impact.time > 0.24) return;

    const auto after = motion.VelocityAt(static_cast<float>(impact.time - time));
    const auto& before = impact.incomingVelocity;
    const auto& normal = impact.hit.normal;
    const float beforeNormal = dot(before, normal);
    const float afterNormal = dot(after, normal);
    if (beforeNormal >= -0.75f || afterNormal < 0.25f) return;

    const auto beforeTangent = before - normal * beforeNormal;
    const auto afterTangent = after - normal * afterNormal;
    const float tangentSpeedSquared = LengthSquared(beforeTangent);
    if (tangentSpeedSquared < 0.25f) return;
    const float tangent = dot(afterTangent, beforeTangent) / tangentSpeedSquared;
    const float rebound = -afterNormal / beforeNormal;
    const float tangentError = LengthSquared(afterTangent - beforeTangent * tangent);
    if (tangent < 0.f || tangent > 1.05f || rebound > 1.05f ||
        tangentError > std::max(0.25f, tangentSpeedSquared * 0.04f))
        return;

    // Cap calibration state even if a session keeps replacing description objects.
    if (rebounds.size() >= 64 && !rebounds.contains(track.description)) return;
    auto& model = rebounds[track.description][SurfaceKey(impact.hit)];
    if (model.observations == 0 || std::abs(model.normalRetention - rebound) > 0.2f ||
        std::abs(model.tangentRetention - tangent) > 0.2f) {
        model = { rebound, tangent, 1 };
        return;
    }
    model.observations = std::min(model.observations + 1, 8);
    model.normalRetention += (rebound - model.normalRetention) / model.observations;
    model.tangentRetention += (tangent - model.tangentRetention) / model.observations;
}

std::vector<Trajectory> Predictor::Update(std::uint64_t stampMs,
    const std::vector<RobRunWorld::GrenadeSample>& samples, const CollisionBVH::Scene* scene,
    const std::optional<Float3>& groundReference)
{
    const double now = stampMs / 1000.0;
    if (scene && !scene->valid) scene = nullptr;
    std::vector<Trajectory> paths;
    for (const auto& sample : samples) {
        auto& track = tracks[sample.id];
        if (track.model != sample.model || track.transform != sample.transform ||
            (sample.description && track.description && track.description != sample.description)) {
            track = {};
            track.model = sample.model;
            track.transform = sample.transform;
        }
        if (sample.description) track.description = sample.description;
        if (sample.effectRadius > 0.f) track.effectRadius = sample.effectRadius;
        track.lastSeen = stampMs;
        const Point position{ sample.position.x, sample.position.y, sample.position.z };
        if (sample.settled) {
            paths.push_back({ { { position }, true }, {}, track.effectRadius });
            track.samples.clear();
            continue;
        }
        if (!track.ground && groundReference &&
            HorizontalDistanceSquared(position, *groundReference) <= 24.f * 24.f &&
            position.y >= groundReference->y - 0.35f && position.y <= groundReference->y + 12.f) {
            track.ground = groundReference;
        }
        const auto contact = sample.contact
            ? std::optional<Point>{ { sample.contact->x, sample.contact->y, sample.contact->z } }
            : std::nullopt;
        AddSample(track, { now, position });
        if (track.samples.empty()) continue;
        const double stationaryFor = now - track.samples.back().time;
        if (stationaryFor >= kMaxUnmovedTime) {
            const bool nearGround = track.ground && std::abs(position.y - track.ground->y) <= 0.8f;
            const bool nearContact = (contact && LengthSquared(position - *contact) <= 0.8f * 0.8f) ||
                (track.upcomingImpact && LengthSquared(position - track.upcomingImpact->hit.point) <= 0.5f * 0.5f);
            const bool stoppedWithRadius = track.effectRadius > 0.f && stationaryFor >= 0.30;
            if (track.moving && (nearGround || nearContact || stoppedWithRadius))
                paths.push_back({ { { position }, true, !nearGround && !nearContact }, {}, track.effectRadius });
            continue;
        }
        const double time = track.samples.back().time;
        const auto motion = FitMotion(track.samples);
        if (!motion) {
            // Replicated positions can arrive in uneven steps. Show a short
            // direction preview while gathering enough samples for curvature.
            const auto direction = EstimateDirection(track.samples);
            if (direction) {
                track.moving = true;
                std::optional<Impact> hit;
                const bool descendingTowardGround = track.ground && direction->velocity.y < -0.5f &&
                    position.y >= track.ground->y;
                auto arc = Trace(*direction, scene, time, descendingTowardGround ? 0.85f : 0.35f,
                    hit, track.ground, contact);
                if (hit) arc.estimatedImpact = true;
                paths.push_back({ std::move(arc), {}, track.effectRadius });
            }
            continue;
        }

        LearnRebound(track, *motion, time);
        track.moving = true;
        std::optional<Impact> hit;
        Trajectory path;
        path.effectRadius = track.effectRadius;
        const float horizon = scene || track.ground || contact ? kHorizon : 0.6f;
        path.flight = Trace(*motion, scene, time, horizon, hit, track.ground, contact);
        if (hit && hit->time >= now) {
            if (!path.flight.estimatedImpact && hit->time - time <= kImpactTolerance)
                track.upcomingImpact = hit;
            const auto reference = rebounds.find(track.description);
            if (reference != rebounds.end()) {
                const auto model = reference->second.find(SurfaceKey(hit->hit));
                const float remainingTime = kHorizon - static_cast<float>(hit->time - time);
                if (!path.flight.estimatedImpact && model != reference->second.end() && model->second.observations >= 2 &&
                    remainingTime > kStep) {
                    const auto& normal = hit->hit.normal;
                    const float normalSpeed = dot(hit->incomingVelocity, normal);
                    const auto tangent = hit->incomingVelocity - normal * normalSpeed;
                    const auto velocity = tangent * model->second.tangentRetention
                        - normal * (normalSpeed * model->second.normalRetention);
                    if (dot(velocity, normal) > 0.25f) {
                        const Motion outgoing{ hit->hit.point + normal * 0.015f, velocity,
                            motion->accelerationY };
                        std::optional<Impact> secondHit;
                        path.rebound = Trace(outgoing, scene, hit->time, remainingTime, secondHit);
                        path.rebound.points.front() = hit->hit.point;
                    }
                }
            }
        } else if (hit) {
            // A frozen sample may still have a valid fit briefly after its impact time.
            continue;
        }
        paths.push_back(std::move(path));
    }
    std::erase_if(tracks, [stampMs](const auto& entry) { return entry.second.lastSeen != stampMs; });
    return paths;
}

} // namespace RobRunGrenades
