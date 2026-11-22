#pragma once

#include "GraphSlimming.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace OfflineGraph
{
	using Clock = std::chrono::steady_clock;

	enum class Status { unsupported, belowLimit, planned, cancelled };
	inline constexpr std::size_t packfileHeaderBytes = 208;

	struct Candidate
	{
		std::string name;
		GraphPlan::Shape shape;
		GraphSlimming::Payload payload;
	};

	struct Analysis
	{
		Status status{Status::unsupported};
		std::optional<Candidate> candidate;
	};

	// A result means the full file cannot produce a plan.
	[[nodiscard]] std::optional<Status> ScreenHeader(std::span<const char> header, std::uint64_t size) noexcept;
	[[nodiscard]] Analysis Analyze(std::span<const char> file, const std::atomic_bool& stop,
		Clock::time_point deadline = (Clock::time_point::max)());

	struct Summary
	{
		unsigned checked{};
		unsigned reused{};
		unsigned built{};
		unsigned admitted{};
		unsigned belowLimit{};
		unsigned unsupported{};
		std::uint64_t sourceBytesRead{};
		bool interrupted{};
	};

	using Publish = bool (*)(Candidate&&) noexcept;
	using Progress = void (*)(unsigned checked, unsigned selected) noexcept;
	[[nodiscard]] Summary Prepare(const std::filesystem::path& meshes, const std::filesystem::path& directory,
		const std::atomic_bool& stop, Publish publish, Progress progress) noexcept;
}

