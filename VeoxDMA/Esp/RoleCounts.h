#pragma once
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "RobRunReader.h"

namespace Esp {

enum class RoleBucket {
	Police,
	Civilian,
	Criminal,
	Unknown
};

namespace detail {

inline bool ContainsInsensitive(std::string_view haystack, std::string_view needle)
{
	if (needle.empty() || haystack.size() < needle.size())
		return false;
	const std::size_t limit = haystack.size() - needle.size();
	for (std::size_t i = 0; i <= limit; ++i) {
		bool match = true;
		for (std::size_t j = 0; j < needle.size(); ++j) {
			const unsigned char hc = static_cast<unsigned char>(haystack[i + j]);
			const unsigned char nc = static_cast<unsigned char>(needle[j]);
			if (std::tolower(hc) != std::tolower(nc)) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

inline bool WithinRadius300m(const Vector3& a, const Vector3& b)
{
	if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
		!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z))
		return false;
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return (dx * dx + dy * dy + dz * dz) <= (300.f * 300.f);
}

} // namespace detail

// Case-insensitive, substring-based role classifier shared by the in-game map
// overlay and the web radar so both surfaces always agree.
inline RoleBucket ClassifyRole(int role, std::string_view roleName)
{
	if (roleName.empty()) {
		if (role == 1257638368) return RoleBucket::Criminal;
		if (role == 6)          return RoleBucket::Civilian; // Hubber
		return RoleBucket::Unknown;
	}

	if (detail::ContainsInsensitive(roleName, "police") ||
		detail::ContainsInsensitive(roleName, "cop") ||
		detail::ContainsInsensitive(roleName, "officer") ||
		detail::ContainsInsensitive(roleName, "law"))
	{
		return RoleBucket::Police;
	}

	if (detail::ContainsInsensitive(roleName, "criminal") ||
		detail::ContainsInsensitive(roleName, "gang") ||
		detail::ContainsInsensitive(roleName, "mafia") ||
		detail::ContainsInsensitive(roleName, "cartel") ||
		detail::ContainsInsensitive(roleName, "convict"))
	{
		return RoleBucket::Criminal;
	}

	if (detail::ContainsInsensitive(roleName, "civilian") ||
		detail::ContainsInsensitive(roleName, "citizen") ||
		detail::ContainsInsensitive(roleName, "hubber") ||
		detail::ContainsInsensitive(roleName, "worker"))
	{
		return RoleBucket::Civilian;
	}

	return RoleBucket::Unknown;
}

struct RoleCounts {
	int police = 0;
	int civilian = 0;
	int criminal = 0;
	int unknown = 0;
	int total = 0;
};

struct RemoteDriverHint {
	int vehicleId = 0;
	std::int32_t entityId = -1;
	int role = 0;
	std::string_view roleName;
	bool isMoving = false;
};

// Counts living non-local players in an already-published Snapshot.
// scope: 0 = Full Map (includes active remote drivers in fastVehicles), 1 = In 300m Radius.
// Teammates, knocked-out, and handcuffed players remain counted.
inline RoleCounts CountRoles(
	const RobRunReader::Snapshot& snap,
	int scope = 0,
	const RemoteDriverHint* hints = nullptr,
	std::size_t hintCount = 0)
{
	RoleCounts out;
	const bool radiusOnly = (scope == 1) && !snap.localPosition.IsZero();

	auto addBucket = [&out](RoleBucket b) {
		switch (b) {
		case RoleBucket::Police:   ++out.police;   break;
		case RoleBucket::Civilian: ++out.civilian; break;
		case RoleBucket::Criminal: ++out.criminal; break;
		case RoleBucket::Unknown:
		default:                   ++out.unknown;  break;
		}
	};

	std::uint64_t localPublicId = 0;
	std::int32_t countedEntityIds[128]{};
	std::size_t countedEntityCount = 0;
	std::uint64_t countedPublicIds[128]{};
	std::size_t countedPublicCount = 0;

	auto isEntityCounted = [&](std::int32_t eid) -> bool {
		if (eid <= 0) return false;
		for (std::size_t i = 0; i < countedEntityCount; ++i)
			if (countedEntityIds[i] == eid) return true;
		return false;
	};
	auto isPublicCounted = [&](std::uint64_t pid) -> bool {
		if (pid == 0) return false;
		for (std::size_t i = 0; i < countedPublicCount; ++i)
			if (countedPublicIds[i] == pid) return true;
		return false;
	};

	for (const auto& ent : snap.entities) {
		if (ent.entityId == snap.localEntityId) {
			if (ent.publicId > 0) localPublicId = ent.publicId;
			continue;
		}
		if (ent.isDead)
			continue;
		if (radiusOnly && !detail::WithinRadius300m(ent.position, snap.localPosition))
			continue;

		addBucket(ClassifyRole(ent.role, ent.roleName));
		if (ent.entityId > 0 && countedEntityCount < 128)
			countedEntityIds[countedEntityCount++] = ent.entityId;
		if (ent.publicId > 0 && countedPublicCount < 128)
			countedPublicIds[countedPublicCount++] = ent.publicId;
	}

	// Include active drivers in remote vehicles outside the 300 m entity bubble:
	// a vehicle is considered actively driven if fv.hasDriver is set OR it moved
	// recently (hint.isMoving), since hasDriver is often not replicated past ~300 m.
	// Each actively driving remote vehicle counts as 1 driver.
	if (!snap.fastVehicles.empty()) {
		for (const auto& fv : snap.fastVehicles) {
			const RemoteDriverHint* matchedHint = nullptr;
			if (hints && hintCount > 0) {
				for (std::size_t i = 0; i < hintCount; ++i) {
					if (hints[i].vehicleId == fv.id) {
						matchedHint = &hints[i];
						break;
					}
				}
			}

			const bool isDriving = fv.hasDriver || (matchedHint && matchedHint->isMoving);
			if (!isDriving)
				continue;
			if (radiusOnly && !detail::WithinRadius300m(fv.position, snap.localPosition))
				continue;

			// Skip if any living entity (including local player) is already seated in this vehicle.
			bool alreadyInEntities = false;
			for (const auto& ent : snap.entities) {
				if (ent.isDead) continue;
				if (!ent.inVehicle && ent.vehicleId <= 0) continue;
				if (ent.vehicleId > 0 && ent.vehicleId == fv.id) {
					alreadyInEntities = true;
					break;
				}
				if (ent.inVehicle &&
					std::isfinite(ent.position.x) && std::isfinite(ent.position.z) &&
					std::isfinite(fv.position.x) && std::isfinite(fv.position.z))
				{
					const float dx = fv.position.x - ent.position.x;
					const float dz = fv.position.z - ent.position.z;
					if (dx * dx + dz * dz < 36.f) {
						alreadyInEntities = true;
						break;
					}
				}
			}
			if (alreadyInEntities)
				continue;

			RoleBucket bucket = RoleBucket::Unknown;
			const bool hasDriverInfo = matchedHint && (matchedHint->role != 0 || !matchedHint->roleName.empty());
			if (hasDriverInfo) {
				if (matchedHint->entityId == snap.localEntityId)
					continue;
				if (isEntityCounted(matchedHint->entityId))
					continue;
				bucket = ClassifyRole(matchedHint->role, matchedHint->roleName);
			}
			if (bucket == RoleBucket::Unknown && !fv.ownerRoleName.empty()) {
				if (localPublicId > 0 && fv.ownerPublicId == localPublicId)
					continue;
				if (isPublicCounted(fv.ownerPublicId))
					continue;
				bucket = ClassifyRole(0, fv.ownerRoleName);
			}

			if (bucket != RoleBucket::Unknown)
				addBucket(bucket);
		}
	}

	out.total = out.police + out.civilian + out.criminal + out.unknown;
	return out;
}

} // namespace Esp
