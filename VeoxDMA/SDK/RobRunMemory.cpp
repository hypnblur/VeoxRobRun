#include "pch.h"
#include "RobRunMemory.h"
#include "RobRunSDK.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include <cmath>
#include <cstring>

namespace RobRunMemory {

bool ReadBytes(std::uintptr_t address, void* buffer, std::size_t size)
{
    if (!address || !buffer || !size) return false;
    return TargetProcess.Read(address, buffer, size);
}

bool ReadBytesFresh(std::uintptr_t address, void* buffer, std::size_t size)
{
    if (!address || !buffer || !size) return false;
    return TargetProcess.ReadFresh(address, buffer, size);
}

// A dictionary's header fields are contiguous (entries, count, free list,
// free count, version). Reading them in one transaction instead of three
// removes a large amount of DMA traffic on the world pass.
namespace {

struct DictHeader {
    std::uintptr_t entries = 0;
    std::int32_t count = 0;
    std::int32_t freeList = 0;
    std::int32_t freeCount = 0;
    std::int32_t version = 0;
};
static_assert(sizeof(DictHeader) == 0x18, "Dictionary header layout");

struct ListHeader {
    std::uintptr_t items = 0;
    std::int32_t count = 0;
    std::int32_t version = 0;
};
static_assert(sizeof(ListHeader) == 0x10, "List header layout");

bool ReadDictHeader(std::uintptr_t dictionary, DictHeader& out)
{
    if (dictionary <= 0x10000) return false;
    if (!ReadBytes(dictionary + rr::Dict::entries, &out, sizeof(out))) return false;
    return out.entries > 0x10000 && out.count >= 0 && out.count <= 65536;
}

bool DictHeaderMatches(const DictHeader& before, const DictHeader& after,
    bool strictVersion)
{
    if (before.entries != after.entries || before.count != after.count)
        return false;
    return !strictVersion || before.version == after.version;
}

template <typename T>
bool ReadDictOnce(std::uintptr_t dictionary, std::vector<T>& out,
    bool requireStable)
{
    out.clear();
    DictHeader header;
    if (!ReadDictHeader(dictionary, header)) return false;

    // Read the backing-array length and the entries in one transaction. The
    // old path issued a separate read for capacity and another for the data.
    const std::size_t elementBytes =
        static_cast<std::size_t>(header.count) * sizeof(T);
    std::vector<unsigned char> raw(sizeof(std::uint64_t) + elementBytes);
    if (!ReadBytes(header.entries + rr::Array::max_length, raw.data(), raw.size()))
        return false;

    std::uint64_t capacity = 0;
    std::memcpy(&capacity, raw.data(), sizeof(capacity));
    if (capacity < static_cast<std::uint64_t>(header.count) ||
        capacity > 0x1000000ull)
        return false;

    std::vector<T> snapshot(static_cast<std::size_t>(header.count));
    if (elementBytes)
        std::memcpy(snapshot.data(), raw.data() + sizeof(capacity), elementBytes);

    // Re-read the header. A structural change while the array was copied means
    // the snapshot belongs to a different bucket generation. Version-only
    // changes are tolerated when the caller opts out of strict stability, which
    // the fast-moving world dictionaries do.
    DictHeader after;
    if (!ReadDictHeader(dictionary, after) ||
        !DictHeaderMatches(header, after, requireStable))
        return false;

    out.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        if (entry.next < rr::Dict::entry_allocated_min) continue;
        if (entry.hash < 0) continue;
        if (!entry.value) continue;
        out.push_back(entry);
    }
    return true;
}

} // namespace

bool LooksLikeDictionary(std::uintptr_t dictionary)
{
    DictHeader header;
    if (!ReadDictHeader(dictionary, header)) return false;
    std::uint64_t capacity = 0;
    if (!ReadBytes(header.entries + rr::Array::max_length, &capacity,
            sizeof(capacity)))
        return false;
    return capacity >= static_cast<std::uint64_t>(header.count) &&
        capacity <= 0x1000000ull;
}

std::uintptr_t ReadPtr(std::uintptr_t address)
{
    std::uintptr_t value = 0;
    return ReadBytes(address, &value, sizeof(value)) ? value : 0;
}

std::int32_t ReadInt32(std::uintptr_t address)
{
    std::int32_t value = 0;
    ReadBytes(address, &value, sizeof(value));
    return value;
}

