#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace InstanceTopology
{
	inline constexpr std::size_t instanceBytes = 32u << 20;
	inline constexpr std::size_t residentBytes = 64u << 20;

	// A verifier owns its snapshot only while it is checking live links.
	// Eviction removes the snapshot from the cache immediately.
	template <class Key, class Value, std::size_t slots = 8>
	class Cache
	{
	public:
		using Snapshot = std::shared_ptr<const Value>;
		using Retired = std::array<Snapshot, slots>;

		struct Admission
		{
			bool stored{};
			Retired retired{};
		};

		struct SharedStorage
		{
			const void* identity{};
			std::size_t bytes{};
		};

		explicit Cache(std::size_t byteBudget = residentBytes) : budget(byteBudget) {}

		[[nodiscard]] Snapshot Find(const Key& key)
		{
			std::scoped_lock lock(mutex);
			if (auto* slot = FindSlot(key)) {
				slot->lastUse = ++sequence;
				return slot->snapshot;
			}
			return {};
		}

		[[nodiscard]] bool IsCurrent(const Key& key, const Value* snapshot)
		{
			std::scoped_lock lock(mutex);
			const auto* slot = FindSlot(key);
			return slot && slot->snapshot.get() == snapshot;
		}

		void Invalidate(const Key& key, const Value* expected = nullptr)
		{
			Snapshot retired;
			std::scoped_lock lock(mutex);
			if (auto* slot = FindSlot(key); slot && (!expected || slot->snapshot.get() == expected)) retired = Remove(*slot);
		}

		template <class Predicate>
		[[nodiscard]] Retired ExtractIf(Predicate matches)
		{
			Retired retired;
			std::size_t count{};
			std::scoped_lock lock(mutex);
			for (auto& slot : entries) {
				if (slot.snapshot && matches(slot.key, *slot.snapshot)) retired[count++] = Remove(slot);
			}
			return retired;
		}

		[[nodiscard]] bool Remember(const Key& key, Snapshot snapshot, std::size_t bytes, SharedStorage shared = {})
		{
			return Admit(key, std::move(snapshot), bytes, shared).stored;
		}

		// Callers holding another lock can keep evicted snapshots alive until that lock is released.
		[[nodiscard]] Admission Admit(const Key& key, Snapshot snapshot, std::size_t bytes, SharedStorage shared = {})
		{
			if (!snapshot || (!shared.identity && shared.bytes) || bytes > instanceBytes || bytes > budget ||
				shared.bytes > instanceBytes - bytes || shared.bytes > budget - bytes) return {};
			Admission result;
			std::size_t retiredCount{};
			std::scoped_lock lock(mutex);
			if (const auto* owner = FindShared(shared.identity); owner && owner->shared.bytes != shared.bytes) return {};
			if (auto* old = FindSlot(key)) result.retired[retiredCount++] = Remove(*old);
			Slot* available = nullptr;
			std::size_t added{};
			for (;;) {
				Slot* oldest = nullptr;
				for (auto& slot : entries) {
					if (!slot.snapshot) {
						available = &slot;
					} else if (!oldest || slot.lastUse < oldest->lastUse) {
						oldest = &slot;
					}
				}
				added = bytes + (FindShared(shared.identity) ? 0 : shared.bytes);
				if (available && added <= budget - used) break;
				result.retired[retiredCount++] = Remove(*oldest);
			}
			*available = {key, std::move(snapshot), bytes, shared, ++sequence};
			used += added;
			result.stored = true;
			return result;
		}

	private:
		static_assert(slots > 0);
		struct Slot
		{
			Key key{};
			Snapshot snapshot;
			std::size_t bytes{};
			SharedStorage shared;
			std::uint64_t lastUse{};
		};

		Slot* FindSlot(const Key& key)
		{
			for (auto& slot : entries) {
				if (slot.snapshot && slot.key == key) return &slot;
			}
			return nullptr;
		}

		const Slot* FindShared(const void* identity) const
		{
			if (identity) {
				for (const auto& slot : entries) {
					if (slot.snapshot && slot.shared.identity == identity) return &slot;
				}
			}
			return nullptr;
		}

		Snapshot Remove(Slot& slot)
		{
			used -= slot.bytes;
			const auto shared = slot.shared;
			auto retired = std::move(slot.snapshot);
			slot = {};
			if (shared.identity && !FindShared(shared.identity)) used -= shared.bytes;
			return retired;
		}

		const std::size_t budget;
		std::mutex mutex;
		std::array<Slot, slots> entries{};
		std::size_t used{};
		std::uint64_t sequence{};
	};
}
