#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../Misc/Vector.h"

namespace RobRunMemory {

struct DictEntry {
    std::int32_t hash = 0;
    std::int32_t next = 0;
    std::int32_t key = 0;
    std::int32_t pad = 0;
    std::uint64_t value = 0;
};
static_assert(sizeof(DictEntry) == 0x18, "Dictionary<int,reference> entry stride");

struct RefDictEntry {
    std::int32_t hash = 0;
    std::int32_t next = 0;
    std::uint64_t key = 0;
    std::uint64_t value = 0;
};
static_assert(sizeof(RefDictEntry) == 0x18, "Dictionary<reference,reference> entry stride");

bool ReadBytes(std::uintptr_t address, void* buffer, std::size_t size);
// Bypasses VMM's data cache. Only for small, fast-changing values such as
// entity positions and animation poses; each call is a device transaction.
bool ReadBytesFresh(std::uintptr_t address, void* buffer, std::size_t size);
std::uintptr_t ReadPtr(std::uintptr_t address);
std::int32_t ReadInt32(std::uintptr_t address);
std::uint32_t ReadUInt32(std::uintptr_t address);
std::uint64_t ReadUInt64(std::uintptr_t address);
float ReadFloat(std::uintptr_t address);
bool ReadVector3(std::uintptr_t address, Vector3& out);

bool Finite(const Vector3& value);

// UTF-16 managed string. The length at +0x10 is a code-unit count; the data
// at +0x14 is not a char* and is not null-terminated.
bool ReadString(std::uintptr_t stringObject, std::wstring& out,
    std::size_t maxLength = 256);
std::string WideToUtf8(const std::wstring& value);
bool ReadWrapperString(std::uintptr_t wrapper, std::wstring& out);
bool ReadObjectString(std::uintptr_t object, std::wstring& out);

// Dictionary<int, reference>. Reads are revalidated against the version,
// count and entries pointer before the snapshot is accepted.
bool ReadDictInt(std::uintptr_t dictionary, std::vector<DictEntry>& out,
    bool requireStable = true);

// Dictionary<reference, reference>.
bool ReadDictRef(std::uintptr_t dictionary, std::vector<RefDictEntry>& out,
    bool requireStable = true);

bool ReadList(std::uintptr_t list, std::vector<std::uintptr_t>& out,
    bool requireStable = true);

// Returns true when the object's runtime class name matches.
bool IsClass(std::uintptr_t object, const char* name);

} // namespace RobRunMemory
