#include "pch.h"
#include "Il2Cpp.h"
#include "RobRunSDK.h"
#include "Memory.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Il2Cpp {

static std::uintptr_t gaBase = 0;

// Positive class-name lookups only. A miss is deliberately not cached because
// the class may not be loaded into the table yet when the catalog is first
// requested.
static std::mutex g_classCacheMutex;
static std::unordered_map<std::string, std::uintptr_t> g_classCache;

// Resolved primitive-value field offsets, keyed by Il2CppClass*. Walking the
// field table (and its UTF-8 names) on every health/role read is far more
// expensive than the read itself, so the offset is resolved once per class.
static std::mutex g_primitiveMutex;
static std::unordered_map<std::uintptr_t, std::uintptr_t> g_primitiveOffsets;

// Loaded-class-table slot. Resolved once per session: the hardcoded RVA is
// preferred while it validates, otherwise the AOB scanner recovers it from the
// GetClassByIndex accessor. Guarded because resolution may run from the reader
// worker and the menu thread at the same time.
static std::mutex g_classTableMutex;
static std::uintptr_t g_classTableSlot = 0;
static bool g_classTableAttempted = false;

static bool ReadBytes(std::uintptr_t address, void* buffer, std::size_t size)
{
    if (!address || !buffer || !size) return false;
    return TargetProcess.Read(address, buffer, size);
}

static std::uintptr_t ReadPtr(std::uintptr_t address)
{
    std::uintptr_t value = 0;
    return ReadBytes(address, &value, sizeof(value)) ? value : 0;
}

static std::string ReadCString(std::uintptr_t address, std::size_t maxLength = 128)
{
    if (address <= 0x10000) return {};
    char buffer[128]{};
    const std::size_t size = (maxLength < sizeof(buffer)) ? maxLength : sizeof(buffer) - 1;
    if (ReadBytes(address, buffer, size)) {
        buffer[size] = '\0';
        return std::string(buffer);
    }
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        char c = 0;
        if (!ReadBytes(address + i, &c, 1) || !c) break;
        out.push_back(c);
    }
    return out;
}

static bool ValidPtr(std::uintptr_t value)
{
    return value > 0x10000 && value < 0x00007FFFFFFFFFFFull;
}

static bool ValidClass(std::uintptr_t klass)
{
    if (!ValidPtr(klass)) return false;
    const std::uintptr_t name = ReadPtr(klass + rr::Klass::name_ptr);
    if (!ValidPtr(name)) return false;
    char first = 0;
    return ReadBytes(name, &first, 1) && first >= 0x20 && first < 0x7F;
}

