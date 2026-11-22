#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace GraphPerformance
{
	enum class Hook { preSetup, initNodeIds, count };
	enum class Outcome { noGraph, active, notReady, idsInUse, changed, belowLimit, blocked, analysisBusy, full, prepared, diskTopology, offlineTopology, offlineVerified, instance, count };
	using Duration = std::chrono::steady_clock::duration;

	struct LatencyHistogram
	{
		// Buckets cover <= 125 us, then powers of two, with a final overflow bucket.
		std::array<std::uint64_t, 17> buckets{};

		void Add(Duration duration) noexcept
		{
			const auto micros = std::chrono::ceil<std::chrono::microseconds>(duration).count();
			const auto units = micros > 0 ? static_cast<std::uint64_t>((micros - 1) / 125) : 0;
			const auto bucket = (std::min)(static_cast<std::size_t>(std::bit_width(units)), buckets.size() - 1);
			++buckets[bucket];
		}

		[[nodiscard]] std::uint64_t P95UpperMicros(std::uint64_t count, Duration maximum) const noexcept
		{
			if (!count) return 0;
			const auto measured = std::chrono::ceil<std::chrono::microseconds>(maximum).count();
			const auto observedMax = measured > 0 ? static_cast<std::uint64_t>(measured) : 0;
			auto rank = count - count / 20;
			for (std::size_t i = 0; i + 1 < buckets.size(); ++i) {
				if (rank <= buckets[i]) return (std::min)(observedMax, std::uint64_t{125} << i);
				rank -= buckets[i];
			}
			return observedMax;
		}
	};

	struct Counters
	{
		std::array<std::uint64_t, static_cast<std::size_t>(Outcome::count)> outcomes{};
		std::uint64_t calls{};
		Duration nativeTotal{}, nativeMax{}, pluginTotal{}, pluginMax{};
		LatencyHistogram nativeLatency, pluginLatency;

		void Add(Outcome outcome, Duration native, Duration plugin) noexcept
		{
			++calls;
			++outcomes[static_cast<std::size_t>(outcome)];
			nativeTotal += native;
			nativeMax = (std::max)(nativeMax, native);
			pluginTotal += plugin;
			pluginMax = (std::max)(pluginMax, plugin);
			nativeLatency.Add(native);
			pluginLatency.Add(plugin);
		}

		[[nodiscard]] std::uint64_t Count(Outcome outcome) const noexcept
		{
			return outcomes[static_cast<std::size_t>(outcome)];
		}
	};

	[[nodiscard]] bool Start();
	void Record(Hook hook, Outcome outcome, Duration native, Duration plugin) noexcept;
	[[nodiscard]] bool WantsDetail() noexcept;
	void QueueDetail(std::chrono::system_clock::time_point time, std::string line) noexcept;
}
