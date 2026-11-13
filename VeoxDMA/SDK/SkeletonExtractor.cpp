#include "pch.h"
#include "SkeletonExtractor.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace Skeleton {

using U = std::uint64_t;
using Read = ReadFn;

struct Quat { float x, y, z, w; };
struct TRS { Vec p; float padP; Quat q; Vec s; float padS; };
static_assert(sizeof(Vec) == 12 && sizeof(Quat) == 16);
static_assert(sizeof(TRS) == 48 && offsetof(TRS, q) == 0x10);
static_assert(offsetof(TRS, s) == 0x20);

static Vec Add(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static Vec Mul(Vec a, Vec b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
static Vec Cross(Vec a, Vec b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
static Vec Rotate(Quat q, Vec v) {
    Vec t = Cross({q.x,q.y,q.z},v);
    t = {2*t.x,2*t.y,2*t.z};
    return Add(v,Add({q.w*t.x,q.w*t.y,q.w*t.z},Cross({q.x,q.y,q.z},t)));
}
static bool Finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Snapshot {
    const Read& read;
    struct Item { U address; std::vector<unsigned char> bytes; };
    std::vector<Item> watch;
    bool bytes(U a, void* p, std::size_t n) const {
        return a > 0x10000 && n && a <= UINT64_MAX-n && read(a,p,n);
    }
    template<class T> bool get(U a, T& v) {
        if (!bytes(a,&v,sizeof(v))) return false;
        Item item{a,std::vector<unsigned char>(sizeof(v))};
        std::memcpy(item.bytes.data(),&v,sizeof(v));
        watch.push_back(std::move(item)); return true;
    }
    U ptr(U owner, U offset) {
        U value = 0;
        return owner > 0x10000 && owner <= UINT64_MAX-offset &&
            get(owner+offset,value) ? value : 0;
    }
    template<class T> bool array(U a, std::vector<T>& v, bool track) {
        const auto n = v.size()*sizeof(T);
        if (!bytes(a,v.data(),n)) return false;
        if (track) {
            Item item{a,std::vector<unsigned char>(n)};
            std::memcpy(item.bytes.data(),v.data(),n);
            watch.push_back(std::move(item));
        }
        return true;
    }
    bool stable() const {
        for (const auto& item : watch) {
            std::vector<unsigned char> now(item.bytes.size());
            if (!bytes(item.address,now.data(),now.size()) || now != item.bytes)
                return false;
        }
        return true;
    }
    std::string className(U object) {
        U klass = ptr(object,0), name = ptr(klass,0x10);
        std::string result;
        for (U i = 0; name > 0x10000 && i < 95; ++i) {
            char c = 0;
            if (!bytes(name+i,&c,1)) return {};
            if (!c) return result;
            result.push_back(c);
        }
        return {};
    }
    bool is(U object, const char* expected) { return className(object) == expected; }
    bool isOneOf(U object, std::initializer_list<const char*> expected) {
        const std::string name = className(object);
        if (name.empty()) return false;
        for (const char* candidate : expected)
            if (name == candidate) return true;
        return false;
    }
};

struct ResolvedHierarchy {
    U native = 0;
    U hierarchy = 0;
    U matrices = 0;
    U parents = 0;
    U pointers = 0;
    std::int32_t count = 0;
    int root = -1;
    std::vector<TRS> local;
    std::vector<std::int32_t> parent;
    std::vector<U> transforms;
    std::vector<unsigned char> reachable;

    bool isAncestor(int a, int b) const {
        for (int steps = 0; b != -1 && b >= 0 && b < count && steps < count; b = parent[b], ++steps)
            if (a == b) return true;
        return false;
    }

    int indexOf(U n) const {
        if (n <= 0x10000) return -1;
        int index = -1;
        for (int b = 0; b < count; ++b) {
            if (transforms[b] == n && reachable[b]) {
                if (index != -1) return -1;
                index = b;
            }
        }
        return index;
    }

    int resolveTransform(Snapshot& m, U managedTransform) const {
        if (!m.is(managedTransform, "Transform")) return -1;
        U n = m.ptr(managedTransform, 0x10);
        if (n <= 0x10000 || m.ptr(n, 0x28) != hierarchy) return -1;
        const int idx = indexOf(n);
        return (idx >= 0 && reachable[idx] && isAncestor(root, idx)) ? idx : -1;
    }
};

// BattleCharacterEntityView is a MonoBehaviour whose native struct +0x28 holds
// unrelated non-Transform fields on most characters. Resolve the skeleton
// TransformHierarchy via the character's verified Transform references first,
// and confirm native appears uniquely inside the hierarchy's Transform table.
static bool TryLoadHierarchyFromTransform(Snapshot& m, U nativeTransform, bool walkToTopRoot, ResolvedHierarchy& out)
{
    if (nativeTransform <= 0x10000) return false;
    const U hierarchy = m.ptr(nativeTransform, 0x28);
    if (hierarchy <= 0x10000) return false;
    const U matrices = m.ptr(hierarchy, 0x18);
    const U parents = m.ptr(hierarchy, 0x20);
    const U pointers = m.ptr(hierarchy, 0x30);
    std::int32_t count = 0;
    if (!m.get(hierarchy + 0x38, count) || count < 2 || count > 512) return false;

    std::vector<TRS> local(static_cast<std::size_t>(count));
    std::vector<std::int32_t> parent(static_cast<std::size_t>(count));
    std::vector<U> transforms(static_cast<std::size_t>(count));
    if (!m.array(matrices, local, false) ||
        !m.array(parents, parent, true) ||
        !m.array(pointers, transforms, true))
        return false;

    std::vector<unsigned char> reachable(static_cast<std::size_t>(count), 0);
    for (int b = 0; b < count; ++b) {
        bool ok = true;
        int p = b, steps = 0;
        while (p != -1 && steps < count) {
            if (p < -1 || p >= count) { ok = false; break; }
            p = parent[p];
            ++steps;
        }
        if (p != -1) ok = false;
        if (ok) reachable[b] = 1;
    }

    int idx = -1;
    for (int b = 0; b < count; ++b) {
        if (transforms[b] == nativeTransform && reachable[b]) {
            if (idx != -1) { idx = -1; break; }
            idx = b;
        }
    }
    if (idx < 0) return false;

    int rootIdx = idx;
    if (walkToTopRoot) {
        while (parent[rootIdx] != -1)
            rootIdx = parent[rootIdx];
        if (transforms[rootIdx] <= 0x10000) return false;
        nativeTransform = transforms[rootIdx];
    }

    out.native = nativeTransform;
    out.hierarchy = hierarchy;
    out.matrices = matrices;
    out.parents = parents;
    out.pointers = pointers;
    out.count = count;
    out.root = rootIdx;
    out.local = std::move(local);
    out.parent = std::move(parent);
    out.transforms = std::move(transforms);
    out.reachable = std::move(reachable);
    return true;
}

static bool ResolveViewHierarchy(Snapshot& m, U view, ResolvedHierarchy& out)
{
    auto tryManagedTransform = [&](U managed, bool walkToRoot) -> bool {
        if (!m.is(managed, "Transform")) return false;
        return TryLoadHierarchyFromTransform(m, m.ptr(managed, 0x10), walkToRoot, out);
    };

    const U movement = m.ptr(view, 0x20);
    if (m.isOneOf(movement, {"CharacterMovementView", "MovementView"}) &&
        tryManagedTransform(m.ptr(movement, 0x20), true))
        return true;

    if (tryManagedTransform(m.ptr(view, 0xE8), true))
        return true;

    const U animation = m.ptr(view, 0x28);
    if (m.isOneOf(animation, {"CharacterAnimationView", "AnimationView"})) {
        const U torso = m.ptr(animation, 0x30);
        if (m.isOneOf(torso, {"CharacterTorsoDeformationView", "TorsoView"}) &&
            tryManagedTransform(m.ptr(torso, 0x20), true))
            return true;
        const U head = m.ptr(animation, 0x28);
        if (m.isOneOf(head, {"CharacterHeadManualControlView", "HeadView"}) &&
            tryManagedTransform(m.ptr(head, 0x20), true))
            return true;
    }

    return TryLoadHierarchyFromTransform(m, m.ptr(view, 0x10), false, out);
}

bool BuildLayoutFromView(U view, const Read& read, RigLayout& out)
{
    out = RigLayout{};
    if (!read || view <= 0x10000) return false;
    Snapshot m{read,{}};
    if (!m.is(view,"BattleCharacterEntityView")) return false;
    ResolvedHierarchy rh;
    if (!ResolveViewHierarchy(m, view, rh)) return false;

    const U native = rh.native;
    const U hierarchy = rh.hierarchy;
    const U matrices = rh.matrices;
    const std::int32_t count = rh.count;
    const int root = rh.root;
    std::vector<TRS>& local = rh.local;
    std::vector<std::int32_t>& parent = rh.parent;
    auto ancestor = [&](int a, int b) { return rh.isAncestor(a, b); };
    auto resolve = [&](U transform) { return rh.resolveTransform(m, transform); };
    U animation = m.ptr(view,0x28), weapon = m.ptr(view,0x30);
    const bool animationOk = m.isOneOf(animation,{"CharacterAnimationView","AnimationView"});
    const bool weaponOk = m.isOneOf(weapon,{"CharacterWithWeaponAnimationView","WithWeaponView"});

    std::vector<int> anchors;
    auto pushAnchor = [&](int index) {
        if (index < 0) return;
        for (int other : anchors) if (other == index) return;
        anchors.push_back(index);
    };

    // Limbs are optional. Previously a single short IK chain, a missing limb
    // view or one bone that failed to resolve rejected the whole layout, which
    // is why some characters never received a skeleton at all. Collect what
    // validates and let the shared ancestor walk connect it.
    if (weaponOk) {
        int limbCounter = 0;
        for (U offset : {U(0x58),U(0x60),U(0x68),U(0x70)}) {
            const int slot = limbCounter++;
            U limb = m.ptr(weapon,offset);
            if (!m.isOneOf(limb,{"LimbIKView","LimbIK"})) continue;
            U array = m.ptr(limb,0x40), length = 0;
            if (array <= 0x10000 || !m.get(array+0x18,length) ||
                length < 1 || length > static_cast<U>(count)) continue;
            std::vector<U> bones(static_cast<std::size_t>(length));
            if (!m.array(array+0x20,bones,true)) continue;
            std::vector<int> limbIndices;
            for (U bone : bones) {
                int index = resolve(bone);
                if (index < 0) continue;
                bool duplicate = false;
                for (int other : limbIndices)
                    if (other == index) { duplicate = true; break; }
                if (duplicate) continue;
                limbIndices.push_back(index);
            }
            // The IK view lists the limb top-down, but the array can also carry
            // prop/weapon bones. Keep only the leading bones that form a real
            // parent -> child chain: an extra entry used to be pushed as an
            // anchor, which drew "arms" out to a weapon or a finger.
            {
                std::vector<int> chain;
                for (int index : limbIndices) {
                    if (chain.size() >= 3) break;
                    if (chain.empty()) { chain.push_back(index); continue; }
                    if (!ancestor(chain.back(), index)) break;
                    chain.push_back(index);
                }
                if (chain.size() >= 2) limbIndices = std::move(chain);
            }
            for (int index : limbIndices) pushAnchor(index);
            auto assign = [&](std::int32_t& a, std::int32_t& b, std::int32_t& c) {
                if (limbIndices.size() > 0) a = limbIndices[0];
                if (limbIndices.size() > 1) b = limbIndices[1];
                if (limbIndices.size() > 2) c = limbIndices[2];
            };
            switch (slot) {
            case 0: assign(out.leftShoulder, out.leftElbow, out.leftHand); break;
            case 1: assign(out.rightShoulder, out.rightElbow, out.rightHand); break;
            case 2: assign(out.leftHip, out.leftKnee, out.leftFoot); break;
            case 3: assign(out.rightHip, out.rightKnee, out.rightFoot); break;
            }
        }
    }

    // The head/torso chain provides the spine, which every useful skeleton
    // needs. Resolve each anchor independently so a missing upper-spine or
    // head-top bone does not discard the rest of the rig.
    int centralAnchors = 0;
    if (animationOk) {
        U head = m.ptr(animation,0x28), torso = m.ptr(animation,0x30);
        if (m.isOneOf(head,{"CharacterHeadManualControlView","HeadView"})) {
            const int neck = resolve(m.ptr(head,0x20));
            const int skull = resolve(m.ptr(head,0x28));
            const int top = resolve(m.ptr(head,0x30));
            if (neck >= 0 && skull >= 0 && neck != skull && ancestor(neck,skull)) {
                out.neck = neck;
                out.head = skull;
                pushAnchor(neck);
                pushAnchor(skull);
                ++centralAnchors;
                if (top >= 0 && ancestor(top,neck)) {
                    pushAnchor(top);
                    ++centralAnchors;
                }
            } else if (skull >= 0) {
                out.head = skull;
                pushAnchor(skull);
                ++centralAnchors;
            } else if (neck >= 0) {
                out.neck = neck;
                pushAnchor(neck);
                ++centralAnchors;
            }
        }
        if (m.isOneOf(torso,{"CharacterTorsoDeformationView","TorsoView"})) {
            const int spine2 = resolve(m.ptr(torso,0x28));
            const int spine3 = resolve(m.ptr(torso,0x30));
            if (spine2 >= 0 && spine3 >= 0 && spine2 != spine3 && ancestor(spine2,spine3)) {
                out.pelvis = spine2;
                out.chest = spine3;
                pushAnchor(spine2);
                pushAnchor(spine3);
                ++centralAnchors;
            } else if (spine3 >= 0) {
                out.chest = spine3;
                pushAnchor(spine3);
                ++centralAnchors;
            } else if (spine2 >= 0) {
                out.pelvis = spine2;
                pushAnchor(spine2);
                ++centralAnchors;
            }
        }
    }

    // Include crown/HeadPosition (+0xE8) when directly parented to skull so the
    // full head height is represented on the spine without shifting out.head.
    const int headTop = resolve(m.ptr(view, 0xE8));
    if (headTop >= 0 && out.head >= 0 && headTop != out.head && parent[headTop] == out.head) {
        const float dx = local[headTop].p.x, dy = local[headTop].p.y, dz = local[headTop].p.z;
        if (dx*dx + dy*dy + dz*dz < 0.25f)
            pushAnchor(headTop);
    }

    // TorsoView._spine2 is waist-level; prefer the shared thigh parent (true hips)
    // as out.pelvis whenever both leg roots attach to an ancestor of the chest.
    if (out.leftHip >= 0 && out.rightHip >= 0) {
        const int trueHips = parent[out.leftHip];
        if (trueHips >= 0 && trueHips == parent[out.rightHip] &&
            out.chest >= 0 && ancestor(trueHips, out.chest)) {
            out.pelvis = trueHips;
            pushAnchor(trueHips);
        }
    }

    // Parent-tree limb fallback: when the weapon view is unbound (holster,
    // reload, vehicle) we get no limb anchors from the IK views. Walk the
    // parent array — already in memory, no extra DMA — to find branches
    // that diverge from the spine chain.
    const bool missingLegs = out.leftHip < 0 && out.rightHip < 0;
    const bool missingArms = out.leftShoulder < 0 && out.rightShoulder < 0;
    if ((missingLegs || missingArms) && out.pelvis >= 0 && out.chest >= 0) {
        std::vector<bool> onSpine(count, false);
        int top = out.head >= 0 ? out.head : (out.neck >= 0 ? out.neck : out.chest);
        for (int b = top; b >= 0; b = parent[b]) {
            onSpine[b] = true;
            if (b == root) break;
        }

        auto chainLength = [&](const std::vector<int>& chain) {
            float total = 0.f;
            for (std::size_t i = 1; i < chain.size(); ++i) {
                const Vec& p = local[chain[i]].p;
                total += std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
            }
            return total;
        };

        auto walkChain = [&](int start, int depth) -> std::vector<int> {
            std::vector<int> chain;
            chain.push_back(start);
            int cur = start;
            for (int d = 0; d < depth; ++d) {
                int best = -1;
                float bestLen = -1.f;
                for (int b = 0; b < count; ++b) {
                    if (parent[b] != cur) continue;
                    const float dx = local[b].p.x, dy = local[b].p.y, dz = local[b].p.z;
                    const float len = dx*dx + dy*dy + dz*dz;
                    if (len > bestLen) { bestLen = len; best = b; }
                }
                if (best < 0) break;
                chain.push_back(best);
                cur = best;
            }
            return chain;
        };

        if (missingLegs) {
            std::vector<std::vector<int>> legs;
            auto collectLegsFrom = [&](int hipNode) {
                for (int b = 0; b < count; ++b) {
                    if (parent[b] != hipNode || onSpine[b]) continue;
                    auto chain = walkChain(b, 3);
                    if (chain.size() >= 2 && chainLength(chain) > 0.20f)
                        legs.push_back(std::move(chain));
                }
            };
            const int hipsCandidate = (parent[out.pelvis] >= 0) ? parent[out.pelvis] : out.pelvis;
            collectLegsFrom(hipsCandidate);
            if (legs.size() < 2 && hipsCandidate != out.pelvis)
                collectLegsFrom(out.pelvis);

            std::sort(legs.begin(), legs.end(), [&](const auto& a, const auto& b) {
                return chainLength(a) > chainLength(b);
            });
            if (legs.size() >= 2) {
                auto& lA = legs[0]; auto& lB = legs[1];
                bool aIsLeft = local[lA[0]].p.x < local[lB[0]].p.x;
                auto& left  = aIsLeft ? lA : lB;
                auto& right = aIsLeft ? lB : lA;
                const int hipNode = parent[left[0]];
                if (hipNode >= 0 && hipNode == parent[right[0]] && ancestor(hipNode, out.chest)) {
                    out.pelvis = hipNode;
                    pushAnchor(hipNode);
                }
                for (int idx : left) pushAnchor(idx);
                for (int idx : right) pushAnchor(idx);
                if (left.size() > 0) out.leftHip   = left[0];
                if (left.size() > 1) out.leftKnee  = left[1];
                if (left.size() > 2) out.leftFoot  = left[2];
                if (right.size() > 0) out.rightHip  = right[0];
                if (right.size() > 1) out.rightKnee = right[1];
                if (right.size() > 2) out.rightFoot = right[2];
            }
        }

        if (missingArms) {
            std::vector<std::vector<int>> arms;
            for (int b = 0; b < count; ++b) {
                if (parent[b] != out.chest || onSpine[b]) continue;
                auto chain = walkChain(b, 3);
                if (chain.size() >= 3 && chainLength(chain) > 0.25f)
                    arms.push_back(std::move(chain));
            }
            std::sort(arms.begin(), arms.end(), [&](const auto& a, const auto& b) {
                return chainLength(a) > chainLength(b);
            });
            if (arms.size() >= 2) {
                auto& aA = arms[0]; auto& aB = arms[1];
                bool aIsLeft = local[aA[0]].p.x < local[aB[0]].p.x;
                auto& left  = aIsLeft ? aA : aB;
                auto& right = aIsLeft ? aB : aA;
                for (int idx : left) pushAnchor(idx);
                for (int idx : right) pushAnchor(idx);
                const std::size_t ls = left.size() >= 4 ? 1 : 0;
                const std::size_t rs = right.size() >= 4 ? 1 : 0;
                if (left.size() > ls)     out.leftShoulder  = left[ls];
                if (left.size() > ls + 1) out.leftElbow     = left[ls + 1];
                if (left.size() > ls + 2) out.leftHand      = left[ls + 2];
                if (right.size() > rs)     out.rightShoulder = right[rs];
                if (right.size() > rs + 1) out.rightElbow    = right[rs + 1];
                if (right.size() > rs + 2) out.rightHand     = right[rs + 2];
            }
        }
    }

    // Two connected anchors are the minimum for a line, and at least one must
    // come from the head/torso chain; limb-only data would draw floating arms.
    if (anchors.size() < 2 || centralAnchors == 0) return false;

    int lca = anchors.front();
    for (int index : anchors) {
        while (lca != -1 && !ancestor(lca,index)) lca = parent[lca];
        if (lca == -1) return false;
    }
    if (lca < 0) return false;

    std::vector<unsigned char> selected(count,0);
    for (int index : anchors) {
        for (int p = index;; p = parent[p]) {
            if (p < 0 || !ancestor(root,p)) return false;
            selected[p] = 1;
            if (p == lca) break;
        }
    }
    // The parent/pointer arrays must not have changed while they were read;
    // a layout built from a half-written table indexes the wrong bones.
    if (!m.stable() ||
        m.ptr(native, 0x28) != hierarchy || m.ptr(hierarchy, 0x18) != matrices)
        return false;

    out.matrices = matrices;
    out.nativeTransform = native;
    out.hierarchy = hierarchy;
    out.count = count;
    out.parent = std::move(parent);
    out.selected = std::move(selected);
    out.segments.clear();
    for (int b = 0; b < count; ++b) {
        const int p = out.parent[b];
        if (out.selected[b] && p >= 0 && out.selected[p])
            out.segments.push_back({p, b});
    }
    return !out.segments.empty();
}

static bool BuildPoseOutput(const RigLayout& layout, std::vector<TRS>& local,
    std::vector<BoneSegment>& out, BonePoints* points, BoneLocal* localOut)
{
    const int count = layout.count;
    std::vector<unsigned char> boneValid(static_cast<std::size_t>(count), 0);
    for (int b = 0; b < count; ++b) {
        auto& t = local[b];
        const float n = t.q.x*t.q.x+t.q.y*t.q.y+t.q.z*t.q.z+t.q.w*t.q.w;
        if (!Finite(t.p) || !Finite(t.s) || !std::isfinite(n) ||
            n < 0.5f || n > 1.5f)
            continue;
        const float inv = 1.0f/std::sqrt(n);
        t.q = {t.q.x*inv,t.q.y*inv,t.q.z*inv,t.q.w*inv};
        boneValid[b] = 1;
    }

    std::vector<Vec> world(static_cast<std::size_t>(count));
    std::vector<unsigned char> worldValid(static_cast<std::size_t>(count), 0);
    for (int b = 0; b < count; ++b) {
        if (!boneValid[b]) continue;
        Vec v = local[b].p;
        bool ok = true;
        int steps = 0;
        for (int p = layout.parent[b]; p != -1; ++steps) {
            if (steps >= count || p < 0 || p >= count || !boneValid[p]) { ok = false; break; }
            v = Add(local[p].p, Rotate(local[p].q, Mul(local[p].s, v)));
            p = layout.parent[p];
        }
        if (!ok || !Finite(v)) continue;
        world[b] = v;
        worldValid[b] = 1;
    }

    std::vector<BoneSegment> result;
    result.reserve(layout.segments.size());
    for (const auto& seg : layout.segments) {
        if (!worldValid[seg.first] || !worldValid[seg.second]) continue;
        result.push_back({world[seg.first], world[seg.second]});
    }
    out = std::move(result);

    if (localOut) {
        for (int b = 0; b < count; ++b) {
            if (!boneValid[b]) {
                localOut[b].position = {};
                localOut[b].rotation[0] = 0.f;
                localOut[b].rotation[1] = 0.f;
                localOut[b].rotation[2] = 0.f;
                localOut[b].rotation[3] = 1.f;
                localOut[b].scale = {1.f, 1.f, 1.f};
                continue;
            }
            localOut[b].position = local[b].p;
            localOut[b].rotation[0] = local[b].q.x;
            localOut[b].rotation[1] = local[b].q.y;
            localOut[b].rotation[2] = local[b].q.z;
            localOut[b].rotation[3] = local[b].q.w;
            localOut[b].scale = local[b].s;
        }
    }

    if (points) {
        *points = BonePoints{};
        auto anchorOk = [&](int idx) {
            return idx >= 0 && idx < count && worldValid[idx] != 0;
        };
        points->hasHead = anchorOk(layout.head);
        points->hasNeck = anchorOk(layout.neck);
        points->hasChest = anchorOk(layout.chest);
        points->hasPelvis = anchorOk(layout.pelvis);
        points->valid = points->hasHead || points->hasNeck || points->hasChest || points->hasPelvis;
        if (points->valid) {
            auto fill = [&](int idx, Vec& dst) {
                if (anchorOk(idx)) dst = world[idx];
            };
            fill(layout.head, points->head);
            fill(layout.neck, points->neck);
            fill(layout.chest, points->chest);
            fill(layout.pelvis, points->pelvis);
            fill(layout.leftShoulder, points->leftShoulder);
            fill(layout.leftElbow, points->leftElbow);
            fill(layout.leftHand, points->leftHand);
            fill(layout.rightShoulder, points->rightShoulder);
            fill(layout.rightElbow, points->rightElbow);
            fill(layout.rightHand, points->rightHand);
            fill(layout.leftHip, points->leftHip);
            fill(layout.leftKnee, points->leftKnee);
            fill(layout.leftFoot, points->leftFoot);
            fill(layout.rightHip, points->rightHip);
            fill(layout.rightKnee, points->rightKnee);
            fill(layout.rightFoot, points->rightFoot);
        }
    }
    return !out.empty() || (points && points->valid);
}

bool SampleLayout(const RigLayout& layout, const Read& read,
    std::vector<BoneSegment>& out, BonePoints* points,
    BoneLocal* localOut)
{
    const int count = layout.count;
    if (!read || layout.matrices <= 0x10000 || count < 2 ||
        layout.parent.size() != static_cast<std::size_t>(count) ||
        layout.selected.size() != static_cast<std::size_t>(count))
        return false;

    std::vector<TRS> local(static_cast<std::size_t>(count));
    constexpr std::size_t kPoseChunk = 0x1000;
    auto* poseBytes = reinterpret_cast<unsigned char*>(local.data());
    const std::size_t poseSize = local.size() * sizeof(TRS);
    for (std::size_t offset = 0; offset < poseSize; offset += kPoseChunk) {
        const std::size_t chunk = std::min(kPoseChunk, poseSize - offset);
        if (!read(layout.matrices + offset, poseBytes + offset, chunk))
            return false;
    }
    return BuildPoseOutput(layout, local, out, points, localOut);
}

bool ProcessPoseData(const RigLayout& layout, const void* poseData,
    std::size_t poseSize, std::vector<BoneSegment>& out,
    BonePoints* points, BoneLocal* localOut)
{
    const int count = layout.count;
    if (!poseData || count < 2 ||
        layout.parent.size() != static_cast<std::size_t>(count) ||
        layout.selected.size() != static_cast<std::size_t>(count))
        return false;
    if (poseSize < static_cast<std::size_t>(count) * sizeof(TRS))
        return false;

    std::vector<TRS> local(static_cast<std::size_t>(count));
    std::memcpy(local.data(), poseData, static_cast<std::size_t>(count) * sizeof(TRS));
    return BuildPoseOutput(layout, local, out, points, localOut);
}

std::vector<BoneSegment> BuildFromView(U view, const Read& read)
{
    RigLayout layout;
    if (!BuildLayoutFromView(view, read, layout)) return {};
    std::vector<BoneSegment> result;
    if (!SampleLayout(layout, read, result)) return {};
    return result;
}

std::vector<BoneSegment> BuildSkeleton(
    U game, U battleModel, std::int32_t entityId, const Read& read)
{
    if (!read || battleModel <= 0x10000) return {};
    Snapshot m{read,{}};

    // Verified active-updater path:
    //   Game+0x20 -> List -> array[i] -> +0x50 -> +0x18 -> +0x90.
    U updater = 0;
    {
        U list = m.ptr(game, 0x20);
        U array = m.ptr(list, 0x10);
        std::int32_t count = 0;
        if (array > 0x10000 && m.get(list + 0x18, count) &&
            count >= 1 && count <= 512) {
            for (std::int32_t i = 0; i < count; ++i) {
                U a = m.ptr(array + 0x20 + static_cast<U>(i) * 8, 0);
                U b = m.ptr(a, 0x50);
                U c = m.ptr(b, 0x18);
                U candidate = m.ptr(c, 0x90);
                if (!m.is(candidate, "BattleEntitiesUpdater")) continue;
                if (m.ptr(candidate, 0x70) != battleModel) continue;
                updater = candidate;
                break;
            }
        }
    }

    // Legacy fallback chain retained for sessions where the active list has
    // not been populated yet.
    if (!updater) {
        const U chainOffsets[] = {0x98,0x20,0x50,0x18,0x90};
        const char* classes[] = {"Game","StartGameProcessor","ScreenManager",
            "BattleScreenUpdater","BattleUpdater","BattleEntitiesUpdater"};
        updater = game;
        for (int i = 0; i < 6; ++i) {
            if (!m.is(updater,classes[i])) return {};
            if (i == 4 && m.ptr(updater,0xE0) != battleModel) return {};
            if (i < 5) updater = m.ptr(updater,chainOffsets[i]);
        }
    }

    U dict = m.ptr(updater,0x110), entries = m.ptr(dict,0x18);
    std::int32_t used = 0;
    U capacity = 0;
    if (!dict || !entries || !m.get(dict+0x20,used) ||
        !m.get(entries+0x18,capacity) || used < 1 || used > 512 ||
        capacity < static_cast<U>(used)) return {};
    struct Entry { std::int32_t hash,next,key,pad; U value; };
    static_assert(sizeof(Entry) == 24 && offsetof(Entry,value) == 0x10);
    std::vector<Entry> slots(used);
    if (!m.array(entries+0x20,slots,true)) return {};
    U view = 0;
    for (const auto& e : slots) if (e.hash >= 0 && e.key == entityId) {
        if (view || e.value <= 0x10000) return {};
        view = e.value;
    }
    if (!m.is(view,"BattleCharacterEntityView")) return {};
    return BuildFromView(view, read);
}

bool ValidateHierarchy(const RigLayout& layout, const Read& read)
{
    if (!read || layout.nativeTransform <= 0x10000 || layout.hierarchy <= 0x10000)
        return false;
    U currentHierarchy = 0;
    if (!read(layout.nativeTransform + 0x28, &currentHierarchy, sizeof(currentHierarchy)))
        return false;
    if (currentHierarchy != layout.hierarchy)
        return false;
    U currentMatrices = 0;
    if (!read(currentHierarchy + 0x18, &currentMatrices, sizeof(currentMatrices)))
        return false;
    if (currentMatrices != layout.matrices)
        return false;
    std::int32_t currentCount = 0;
    if (!read(currentHierarchy + 0x38, &currentCount, sizeof(currentCount)))
        return false;
    if (currentCount < 2 || currentCount > 512)
        return false;
    // Tolerate count drift: the hierarchy is valid as long as every
    // selected bone index is still within the current count.
    for (std::size_t b = 0; b < layout.selected.size(); ++b)
        if (layout.selected[b] && static_cast<std::int32_t>(b) >= currentCount)
            return false;
    return true;
}

// Name-based bone fallback. Reads each transform's GameObject name from the
// native hierarchy and matches against Unity humanoid bone names. This works
// even when the weapon/IK views are unbound (holster, reload, vehicle).
bool BuildLayoutByName(U view, const Read& read, RigLayout& out)
{
    out = RigLayout{};
    if (!read || view <= 0x10000) return false;
    Snapshot m{read,{}};
    if (!m.is(view, "BattleCharacterEntityView")) return false;

    ResolvedHierarchy rh;
    if (!ResolveViewHierarchy(m, view, rh)) return false;

    const U native = rh.native;
    const U hierarchy = rh.hierarchy;
    const U matrices = rh.matrices;
    const std::int32_t count = rh.count;
    const int root = rh.root;
    std::vector<std::int32_t>& parent = rh.parent;
    const std::vector<U>& transforms = rh.transforms;
    const std::vector<unsigned char>& reachable = rh.reachable;
    auto ancestor = [&](int a, int b) { return rh.isAncestor(a, b); };

    // Read each transform's GameObject name via native Transform -> GameObject(0x30) -> name(0x48)
    // Unity native name is a char* at GameObject+0x48, length at +0x40 (or just read until null).
    struct NamedBone { int index; std::string name; };
    std::vector<NamedBone> namedBones;
    for (int b = 0; b < count; ++b) {
        if (!reachable[b] || !ancestor(root, b)) continue;
        U gameObject = 0;
        if (!read(transforms[b] + 0x30, &gameObject, sizeof(gameObject)) || gameObject <= 0x10000)
            continue;
        char buf[64]{};
        auto tryNameOffset = [&](U off) -> bool {
            U namePtr = 0;
            if (!read(gameObject + off, &namePtr, sizeof(namePtr)) || namePtr <= 0x10000)
                return false;
            std::memset(buf, 0, sizeof(buf));
            if (!read(namePtr, buf, sizeof(buf) - 1))
                return false;
            buf[63] = '\0';
            int len = 0;
            for (; len < 63 && buf[len]; ++len) {
                const unsigned char ch = static_cast<unsigned char>(buf[len]);
                if (ch < 0x20 || ch > 0x7E) return false;
            }
            return len >= 2;
        };
        if (!tryNameOffset(0x60) && !tryNameOffset(0x48))
            continue;
        namedBones.push_back({b, std::string(buf)});
    }

    struct BoneMapping { const char* name; std::int32_t* target; };
    BoneMapping mappings[] = {
        {"Hips", &out.pelvis},
        {"Pelvis", &out.pelvis},
        {"Spine1", &out.chest},
        {"Spine2", &out.chest},
        {"Chest", &out.chest},
        {"Neck", &out.neck},
        {"Head", &out.head},
        {"LeftShoulder", &out.leftShoulder},
        {"LeftUpperArm", &out.leftShoulder},
        {"LeftLowerArm", &out.leftElbow},
        {"LeftForeArm", &out.leftElbow},
        {"LeftHand", &out.leftHand},
        {"RightShoulder", &out.rightShoulder},
        {"RightUpperArm", &out.rightShoulder},
        {"RightLowerArm", &out.rightElbow},
        {"RightForeArm", &out.rightElbow},
        {"RightHand", &out.rightHand},
        {"LeftUpperLeg", &out.leftHip},
        {"LeftThigh", &out.leftHip},
        {"LeftLowerLeg", &out.leftKnee},
        {"LeftCalf", &out.leftKnee},
        {"LeftFoot", &out.leftFoot},
        {"RightUpperLeg", &out.rightHip},
        {"RightThigh", &out.rightHip},
        {"RightLowerLeg", &out.rightKnee},
        {"RightCalf", &out.rightKnee},
        {"RightFoot", &out.rightFoot},
    };

    auto nameContains = [](const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    };

    for (const auto& bone : namedBones) {
        for (auto& mp : mappings) {
            if (*mp.target >= 0) continue;
            if (nameContains(bone.name, mp.name))
                *mp.target = bone.index;
        }
    }

    std::vector<int> anchors;
    auto pushAnchor = [&](int index) {
        if (index < 0) return;
        for (int other : anchors) if (other == index) return;
        anchors.push_back(index);
    };

    int centralAnchors = 0;
    if (out.head >= 0) { pushAnchor(out.head); ++centralAnchors; }
    if (out.neck >= 0) { pushAnchor(out.neck); ++centralAnchors; }
    if (out.chest >= 0) { pushAnchor(out.chest); ++centralAnchors; }
    if (out.pelvis >= 0) { pushAnchor(out.pelvis); ++centralAnchors; }
    pushAnchor(out.leftShoulder); pushAnchor(out.leftElbow); pushAnchor(out.leftHand);
    pushAnchor(out.rightShoulder); pushAnchor(out.rightElbow); pushAnchor(out.rightHand);
    pushAnchor(out.leftHip); pushAnchor(out.leftKnee); pushAnchor(out.leftFoot);
    pushAnchor(out.rightHip); pushAnchor(out.rightKnee); pushAnchor(out.rightFoot);

    if (anchors.size() < 2 || centralAnchors == 0) return false;

    int lca = anchors.front();
    for (int index : anchors) {
        while (lca != -1 && !ancestor(lca, index)) lca = parent[lca];
        if (lca == -1) return false;
    }
    if (lca < 0) return false;

    std::vector<unsigned char> selected(count, 0);
    for (int index : anchors) {
        for (int p = index;; p = parent[p]) {
            if (p < 0 || !ancestor(root, p)) return false;
            selected[p] = 1;
            if (p == lca) break;
        }
    }

    out.matrices = matrices;
    out.nativeTransform = native;
    out.hierarchy = hierarchy;
    out.count = count;
    out.parent = std::move(parent);
    out.selected = std::move(selected);
    out.segments.clear();
    for (int b = 0; b < count; ++b) {
        const int p = out.parent[b];
        if (out.selected[b] && p >= 0 && out.selected[p])
            out.segments.push_back({p, b});
    }
    return !out.segments.empty();
}

bool ResolveRigTarget(U view, const Read& read, RigTarget& out)
{
    out = RigTarget{};
    if (!read || view <= 0x10000) return false;
    Snapshot m{read,{}};

    // Documented chain: the entity view exposes the movement view, and the
    // CharacterMovementView exposes the managed root Transform through
    // _transform(). The native Unity object is what owns the transform
    // hierarchy, so both hops are needed.
    U movement = m.ptr(view, 0x20);
    U managed = (movement > 0x10000) ? m.ptr(movement, 0x20) : 0;
    U native = (managed > 0x10000) ? m.ptr(managed, 0x10) : 0;

    // Some rigs expose the same native transform straight off the view; keep
    // that as a fallback rather than rejecting the character.
    if (native <= 0x10000) native = m.ptr(view, 0x10);
    if (native <= 0x10000) return false;

    U hierarchy = m.ptr(native, 0x28);
    U matrices = m.ptr(hierarchy, 0x18);
    std::int32_t count = 0;
    if (hierarchy <= 0x10000 || matrices <= 0x10000 ||
        !m.get(hierarchy + 0x38, count) || count < 2 || count > 512)
        return false;

    out.view = view;
    out.movementView = movement;
    out.managedTransform = managed;
    out.nativeTransform = native;
    out.hierarchy = hierarchy;
    out.matrices = matrices;
    out.count = count;
    return true;
}

bool RootWorldPosition(U view, const Read& read, Vec& out)
{
    out = {};
    if (!read || view <= 0x10000) return false;
    Snapshot m{read,{}};

    // Same chain as BuildLayoutFromView: the view exposes the movement view,
    // which exposes the managed root Transform, which owns the native object
    // and its transform hierarchy. The native object straight off the view is
    // the fallback used by some characters.
    U movement = m.ptr(view, 0x20);
    U managed = (movement > 0x10000) ? m.ptr(movement, 0x20) : 0;
    U native = (managed > 0x10000) ? m.ptr(managed, 0x10) : 0;
    if (native <= 0x10000) native = m.ptr(view, 0x10);
    if (native <= 0x10000) return false;

    U hierarchy = m.ptr(native, 0x28);
    if (hierarchy <= 0x10000) return false;
    U matrices = m.ptr(hierarchy, 0x18);
    U parents = m.ptr(hierarchy, 0x20);
    std::int32_t count = 0;
    if (matrices <= 0x10000 || parents <= 0x10000 ||
        !m.get(hierarchy + 0x38, count) || count < 2 || count > 512)
        return false;

    // The root is the only node without a parent, so its local TRS is already
    // the world transform. Everything below it is composed by SampleLayout.
    std::vector<std::int32_t> parent(static_cast<std::size_t>(count));
    if (!m.array(parents, parent, false)) return false;
    int root = -1;
    for (int i = 0; i < count; ++i) {
        if (parent[static_cast<std::size_t>(i)] == -1) { root = i; break; }
    }
    if (root < 0) return false;

    TRS trs{};
    if (!m.bytes(matrices + static_cast<U>(root) * sizeof(TRS), &trs, sizeof(TRS)))
        return false;
    if (!Finite(trs.p)) return false;
    out = trs.p;
    return true;
}

bool MatchesRigTarget(const RigLayout& layout, const RigTarget& target)
{
    // The parent table and the matrix array belong to one hierarchy instance.
    // A new native object, hierarchy, array pointer or bone count means the
    // cached indices describe something that no longer exists.
    return target.nativeTransform == layout.nativeTransform &&
        target.hierarchy == layout.hierarchy &&
        target.matrices == layout.matrices &&
        target.count == layout.count;
}

} // namespace Skeleton
