#include "pch.h"
#include "SkeletonExtractor.h"
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
    // Verified path: the entity view exposes the native Unity object at +0x10.
    // Fall back to the movement view -> managed Transform path when that
    // pointer is not a hierarchy-bearing Transform.
    U native = m.ptr(view,0x10), hierarchy = m.ptr(native,0x28);
    if (native <= 0x10000 || hierarchy <= 0x10000) {
        U movement = m.ptr(view,0x20);
        if (!m.isOneOf(movement,{"CharacterMovementView","MovementView"})) return {};
        U managed = m.ptr(movement,0x20);
        if (!m.is(managed,"Transform")) return {};
        native = m.ptr(managed,0x10);
        hierarchy = m.ptr(native,0x28);
    }
    if (native <= 0x10000 || hierarchy <= 0x10000) return {};
    U matrices = m.ptr(hierarchy,0x18), parents = m.ptr(hierarchy,0x20);
    U pointers = m.ptr(hierarchy,0x30);
    std::int32_t count = 0;
    if (!m.get(hierarchy+0x38,count) || count < 2 || count > 512) return {};
    std::vector<TRS> local(count);
    std::vector<std::int32_t> parent(count);
    std::vector<U> transforms(count);
    if (!m.array(matrices,local,false) || !m.array(parents,parent,true) ||
        !m.array(pointers,transforms,true) || !m.stable()) return {};
    for (int p : parent) if (p < -1 || p >= count) return {};
    for (int b = 0; b < count; ++b) {
        int p = b, steps = 0;
        while (p != -1 && steps < count) { p = parent[p]; ++steps; }
        if (p != -1) return {};
    }
    auto ancestor = [&](int a, int b) {
        for (; b != -1; b = parent[b]) if (a == b) return true;
        return false;
    };
    auto indexOf = [&](U n) {
        int index = -1;
        for (int b = 0; b < count; ++b) if (transforms[b] == n) {
            if (index != -1) return -1;
            index = b;
        }
        return index;
    };
    int root = indexOf(native);
    if (root < 0) return {};
    for (auto& t : local) {
        float n = t.q.x*t.q.x+t.q.y*t.q.y+t.q.z*t.q.z+t.q.w*t.q.w;
        if (!Finite(t.p) || !Finite(t.s) || !std::isfinite(n) ||
            n < 0.5f || n > 1.5f) return {};
        float inv = 1.0f/std::sqrt(n);
        t.q = {t.q.x*inv,t.q.y*inv,t.q.z*inv,t.q.w*inv};
    }
    std::vector<Vec> world(count);
    for (int b = 0; b < count; ++b) {
        Vec v = local[b].p;
        for (int p = parent[b]; p != -1; p = parent[p])
            v = Add(local[p].p,Rotate(local[p].q,Mul(local[p].s,v)));
        if (!Finite(v)) return {};
        world[b] = v;
    }
    auto resolve = [&](U transform) {
        if (!m.is(transform,"Transform")) return -1;
        U n = m.ptr(transform,0x10);
        if (n <= 0x10000 || m.ptr(n,0x28) != hierarchy) return -1;
        int index = indexOf(n);
        return index >= 0 && ancestor(root,index) ? index : -1;
    };
    U animation = m.ptr(view,0x28), weapon = m.ptr(view,0x30);
    if (!m.isOneOf(animation,{"CharacterAnimationView","AnimationView"}) ||
        !m.isOneOf(weapon,{"CharacterWithWeaponAnimationView","WithWeaponView"})) return {};
    std::vector<int> anchors;
    for (U offset : {U(0x58),U(0x60),U(0x68),U(0x70)}) {
        U limb = m.ptr(weapon,offset);
        if (!m.isOneOf(limb,{"LimbIKView","LimbIK"})) return {};
        U array = m.ptr(limb,0x40), length = 0;
        if (array <= 0x10000 || !m.get(array+0x18,length) ||
            length < 2 || length > static_cast<U>(count)) return {};
        std::vector<U> bones(static_cast<std::size_t>(length));
        if (!m.array(array+0x20,bones,true)) return {};
        std::vector<int> limbIndices;
        for (U bone : bones) {
            int index = resolve(bone);
            if (index < 0) return {};
            for (int other : limbIndices)
                if (other == index || (!ancestor(other,index) &&
                    !ancestor(index,other))) return {};
            limbIndices.push_back(index); anchors.push_back(index);
        }
    }
    U head = m.ptr(animation,0x28), torso = m.ptr(animation,0x30);
    if (!m.isOneOf(head,{"CharacterHeadManualControlView","HeadView"}) ||
        !m.isOneOf(torso,{"CharacterTorsoDeformationView","TorsoView"})) return {};
    int neck = resolve(m.ptr(head,0x20)), skull = resolve(m.ptr(head,0x28));
    int top = resolve(m.ptr(head,0x30));
    int spine2 = resolve(m.ptr(torso,0x28)), spine3 = resolve(m.ptr(torso,0x30));
    if (neck < 0 || skull < 0 || top < 0 || spine2 < 0 || spine3 < 0 ||
        neck == skull || !ancestor(neck,skull) || !ancestor(top,neck)) return {};
    for (int index : {neck,skull,top,spine2,spine3}) anchors.push_back(index);
    int lca = anchors.front();
    for (int index : anchors) {
        while (lca != -1 && !ancestor(lca,index)) lca = parent[lca];
        if (lca == -1) return {};
    }
    std::vector<unsigned char> selected(count,0);
    for (int index : anchors) {
        for (int p = index;; p = parent[p]) {
            if (!ancestor(root,p)) return {};
            selected[p] = 1;
            if (p == lca) break;
        }
    }
    if (!m.stable()) return {};
    std::vector<BoneSegment> result;
    for (int b = 0; b < count; ++b) {
        int p = parent[b];
        if (selected[b] && p >= 0 && selected[p])
            result.push_back({world[p],world[b]});
    }
    return result;
}

