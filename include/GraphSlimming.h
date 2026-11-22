#pragma once

#include "InstanceTopology.h"
#include <array>
#include <memory>
#include <mutex>

namespace GraphSlimming
{
	struct Settings
	{
		bool enabled{true};
		bool safeDeduplication{true};
		bool graphOptimization{true};
		bool instanceFastPath{true};
		bool persistentCache{true};
		bool experimental{};
		bool performanceLogging{true};
		bool adaptivePreloading{true};
		bool showPreloadProgress{true};
		bool preloadAnimationFiles{};
		bool offlineGraphPreparation{true};
		unsigned preloadMaxPlans{128};
		unsigned preloadAssetMaxMiB{4096};

		[[nodiscard]] bool Optimize() const noexcept { return enabled && graphOptimization; }
		[[nodiscard]] bool SharePlans() const noexcept { return enabled && safeDeduplication; }
	};

	[[nodiscard]] Settings ReadSettings(std::istream& input);
	[[nodiscard]] const Settings& Options() noexcept;
	void Configure(Settings settings) noexcept;

	struct Payload
	{
		InstanceTopology::Trace links;
		std::shared_ptr<const GraphPlan::Entry> plan;

		[[nodiscard]] std::size_t CapacityBytes() const noexcept
		{
			return sizeof(*this) + links.CapacityBytes() + sizeof(GraphPlan::Entry) +
				plan->stateMachines.capacity() * sizeof(std::uint32_t);
		}
	};

	struct Preparation
	{
		std::shared_ptr<const Payload> payload;
		std::size_t bytesBefore{};
		std::size_t bytesAfter{};
		bool shared{};
		bool packed{};
	};

	class Engine
	{
	public:
		[[nodiscard]] Preparation Prepare(
			GraphPlan::Shape shape, InstanceTopology::Trace links,
			std::shared_ptr<const GraphPlan::Entry> plan, const Settings& settings);

	private:
		static constexpr std::size_t candidatesPerBucket = 2;

		struct Candidate
		{
			std::uint64_t key{};
			bool packing{};
			std::weak_ptr<const Payload> payload;
			std::uint64_t published{};
		};

		struct Bucket
		{
			std::mutex mutex;
			std::array<Candidate, candidatesPerBucket> candidates;
			std::uint64_t revision{};
		};

		// The instance cache owns plans. An index entry alone never keeps a
		// plan alive. Equality checking runs outside the stripe lock.
		std::array<Bucket, 32> index;
	};
}