namespace {

struct SectionRange {
    std::uintptr_t va = 0;
    std::uint32_t size = 0;
};

// Reads a PE section header from the live module. Only the fields needed to
// scan code are returned; the header was already validated by
// VerifyFingerprint before this path is used.
bool ReadSection(const char* wanted, SectionRange& out)
{
    if (!gaBase || !wanted) return false;
    std::uint32_t lfanew = 0;
    if (!ReadBytes(gaBase + 0x3C, &lfanew, sizeof(lfanew)) || lfanew == 0 || lfanew > 0x1000)
        return false;
    const std::uintptr_t nt = gaBase + lfanew;
    std::uint16_t sectionCount = 0;
    std::uint16_t optionalSize = 0;
    if (!ReadBytes(nt + 6, &sectionCount, sizeof(sectionCount))) return false;
    if (!ReadBytes(nt + 20, &optionalSize, sizeof(optionalSize))) return false;
    if (sectionCount == 0 || sectionCount > 96 || optionalSize == 0 || optionalSize > 0x400)
        return false;

    const std::uintptr_t headers = nt + 24 + optionalSize;
    for (std::uint16_t i = 0; i < sectionCount; ++i) {
        const std::uintptr_t entry = headers + static_cast<std::uintptr_t>(i) * 40;
        char name[8]{};
        if (!ReadBytes(entry, name, sizeof(name))) return false;
        if (std::strncmp(name, wanted, sizeof(name)) != 0) continue;

        std::uint32_t virtualSize = 0;
        std::uint32_t virtualAddress = 0;
        if (!ReadBytes(entry + 8, &virtualSize, sizeof(virtualSize))) return false;
        if (!ReadBytes(entry + 12, &virtualAddress, sizeof(virtualAddress))) return false;
        if (virtualSize == 0 || virtualAddress == 0) return false;
        out.va = virtualAddress;
        out.size = virtualSize;
        return true;
    }
    return false;
}

// A slot is usable when it dereferences to an array that contains at least one
// printable Il2CppClass. This rejects stale pointers left behind by a remap.
bool ClassTableLooksValid(std::uintptr_t slotAddress)
{
    const std::uintptr_t table = ReadPtr(slotAddress);
    if (!ValidPtr(table)) return false;

    std::uintptr_t entries[16]{};
    if (!ReadBytes(table, entries, sizeof(entries))) return false;
    for (std::uintptr_t klass : entries) {
        if (ValidClass(klass)) return true;
    }
    return false;
}

// Scans .text for the GetClassByIndex accessor:
//
//   48 63 C8              movsxd rcx, eax
//   48 8B 05 <disp32>     mov    rax, [rip + disp32]  ; class-table slot
//   48 8B 04 C8           mov    rax, [rax + rcx*8]
//   C3                    ret
//
// The displacement is decoded into a module RVA. The scan is chunked so a
// multi-megabyte section never has to fit in one allocation or one DMA
// transaction.
std::uintptr_t ScanForClassTableSlot()
{
    SectionRange text{};
    if (!ReadSection(".text", text) || text.size < 16) return 0;

    static constexpr std::uint8_t kSignature[] = {
        0x48, 0x63, 0xC8, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x04, 0xC8, 0xC3
    };
    static constexpr bool kWildcard[] = {
        false, false, false, false, false, false, true, true, true, true,
        false, false, false, false, false
    };
    constexpr std::size_t kSignatureLength = sizeof(kSignature);
    constexpr std::size_t kChunk = 0x100000;

    std::vector<std::uint8_t> buffer(kChunk + kSignatureLength);
    for (std::size_t offset = 0; offset < text.size; offset += kChunk) {
        const std::size_t remaining = text.size - offset;
        const std::size_t want = (std::min)(kChunk + kSignatureLength, remaining);
        if (!ReadBytes(gaBase + text.va + offset, buffer.data(), want))
            continue;
        if (want < kSignatureLength) break;

        const std::size_t limit = want - kSignatureLength + 1;
        for (std::size_t i = 0; i < limit; ++i) {
            bool match = true;
            for (std::size_t j = 0; j < kSignatureLength; ++j) {
                if (!kWildcard[j] && buffer[i + j] != kSignature[j]) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;

            std::int32_t displacement = 0;
            std::memcpy(&displacement, buffer.data() + i + 6, sizeof(displacement));
            const std::uintptr_t matchRva = text.va + offset + i;
            const std::uintptr_t slot = static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(matchRva) + 10 + displacement);
            // The table slot lives in .data, past the end of .text.
            if (slot <= text.va + text.size) continue;
            return slot;
        }
    }
    return 0;
}

} // namespace

std::uintptr_t ClassTableSlotRva()
{
    std::lock_guard<std::mutex> lock(g_classTableMutex);
    if (g_classTableAttempted) return g_classTableSlot;
    g_classTableAttempted = true;

    if (ClassTableLooksValid(gaBase + rr::ClassTable::Slot)) {
        g_classTableSlot = rr::ClassTable::Slot;
        return g_classTableSlot;
    }

    const std::uintptr_t scanned = ScanForClassTableSlot();
    if (scanned && ClassTableLooksValid(gaBase + scanned)) {
        g_classTableSlot = scanned;
        return g_classTableSlot;
    }

    // Keep the header value so the caller's own validation can report the
    // failure; a later session reopen will retry the scan.
    g_classTableSlot = rr::ClassTable::Slot;
    return g_classTableSlot;
}

void Init(std::uintptr_t gameAssemblyBase)
{
    gaBase = gameAssemblyBase;
    std::lock_guard<std::mutex> lock(g_classCacheMutex);
    g_classCache.clear();
    std::lock_guard<std::mutex> primitiveLock(g_primitiveMutex);
    g_primitiveOffsets.clear();
    std::lock_guard<std::mutex> tableLock(g_classTableMutex);
    g_classTableSlot = 0;
    g_classTableAttempted = false;
}

std::uintptr_t Base()
{
    return gaBase;
}