std::vector<BoneSegment> BuildFromView(U view, const Read& read)
{
    if (!read || view <= 0x10000) return {};
    Snapshot m{read,{}};
    if (!m.is(view,"BattleCharacterEntityView")) return {};
    // Verified path: the entity view exposes the native Unity object at +0x10.
    // Fall back to the movement view -> managed Transform path when that
    // pointer is not a hierarchy-bearing Transform.
    U native = m.ptr(view,0x10), hierarchy = m.ptr(native,0x28);
    if (native <= 0x10000 || hierarchy <= 0x10000) {
        U movement = m.ptr(view,0x20);
        if (!m.isOneOf(movement,{"CharacterMovementView","MovementView"})) return {};
        U managed = m.ptr(movement,0x20);
        if (!m.is(managed,"Transform")) return {};
        native = m.ptr(managed,0x10);
        hierarchy = m.ptr(native,0x28);
    }
    if (native <= 0x10000 || hierarchy <= 0x10000) return {};
    U matrices = m.ptr(hierarchy,0x18), parents = m.ptr(hierarchy,0x20);
    U pointers = m.ptr(hierarchy,0x30);
    std::int32_t count = 0;
    if (!m.get(hierarchy+0x38,count) || count < 2 || count > 512) return {};
    std::vector<TRS> local(count);
    std::vector<std::int32_t> parent(count);
    std::vector<U> transforms(count);
    if (!m.array(matrices,local,false) || !m.array(parents,parent,true) ||
        !m.array(pointers,transforms,true) || !m.stable()) return {};
    for (int p : parent) if (p < -1 || p >= count) return {};
    for (int b = 0; b < count; ++b) {
        int p = b, steps = 0;
        while (p != -1 && steps < count) { p = parent[p]; ++steps; }
        if (p != -1) return {};
    }
    auto ancestor = [&](int a, int b) {
        for (; b != -1; b = parent[b]) if (a == b) return true;
        return false;
    };
    auto indexOf = [&](U n) {
        int index = -1;
        for (int b = 0; b < count; ++b) if (transforms[b] == n) {
            if (index != -1) return -1;
            index = b;
        }
        return index;
    };
    int root = indexOf(native);
    if (root < 0) return {};
    for (auto& t : local) {
        float n = t.q.x*t.q.x+t.q.y*t.q.y+t.q.z*t.q.z+t.q.w*t.q.w;
        if (!Finite(t.p) || !Finite(t.s) || !std::isfinite(n) ||
            n < 0.5f || n > 1.5f) return {};
        float inv = 1.0f/std::sqrt(n);
        t.q = {t.q.x*inv,t.q.y*inv,t.q.z*inv,t.q.w*inv};
    }
    std::vector<Vec> world(count);
    for (int b = 0; b < count; ++b) {
        Vec v = local[b].p;
        for (int p = parent[b]; p != -1; p = parent[p])
            v = Add(local[p].p,Rotate(local[p].q,Mul(local[p].s,v)));
        if (!Finite(v)) return {};
        world[b] = v;
    }
    auto resolve = [&](U transform) {
        if (!m.is(transform,"Transform")) return -1;
        U n = m.ptr(transform,0x10);
        if (n <= 0x10000 || m.ptr(n,0x28) != hierarchy) return -1;
        int index = indexOf(n);
        return index >= 0 && ancestor(root,index) ? index : -1;
    };
    U animation = m.ptr(view,0x28), weapon = m.ptr(view,0x30);
    if (!m.isOneOf(animation,{"CharacterAnimationView","AnimationView"}) ||
        !m.isOneOf(weapon,{"CharacterWithWeaponAnimationView","WithWeaponView"})) return {};
    std::vector<int> anchors;
    for (U offset : {U(0x58),U(0x60),U(0x68),U(0x70)}) {
        U limb = m.ptr(weapon,offset);
        if (!m.isOneOf(limb,{"LimbIKView","LimbIK"})) return {};
        U array = m.ptr(limb,0x40), length = 0;
        if (array <= 0x10000 || !m.get(array+0x18,length) ||
            length < 2 || length > static_cast<U>(count)) return {};
        std::vector<U> bones(static_cast<std::size_t>(length));
        if (!m.array(array+0x20,bones,true)) return {};
        std::vector<int> limbIndices;
        for (U bone : bones) {
            int index = resolve(bone);
            if (index < 0) return {};
            for (int other : limbIndices)
                if (other == index || (!ancestor(other,index) &&
                    !ancestor(index,other))) return {};
            limbIndices.push_back(index); anchors.push_back(index);
        }
    }
    U head = m.ptr(animation,0x28), torso = m.ptr(animation,0x30);
    if (!m.isOneOf(head,{"CharacterHeadManualControlView","HeadView"}) ||
        !m.isOneOf(torso,{"CharacterTorsoDeformationView","TorsoView"})) return {};
    int neck = resolve(m.ptr(head,0x20)), skull = resolve(m.ptr(head,0x28));
    int top = resolve(m.ptr(head,0x30));
    int spine2 = resolve(m.ptr(torso,0x28)), spine3 = resolve(m.ptr(torso,0x30));
    if (neck < 0 || skull < 0 || top < 0 || spine2 < 0 || spine3 < 0 ||
        neck == skull || !ancestor(neck,skull) || !ancestor(top,neck)) return {};
    for (int index : {neck,skull,top,spine2,spine3}) anchors.push_back(index);
    int lca = anchors.front();
    for (int index : anchors) {
        while (lca != -1 && !ancestor(lca,index)) lca = parent[lca];
        if (lca == -1) return {};
    }
    std::vector<unsigned char> selected(count,0);
    for (int index : anchors) {
        for (int p = index;; p = parent[p]) {
            if (!ancestor(root,p)) return {};
            selected[p] = 1;
            if (p == lca) break;
        }
    }
    if (!m.stable()) return {};
    std::vector<BoneSegment> result;
    for (int b = 0; b < count; ++b) {
        int p = parent[b];
        if (selected[b] && p >= 0 && selected[p])
            result.push_back({world[p],world[b]});
    }
    return result;
}

} // namespace Skeleton
