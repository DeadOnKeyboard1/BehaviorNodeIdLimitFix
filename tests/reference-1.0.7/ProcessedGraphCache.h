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

		[[nodiscard]] bool Remember(const Key& key, Snapshot snapshot, std::size_t bytes)
		{
			if (!snapshot || bytes > instanceBytes || bytes > budget) return false;
			std::array<Snapshot, slots> retired;
			std::size_t retiredCount{};
			std::scoped_lock lock(mutex);
			if (auto* old = FindSlot(key)) retired[retiredCount++] = Remove(*old);
			Slot* available = nullptr;
			for (;;) {
				Slot* oldest = nullptr;
				for (auto& slot : entries) {
					if (!slot.snapshot) {
						available = &slot;
					} else if (!oldest || slot.lastUse < oldest->lastUse) {
						oldest = &slot;
					}
				}
				if (available && bytes <= budget - used) break;
				retired[retiredCount++] = Remove(*oldest);
			}
			*available = {key, std::move(snapshot), bytes, ++sequence};
			used += bytes;
			return true;
		}

	private:
		static_assert(slots > 0);
		struct Slot
		{
			Key key{};
			Snapshot snapshot;
			std::size_t bytes{};
			std::uint64_t lastUse{};
		};

		Slot* FindSlot(const Key& key)
		{
			for (auto& slot : entries) {
				if (slot.snapshot && slot.key == key) return &slot;
			}
			return nullptr;
		}

		Snapshot Remove(Slot& slot)
		{
			used -= slot.bytes;
			auto retired = std::move(slot.snapshot);
			slot = {};
			return retired;
		}

		const std::size_t budget;
		std::mutex mutex;
		std::array<Slot, slots> entries{};
		std::size_t used{};
		std::uint64_t sequence{};
	};
}
