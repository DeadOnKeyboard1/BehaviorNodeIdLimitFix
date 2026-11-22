#include "GraphPerformance.h"

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <utility>

namespace
{
	using GraphPerformance::Counters;
	using GraphPerformance::Outcome;
	constexpr DWORD reportPeriodMs = 3000;
	constexpr std::size_t detailCapacity = 64;
	constexpr auto hookCount = static_cast<std::size_t>(GraphPerformance::Hook::count);
	struct CounterSlot
	{
		std::mutex mutex;
		Counters counts;
	};
	std::array<CounterSlot, hookCount> interval;
	std::atomic_flag reporting{};
	std::atomic_bool collecting{};
	bool firstReport = true;
	struct DetailLine
	{
		std::chrono::system_clock::time_point time;
		std::string message;
	};
	std::mutex detailMutex;
	std::array<DetailLine, detailCapacity> pendingDetails;
	std::atomic_size_t pendingDetailCount{};
	std::atomic_uint64_t droppedDetails{};

	std::int64_t Microseconds(GraphPerformance::Duration duration) noexcept
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
	}

	void CALLBACK Report(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER)
	{
		if (reporting.test_and_set(std::memory_order_acquire)) return;
		struct ResetReporting {
			~ResetReporting() { reporting.clear(std::memory_order_release); }
		} reset;
		std::array<Counters, hookCount> recent;
		for (std::size_t i = 0; i < recent.size(); ++i) {
			std::scoped_lock lock(interval[i].mutex);
			recent[i] = std::exchange(interval[i].counts, {});
		}
		std::array<DetailLine, detailCapacity> details;
		std::size_t detailCount{};
		{
			std::scoped_lock lock(detailMutex);
			detailCount = pendingDetailCount.load(std::memory_order_relaxed);
			for (std::size_t i = 0; i < detailCount; ++i) details[i] = std::move(pendingDetails[i]);
			pendingDetailCount.store(0, std::memory_order_relaxed);
		}
		const auto dropped = droppedDetails.exchange(0, std::memory_order_relaxed);
		auto log = spdlog::default_logger();
		if (log) {
			for (std::size_t i = 0; i < detailCount; ++i) {
				const auto& detail = details[i];
				log->log(detail.time, {}, spdlog::level::info, spdlog::string_view_t(detail.message.data(), detail.message.size()));
			}
		}
		if (dropped) logger::info("Graph performance detail rows dropped={}.", dropped);
		bool wrote = detailCount != 0 || dropped != 0;
		constexpr const char* sources[]{"BShkbLink", "initNodeIds"};
		static_assert(std::size(sources) == static_cast<std::size_t>(GraphPerformance::Hook::count));
		for (std::size_t i = 0; i < recent.size(); ++i) {
			const auto& data = recent[i];
			if (!data.calls) continue;
			logger::info("Graph performance [{}] calls={} belowLimit={} noGraph={} active={} notReady={} idsInUse={} changed={} "
				"blocked={} analysisBusy={} full={} preloadFast={} diskTopology={} offlineReplay={} offlineVerified={} instanceFast={} "
				"nativeTotal={}us nativeMax={}us nativeP95Upper={}us pluginTotal={}us pluginMax={}us pluginP95Upper={}us",
				sources[i], data.calls, data.Count(Outcome::belowLimit), data.Count(Outcome::noGraph),
				data.Count(Outcome::active), data.Count(Outcome::notReady), data.Count(Outcome::idsInUse),
				data.Count(Outcome::changed), data.Count(Outcome::blocked), data.Count(Outcome::analysisBusy),
				data.Count(Outcome::full), data.Count(Outcome::prepared), data.Count(Outcome::diskTopology),
				data.Count(Outcome::offlineTopology), data.Count(Outcome::offlineVerified), data.Count(Outcome::instance),
				Microseconds(data.nativeTotal), Microseconds(data.nativeMax), data.nativeLatency.P95UpperMicros(data.calls, data.nativeMax),
				Microseconds(data.pluginTotal), Microseconds(data.pluginMax), data.pluginLatency.P95UpperMicros(data.calls, data.pluginMax));
			wrote = true;
		}
		if (firstReport && !wrote) {
			logger::info("Graph performance: waiting for the first graph hook call.");
			wrote = true;
		}
		firstReport = false;
		if (wrote && log) log->flush();
	}
}

namespace GraphPerformance
{
	bool Start()
	{
		if (collecting.load(std::memory_order_relaxed)) return true;
		HMODULE module{};
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&collecting), &module)) return false;
		const auto timer = CreateThreadpoolTimer(Report, nullptr, nullptr);
		if (!timer) return false;

		// The timer shares the process lifetime of the installed native hooks.
		LARGE_INTEGER ticks{};
		ticks.QuadPart = -std::int64_t{reportPeriodMs} * 10'000;
		FILETIME due{ticks.LowPart, static_cast<DWORD>(ticks.HighPart)};
		collecting.store(true, std::memory_order_release);
		SetThreadpoolTimer(timer, &due, reportPeriodMs, 100);
		logger::info("Graph performance logging active. Interval {} ms; both graph hooks are sampled.", reportPeriodMs);
		return true;
	}

	void Record(Hook hook, Outcome outcome, Duration native, Duration plugin) noexcept
	{
		if (!collecting.load(std::memory_order_acquire)) return;
		auto& slot = interval[static_cast<std::size_t>(hook)];
		std::scoped_lock lock(slot.mutex);
		slot.counts.Add(outcome, native, plugin);
	}

	bool WantsDetail() noexcept
	{
		if (!collecting.load(std::memory_order_acquire)) return false;
		if (pendingDetailCount.load(std::memory_order_relaxed) < detailCapacity) return true;
		droppedDetails.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	void QueueDetail(std::chrono::system_clock::time_point time, std::string line) noexcept
	{
		std::unique_lock lock(detailMutex, std::try_to_lock);
		const auto index = pendingDetailCount.load(std::memory_order_relaxed);
		if (!lock || index >= pendingDetails.size()) {
			droppedDetails.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		pendingDetails[index] = {time, std::move(line)};
		pendingDetailCount.store(index + 1, std::memory_order_relaxed);
	}
}