bool VerifyFingerprint()
{
    if (!gaBase) return false;
    std::uint32_t lfanew = 0;
    if (!ReadBytes(gaBase + 0x3C, &lfanew, sizeof(lfanew)) || lfanew == 0 || lfanew > 0x1000)
        return false;
    const std::uintptr_t nt = gaBase + lfanew;
    std::uint32_t signature = 0;
    if (!ReadBytes(nt, &signature, sizeof(signature)) || signature != 0x00004550u)
        return false;
    std::uint32_t timestamp = 0;
    std::uint32_t sizeOfImage = 0;
    if (!ReadBytes(nt + 0x08, &timestamp, sizeof(timestamp))) return false;
    if (!ReadBytes(nt + 0x50, &sizeOfImage, sizeof(sizeOfImage))) return false;
    return timestamp == rr::kPeTimestamp && sizeOfImage == rr::kPeSizeOfImage;
}

namespace {

struct AccessorCheck {
    std::uintptr_t rva;
    std::uint8_t bytes[8];
    std::size_t length;
};

constexpr AccessorCheck kAccessors[] = {
    { rr::AccessorRva::ClassName,      { 0x48, 0x8B, 0x41, 0x10, 0xC3 }, 5 },
    { rr::AccessorRva::ClassNamespace, { 0x48, 0x8B, 0x41, 0x18, 0xC3 }, 5 },
    { rr::AccessorRva::StaticFields,   { 0x48, 0x8B, 0x81, 0xB8, 0x00, 0x00, 0x00, 0xC3 }, 8 },
    { rr::AccessorRva::InstanceSize,   { 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0xC3 }, 7 },
    { rr::AccessorRva::ClassFlags,     { 0x8B, 0x81, 0x18, 0x01, 0x00, 0x00, 0xC3 }, 7 },
    { rr::AccessorRva::FieldOffset,    { 0x48, 0x63, 0x41, 0x18, 0xC3 }, 5 },
    { rr::AccessorRva::FieldType,      { 0x48, 0x8B, 0x41, 0x08, 0xC3 }, 5 },
    { rr::AccessorRva::TypeCode,       { 0x0F, 0xBE, 0x41, 0x0A, 0xC3 }, 5 },
};

} // namespace

bool VerifyAccessors()
{
    if (!gaBase) return false;
    for (const auto& check : kAccessors) {
        std::uint8_t actual[8]{};
        if (!ReadBytes(gaBase + check.rva, actual, check.length)) return false;
        if (std::memcmp(actual, check.bytes, check.length) != 0) return false;
    }
    return true;
}

std::uintptr_t GetClassByIndex(int index)
{
    if (!gaBase || index < 0) return 0;
    const std::uintptr_t slot = ClassTableSlotRva();
    if (!slot) return 0;
    const std::uintptr_t table = ReadPtr(gaBase + slot);
    if (!ValidPtr(table)) return 0;
    const std::uintptr_t klass = ReadPtr(table + static_cast<std::uintptr_t>(index) * 8);
    return ValidClass(klass) ? klass : 0;
}

ClassInfo GetClassInfo(std::uintptr_t klass)
{
    ClassInfo info{};
    if (!ValidClass(klass)) return info;
    info.ptr = klass;
    info.name = ClassNameOfClass(klass);
    info.ns = NamespaceOfClass(klass);
    return info;
}

std::string ClassNameOfClass(std::uintptr_t klass)
{
    if (!ValidPtr(klass)) return {};
    return ReadCString(ReadPtr(klass + rr::Klass::name_ptr));
}

std::string NamespaceOfClass(std::uintptr_t klass)
{
    if (!ValidPtr(klass)) return {};
    return ReadCString(ReadPtr(klass + rr::Klass::namespace_ptr));
}

std::uintptr_t ClassOf(std::uintptr_t object)
{
    if (!ValidPtr(object)) return 0;
    return ReadPtr(object + rr::Object::klass);
}

std::string ClassNameOf(std::uintptr_t object)
{
    if (!ValidPtr(object)) return {};
    return ClassNameOfClass(ClassOf(object));
}

bool IsClass(std::uintptr_t object, const char* name)
{
    if (!name) return false;
    return ClassNameOf(object) == name;
}

bool IsClassOrDerived(std::uintptr_t object, const char* name)
{
    if (!name || !ValidPtr(object)) return false;
    std::uintptr_t klass = ReadPtr(object + rr::Object::klass);
    for (int depth = 0; ValidPtr(klass) && depth < 32; ++depth) {
        if (ClassNameOfClass(klass) == name) return true;
        klass = ReadPtr(klass + rr::Klass::parent);
    }
    return false;
}

