#include "GraphCacheStore.h"
#include "GraphSlimming.h"
#include "OfflineGraph.h"

#include <Windows.h>

namespace
{
	GraphCache::Key MakeKey(std::string_view name, GraphPlan::Shape shape)
	{
		GraphPlan::Signature key;
		for (const auto byte : name) key.Add(static_cast<unsigned char>(byte));
		static const auto version = REL::Module::get().version();
		for (std::size_t i = 0; i < 4; ++i) key.Add(version[i]);
		return {key.value, shape.CacheVariant()};
	}

	GraphCache::Store& Cache()
	{
		static GraphCache::Store cache([] {
			const auto path = std::filesystem::path(REL::Module::get().filePath().data()).parent_path();
			return path.is_absolute() ? path / "Data/SKSE/Plugins/BehaviorNodeIdLimitFix/Cache" : std::filesystem::path{};
		}());
		return cache;
	}

	struct TopologyJob
	{
		GraphCache::Key key;
		std::uint64_t ticket{};
		std::shared_ptr<const GraphSlimming::Payload> payload;
	};

	constexpr unsigned maxTopologyJobs = 4;
	std::atomic_uint topologyJobs{};

	bool ReserveTopologyJob() noexcept
	{
		auto active = topologyJobs.load(std::memory_order_relaxed);
		while (active < maxTopologyJobs) {
			if (topologyJobs.compare_exchange_weak(active, active + 1, std::memory_order_relaxed)) return true;
		}
		return false;
	}

	DWORD WINAPI PersistTopology(void* context)
	{
		const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
		{
			const std::unique_ptr<TopologyJob> job(static_cast<TopologyJob*>(context));
			(void)Cache().SaveTopology(job->key, job->ticket, *job->payload);
		}
		if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
		topologyJobs.fetch_sub(1, std::memory_order_relaxed);
		return 0;
	}
}

namespace GraphCache
{
	bool QueueTopology(std::string_view name, GraphPlan::Shape shape,
		const std::shared_ptr<const GraphSlimming::Payload>& payload) noexcept
	{
		if (name.empty() || name == "<unnamed>" || !payload || !payload->plan) return false;
		try {
			static const bool pinned = [] {
				HMODULE handle{};
				return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
					reinterpret_cast<LPCWSTR>(&topologyJobs), &handle) != FALSE;
			}();
			if (!pinned) return false;
			auto& cache = Cache();
			const auto key = MakeKey(name, shape);
			const bool canQueue = topologyJobs.load(std::memory_order_relaxed) < maxTopologyJobs &&
				payload->links.PortableBytes() <= InstanceTopology::Trace::maxPortableBytes;
			const auto claim = cache.ClaimTopology(key, *payload->plan, Store::Clock::now(), canQueue);
			if (!claim.ticket) return claim.saved;

			if (ReserveTopologyJob()) {
				// Transfer ownership before the callback can run.
				auto* job = new (std::nothrow) TopologyJob{key, claim.ticket, payload};
				if (job && QueueUserWorkItem(PersistTopology, job, WT_EXECUTEDEFAULT)) return false;
				delete job;
				topologyJobs.fetch_sub(1, std::memory_order_relaxed);
			}
			cache.CancelTopology(key, claim.ticket);
		} catch (const std::exception&) {
		}
		return false;
	}

	PreloadStats Preload(unsigned maxPlans, std::chrono::steady_clock::time_point deadline,
		const std::atomic_bool& stop, PreloadProgress progress) noexcept
	{
		try {
			return Cache().Preload(maxPlans, deadline, stop, progress);
		} catch (const std::exception&) {
			return {.interrupted = true};
		}
	}

	bool AdmitOffline(OfflineGraph::Candidate&& candidate) noexcept
	{
		try {
			const auto key = MakeKey(candidate.name, candidate.shape);
			return Cache().AdmitOffline(key, candidate.shape, std::move(candidate.payload));
		} catch (const std::exception&) {
			return false;
		}
	}

	std::shared_ptr<const PreparedPlan> FindOffline(std::string_view name, GraphPlan::Shape shape) noexcept
	{
		if (name.empty() || name == "<unnamed>") return {};
		try {
			return Cache().FindOffline(MakeKey(name, shape));
		} catch (const std::exception&) {
			return {};
		}
	}

	std::shared_ptr<const PreparedPlan> FindPrepared(std::string_view name) noexcept
	{
		if (name.empty() || name == "<unnamed>") return {};
		try {
			return Cache().FindPrepared(MakeKey(name, {}).graph);
		} catch (const std::exception&) {
			return {};
		}
	}

	void ResolvePreparedMismatch(std::string_view name, const PreparedPlan& expected, PreparedMismatch mismatch) noexcept
	{
		try {
			Cache().ResolvePreparedMismatch(MakeKey(name, {}).graph, expected, mismatch);
		} catch (const std::exception&) {
		}
	}

	Lookup Read(std::string_view name, GraphPlan::Shape shape) noexcept
	{
		if (name.empty() || name == "<unnamed>") return {Result::unavailable, {}};
		try {
			return Cache().Read(MakeKey(name, shape));
		} catch (const std::exception&) {
			return {Result::unavailable, {}};
		}
	}

	WriteResult Write(std::string_view name, GraphPlan::Shape shape, const GraphPlan::Entry& entry) noexcept
	{
		if (name.empty() || name == "<unnamed>") return WriteResult::skipped;
		try {
			return Cache().Write(MakeKey(name, shape), entry);
		} catch (const std::exception&) {
			return WriteResult::failed;
		}
	}
}
