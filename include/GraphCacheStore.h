#pragma once

#include "GraphCache.h"
#include <array>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GraphCache
{
	struct Key
	{
		std::uint64_t graph;
		std::uint64_t shape;
		bool operator==(const Key&) const = default;
	};

	struct KeyHash
	{
		std::size_t operator()(Key key) const noexcept { return key.graph ^ std::rotl(key.shape, 29); }
	};

	class Store
	{
	public:
		using Clock = std::chrono::steady_clock;
		explicit Store(std::filesystem::path directory);
		[[nodiscard]] Lookup Read(Key key, Clock::time_point now = Clock::now()) noexcept;
		[[nodiscard]] WriteResult Write(Key key, const GraphPlan::Entry& entry, Clock::time_point now = Clock::now()) noexcept;
		[[nodiscard]] PreloadStats Preload(unsigned maxPlans, Clock::time_point deadline,
			const std::atomic_bool& stop, PreloadProgress progress) noexcept;
		[[nodiscard]] bool AdmitOffline(Key key, GraphPlan::Shape shape, GraphSlimming::Payload&& payload);
		[[nodiscard]] std::shared_ptr<const PreparedPlan> FindOffline(Key key);
		[[nodiscard]] std::shared_ptr<const PreparedPlan> FindPrepared(std::uint64_t graph);
		void ResolvePreparedMismatch(std::uint64_t graph, const PreparedPlan& expected,
			PreparedMismatch mismatch = PreparedMismatch::plan);
		struct TopologyClaim
		{
			std::uint64_t ticket{};
			bool saved{};
		};

		[[nodiscard]] TopologyClaim ClaimTopology(Key key, const GraphPlan::Entry& plan,
			Clock::time_point now = Clock::now(), bool allowNewJob = true);
		void CancelTopology(Key key, std::uint64_t ticket) noexcept;
		[[nodiscard]] bool SaveTopology(Key key, std::uint64_t ticket, const GraphSlimming::Payload& payload) noexcept;

	private:
		struct Record
		{
			std::shared_ptr<const GraphPlan::Entry> entry;
			std::optional<Clock::time_point> retryAfter;
			std::uint64_t lastUse{};
			bool writing{};
			bool preloaded{};
			std::shared_ptr<const PreparedPlan> prepared;
			std::uint64_t topologyTicket{};
			bool topologySaved{};
			std::optional<Clock::time_point> topologyRetryAfter;
		};

		struct PreparedVariants
		{
			std::vector<std::uint64_t> shapes;
			bool requiresExactLookup{};
		};

		// Hold memoryMutex when accessing records or changing their state.
		[[nodiscard]] Lookup FindLoaded(Key key, Clock::time_point now);
		[[nodiscard]] bool MakeRoom();
		void FinishTopology(Key key, std::uint64_t ticket, bool saved, std::optional<Clock::time_point> retry) noexcept;
		[[nodiscard]] std::filesystem::path PathFor(Key key, bool legacy = false) const;

		std::filesystem::path directory;
		std::mutex memoryMutex;
		std::array<std::mutex, 8> fileMutexes;
		std::array<std::mutex, 8> topologyFileMutexes;
		std::unordered_map<Key, Record, KeyHash> loaded;
		std::unordered_map<std::uint64_t, PreparedVariants> preparedGraphs;
		struct OfflineVariants
		{
			std::vector<std::shared_ptr<const PreparedPlan>> plans;
			bool exactOnly{};
		};
		std::unordered_map<std::uint64_t, OfflineVariants> offlineGraphs;
		std::size_t offlinePlanCount{};
		std::size_t offlineBytes{};
		std::uint64_t useSequence{};
		std::uint64_t topologySequence{};
		std::size_t memoryLimit{64};
	};
}
