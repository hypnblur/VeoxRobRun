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

void Init(std::uintptr_t gameAssemblyBase)
{
    gaBase = gameAssemblyBase;
    std::lock_guard<std::mutex> lock(g_classCacheMutex);
    g_classCache.clear();
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
    const std::uintptr_t table = ReadPtr(gaBase + rr::ClassTable::Slot);
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

    const std::uintptr_t table = ReadPtr(gaBase + rr::ClassTable::Slot);
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
