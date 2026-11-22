#pragma once

#include "GraphPlan.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <memory>

namespace TransitionMap
{
	inline constexpr auto emptyKey = (std::numeric_limits<std::uint64_t>::max)();
	inline constexpr std::uint32_t borrowed = 0x80000000u;

	using RuntimeVersion = std::array<std::uint16_t, 4>;

	struct RuntimeProfile
	{
		RuntimeVersion version;
		std::uint64_t insertId;
		std::size_t insertBytes;
		std::uint64_t insertFingerprint;
	};

	// Each row has been checked against its own EXE and Address Library.
	inline constexpr RuntimeProfile verifiedRuntimeProfiles[]{
		{{1, 5, 97, 0}, 56771, 0xA9, 0xFCCF8DDBA5FB3167ULL},
		{{1, 6, 1170, 0}, 57198, 0xA9, 0xFCCF8DDBA5FB3167ULL},
		{{1, 7, 104, 0}, 57198, 0xA9, 0xFCCF8DDBA5FB3167ULL},
	};

	[[nodiscard]] constexpr const RuntimeProfile* FindRuntimeProfile(RuntimeVersion version) noexcept
	{
		for (const auto& profile : verifiedRuntimeProfiles) {
			if (profile.version == version) return &profile;
		}
		return nullptr;
	}

	[[nodiscard]] inline bool MatchesVerifiedInsert(const RuntimeProfile& profile, std::span<const std::uint8_t> code) noexcept
	{
		if (code.size() != profile.insertBytes) return false;
		std::uint64_t hash = 14695981039346656037ULL;
		for (const auto byte : code) hash = (hash ^ byte) * 1099511628211ULL;
		return hash == profile.insertFingerprint;
	}

	struct Bucket
	{
		std::uint64_t key;
		std::uint64_t val;
	};
	static_assert(sizeof(Bucket) == 0x10);

	struct Table
	{
		Bucket* elem{};
		std::int32_t numElemsAndFlags{};
		std::int32_t hashMod{-1};
	};
	static_assert(sizeof(Table) == 0x10);
	static_assert(offsetof(Table, numElemsAndFlags) == 0x8);
	static_assert(offsetof(Table, hashMod) == 0xC);

	[[nodiscard]] constexpr std::size_t RequiredCapacity(std::size_t count) noexcept
	{
		if (!count || count > GraphPlan::nodeThreshold) return 0;
		return std::bit_ceil((std::max)(std::size_t{8}, count * 2));
	}

	[[nodiscard]] inline bool ValidHeader(const Table& table, std::size_t buckets) noexcept
	{
		const auto count = static_cast<std::uint32_t>(table.numElemsAndFlags) & ~borrowed;
		if (!buckets) return !table.elem && !count && table.hashMod == -1;
		return table.elem && std::has_single_bit(buckets) && buckets <= (1u << 20) &&
			table.hashMod == static_cast<std::int32_t>(buckets - 1) && count < buckets;
	}

	template <class Value>
	[[nodiscard]] bool Matches(const Table& table, std::size_t buckets, std::size_t count, bool checkProbes, Value valueAt)
	{
		if (count > GraphPlan::nodeThreshold || !ValidHeader(table, buckets) ||
			(static_cast<std::uint32_t>(table.numElemsAndFlags) & ~borrowed) != count) return false;
		if (!buckets) return true;

		std::bitset<GraphPlan::nodeThreshold> seen;
		std::size_t occupied{};
		std::size_t first{};
		const auto mask = buckets - 1;
		if (checkProbes) {
			while (first < buckets && table.elem[first].key != emptyKey) ++first;
			if (first == buckets) return false;
		}
		auto precedingEmpty = first;
		for (std::size_t i = 0; i < buckets; ++i) {
			const auto slot = (first + i) & mask;
			const auto [key, value] = table.elem[slot];
			if (key == emptyKey) {
				precedingEmpty = slot;
				continue;
			}
			if (key >= count || seen[key] || valueAt(key) != value) return false;
			if (checkProbes && !GraphPlan::ReachableMapSlot(key, slot, precedingEmpty, mask)) return false;
			seen.set(key);
			++occupied;
		}
		return occupied == count;
	}

	// The verified Skyrim runtimes use BlockAlloc/BlockFree and grow before insertion
	// when twice the current count exceeds hashMod. With this reservation
	// native Insert will not grow the staged table.
	template <class Allocator, class Insert, class Value>
	[[nodiscard]] bool Replace(Table& live, std::size_t oldCapacity, std::size_t count,
		Allocator& allocator, Insert insert, Value valueAt)
	{
		if (count > GraphPlan::nodeThreshold || !ValidHeader(live, oldCapacity)) return false;
		const auto capacity = RequiredCapacity(count);
		const auto bytes = static_cast<std::int32_t>(capacity * sizeof(Bucket));
		const auto freeStaged = [&](Bucket* buckets) { allocator.BlockFree(buckets, bytes); };
		std::unique_ptr<Bucket, decltype(freeStaged)> storage(capacity ?
			static_cast<Bucket*>(allocator.BlockAlloc(bytes)) : nullptr, freeStaged);
		if (capacity && !storage) return false;

		Table staged;
		if (storage) {
			std::uninitialized_fill_n(storage.get(), capacity, Bucket{emptyKey, 0});
			// Havok must not free our staged storage if a changed native
			// insert unexpectedly tries to grow it.
			staged = {storage.get(), static_cast<std::int32_t>(borrowed), static_cast<std::int32_t>(capacity - 1)};
			for (std::size_t id = 0; id < count; ++id) {
				if (!insert(staged, id, valueAt(id)) || staged.elem != storage.get() ||
					staged.hashMod != static_cast<std::int32_t>(capacity - 1) ||
					static_cast<std::uint32_t>(staged.numElemsAndFlags) != (borrowed | (id + 1))) return false;
			}
			if (!Matches(staged, capacity, count, true, valueAt)) return false;
			staged.numElemsAndFlags = static_cast<std::int32_t>(count);
		}

		const auto old = live;
		live = staged;
		(void)storage.release();
		if (old.elem && !(static_cast<std::uint32_t>(old.numElemsAndFlags) & borrowed)) {
			allocator.BlockFree(old.elem, static_cast<std::int32_t>(oldCapacity * sizeof(Bucket)));
		}
		return true;
	}
}
