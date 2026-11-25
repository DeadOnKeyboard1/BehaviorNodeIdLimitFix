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

	struct LatencyStats
	{
		std::uint64_t samples{};
		Duration total{}, maximum{};
		LatencyHistogram latency;

		void Add(Duration duration) noexcept
		{
			++samples;
			total += duration;
			maximum = (std::max)(maximum, duration);
			latency.Add(duration);
		}

		[[nodiscard]] Duration Mean() const noexcept
		{
			return samples ? total / static_cast<Duration::rep>(samples) : Duration{};
		}

		[[nodiscard]] std::uint64_t P95UpperMicros() const noexcept { return latency.P95UpperMicros(samples, maximum); }
	};

	inline constexpr auto slowInitialization = std::chrono::milliseconds{16};

	struct HookSample
	{
		Duration native{}, plugin{}, nestedTime{};
		std::uint64_t nestedCalls{};
		bool externalTarget{};

		[[nodiscard]] Duration NativeExclusive() const noexcept
		{
			return (std::max)(Duration{}, native - nestedTime);
		}
	};

	// Each immediate nested hook contributes its whole wrapper time once.
	// This excludes its native work and plugin overhead from the parent's native timing.
	class HookTiming
	{
	public:
		explicit HookTiming(bool enabled) noexcept : enabled(enabled)
		{
			if (!enabled) return;
			start = Clock::now();
			parent = current;
			current = this;
		}

		~HookTiming()
		{
			if (!enabled) return;
			current = parent;
			if (parent && parent->insideNative) {
				parent->nestedTime += Clock::now() - start;
				++parent->nestedCalls;
			}
		}

		HookTiming(const HookTiming&) = delete;
		HookTiming& operator=(const HookTiming&) = delete;

		void BeginNative() noexcept
		{
			if (!enabled) return;
			nativeStart = Clock::now();
			insideNative = true;
		}

		void EndNative() noexcept
		{
			if (!enabled) return;
			native = Clock::now() - nativeStart;
			insideNative = false;
		}

		[[nodiscard]] HookSample Finish(bool externalTarget) const noexcept
		{
			if (!enabled) return {};
			return {native, (Clock::now() - start) - native, nestedTime, nestedCalls, externalTarget};
		}

	private:
		using Clock = std::chrono::steady_clock;
		static inline thread_local HookTiming* current{};
		HookTiming* parent{};
		Clock::time_point start{}, nativeStart{};
		Duration native{}, nestedTime{};
		std::uint64_t nestedCalls{};
		bool enabled, insideNative{};
	};

	struct Counters
	{
		std::array<std::uint64_t, static_cast<std::size_t>(Outcome::count)> outcomes{};
		std::uint64_t calls{};
		Duration nativeTotal{}, nativeMax{}, pluginTotal{}, pluginMax{};
		LatencyHistogram nativeLatency, pluginLatency;
		LatencyStats nativeExclusive;
		Duration nestedTime{};
		std::uint64_t nestedCalls{}, externalTargets{}, slow16ms{}, slow50ms{};

		void Add(Outcome outcome, HookSample sample) noexcept
		{
			const auto native = sample.native;
			const auto plugin = sample.plugin;
			++calls;
			++outcomes[static_cast<std::size_t>(outcome)];
			nativeTotal += native;
			nativeMax = (std::max)(nativeMax, native);
			pluginTotal += plugin;
			pluginMax = (std::max)(pluginMax, plugin);
			nativeLatency.Add(native);
			pluginLatency.Add(plugin);
			nativeExclusive.Add(sample.NativeExclusive());
			nestedTime += sample.nestedTime;
			nestedCalls += sample.nestedCalls;
			externalTargets += sample.externalTarget;
			slow16ms += native >= slowInitialization;
			slow50ms += native >= std::chrono::milliseconds{50};
		}

		[[nodiscard]] std::uint64_t Count(Outcome outcome) const noexcept
		{
			return outcomes[static_cast<std::size_t>(outcome)];
		}
	};

	[[nodiscard]] bool Start();
	void Record(Hook hook, Outcome outcome, HookSample sample) noexcept;
	[[nodiscard]] bool WantsDetail() noexcept;
	void QueueDetail(std::chrono::system_clock::time_point time, std::string line) noexcept;
}