std::uintptr_t StaticFields(std::uintptr_t klass)
{
    if (!ValidPtr(klass)) return 0;
    return ReadPtr(klass + rr::Klass::static_fields);
}

std::uintptr_t GetStaticFieldsForClass(std::uintptr_t klass)
{
    return StaticFields(klass);
}

std::vector<Field> Fields(std::uintptr_t klass, bool includeParents)
{
    std::vector<Field> out;
    for (int depth = 0; ValidPtr(klass) && depth < 32; ++depth) {
        const std::uintptr_t fields = ReadPtr(klass + rr::Klass::fields);
        std::uint16_t count = 0;
        if (!ReadBytes(klass + rr::Klass::field_count, &count, sizeof(count)))
            break;
        if (count > 4096) break;
        if (ValidPtr(fields)) {
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::uintptr_t fi = fields + static_cast<std::uintptr_t>(i) * rr::FieldInfo::stride;
                Field field{};
                field.info = fi;
                const std::uintptr_t namePtr = ReadPtr(fi + rr::FieldInfo::name);
                if (!ValidPtr(namePtr)) continue;
                field.name = ReadCString(namePtr);
                if (field.name.empty()) continue;
                field.type = ReadPtr(fi + rr::FieldInfo::type);
                std::int32_t offset = 0;
                if (!ReadBytes(fi + rr::FieldInfo::offset, &offset, sizeof(offset))) continue;
                field.offset = offset;
                std::uint8_t code = 0;
                if (ValidPtr(field.type)) ReadBytes(field.type + rr::Il2CppType::code, &code, 1);
                field.typeCode = code;
                out.push_back(std::move(field));
            }
        }
        if (!includeParents) break;
        klass = ReadPtr(klass + rr::Klass::parent);
    }
    return out;
}

bool FindField(std::uintptr_t klass, const char* name, Field& out, bool includeParents)
{
    if (!name) return false;
    const auto fields = Fields(klass, includeParents);
    for (const auto& field : fields) {
        if (field.name == name) {
            out = field;
            return true;
        }
    }
    return false;
}

std::int32_t InstanceSize(std::uintptr_t klass)
{
    if (!ValidPtr(klass)) return 0;
    std::uint32_t size = 0;
    if (!ReadBytes(klass + rr::Klass::instance_size, &size, sizeof(size))) return 0;
    return (size > 0 && size <= 0x100000) ? static_cast<std::int32_t>(size) : 0;
}

std::uint8_t TypeCode(std::uintptr_t type)
{
    if (!ValidPtr(type)) return 0;
    std::uint8_t code = 0;
    ReadBytes(type + rr::Il2CppType::code, &code, 1);
    return code;
}

