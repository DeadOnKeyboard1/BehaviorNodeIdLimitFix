#include "AnimationPreload.h"
#include "HkxDirectoryIndex.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
	using AnimationPreload::Clock;
	using Path = std::filesystem::path;
	using namespace std::chrono_literals;
	constexpr unsigned fileLimit = 65535;
	constexpr unsigned directoryEntryLimit = 100000;
	constexpr unsigned maxDepth = 24;
	constexpr std::uint64_t minHeadroom = std::uint64_t{2} << 30;

	bool IsPlainDirectory(const Path& path) noexcept
	{
		const auto attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
			!(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
	}

	struct PriorityCoverage
	{
		std::unordered_set<std::wstring> roots;
		bool partial{};
	};

	struct Catalog
	{
		AnimationPreload::DirectoryIndex& index;
		const std::atomic_bool& stop;
		Clock::time_point deadline;
		std::vector<Path> files;
		unsigned scanRemaining{directoryEntryLimit};
		unsigned fileCap{fileLimit};
		const std::unordered_set<std::wstring_view>* excluded{};
		const std::unordered_set<std::wstring>* covered{};
		bool depthLimited{};

		[[nodiscard]] bool Done() const noexcept
		{
			return !scanRemaining || Interrupted();
		}

		[[nodiscard]] bool Interrupted() const noexcept
		{
			return files.size() >= fileCap ||
				stop.load(std::memory_order_relaxed) ||
				(deadline != (Clock::time_point::max)() && Clock::now() >= deadline);
		}

		[[nodiscard]] bool Complete() const noexcept
		{
			return !Done() && !depthLimited;
		}

		void ScanPriority(const Path& root, unsigned depth, PriorityCoverage& coverage)
		{
			const auto before = files.size();
			if (Scan(root, depth)) coverage.roots.emplace(root.native());
			else coverage.partial |= files.size() != before;
		}

		void ScanOar(const Path& animations, PriorityCoverage& coverage)
		{
			if (Done() || !IsPlainDirectory(animations / L"OpenAnimationReplacer")) return;
			const auto entries = index.Read(animations, deadline, stop, scanRemaining);
			if (!entries) return;
			for (const auto& entry : *entries) {
				if (entry.directory && _wcsicmp(entry.name.c_str(), L"OpenAnimationReplacer") == 0) {
					ScanPriority(animations / entry.name, maxDepth - 1, coverage);
				}
			}
		}

		bool Scan(const Path& path, unsigned depth = maxDepth, bool shallow = false)
		{
			if (covered && covered->contains(path.native())) return true;
			if (Done()) return false;
			bool fullyRead{};
			const auto entries = index.Read(path, deadline, stop, scanRemaining, &fullyRead);
			if (!entries) return false;
			for (const auto& entry : *entries) {
				if (Interrupted()) return false;
				if (entry.directory) {
					if (shallow) continue;
					if (depth) {
						fullyRead &= Scan(path / entry.name, depth - 1);
					} else {
						depthLimited = true;
						fullyRead = false;
					}
				} else {
					auto file = path / entry.name;
					if (!excluded || !excluded->contains(file.native())) files.push_back(std::move(file));
				}
			}
			return fullyRead && !Done();
		}
	};

	// Keep the I/O buffered. The system file cache can service a later open of
	// the same deployed file. The plug-in never owns an additional asset copy.
	struct ReadResult
	{
		std::uint64_t bytes{};
		bool complete{};
	};

	ReadResult WarmFile(const Path& path, std::uint64_t budget, std::array<std::byte, 128 * 1024>& buffer,
		const std::atomic_bool& stop, Clock::time_point deadline) noexcept
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE) return {};
		const std::unique_ptr<void, decltype(&CloseHandle)> closeFile(file, CloseHandle);
		BY_HANDLE_FILE_INFORMATION info{};
		if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
			(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
		const auto size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
		if (!size || size > budget) return {};
		auto remaining = size;
		while (remaining) {
			if (stop.load(std::memory_order_relaxed) ||
				(deadline != (Clock::time_point::max)() && Clock::now() >= deadline)) break;
			const auto count = static_cast<DWORD>((std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
			DWORD read{};
			if (!ReadFile(file, buffer.data(), count, &read, nullptr)) break;
			remaining -= read;
			if (read != count) break;
		}
		return {size - remaining, remaining == 0};
	}

	bool HasHeadroom() noexcept
	{
		MEMORYSTATUSEX memory{};
		memory.dwLength = sizeof(memory);
		return GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys > minHeadroom;
	}
}

namespace AnimationPreload
{
	std::uint64_t BudgetBytes(std::uint64_t available, unsigned maximumMiB) noexcept
	{
		if (available <= minHeadroom) return 0;
		return (std::min)({available / 4, available - minHeadroom, std::uint64_t{maximumMiB} << 20});
	}

	Summary Warm(const std::filesystem::path& meshes, std::uint64_t budget, Clock::time_point deadline,
		const std::atomic_bool& stop, Progress progress, const std::filesystem::path& indexFile) noexcept
	{
		Summary summary;
		try {
			const auto stopped = [&] { return stop.load(std::memory_order_relaxed) ||
				(deadline != (Clock::time_point::max)() && Clock::now() >= deadline); };
			if (stopped()) return {.interrupted = true};
			if (!budget || !meshes.is_absolute() || !IsPlainDirectory(meshes)) return summary;
			DirectoryIndex index(meshes, indexFile, &stop, deadline);
			if (stopped()) return {.interrupted = true};
			std::array<std::byte, 128 * 1024> buffer;
			const auto warmBatch = [&](const Catalog& catalog, bool behavior) {
				const auto selected = static_cast<unsigned>(catalog.files.size());
				summary.catalogLimited |= !catalog.Complete();
				summary.selected += selected;
				if (progress && selected && !stopped()) progress(behavior, 0, selected);
				unsigned checked{};
				for (const auto& path : catalog.files) {
					if (stopped() || (checked % 64 == 0 && !HasHeadroom())) {
						summary.interrupted = true;
						break;
					}
					if (summary.bytes == budget) break;
					const auto read = WarmFile(path, budget - summary.bytes, buffer, stop, deadline);
					summary.bytes += read.bytes;
					if (read.complete) {
						if (behavior) ++summary.behaviorFiles;
						else ++summary.animationFiles;
					}
					++checked;
					++summary.checked;
					if (progress) progress(behavior, checked, selected);
				}
			};
			if (progress) progress(true, 0, 0);
			Catalog behaviors{index, stop, deadline};
			const auto actors = meshes / L"actors";
			std::vector<Path> actorsToScan;
			const auto character = actors / L"character";
			if (IsPlainDirectory(actors) && IsPlainDirectory(character)) actorsToScan.push_back(character);
			if (const auto entries = index.Read(actors, behaviors.deadline, stop, behaviors.scanRemaining)) {
				for (const auto& entry : *entries) {
					if (behaviors.Interrupted()) break;
					if (entry.directory && _wcsicmp(entry.name.c_str(), L"character") != 0) actorsToScan.push_back(actors / entry.name);
				}
			}
			std::vector<Path> actorRoots;
			actorRoots.reserve(actorsToScan.size() * 2);
			for (auto& actor : actorsToScan) {
				if (behaviors.Interrupted()) break;
				auto firstPerson = actor / L"_1stperson";
				const bool hasFirstPerson = IsPlainDirectory(firstPerson);
				actorRoots.push_back(std::move(actor));
				if (hasFirstPerson) actorRoots.push_back(std::move(firstPerson));
			}
			for (const auto& root : actorRoots) {
				if (behaviors.Done()) break;
				behaviors.Scan(root, 0, true);
				for (const auto* folder : {L"behaviors", L"characters", L"characters female"}) behaviors.Scan(root / folder);
				for (const auto* folder : {L"character assets", L"character assets female"}) behaviors.Scan(root / folder, 0, true);
			}
			warmBatch(behaviors, true);
			if (!summary.interrupted && !stopped() && summary.bytes < budget) {
				if (progress) progress(false, 0, 0);
				// Reserve a bounded first tranche for OAR's two documented loose
				// layouts. Do not interpret OAR conditions or configuration.
				Catalog priority{index, stop, deadline};
				priority.fileCap = fileLimit / 8;
				PriorityCoverage coverage;
				for (const auto& root : actorRoots) priority.ScanOar(root / L"animations", coverage);
				priority.ScanPriority(meshes / L"OpenAnimationReplacer", maxDepth, coverage);

				std::unordered_set<std::wstring_view> alreadySelected;
				if (coverage.partial) {
					alreadySelected.reserve(priority.files.size());
					for (const auto& path : priority.files) alreadySelected.emplace(path.native());
				}

				Catalog animations{index, stop, deadline};
				animations.fileCap = fileLimit - static_cast<unsigned>(priority.files.size());
				animations.excluded = coverage.partial ? &alreadySelected : nullptr;
				animations.covered = coverage.roots.empty() ? nullptr : &coverage.roots;
				for (const auto& root : actorRoots) {
					if (animations.Done()) break;
					animations.Scan(root / L"animations");
				}
				animations.Scan(meshes / L"OpenAnimationReplacer");
				index.Save(behaviors.Complete() && animations.Complete(), deadline, stop);

				// The set contains views of priority paths and must be discarded before moving those paths.
				alreadySelected.clear();
				animations.excluded = nullptr;
				priority.fileCap = fileLimit;
				priority.scanRemaining = animations.scanRemaining;
				priority.depthLimited = animations.depthLimited;
				priority.files.reserve(priority.files.size() + animations.files.size());
				for (auto& path : animations.files) priority.files.push_back(std::move(path));
				warmBatch(priority, false);
			} else {
				index.Save(false, deadline, stop);
			}
			summary.indexedDirectories = index.reused;
			summary.scannedDirectories = index.scanned;
			summary.indexSaved = index.saved;
			summary.indexLimited = index.limited;
			summary.interrupted |= stopped();
		} catch (const std::exception&) {
			summary.interrupted = true;
		}
		return summary;
	}
}
