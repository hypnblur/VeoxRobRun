#pragma once
#include "pch.h"
#include "LootFilterData.h"
#include <string>
#include <string_view>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "Diag.h"

inline std::string FormatCash(int amount)
{
	std::string s = std::to_string(amount);
	int n = (int)s.length() - 3;
	while (n > 0) {
		s.insert(n, ",");
		n -= 3;
	}
	return s;
}

inline std::uint32_t ColorToImU32(const D2D1::ColorF& c)
{
	return ((std::uint32_t)(c.a * 255.f) << 24) |
	       ((std::uint32_t)(c.b * 255.f) << 16) |
	       ((std::uint32_t)(c.g * 255.f) << 8) |
	       ((std::uint32_t)(c.r * 255.f));
}

// Packed IM_COL32 form of a catalog rarity colour (RarityColors stores
// 0xAARRGGBB, ImGui wants 0xAABBGGRR).
inline std::uint32_t RarityColorU32(LootFilter::Rarity rarity)
{
	const std::uint32_t argb = LootFilter::RarityColors[(int)rarity];
	const std::uint32_t a = (argb >> 24) & 0xFFu;
	const std::uint32_t r = (argb >> 16) & 0xFFu;
	const std::uint32_t g = (argb >> 8)  & 0xFFu;
	const std::uint32_t b =  argb        & 0xFFu;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

struct ItemSettings {
	bool visible = true;
	std::uint8_t flags = LootFilter::RenderDefault;
		bool useGlobalDistance = true;   // ignore maxDistance, follow the category/global range
		std::string customName;          // shown instead of the catalog name when set
	int maxDistance = 500;
	D2D1::ColorF color{0.f, 0.f, 0.f, 1.f};

	bool Has(std::uint8_t flag) const { return (flags & flag) != 0; }
	void Set(std::uint8_t flag, bool on)
	{
		if (on) flags = (std::uint8_t)(flags | flag);
		else    flags = (std::uint8_t)(flags & ~flag);
	}
};

// Per-category overrides for the marker itself. Zero means "inherit the global
// value", so a category only carries what was deliberately changed.
struct CategorySizing {
	int IconSize = 0;
	int FontSize = 0;
	float DotSize = 0.f;
	int MaxDistance = 0;   // 0 = inherit the per-row / global distance
};

class LootFilterConfig
{
	std::string ConfigName;

public:
	LootFilterConfig(const std::string& name) : ConfigName(name) {}

	bool Enabled = true;
	bool ShowOnMap = true;
	bool ShowOnMinimap = true;
	bool HideEmpty = true;
	float BaseDotSize = 3.5f;
	int FontSize = 14;
	int IconSize = 20;
	int GlobalMaxDistance = 200;

	bool FilterByMinPrice = false;
	int MinPrice = 0;
	int HighValueThreshold = 10000;
	int MidValueThreshold = 2500;
	bool HighValuePulse = true;

	D2D1::ColorF HighValueColour = Colour(255, 215, 0);
	D2D1::ColorF MidValueColour = Colour(180, 130, 255);
	D2D1::ColorF StandardColour = Colour(120, 255, 140);

	std::string SearchQuery;
	int SelectedCategory = (int)LootFilter::Category::Cargo + 1;   // open on Cargo, not "All Items"
	int SortMode = 0;   // 0 name->rarity, 1 rarity->name, 2 price high->low
	int PriceMode = 0;  // 0 sell, 1 buy, 2 unlock
	int AuthorityLevel = 1;  // 1..5, scales prices by the game ladder
	bool FilterTakenLoot = true;         // hide loot objects whose _isTaken flag is set
	bool ReadVehicleInventories = false; // read trunk/table slot grids (experimental)

	std::unordered_map<std::string, ItemSettings> Items;

	// Label lookup. Two indexes: one keyed by kind+label so an exact id always
	// wins for the entity kind that owns it, and one keyed by label alone for
	// cross-kind hits (a loot object whose id equals an item id).
	std::unordered_map<std::string, const LootFilter::ItemDef*> LabelIndex;
	std::unordered_map<std::string, const LootFilter::ItemDef*> AnyLabelIndex;
	std::array<const LootFilter::ItemDef*, 64> KindFallback{};

	// Per-category overrides; index is LootFilter::Category.
	std::array<CategorySizing, (std::size_t)LootFilter::Category::COUNT> Sizing{};
	int SizingCategory = 0;   // menu selection only

	const CategorySizing& SizingFor(LootFilter::Category cat) const
	{
		const std::size_t i = (std::size_t)cat;
		static const CategorySizing kNone{};
		return i < Sizing.size() ? Sizing[i] : kNone;
	}

	// A zero size means "nothing to draw", which is indistinguishable from a bug.
	// These floors make a corrupted config visible instead of silent.
	static constexpr int kMinIconSize = 8;
	static constexpr int kMinFontSize = 6;
	static constexpr float kMinDotSize = 0.5f;

	void ResetToDefaults() { InitDefaults(); }

	int ResolvedIconSize(LootFilter::Category cat) const
	{
		const int v = SizingFor(cat).IconSize;
		const int resolved = v > 0 ? v : IconSize;
		return resolved > 0 ? resolved : kMinIconSize;
	}
	int ResolvedFontSize(LootFilter::Category cat) const
	{
		const int v = SizingFor(cat).FontSize;
		const int resolved = v > 0 ? v : FontSize;
		return resolved > 0 ? resolved : kMinFontSize;
	}
	float ResolvedDotSize(LootFilter::Category cat) const
	{
		const float v = SizingFor(cat).DotSize;
		const float resolved = v > 0.f ? v : BaseDotSize;
		return resolved > 0.f ? resolved : kMinDotSize;
	}
	int ResolvedMaxDistance(LootFilter::Category cat) const
	{
		return SizingFor(cat).MaxDistance;
	}

	// Ids are spelled differently by different parts of the game: placement ids
	// use "vest_4_mk_1" while item ids use "vest4_m1". Fold separators and
	// "mk" -> "m" so both meet on the same key (mirrors Norm in the generator).
	// RobberAuthorityLevelDescriptions, read from the live registry: the loot
	// column is what a fence pays extra, the cargo column is what an exchange
	// pays extra. Both are additive on top of the sell base.
	static float LootRevenueBonus(int level)
	{
		static constexpr float kBonus[5] = { 0.f, 0.10f, 0.20f, 0.35f, 0.50f };
		return kBonus[std::clamp(level, 1, 5) - 1];
	}
	static float CargoRevenueBonus(int level)
	{
		static constexpr float kBonus[5] = { 0.f, 0.05f, 0.10f, 0.15f, 0.25f };
		return kBonus[std::clamp(level, 1, 5) - 1];
	}

	static std::string NormLabel(const std::string& value)
	{
		std::string flat;
		flat.reserve(value.size());
		for (unsigned char c : value)
			if (std::isalnum(c)) flat.push_back((char)std::tolower(c));
		std::string out;
		out.reserve(flat.size());
		for (std::size_t i = 0; i < flat.size(); ++i) {
			if (flat[i] == 'm' && i + 1 < flat.size() && flat[i + 1] == 'k') {
				out.push_back('m');
				++i;
				continue;
			}
			out.push_back(flat[i]);
		}
		return out;
	}

	static std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}


	// ------------------------------------------------------------------ json
	// A plain JSON document so the filter can be shared, diffed and edited by
	// hand. Import is deliberately lenient: keys are looked up by name, order
	// does not matter, and anything missing keeps its current value.
	static void AppendJsonString(std::string& out, const std::string& value)
	{
		out += '"';
		for (char c : value) {
			if (c == '"' || c == '\\') { out += '\\'; out += c; }
			else if (c == '\n') out += "\\n";
			else out += c;
		}
		out += '"';
	}

	std::string ExportJson() const
	{
		std::string o;
		auto num = [&](const char* key, double value, bool last = false) {
			o += "  \""; o += key; o += "\": "; o += std::to_string(value);
			o += last ? "\n" : ",\n";
		};
		auto flag = [&](const char* key, bool value, bool last = false) {
			o += "  \""; o += key; o += "\": "; o += value ? "true" : "false";
			o += last ? "\n" : ",\n";
		};
		auto colour = [&](const char* key, const D2D1::ColorF& c, bool last = false) {
			char buf[160];
			std::snprintf(buf, sizeof(buf),
				"  \"%s\": { \"r\": %.4f, \"g\": %.4f, \"b\": %.4f, \"a\": %.4f }%s\n",
				key, c.r, c.g, c.b, c.a, last ? "" : ",");
			o += buf;
		};

		o += "{\n";
		num("version", 1);
		flag("enabled", Enabled);
		num("category", SelectedCategory);
		num("sizingCategory", SizingCategory);
		num("sortMode", SortMode);
		num("priceMode", PriceMode);
		num("authorityLevel", AuthorityLevel);
		flag("showOnMap", ShowOnMap);
		flag("showOnMinimap", ShowOnMinimap);
		flag("hideEmpty", HideEmpty);
		num("baseDotSize", BaseDotSize);
		num("fontSize", FontSize);
		num("iconSize", IconSize);
		num("globalMaxDistance", GlobalMaxDistance);
		flag("filterByMinPrice", FilterByMinPrice);
		num("minPrice", MinPrice);
		num("highValueThreshold", HighValueThreshold);
		num("midValueThreshold", MidValueThreshold);
		flag("highValuePulse", HighValuePulse);
		colour("highValueColour", HighValueColour);
		colour("midValueColour", MidValueColour);
		colour("standardColour", StandardColour, true);

		o += "  ,\"sizing\": [\n";
		for (std::size_t i = 0; i < Sizing.size(); ++i) {
			char buf[200];
			std::snprintf(buf, sizeof(buf),
				"    { \"icon\": %d, \"font\": %d, \"dot\": %.3f, \"distance\": %d }%s\n",
				Sizing[i].IconSize, Sizing[i].FontSize, Sizing[i].DotSize, Sizing[i].MaxDistance,
				(i + 1 == Sizing.size()) ? "" : ",");
			o += buf;
		}
		o += "  ],\n";

		o += "  \"items\": [\n";
		std::size_t written = 0;
		for (const auto& [id, item] : Items) {
			char buf[256];
			std::snprintf(buf, sizeof(buf),
				"    { \"id\": \"%s\", \"visible\": %s, \"flags\": %u, \"distance\": %d, "
				"\"r\": %.3f, \"g\": %.3f, \"b\": %.3f, \"a\": %.3f, \"useGlobalDistance\": %s }%s\n",
				id.c_str(), item.visible ? "true" : "false", (unsigned)item.flags,
				item.maxDistance, item.color.r, item.color.g, item.color.b, item.color.a,
				item.useGlobalDistance ? "true" : "false",
				(++written == Items.size()) ? "" : ",");
			o += buf;
		}
		o += "  ]\n}\n";
		return o;
	}

	// Lenient JSON import: find "key" then the value after the colon.
	static bool JsonNumber(const std::string& text, const char* key, double& out)
	{
		const std::string pattern = std::string("\"") + key + "\"";
		const std::size_t at = text.find(pattern);
		if (at == std::string::npos) return false;
		const std::size_t colon = text.find(':', at);
		if (colon == std::string::npos) return false;
		const char* begin = text.c_str() + colon + 1;
		char* end = nullptr;
		const double value = std::strtod(begin, &end);
		if (end == begin) return false;
		out = value;
		return true;
	}

	static bool JsonBool(const std::string& text, const char* key, bool& out)
	{
		const std::string pattern = std::string("\"") + key + "\"";
		const std::size_t at = text.find(pattern);
		if (at == std::string::npos) return false;
		const std::size_t colon = text.find(':', at);
		if (colon == std::string::npos) return false;
		const std::size_t token = text.find_first_not_of(" \t\r\n", colon + 1);
		if (token == std::string::npos) return false;
		if (text.compare(token, 4, "true") == 0) { out = true; return true; }
		if (text.compare(token, 5, "false") == 0) { out = false; return true; }
		return false;
	}

	bool ImportJson(const std::string& text)
	{
		int changed = 0;
		double number = 0.0;
		bool boolean = false;
		auto takeNumber = [&](const char* key, auto& target) {
			if (!JsonNumber(text, key, number)) return;
			target = (std::decay_t<decltype(target)>)std::lround(number);
			++changed;
		};
		auto takeFloat = [&](const char* key, float& target) {
			if (!JsonNumber(text, key, number)) return;
			target = (float)number;
			++changed;
		};
		auto takeBool = [&](const char* key, bool& target) {
			if (!JsonBool(text, key, boolean)) return;
			target = boolean;
			++changed;
		};

		takeBool("enabled", Enabled);
		takeNumber("category", SelectedCategory);
		takeNumber("sizingCategory", SizingCategory);
		takeNumber("sortMode", SortMode);
		takeNumber("priceMode", PriceMode);
		takeNumber("authorityLevel", AuthorityLevel);
		takeBool("showOnMap", ShowOnMap);
		takeBool("showOnMinimap", ShowOnMinimap);
		takeBool("hideEmpty", HideEmpty);
		takeFloat("baseDotSize", BaseDotSize);
		takeNumber("fontSize", FontSize);
		takeNumber("iconSize", IconSize);
		takeNumber("globalMaxDistance", GlobalMaxDistance);
		takeBool("filterByMinPrice", FilterByMinPrice);
		takeNumber("minPrice", MinPrice);
		takeNumber("highValueThreshold", HighValueThreshold);
		takeNumber("midValueThreshold", MidValueThreshold);
		takeBool("highValuePulse", HighValuePulse);
		RebuildIndex();

		// Items: walk each { ... } block inside the items array.
		const std::size_t itemsAt = text.find("\"items\"");
		if (itemsAt != std::string::npos) {
			std::size_t cursor = text.find('[', itemsAt);
			if (cursor != std::string::npos) {
				while (true) {
					const std::size_t open = text.find('{', cursor);
					const std::size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
					if (open == std::string::npos || close == std::string::npos) break;
					const std::string entry = text.substr(open, close - open + 1);

					const std::size_t idKey = entry.find("\"id\"");
					if (idKey != std::string::npos) {
						const std::size_t q1 = entry.find('"', entry.find(':', idKey));
						const std::size_t q2 = q1 == std::string::npos ? std::string::npos : entry.find('"', q1 + 1);
						if (q1 != std::string::npos && q2 != std::string::npos) {
							const std::string id = entry.substr(q1 + 1, q2 - q1 - 1);
							auto it = Items.find(id);
							if (it != Items.end()) {
								ItemSettings& item = it->second;
								bool v = item.visible;
								if (JsonBool(entry, "visible", v)) item.visible = v;
								double d = 0.0;
								if (JsonNumber(entry, "flags", d)) item.flags = (std::uint8_t)std::lround(d);
								if (JsonNumber(entry, "distance", d)) item.maxDistance = (int)std::lround(d);
								bool useGlobal = item.useGlobalDistance;
								if (JsonBool(entry, "useGlobalDistance", useGlobal)) item.useGlobalDistance = useGlobal;
								double r = 0, g = 0, b = 0, a = 0;
								const bool haveColour = JsonNumber(entry, "r", r) && JsonNumber(entry, "g", g) &&
									JsonNumber(entry, "b", b) && JsonNumber(entry, "a", a);
								if (haveColour) item.color = D2D1::ColorF((float)r, (float)g, (float)b, (float)a);
								++changed;
							}
						}
					}
					cursor = close + 1;
				}
			}
		}

		// Sizing array: positional, same order as CategoryNames.
		const std::size_t sizingAt = text.find("\"sizing\"");
		if (sizingAt != std::string::npos) {
			std::size_t cursor = text.find('[', sizingAt);
			std::size_t index = 0;
			while (cursor != std::string::npos && index < Sizing.size()) {
				const std::size_t open = text.find('{', cursor);
				const std::size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
				if (open == std::string::npos || close == std::string::npos) break;
				const std::string entry = text.substr(open, close - open + 1);
				double d = 0.0;
				if (JsonNumber(entry, "icon", d)) Sizing[index].IconSize = (int)std::lround(d);
				if (JsonNumber(entry, "font", d)) Sizing[index].FontSize = (int)std::lround(d);
				if (JsonNumber(entry, "dot", d)) Sizing[index].DotSize = (float)d;
				if (JsonNumber(entry, "distance", d)) Sizing[index].MaxDistance = (int)std::lround(d);
				++index;
				cursor = close + 1;
			}
		}

		return changed > 0;
	}

	// A row created before an item had art keeps the dot-only default forever,
	// so a later catalog with sprites would still render as a dot. Upgrade only
	// the exact old default (dot+name+price, no icon) and only when art now
	// exists - anything the user has customised is left alone.
	void MigrateIconFlags()
	{
		for (const auto& def : LootFilter::kItemCatalog) {
			if (!def.iconSprite[0]) continue;
			auto it = Items.find(def.id);
			if (it == Items.end()) continue;
			ItemSettings& item = it->second;
			// Both variants the old sprite-less default could produce: with and
			// without the price flag (prices were hidden for unknown items).
			constexpr std::uint8_t kDotName = LootFilter::RenderDot | LootFilter::RenderName;
			constexpr std::uint8_t kDotNamePrice = kDotName | LootFilter::RenderPrice;
			constexpr std::uint8_t kDotOnly = LootFilter::RenderDot;
			if (item.flags == kDotName || item.flags == kDotNamePrice || item.flags == kDotOnly) {
				item.flags = (std::uint8_t)(item.flags | LootFilter::RenderIcon);
			}
		}
	}

	void RebuildIndex()
	{
		LabelIndex.clear();
		AnyLabelIndex.clear();
		KindFallback.fill(nullptr);
		LabelIndex.reserve(LootFilter::kItemCount * 2);
		AnyLabelIndex.reserve(LootFilter::kItemCount * 2);

		auto key = [](const LootFilter::ItemDef& def, const std::string& label) {
			return std::to_string((int)def.kind) + '\x1f' + Lower(label);
		};
		auto add = [&](const LootFilter::ItemDef& def, const char* label) {
			if (!label || !label[0]) return;
			const std::string plain(label);
			LabelIndex.emplace(key(def, plain), &def);
			AnyLabelIndex.emplace(Lower(plain), &def);
			// Underscore-free form, so "ap_scalar_m_1" (world loot object) and
			// "ap_scalar_m1" (the item) resolve to the same row.
			const std::string flat = NormLabel(plain);
			if (!flat.empty()) {
				LabelIndex.emplace(std::to_string((int)def.kind) + "\x1f#" + flat, &def);
				AnyLabelIndex.emplace("#" + flat, &def);
			}
		};

		for (const auto& def : LootFilter::kItemCatalog) {
			add(def, def.labelMatch);
			if (!def.labelMatch[0]) {
				const std::size_t ki = static_cast<std::size_t>(def.kind);
				if (ki < KindFallback.size() && !KindFallback[ki])
					KindFallback[ki] = &def;
			}
		}
		for (const auto& def : LootFilter::kItemCatalog) add(def, def.displayName);
	}

	void InitDefaults()
	{
		Items.clear();
		Items.reserve(LootFilter::kItemCount);
		for (const auto& def : LootFilter::kItemCatalog) {
			ItemSettings s;
			// Sprite rows default to the icon; sprite-less rows (cash nodes,
			// workbenches, containers) keep the dot so nothing vanishes.
			s.flags = def.iconSprite[0]
				? (std::uint8_t)(LootFilter::RenderIcon | LootFilter::RenderName | LootFilter::RenderPrice)
				: (std::uint8_t)(LootFilter::RenderDot | LootFilter::RenderName | LootFilter::RenderPrice);
			s.color = Colour(def.defaultR, def.defaultG, def.defaultB);
			s.useGlobalDistance = (s.maxDistance == 0);
			// A fresh filter starts focused on things worth picking up: props and
			// spawn nodes are noise on a first run, so those categories start off.
			// Anything the user then enables is remembered in the config.
			if (def.category == LootFilter::Category::LootNodes ||
				def.category == LootFilter::Category::Containers)
				s.visible = false;
			Items[def.id] = s;
		}
		RebuildIndex();
	}

	const ItemSettings& GetItem(const char* id) const
	{
		static const ItemSettings kDefault{};
		auto it = Items.find(id);
		return it != Items.end() ? it->second : kDefault;
	}

	ItemSettings& GetItemMut(const char* id)
	{
		auto it = Items.find(id);
		if (it == Items.end()) {
			ItemSettings s;
			for (const auto& def : LootFilter::kItemCatalog) {
				if (std::string_view(def.id) == id) {
					s.color = Colour(def.defaultR, def.defaultG, def.defaultB);
					s.useGlobalDistance = (s.maxDistance == 0);
					break;
				}
			}
			it = Items.emplace(id, s).first;
		}
		return it->second;
	}

	// Resolve an entity to its catalog row. An exact id/name hit wins, but only
	// when it belongs to the same entity kind; otherwise prefer the longest
	// label substring among rows of that kind, and fall back to the generic row.
	// (A loot node called "bank_cash" must not resolve to an inventory item.)
	const LootFilter::ItemDef* FindItemForEntity(
		RobRunWorld::Kind kind, const std::string& label) const
	{
		if (LabelIndex.empty())
			const_cast<LootFilterConfig*>(this)->RebuildIndex();

		if (label.empty()) {
			const std::size_t ki = static_cast<std::size_t>(kind);
			return ki < KindFallback.size() ? KindFallback[ki] : nullptr;
		}

		if (kind == RobRunWorld::Kind::RandomLoot) {
			const std::string low = Lower(label);
			const auto hit = AnyLabelIndex.find(low);
			if (hit != AnyLabelIndex.end() &&
				(hit->second->kind == RobRunWorld::Kind::Loot ||
				 hit->second->kind == RobRunWorld::Kind::StaticLoot) &&
				Lower(hit->second->labelMatch) == low)
				return hit->second;
			const std::size_t ki = static_cast<std::size_t>(kind);
			return ki < KindFallback.size() ? KindFallback[ki] : nullptr;
		}

		const LootFilter::ItemDef* exact = nullptr;
		// 1. The row of this exact kind that carries this id/name.
		const std::string low = Lower(label);
		auto hit = LabelIndex.find(std::to_string((int)kind) + '\x1f' + low);
		if (hit != LabelIndex.end())
			return hit->second;

		// 2. Same id with separators removed ("ap_scalar_m_1" -> "ap_scalarm1"),
		//    which is how the world loot object points at the item.
		const std::string flat = NormLabel(label);
		if (!flat.empty()) {
			auto normKind = LabelIndex.find(std::to_string((int)kind) + "\x1f#" + flat);
			if (normKind != LabelIndex.end())
				return normKind->second;
		}

		// 3. Same label, another kind (loot object vs item), last resort.
		auto any = AnyLabelIndex.find(low);
		if (any != AnyLabelIndex.end())
			exact = any->second;
		if (!exact && !flat.empty()) {
			auto anyNorm = AnyLabelIndex.find("#" + flat);
			if (anyNorm != AnyLabelIndex.end())
				exact = anyNorm->second;
		}

		const std::size_t ki = static_cast<std::size_t>(kind);
		const LootFilter::ItemDef* fallback =
			ki < KindFallback.size() ? KindFallback[ki] : nullptr;
		const LootFilter::ItemDef* best = nullptr;
		std::size_t bestLen = 0;

		for (const auto& def : LootFilter::kItemCatalog) {
			if (def.kind != kind) continue;
			if (!def.labelMatch[0]) continue;
			if (label.find(def.labelMatch) == std::string::npos) continue;
			const std::size_t len = std::strlen(def.labelMatch);
			if (len > bestLen) { bestLen = len; best = &def; }
		}
		if (best) return best;
		if (exact) return exact;

		// Nothing matched: log the label once so a missing catalog row can be
		// identified from the field instead of guessed at.
		{
			static std::atomic<int> s_missCount{ 0 };
			if (s_missCount.load(std::memory_order_relaxed) < 200 && !label.empty()) {
				static std::mutex s_missMutex;
				static std::unordered_set<std::string> s_logged;
				std::lock_guard<std::mutex> lock(s_missMutex);
				if (s_logged.insert(label).second) {
					s_missCount.fetch_add(1, std::memory_order_relaxed);
					VeoxDiag("unresolved label '%s' (kind %d)", label.c_str(), (int)kind);
				}
			}
		}
		return fallback;
	}

	bool IsItemVisible(const char* id) const
	{
		auto it = Items.find(id);
		return it == Items.end() || it->second.visible;
	}

	void ToJsonColour(json* j, const std::string& name, const D2D1::ColorF& colour)
	{
		(*j)[ConfigName][name][LIT("r")] = colour.r;
		(*j)[ConfigName][name][LIT("g")] = colour.g;
		(*j)[ConfigName][name][LIT("b")] = colour.b;
		(*j)[ConfigName][name][LIT("a")] = colour.a;
	}

	void FromJsonColour(const json& j, const std::string& name, D2D1::ColorF& colour)
	{
		if (j.contains(ConfigName) && j[ConfigName].contains(name)) {
			colour.r = j[ConfigName][name].value(LIT("r"), colour.r);
			colour.g = j[ConfigName][name].value(LIT("g"), colour.g);
			colour.b = j[ConfigName][name].value(LIT("b"), colour.b);
			colour.a = j[ConfigName][name].value(LIT("a"), colour.a);
		}
	}

	json ToJson()
	{
		json j;
		auto& s = j[ConfigName];
		s[LIT("Enabled")] = Enabled;
		s[LIT("ShowOnMap")] = ShowOnMap;
		s[LIT("ShowOnMinimap")] = ShowOnMinimap;
		s[LIT("HideEmpty")] = HideEmpty;
		s[LIT("BaseDotSize")] = BaseDotSize;
		s[LIT("FontSize")] = FontSize;
		s[LIT("IconSize")] = IconSize;
		s[LIT("GlobalMaxDistance")] = GlobalMaxDistance;
		s[LIT("SortMode")] = SortMode;
		s[LIT("PriceMode")] = PriceMode;
		s[LIT("AuthorityLevel")] = AuthorityLevel;
		s[LIT("FilterTakenLoot")] = FilterTakenLoot;
		s[LIT("ReadVehicleInventories")] = ReadVehicleInventories;

		s[LIT("FilterByMinPrice")] = FilterByMinPrice;
		s[LIT("MinPrice")] = MinPrice;
		s[LIT("HighValueThreshold")] = HighValueThreshold;
		s[LIT("MidValueThreshold")] = MidValueThreshold;
		s[LIT("HighValuePulse")] = HighValuePulse;

		ToJsonColour(&j, LIT("HighValueColour"), HighValueColour);
		ToJsonColour(&j, LIT("MidValueColour"), MidValueColour);
		ToJsonColour(&j, LIT("StandardColour"), StandardColour);

		auto& items = s[LIT("Items")];
		for (const auto& [id, item] : Items) {
			auto& entry = items[id];
			entry[LIT("v")] = item.visible;
			entry[LIT("f")] = static_cast<int>(item.flags);
			entry[LIT("d")] = item.maxDistance;
			entry[LIT("useGlobalDistance")] = item.useGlobalDistance;
			if (!item.customName.empty()) entry[LIT("customName")] = item.customName;
			entry[LIT("r")] = item.color.r;
			entry[LIT("g")] = item.color.g;
			entry[LIT("b")] = item.color.b;
			entry[LIT("a")] = item.color.a;
		}

		auto& sizing = s[LIT("Sizing")];
		for (std::size_t i = 0; i < Sizing.size(); ++i) {
			auto& entry = sizing[i];
			entry[LIT("i")] = Sizing[i].IconSize;
			entry[LIT("f")] = Sizing[i].FontSize;
			entry[LIT("d")] = Sizing[i].DotSize;
			entry[LIT("m")] = Sizing[i].MaxDistance;
		}

		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		const auto& s = j[ConfigName];
		Enabled = s.value(LIT("Enabled"), Enabled);
		ShowOnMap = s.value(LIT("ShowOnMap"), ShowOnMap);
		ShowOnMinimap = s.value(LIT("ShowOnMinimap"), ShowOnMinimap);
		HideEmpty = s.value(LIT("HideEmpty"), HideEmpty);
		BaseDotSize = s.value(LIT("BaseDotSize"), BaseDotSize);
		FontSize = s.value(LIT("FontSize"), FontSize);
		IconSize = s.value(LIT("IconSize"), IconSize);
		GlobalMaxDistance = s.value(LIT("GlobalMaxDistance"), GlobalMaxDistance);
		SortMode = s.value(LIT("SortMode"), SortMode);
		PriceMode = s.value(LIT("PriceMode"), PriceMode);
		AuthorityLevel = s.value(LIT("AuthorityLevel"), AuthorityLevel);
		FilterTakenLoot = s.value(LIT("FilterTakenLoot"), FilterTakenLoot);
		ReadVehicleInventories = s.value(LIT("ReadVehicleInventories"), ReadVehicleInventories);

		FilterByMinPrice = s.value(LIT("FilterByMinPrice"), FilterByMinPrice);
		MinPrice = s.value(LIT("MinPrice"), MinPrice);
		HighValueThreshold = s.value(LIT("HighValueThreshold"), HighValueThreshold);
		MidValueThreshold = s.value(LIT("MidValueThreshold"), MidValueThreshold);
		HighValuePulse = s.value(LIT("HighValuePulse"), HighValuePulse);

		FromJsonColour(j, LIT("HighValueColour"), HighValueColour);
		FromJsonColour(j, LIT("MidValueColour"), MidValueColour);
		FromJsonColour(j, LIT("StandardColour"), StandardColour);

		InitDefaults();

		if (s.contains(LIT("Sizing"))) {
			const auto& sizing = s[LIT("Sizing")];
			for (std::size_t i = 0; i < Sizing.size() && i < sizing.size(); ++i) {
				const auto& entry = sizing[i];
				if (!entry.is_object()) continue;
				Sizing[i].IconSize = entry.value(LIT("i"), Sizing[i].IconSize);
				Sizing[i].FontSize = entry.value(LIT("f"), Sizing[i].FontSize);
				Sizing[i].DotSize  = entry.value(LIT("d"), Sizing[i].DotSize);
				Sizing[i].MaxDistance = entry.value(LIT("m"), Sizing[i].MaxDistance);
			}
		}

		if (s.contains(LIT("Items"))) {
			const auto& items = s[LIT("Items")];
			for (auto it = items.begin(); it != items.end(); ++it) {
				const auto& key = it.key();
				const auto& val = it.value();
				auto& item = Items[key];
				item.visible = val.value(LIT("v"), item.visible);				// "f" is the render-flag mask; "s" was the old icon/name/both
				// style enum and is migrated so saved filters keep their look.
				if (val.contains(LIT("f")))
					item.flags = (std::uint8_t)val.value(LIT("f"), (int)item.flags);
				else if (val.contains(LIT("s"))) {
					switch (val.value(LIT("s"), 0)) {
					case 0: item.flags = LootFilter::RenderIcon; break;   // Icon
					case 1: item.flags = LootFilter::RenderName; break;   // Name
					default: item.flags = LootFilter::RenderIcon | LootFilter::RenderName; break;
					}
				}
				item.maxDistance = val.value(LIT("d"), item.maxDistance);
				item.useGlobalDistance = val.value(LIT("useGlobalDistance"), item.maxDistance <= 0);
				item.customName = val.value(LIT("customName"), std::string{});
				constexpr std::string_view paintingPrefix = "original_picture_";
				if (key.starts_with(paintingPrefix) &&
					item.customName == "Painting " + key.substr(paintingPrefix.size()))
					item.customName.clear();
				item.color.r = val.value(LIT("r"), item.color.r);
				item.color.g = val.value(LIT("g"), item.color.g);
				item.color.b = val.value(LIT("b"), item.color.b);
				item.color.a = val.value(LIT("a"), item.color.a);
			}
		}
	}

	json ExportFilterJson() const
	{
		json out;
		out["version"] = 1;
		auto& items = out["items"];
		for (const auto& [id, item] : Items) {
			auto& entry = items[id];
			entry["v"] = item.visible;
			entry["f"] = static_cast<int>(item.flags);
			entry["d"] = item.maxDistance;
			entry["useGlobalDistance"] = item.useGlobalDistance;
			entry["r"] = item.color.r;
			entry["g"] = item.color.g;
			entry["b"] = item.color.b;
			entry["a"] = item.color.a;
		}
		return out;
	}

	void ImportFilterJson(const json& in)
	{
		if (!in.contains("items")) return;
		const auto& items = in["items"];
		for (auto it = items.begin(); it != items.end(); ++it) {
			const auto& key = it.key();
			const auto& val = it.value();
			auto& item = Items[key];
			item.visible = val.value("v", item.visible);
			if (val.contains("f"))
				item.flags = (std::uint8_t)val.value("f", (int)item.flags);
			else if (val.contains("s")) {
				switch (val.value("s", 0)) {
				case 0: item.flags = LootFilter::RenderIcon; break;
				case 1: item.flags = LootFilter::RenderName; break;
				default: item.flags = LootFilter::RenderIcon | LootFilter::RenderName; break;
				}
			}
			item.maxDistance = val.value("d", item.maxDistance);
			item.useGlobalDistance = val.value("useGlobalDistance",
				val.contains("d") ? item.maxDistance <= 0 : item.useGlobalDistance);
			item.color.r = val.value("r", item.color.r);
			item.color.g = val.value("g", item.color.g);
			item.color.b = val.value("b", item.color.b);
			item.color.a = val.value("a", item.color.a);
		}
	}

	std::string ExportCode() const
	{
		auto j = ExportFilterJson();
		std::string raw = j.dump(-1);
		std::string encoded;
		encoded.reserve(raw.size());
		for (char c : raw)
			encoded.push_back(c ^ 0x5A);
		static const char* kAlphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve((encoded.size() + 2) / 3 * 4);
		size_t i = 0;
		while (i < encoded.size()) {
			uint32_t a = static_cast<uint8_t>(encoded[i++]);
			uint32_t b = i < encoded.size() ? static_cast<uint8_t>(encoded[i++]) : 0;
			uint32_t c = i < encoded.size() ? static_cast<uint8_t>(encoded[i++]) : 0;
			uint32_t triple = (a << 16) | (b << 8) | c;
			out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
			out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
			out.push_back(i > encoded.size() + 1 ? '=' : kAlphabet[(triple >> 6) & 0x3F]);
			out.push_back(i > encoded.size()     ? '=' : kAlphabet[triple & 0x3F]);
		}
		return out;
	}

	bool ImportCode(const std::string& code)
	{
		auto decodeChar = [](char c) -> int {
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '+') return 62;
			if (c == '/') return 63;
			return -1;
		};
		std::string decoded;
		decoded.reserve(code.size() * 3 / 4);
		for (size_t i = 0; i < code.size(); i += 4) {
			int a = i     < code.size() ? decodeChar(code[i])     : 0;
			int b = i + 1 < code.size() ? decodeChar(code[i + 1]) : 0;
			int c = i + 2 < code.size() ? decodeChar(code[i + 2]) : 0;
			int d = i + 3 < code.size() ? decodeChar(code[i + 3]) : 0;
			if (a < 0 || b < 0) return false;
			uint32_t triple = (a << 18) | (b << 12) |
				(c >= 0 ? c << 6 : 0) | (d >= 0 ? d : 0);
			decoded.push_back(static_cast<char>((triple >> 16) & 0xFF));
			if (i + 2 < code.size() && code[i + 2] != '=')
				decoded.push_back(static_cast<char>((triple >> 8) & 0xFF));
			if (i + 3 < code.size() && code[i + 3] != '=')
				decoded.push_back(static_cast<char>(triple & 0xFF));
		}
		for (char& c : decoded)
			c ^= 0x5A;
		try {
			auto j = json::parse(decoded);
			ImportFilterJson(j);
			return true;
		} catch (...) {
			return false;
		}
	}
};
