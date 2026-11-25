#pragma once

#include "GraphPerformance.h"

namespace HkxTiming
{
	struct Stats
	{
		GraphPerformance::LatencyStats open, fileRead;
		std::uint64_t bytes{}, readCalls{}, failedOpens{}, failedReads{}, shortReads{};
	};

	// One instance measures the actual Win32 calls for one loose asset.
	// The accumulated statistics belong to the startup worker.
	class File
	{
	public:
		using Clock = std::chrono::steady_clock;
		explicit File(Stats* stats) noexcept : stats(stats) {}
		~File() { if (stats && readAttempted) stats->fileRead.Add(readElapsed); }

		File(const File&) = delete;
		File& operator=(const File&) = delete;

		[[nodiscard]] bool Enabled() const noexcept { return stats != nullptr; }
		[[nodiscard]] Clock::time_point Now() const noexcept { return stats ? Clock::now() : Clock::time_point{}; }

		void Opened(Clock::time_point start, bool success) noexcept
		{
			if (!stats) return;
			stats->open.Add(Clock::now() - start);
			stats->failedOpens += !success;
		}

		void Read(Clock::time_point start, bool success, std::uint32_t bytes, std::uint32_t requested) noexcept
		{
			if (!stats) return;
			readElapsed += Clock::now() - start;
			readAttempted = true;
			++stats->readCalls;
			if (success) {
				stats->bytes += bytes;
				stats->shortReads += bytes != requested;
			} else {
				++stats->failedReads;
			}
		}

	private:
		Stats* stats;
		Clock::duration readElapsed{};
		bool readAttempted{};
	};
}
