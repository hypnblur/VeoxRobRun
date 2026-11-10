#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../Misc/Vector.h"

// Read-only helpers for the IL2CPP metadata structures documented in the
// ROB RUN reference. Nothing here calls into the game; every accessor is a
// validated DMA read.
namespace Il2Cpp {

struct ClassInfo {
    std::uintptr_t ptr = 0;
    std::string name;
    std::string ns;
};

struct Field {
    std::uintptr_t info = 0;
    std::string name;
    std::uintptr_t type = 0;
    std::int32_t offset = 0;
    std::uint8_t typeCode = 0;
};

void Init(std::uintptr_t gameAssemblyBase);
std::uintptr_t Base();

// PE fingerprint and accessor-byte validation. Call once after attach and
// again after a module remap.
bool VerifyFingerprint();
bool VerifyAccessors();

// class = Q(Q(base + class_table_slot) + index*8)
std::uintptr_t GetClassByIndex(int index);

ClassInfo GetClassInfo(std::uintptr_t klass);
std::string ClassNameOf(std::uintptr_t object);
// Resolves the Il2CppClass* of a managed object. Use this before passing an
// object to FindField/Fields, which expect a class, not an instance.
std::uintptr_t ClassOf(std::uintptr_t object);
std::string ClassNameOfClass(std::uintptr_t klass);
std::string NamespaceOfClass(std::uintptr_t klass);

bool IsClass(std::uintptr_t object, const char* name);
bool IsClassOrDerived(std::uintptr_t object, const char* name);

std::uintptr_t StaticFields(std::uintptr_t klass);
std::uintptr_t GetStaticFieldsForClass(std::uintptr_t klass);

// Walk the runtime class (including parents). Open-generic dump offsets of
// zero are rejected by callers through the returned offset value.
std::vector<Field> Fields(std::uintptr_t klass, bool includeParents = true);
bool FindField(std::uintptr_t klass, const char* name, Field& out,
    bool includeParents = true);

std::int32_t InstanceSize(std::uintptr_t klass);
std::uint8_t TypeCode(std::uintptr_t type);

// Primitive wrappers. The resolver accepts <Value>k__BackingField, _value,
// value and m_value, and follows a _data wrapper when present.
bool ReadPrimitiveBool(std::uintptr_t wrapper, bool& out);
bool ReadPrimitiveFloat(std::uintptr_t wrapper, float& out);
bool ReadPrimitiveInt(std::uintptr_t wrapper, std::int32_t& out);
bool ReadPrimitiveInt64(std::uintptr_t wrapper, std::int64_t& out);
bool ReadPrimitiveVector3(std::uintptr_t wrapper, Vector3& out);

// Bounded, cached class-name lookup. Expensive; intended for one-off catalog
// resolution, never for per-frame work.
std::uintptr_t FindClassByName(const char* name, int maxIndex = 45000);

} // namespace Il2Cpp
