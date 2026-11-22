#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AnimationPreload
{
	class DirectoryIndex
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t maxEncodedBytes = 32u * 1024u * 1024u;

		struct Item
		{
			std::wstring name;
			bool directory;
		};

		using List = std::vector<Item>;

		DirectoryIndex(std::filesystem::path root, std::filesystem::path file,
			const std::atomic_bool* stop = nullptr, Clock::time_point deadline = (Clock::time_point::max)());
		[[nodiscard]] std::shared_ptr<const List> Read(const std::filesystem::path& path, Clock::time_point deadline,
			const std::atomic_bool& stop, unsigned& remaining, bool* fullyRead = nullptr);
		void Save(bool complete, Clock::time_point deadline, const std::atomic_bool& stop,
			std::size_t byteLimit = maxEncodedBytes) noexcept;

		unsigned reused{};
		unsigned scanned{};
		bool saved{};
		bool limited{};

	private:
		struct Stamp
		{
			std::uint64_t volume{};
			std::uint64_t fileId{};
			std::int64_t creation{};
			std::int64_t write{};
			std::int64_t change{};
			bool operator==(const Stamp&) const = default;
		};

		struct Snapshot
		{
			Stamp stamp;
			std::shared_ptr<const List> items;
			std::size_t encodedBytes{};
			unsigned visitOrder{};
		};

		[[nodiscard]] static Stamp Identify(const std::filesystem::path& path) noexcept;
		void Load(const std::atomic_bool* stop, Clock::time_point deadline) noexcept;
		std::filesystem::path root;
		std::filesystem::path file;
		std::unordered_map<std::wstring, Snapshot> snapshots;
		std::optional<std::uint64_t> savedChecksum;
		std::size_t savedBytes{};
		unsigned visitSequence{};
		bool dirty{};
		bool incompleteRead{};
	};
}
