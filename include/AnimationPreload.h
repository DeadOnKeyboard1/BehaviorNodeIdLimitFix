#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>

namespace AnimationPreload
{
	using Clock = std::chrono::steady_clock;
	using Progress = void (*)(bool behavior, unsigned checked, unsigned selected) noexcept;

	struct Summary
	{
		unsigned selected{};
		unsigned checked{};
		unsigned behaviorFiles{};
		unsigned animationFiles{};
		unsigned indexedDirectories{};
		unsigned scannedDirectories{};
		std::uint64_t bytes{};
		bool indexSaved{};
		bool indexLimited{};
		bool catalogLimited{};
		bool interrupted{};
	};

	[[nodiscard]] std::uint64_t BudgetBytes(std::uint64_t available, unsigned maximumMiB) noexcept;
	[[nodiscard]] Summary Warm(const std::filesystem::path& meshes, std::uint64_t budget, Clock::time_point deadline,
		const std::atomic_bool& stop, Progress progress, const std::filesystem::path& indexFile = {}) noexcept;
}