std::uint32_t ReadUInt32(std::uintptr_t address)
{
    std::uint32_t value = 0;
    ReadBytes(address, &value, sizeof(value));
    return value;
}

std::uint64_t ReadUInt64(std::uintptr_t address)
{
    std::uint64_t value = 0;
    ReadBytes(address, &value, sizeof(value));
    return value;
}

float ReadFloat(std::uintptr_t address)
{
    float value = 0.f;
    ReadBytes(address, &value, sizeof(value));
    return value;
}

bool ReadVector3(std::uintptr_t address, Vector3& out)
{
    return ReadBytes(address, &out, sizeof(out)) && Finite(out);
}

bool Finite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ReadString(std::uintptr_t stringObject, std::wstring& out, std::size_t maxLength)
{
    out.clear();
    if (stringObject <= 0x10000) return false;

    std::int32_t length = 0;
    if (!ReadBytes(stringObject + rr::Il2CppString::length, &length, sizeof(length)))
        return false;
    if (length < 0 || static_cast<std::size_t>(length) > maxLength)
        return false;
    if (length == 0) return true;

    std::vector<wchar_t> buffer(static_cast<std::size_t>(length));
    if (!ReadBytes(stringObject + rr::Il2CppString::chars, buffer.data(),
            buffer.size() * sizeof(wchar_t))) {
        return false;
    }
    out.assign(buffer.begin(), buffer.end());
    return true;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        out.data(), length, nullptr, nullptr);
    return out;
}

bool ReadWrapperString(std::uintptr_t wrapper, std::wstring& out)
{
    out.clear();
    if (wrapper <= 0x10000) return false;
    const std::uintptr_t stringObject = ReadPtr(wrapper + rr::BattleCharacterSlotData::wrapper_value);
    return ReadString(stringObject, out);
}

bool ReadObjectString(std::uintptr_t object, std::wstring& out)
{
    out.clear();
    return ReadString(object, out);
}

bool IsClass(std::uintptr_t object, const char* name)
{
    return Il2Cpp::IsClass(object, name);
}

bool ReadDictInt(std::uintptr_t dictionary, std::vector<DictEntry>& out,
    bool requireStable)
{
    // A busy dictionary can change between the header read and the array copy.
    // Retrying once recovers most transient failures instead of dropping the
    // whole collection for that pass.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ReadDictOnce(dictionary, out, requireStable)) return true;
    }
    out.clear();
    return false;
}

bool ReadDictRef(std::uintptr_t dictionary, std::vector<RefDictEntry>& out,
    bool requireStable)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ReadDictOnce(dictionary, out, requireStable)) {
            // Reference dictionaries may legitimately hold a null value.
            out.erase(std::remove_if(out.begin(), out.end(),
                [](const RefDictEntry& entry) {
                    return !entry.key || !entry.value;
                }), out.end());
            return true;
        }
    }
    out.clear();
    return false;
}

bool ReadList(std::uintptr_t list, std::vector<std::uintptr_t>& out,
    bool requireStable)
{
    out.clear();
    if (list <= 0x10000) return false;

    ListHeader header;
    if (!ReadBytes(list + rr::List::items, &header, sizeof(header)))
        return false;
    if (header.items <= 0x10000 || header.count < 0 || header.count > 65536)
        return false;

    const std::size_t elementBytes =
        static_cast<std::size_t>(header.count) * sizeof(std::uintptr_t);
    std::vector<unsigned char> raw(sizeof(std::uint64_t) + elementBytes);
    if (!ReadBytes(header.items + rr::Array::max_length, raw.data(), raw.size()))
        return false;

    std::uint64_t capacity = 0;
    std::memcpy(&capacity, raw.data(), sizeof(capacity));
    if (capacity < static_cast<std::uint64_t>(header.count))
        return false;

    std::vector<std::uintptr_t> snapshot(static_cast<std::size_t>(header.count));
    if (elementBytes)
        std::memcpy(snapshot.data(), raw.data() + sizeof(capacity), elementBytes);

    if (requireStable) {
        ListHeader after;
        if (!ReadBytes(list + rr::List::items, &after, sizeof(after)))
            return false;
        if (after.items != header.items || after.count != header.count)
            return false;
    }

    out = std::move(snapshot);
    return true;
}

} // namespace RobRunMemory
