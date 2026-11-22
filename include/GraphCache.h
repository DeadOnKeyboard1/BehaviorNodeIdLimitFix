#pragma once

#include "GraphPlan.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

namespace GraphSlimming { struct Payload; }
namespace OfflineGraph { struct Candidate; }

namespace GraphCache
{
	enum class Result { missing, hit, invalid, unavailable };
	enum class Origin { none, memory, disk, legacy, preloaded };
	enum class WriteResult { saved, current, pending, failed, skipped };
	enum class PreparedMismatch { plan, differentShape, topology };

	struct PreparedPlan;

	struct Lookup
	{
		Result result{Result::missing};
		std::shared_ptr<const GraphPlan::Entry> entry;
		Origin origin{Origin::none};
		bool retryWrite{};
		bool persisted{};
		std::shared_ptr<const PreparedPlan> prepared;
	};

	struct PreloadStats
	{
		unsigned selected{};
		unsigned checked{};
		unsigned unusable{};
		unsigned warmed{};
		unsigned prepared{};
		unsigned baked{};
		std::size_t bytes{};
		std::size_t preparedBytes{};
		std::size_t topologyBytes{};
		bool interrupted{};
	};

	struct PreparedPlan
	{
		std::shared_ptr<const GraphPlan::Entry> entry;
		GraphPlan::Shape shape;
		std::shared_ptr<const std::vector<std::uint16_t>> ids;
		std::shared_ptr<const GraphSlimming::Payload> payload;
		bool offline{};

		[[nodiscard]] std::span<const std::uint16_t> IDs() const noexcept { return *ids; }
	};

	using PreloadProgress = void (*)(unsigned checked, unsigned selected) noexcept;

	[[nodiscard]] Lookup Read(std::string_view name, GraphPlan::Shape shape) noexcept;
	[[nodiscard]] WriteResult Write(std::string_view name, GraphPlan::Shape shape, const GraphPlan::Entry& entry) noexcept;
	[[nodiscard]] PreloadStats Preload(unsigned maxPlans, std::chrono::steady_clock::time_point deadline,
		const std::atomic_bool& stop, PreloadProgress progress) noexcept;
	[[nodiscard]] bool AdmitOffline(OfflineGraph::Candidate&& candidate) noexcept;
	[[nodiscard]] std::shared_ptr<const PreparedPlan> FindOffline(std::string_view name, GraphPlan::Shape shape) noexcept;
	[[nodiscard]] std::shared_ptr<const PreparedPlan> FindPrepared(std::string_view name) noexcept;
	// Drop only the preparation data disproven by live traversal.
	void ResolvePreparedMismatch(std::string_view name, const PreparedPlan& expected, PreparedMismatch mismatch) noexcept;
	// Returns true when this variant already has a committed topology.
	[[nodiscard]] bool QueueTopology(std::string_view name, GraphPlan::Shape shape,
		const std::shared_ptr<const GraphSlimming::Payload>& payload) noexcept;
}
