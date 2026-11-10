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

static bool SnapshotStable(std::uintptr_t dictionary,
    std::uintptr_t entries, std::int32_t count, std::int32_t version)
{
    return ReadPtr(dictionary + rr::Dict::entries) == entries &&
        ReadInt32(dictionary + rr::Dict::count) == count &&
        ReadInt32(dictionary + rr::Dict::version) == version;
}

bool ReadDictInt(std::uintptr_t dictionary, std::vector<DictEntry>& out,
    bool requireStable)
{
    out.clear();
    if (dictionary <= 0x10000) return false;

    const std::uintptr_t entries = ReadPtr(dictionary + rr::Dict::entries);
    const std::int32_t count = ReadInt32(dictionary + rr::Dict::count);
    const std::int32_t version = ReadInt32(dictionary + rr::Dict::version);

    if (entries <= 0x10000 || count < 0 || count > 65536) return false;

    const std::uint64_t capacity = ReadUInt64(entries + rr::Array::max_length);
    if (capacity < static_cast<std::uint64_t>(count) || capacity > 0x1000000ull)
        return false;

    std::vector<DictEntry> snapshot(static_cast<std::size_t>(count));
    if (count > 0 && !ReadBytes(entries + rr::Array::vector, snapshot.data(),
            snapshot.size() * sizeof(DictEntry))) {
        return false;
    }

    if (requireStable && !SnapshotStable(dictionary, entries, count, version))
        return false;

    out.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        if (entry.next < rr::Dict::entry_allocated_min) continue;
        if (entry.hash < 0) continue; // removed/free slot
        if (!entry.value) continue;
        out.push_back(entry);
    }
    return true;
}

bool ReadDictRef(std::uintptr_t dictionary, std::vector<RefDictEntry>& out,
    bool requireStable)
{
    out.clear();
    if (dictionary <= 0x10000) return false;

    const std::uintptr_t entries = ReadPtr(dictionary + rr::Dict::entries);
    const std::int32_t count = ReadInt32(dictionary + rr::Dict::count);
    const std::int32_t version = ReadInt32(dictionary + rr::Dict::version);

    if (entries <= 0x10000 || count < 0 || count > 65536) return false;

    const std::uint64_t capacity = ReadUInt64(entries + rr::Array::max_length);
    if (capacity < static_cast<std::uint64_t>(count) || capacity > 0x1000000ull)
        return false;

    std::vector<RefDictEntry> snapshot(static_cast<std::size_t>(count));
    if (count > 0 && !ReadBytes(entries + rr::Array::vector, snapshot.data(),
            snapshot.size() * sizeof(RefDictEntry))) {
        return false;
    }

    if (requireStable && !SnapshotStable(dictionary, entries, count, version))
        return false;

    out.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        if (entry.next < rr::Dict::entry_allocated_min) continue;
        if (entry.hash < 0) continue;
        if (!entry.key || !entry.value) continue;
        out.push_back(entry);
    }
    return true;
}

bool ReadList(std::uintptr_t list, std::vector<std::uintptr_t>& out,
    bool requireStable)
{
    out.clear();
    if (list <= 0x10000) return false;

    const std::uintptr_t items = ReadPtr(list + rr::List::items);
    const std::int32_t count = ReadInt32(list + rr::List::count);
    if (items <= 0x10000 || count < 0 || count > 65536) return false;

    const std::uint64_t capacity = ReadUInt64(items + rr::Array::max_length);
    if (capacity < static_cast<std::uint64_t>(count)) return false;

    std::vector<std::uintptr_t> snapshot(static_cast<std::size_t>(count));
    if (count > 0 && !ReadBytes(items + rr::Array::vector, snapshot.data(),
            snapshot.size() * sizeof(std::uintptr_t))) {
        return false;
    }

    if (requireStable &&
        (ReadPtr(list + rr::List::items) != items ||
         ReadInt32(list + rr::List::count) != count)) {
        return false;
    }

    out = std::move(snapshot);
    return true;
}

} // namespace RobRunMemory