namespace {

enum class PrimitiveKind { Bool, Float, Int, Int64, Vector3 };

bool ValidatePrimitive(PrimitiveKind kind, const void* value)
{
    switch (kind) {
    case PrimitiveKind::Bool: {
        const auto v = *static_cast<const std::uint8_t*>(value);
        return v <= 1;
    }
    case PrimitiveKind::Float: {
        const float v = *static_cast<const float*>(value);
        return std::isfinite(v);
    }
    case PrimitiveKind::Int:
        return true;
    case PrimitiveKind::Int64:
        return true;
    case PrimitiveKind::Vector3: {
        const auto* v = static_cast<const Vector3*>(value);
        return std::isfinite(v->x) && std::isfinite(v->y) && std::isfinite(v->z);
    }
    }
    return false;
}

bool ReadPrimitive(std::uintptr_t wrapper, PrimitiveKind kind, void* out, std::size_t size)
{
    if (!ValidPtr(wrapper) || !out) return false;

    // Fast path: the value field offset for this wrapper class was resolved on
    // an earlier pass. This turns a field-table walk into one read.
    const std::uintptr_t wrapperClass = ClassOf(wrapper);
    if (wrapperClass) {
        std::uintptr_t cachedOffset = 0;
        {
            std::lock_guard<std::mutex> lock(g_primitiveMutex);
            const auto it = g_primitiveOffsets.find(wrapperClass);
            if (it != g_primitiveOffsets.end()) cachedOffset = it->second;
        }
        if (cachedOffset && cachedOffset < 0x1000) {
            if (ReadBytes(wrapper + cachedOffset, out, size) &&
                ValidatePrimitive(kind, out)) {
                return true;
            }
            // The class layout changed; drop the cached offset and re-resolve.
            std::lock_guard<std::mutex> lock(g_primitiveMutex);
            g_primitiveOffsets.erase(wrapperClass);
        }
    }

    static const char* const kValueNames[] = {
        "<Value>k__BackingField", "_value", "value", "m_value"
    };

    std::uintptr_t current = wrapper;
    for (int depth = 0; depth < 3 && ValidPtr(current); ++depth) {
        // FindField/Fields take an Il2CppClass*, not an instance pointer.
        const std::uintptr_t klass = ClassOf(current);
        Field valueField{};
        bool found = false;
        for (const char* name : kValueNames) {
            if (klass && FindField(klass, name, valueField, true) &&
                valueField.offset > 0 && valueField.offset < 0x1000) {
                found = true;
                break;
            }
        }
        if (found) {
            if (ReadBytes(current + static_cast<std::uintptr_t>(valueField.offset), out, size) &&
                ValidatePrimitive(kind, out)) {
                if (klass) {
                    std::lock_guard<std::mutex> lock(g_primitiveMutex);
                    g_primitiveOffsets[klass] = static_cast<std::uintptr_t>(valueField.offset);
                }
                return true;
            }
        }

        Field dataField{};
        if (klass && FindField(klass, "_data", dataField, true) &&
            dataField.offset > 0 && dataField.offset < 0x1000) {
            current = ReadPtr(current + static_cast<std::uintptr_t>(dataField.offset));
            continue;
        }
        break;
    }

    // Verified inflated wrapper fallback for this build.
    if (ReadBytes(wrapper + rr::PrimitiveData::value, out, size) &&
        ValidatePrimitive(kind, out)) {
        return true;
    }
    return false;
}

} // namespace

bool ReadPrimitiveBool(std::uintptr_t wrapper, bool& out)
{
    std::uint8_t value = 0;
    if (!ReadPrimitive(wrapper, PrimitiveKind::Bool, &value, sizeof(value))) return false;
    out = value != 0;
    return true;
}

bool ReadPrimitiveFloat(std::uintptr_t wrapper, float& out)
{
    return ReadPrimitive(wrapper, PrimitiveKind::Float, &out, sizeof(out));
}

bool ReadPrimitiveInt(std::uintptr_t wrapper, std::int32_t& out)
{
    return ReadPrimitive(wrapper, PrimitiveKind::Int, &out, sizeof(out));
}

bool ReadPrimitiveInt64(std::uintptr_t wrapper, std::int64_t& out)
{
    return ReadPrimitive(wrapper, PrimitiveKind::Int64, &out, sizeof(out));
}

bool ReadPrimitiveVector3(std::uintptr_t wrapper, Vector3& out)
{
    return ReadPrimitive(wrapper, PrimitiveKind::Vector3, &out, sizeof(out));
}

std::uintptr_t FindClassByName(const char* name, int maxIndex)
{
    if (!gaBase || !name || !*name) return 0;

    std::lock_guard<std::mutex> lock(g_classCacheMutex);
    const auto it = g_classCache.find(name);
    if (it != g_classCache.end()) return it->second;

    const std::uintptr_t slot = ClassTableSlotRva();
    if (!slot) return 0;
    const std::uintptr_t table = ReadPtr(gaBase + slot);
    if (!ValidPtr(table)) return 0;

    constexpr int kChunk = 512;
    std::uintptr_t pointers[kChunk]{};
    for (int start = 0; start < maxIndex; start += kChunk) {
        const int count = (maxIndex - start < kChunk) ? (maxIndex - start) : kChunk;
        if (!ReadBytes(table + static_cast<std::uintptr_t>(start) * 8,
                pointers, static_cast<std::size_t>(count) * sizeof(std::uintptr_t))) {
            break;
        }
        for (int i = 0; i < count; ++i) {
            const std::uintptr_t klass = pointers[i];
            if (!ValidClass(klass)) continue;
            if (ClassNameOfClass(klass) == name) {
                g_classCache[name] = klass;
                return klass;
            }
        }
    }

    return 0;
}

} // namespace Il2Cpp
