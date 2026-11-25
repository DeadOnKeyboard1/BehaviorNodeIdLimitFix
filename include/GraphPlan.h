#pragma once

#include <algorithm>
#include <cstdint>
#include <bit>
#include <cstring>
#include <limits>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace GraphPlan
{
	// Skyrim's counters, state-machine map keys and recycled IDs are signed
	// 16-bit values. Leave one step below the signed ceiling for allocation.
	inline constexpr std::size_t nodeThreshold = 32766;
	static_assert(nodeThreshold < (std::numeric_limits<std::int16_t>::max)());
	inline constexpr std::uint32_t noNode = (std::numeric_limits<std::uint32_t>::max)();
	inline constexpr std::uint16_t invalidId = 0xFFFFu;

	inline constexpr std::uint32_t maxNodes = 1u << 23;
	inline constexpr std::uint64_t storageVersion = 0x314E414C50444942ULL;
	inline constexpr std::size_t headerSize = 2 * sizeof(std::uint64_t) + 3 * sizeof(std::uint32_t);
	inline constexpr std::size_t maxFileSize = headerSize + nodeThreshold * sizeof(std::uint32_t);

	[[nodiscard]] inline bool CanRewriteIDs(bool active, std::size_t activeNodes,
		std::size_t recycledIds, std::int16_t staticNodes, std::int16_t nextId) noexcept
	{
		return !active && !activeNodes && !recycledIds && nextId == staticNodes;
	}

	struct Signature
	{
		static constexpr std::uint64_t emptyNodeMultiplier = 11527715348014283921ULL;
		std::uint64_t value = 14695981039346656037ULL;
		void Add(std::uint32_t input) noexcept
		{
			for (unsigned shift = 0; shift < 32; shift += 8) {
				value ^= (input >> shift) & 0xFFu;
				value *= 1099511628211ULL;
			}
		}

		void AddEmptyNode() noexcept
		{
			// Four zero bytes in the existing FNV stream.
			value *= emptyNodeMultiplier;
		}

		void AddEmptyNodes(std::size_t count) noexcept
		{
			auto factor = emptyNodeMultiplier;
			while (count > 1) {
				if (count & 1u) value *= factor;
				factor *= factor;
				count >>= 1;
			}
			if (count) value *= factor;
		}
	};

	struct Entry
	{
		std::uint64_t topology{};
		std::uint32_t nodes{};
		std::uint32_t analysisMicros{};
		std::vector<std::uint32_t> stateMachines;

		[[nodiscard]] bool ValidHeader(std::size_t count) const noexcept
		{
			return nodes > nodeThreshold && nodes <= maxNodes && analysisMicros <= 60'000'000u && count <= nodeThreshold;
		}

		[[nodiscard]] bool WellFormed() const noexcept
		{
			if (!ValidHeader(stateMachines.size())) return false;
			for (std::size_t i = 0; i < stateMachines.size(); ++i) {
				if (stateMachines[i] >= nodes || (i && stateMachines[i - 1] >= stateMachines[i])) return false;
			}
			return true;
		}
	};

	// This versioned payload contains only indices and measured times.
	[[nodiscard]] inline std::optional<Entry> ReadEntryBytes(auto read, std::optional<std::size_t> fileSize = {})
	{
		if (fileSize && (*fileSize < headerSize || *fileSize > maxFileSize)) return {};
		char header[headerSize];
		if (!read(header, sizeof(header))) return {};
		const auto* cursor = header;
		const auto take = [&cursor]<class T>(T& value) {
			std::memcpy(&value, cursor, sizeof(value));
			cursor += sizeof(value);
		};
		std::uint64_t version{};
		std::uint32_t count{};
		Entry entry;
		take(version);
		take(entry.topology);
		take(entry.nodes);
		take(entry.analysisMicros);
		take(count);
		if (version != storageVersion || !entry.ValidHeader(count)) return {};
		if (fileSize && *fileSize != headerSize + count * sizeof(std::uint32_t)) return {};
		entry.stateMachines.resize(count);
		if (count && !read(reinterpret_cast<char*>(entry.stateMachines.data()), count * sizeof(std::uint32_t))) return {};
		if (!entry.WellFormed()) return {};
		return entry;
	}

	[[nodiscard]] inline std::optional<Entry> ReadEntry(std::istream& input, std::optional<std::size_t> fileSize = {})
	{
		auto entry = ReadEntryBytes([&](char* target, std::size_t count) {
			return static_cast<bool>(input.read(target, static_cast<std::streamsize>(count)));
		}, fileSize);
		if (!entry || input.peek() != std::char_traits<char>::eof() || input.bad()) return {};
		return entry;
	}

	inline void WriteEntry(std::ostream& output, const Entry& entry)
	{
		const auto put = [&output]<class T>(const T& value) {
			output.write(reinterpret_cast<const char*>(&value), sizeof(value));
		};
		put(storageVersion);
		put(entry.topology);
		put(entry.nodes);
		put(entry.analysisMicros);
		put(static_cast<std::uint32_t>(entry.stateMachines.size()));
		if (!entry.stateMachines.empty()) {
			output.write(reinterpret_cast<const char*>(entry.stateMachines.data()),
				entry.stateMachines.size() * sizeof(std::uint32_t));
		}
	}
	struct Shape
	{
		std::uint64_t topology{};
		std::uint64_t classification{};
		std::uint32_t nodes{};

		[[nodiscard]] std::uint64_t CacheVariant() const noexcept
		{
			Signature key;
			key.Add(static_cast<std::uint32_t>(topology));
			key.Add(static_cast<std::uint32_t>(topology >> 32));
			key.Add(static_cast<std::uint32_t>(classification));
			key.Add(static_cast<std::uint32_t>(classification >> 32));
			key.Add(nodes);
			return key.value;
		}
	};

	// Call after validating the stored ordinals. This table can be prepared
	// without any live Havok objects.
	[[nodiscard]] inline std::vector<std::uint16_t> PrepareIds(const Entry& entry)
	{
		std::vector<std::uint16_t> ids(entry.nodes, invalidId);
		for (std::size_t i = 0; i < entry.stateMachines.size(); ++i) {
			ids[entry.stateMachines[i]] = static_cast<std::uint16_t>(i);
		}
		return ids;
	}

	[[nodiscard]] inline bool ValidAssignments(std::size_t nodes, std::span<const std::uint32_t> stateMachines,
		std::span<const std::uint16_t> prepared = {}) noexcept
	{
		if (nodes <= nodeThreshold || nodes > maxNodes || stateMachines.size() > nodeThreshold ||
			(!prepared.empty() && prepared.size() != nodes)) return false;
		for (std::size_t i = 0; i < stateMachines.size(); ++i) {
			if (stateMachines[i] >= nodes || (i && stateMachines[i - 1] >= stateMachines[i])) return false;
		}
		if (!prepared.empty()) {
			std::size_t next{};
			for (std::size_t i = 0; i < nodes; ++i) {
				const auto expected = next < stateMachines.size() && stateMachines[next] == i ?
					static_cast<std::uint16_t>(next++) : invalidId;
				if (prepared[i] != expected) return false;
			}
		}
		return true;
	}

	class PreparedCheck
	{
	public:
		explicit PreparedCheck(std::span<const std::uint16_t> ids, std::size_t verified = 0) :
			ids(ids), prefix((std::min)(verified, ids.size())), matching(!ids.empty() && prefix == verified) {}

		template <class IsMachine>
		void Observe(std::size_t ordinal, IsMachine isMachine)
		{
			if (matching) {
				matching = ordinal == prefix && ordinal < ids.size() && isMachine() == (ids[ordinal] != invalidId);
				if (matching) ++prefix;
			}
		}

		[[nodiscard]] bool Matches(std::size_t nodes, std::uint64_t topology, std::uint64_t expectedTopology) const noexcept
		{
			return matching && prefix == nodes && nodes == ids.size() && topology == expectedTopology;
		}

		[[nodiscard]] std::size_t VerifiedPrefix() const noexcept { return prefix; }

	private:
		std::span<const std::uint16_t> ids;
		std::size_t prefix{};
		bool matching;
	};

	// Store live classification once. A hit needs only the populated indices
	// and the count, and a miss can enumerate the same bits in traversal order.
	class StateMachines
	{
	public:
		explicit StateMachines(std::uint32_t a_nodes) : nodes(a_nodes), bits((a_nodes + 63u) / 64u, 0) {}

		void Add(std::uint32_t index) noexcept
		{
			if (index >= nodes) return;
			auto& word = bits[index / 64];
			const auto flag = std::uint64_t{1} << (index % 64);
			if (word & flag) return;
			word |= flag;
			signature.Add(index);
			++count;
		}

		[[nodiscard]] std::size_t Count() const noexcept { return count; }
		[[nodiscard]] Shape Describe(std::uint64_t topology) const noexcept { return {topology, signature.value, nodes}; }

		[[nodiscard]] bool Matches(const Entry& cached, std::uint64_t topology) const noexcept
		{
			if (cached.topology != topology || cached.nodes != nodes ||
				cached.stateMachines.size() != count || !cached.ValidHeader(count)) return false;
			std::uint32_t next{};
			for (const auto index : cached.stateMachines) {
				if (index < next || index >= nodes) return false;
				if (!(bits[index / 64] & (std::uint64_t{1} << (index % 64)))) return false;
				next = index + 1;
			}
			return true;
		}

		[[nodiscard]] Entry Build(std::uint64_t topology) const
		{
			Entry plan{topology, nodes, 0, {}};
			plan.stateMachines.reserve(count);
			for (std::uint32_t word = 0; word < bits.size(); ++word) {
				for (auto set = bits[word]; set; set &= set - 1) {
					const auto bit = static_cast<std::uint32_t>(std::countr_zero(set));
					plan.stateMachines.push_back(word * 64 + bit);
				}
			}
			return plan;
		}

	private:
		std::uint32_t nodes;
		std::vector<std::uint64_t> bits;
		Signature signature;
		std::size_t count{};
	};

	[[nodiscard]] inline bool ValidChildArray(std::int32_t size, std::uint32_t capacity, bool hasData) noexcept
	{
		return hasData && capacity != 0 && size >= 0 && static_cast<std::uint32_t>(size) <= capacity;
	}
	[[nodiscard]] inline bool InBorrowedBuffer(std::uintptr_t address, std::uintptr_t start, std::size_t bytes) noexcept
	{
		return address >= start && address - start < bytes;
	}
	[[nodiscard]] inline bool BuffersOverlap(std::uintptr_t first, std::size_t firstBytes,
		std::uintptr_t second, std::size_t secondBytes) noexcept
	{
		return firstBytes && secondBytes &&
			(InBorrowedBuffer(first, second, secondBytes) || InBorrowedBuffer(second, first, firstBytes));
	}

	// Skyrim 1.7.104: Havok starts at this bucket and probes forward, stopping
	// at an empty bucket. Distances are circular because capacity is a power of two.
	[[nodiscard]] inline bool ReachableMapSlot(
		std::uint64_t key, std::size_t slot, std::size_t precedingEmpty, std::size_t mask) noexcept
	{
		const auto home = (static_cast<std::uint32_t>(key >> 4) * 0x9e3779b1u) & mask;
		return ((slot - home) & mask) < ((slot - precedingEmpty) & mask);
	}
}
