#include "OfflineGraph.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <cwchar>
#include <string_view>
#include <vector>

namespace
{
	using Path = std::filesystem::path;
	using OfflineGraph::Candidate;
	using OfflineGraph::Status;
	constexpr std::uint32_t format = 0x3142474fu;
	constexpr std::uint64_t minSource = (GraphPlan::nodeThreshold + 1u) * 72;
	constexpr std::uint64_t maxSource = 64u << 20;
	constexpr std::uint64_t totalSourceBytes = 512u << 20;
	constexpr std::size_t maxEntries = 32768;
	constexpr auto zero = std::uint32_t{0};

	struct SourceStamp
	{
		std::uint64_t volume, file, size, created, written, changed;
		bool operator==(const SourceStamp&) const = default;
	};

	std::optional<SourceStamp> Identify(HANDLE file)
	{
		BY_HANDLE_FILE_INFORMATION info{};
		FILE_BASIC_INFO basic{};
		if (GetFileType(file) != FILE_TYPE_DISK ||
			!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
			!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) || !basic.ChangeTime.QuadPart) return {};
		return SourceStamp{info.dwVolumeSerialNumber, (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
			(std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
			static_cast<std::uint64_t>(basic.CreationTime.QuadPart), static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
			static_cast<std::uint64_t>(basic.ChangeTime.QuadPart)};
	}

	bool IsNtfs(HANDLE file) noexcept
	{
		wchar_t filesystem[16]{};
		return GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 16) &&
			_wcsicmp(filesystem, L"NTFS") == 0;
	}

	bool Plain(DWORD attributes, bool directory) noexcept
	{
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
			((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
	}

	struct SourceFile
	{
		Path path;
		bool character;
	};

	bool Earlier(const SourceFile& lhs, const SourceFile& rhs) noexcept
	{
		if (lhs.character != rhs.character) return lhs.character;
		return lhs.path < rhs.path;
	}

	void ListBehaviorFiles(const Path& directory, std::vector<SourceFile>& files, const std::atomic_bool& stop,
		std::size_t& remaining, unsigned depth, bool character)
	{
		if (!remaining || stop.load(std::memory_order_relaxed) || !Plain(GetFileAttributesW(directory.c_str()), true)) return;
		WIN32_FIND_DATAW item{};
		const auto handle = FindFirstFileW((directory / L"*").c_str(), &item);
		if (handle == INVALID_HANDLE_VALUE) return;
		const std::unique_ptr<void, decltype(&FindClose)> close(handle, FindClose);
		do {
			if (!remaining || stop.load(std::memory_order_relaxed)) return;
			--remaining;
			const std::wstring_view name(item.cFileName);
			if (name == L"." || name == L"..") continue;
			if (depth && Plain(item.dwFileAttributes, true)) {
				ListBehaviorFiles(directory / name, files, stop, remaining, depth - 1, character);
			}
			if (!Plain(item.dwFileAttributes, false) || name.size() <= 4 || _wcsnicmp(name.data() + name.size() - 4, L".hkx", 4) != 0) continue;
			const auto size = (std::uint64_t{item.nFileSizeHigh} << 32) | item.nFileSizeLow;
			if (size >= minSource && size <= maxSource) files.push_back({directory / name, character});
		} while (FindNextFileW(handle, &item));
	}

	void ListActorBehaviors(const Path& actor, std::vector<SourceFile>& files, const std::atomic_bool& stop,
		std::size_t& remaining, bool character)
	{
		ListBehaviorFiles(actor / L"behaviors", files, stop, remaining, 3, character);
		if (!remaining || stop.load(std::memory_order_relaxed)) return;
		const auto firstPerson = actor / L"_1stperson";
		if (Plain(GetFileAttributesW(firstPerson.c_str()), true)) {
			ListBehaviorFiles(firstPerson / L"behaviors", files, stop, remaining, 3, character);
		}
	}

	bool ReadSource(HANDLE file, std::span<char> bytes, std::uint64_t& totalRead,
		const std::atomic_bool& stop, HkxTiming::File& timing)
	{
		while (!bytes.empty()) {
			if (stop.load(std::memory_order_relaxed)) return false;
			const auto request = static_cast<DWORD>((std::min)(bytes.size(), std::size_t{256} << 10));
			DWORD read{};
			const auto readStart = timing.Now();
			const bool success = ReadFile(file, bytes.data(), request, &read, nullptr) != FALSE;
			timing.Read(readStart, success, read, request);
			if (!success) return false;
			totalRead += read;
			if (read != request) return false;
			bytes = bytes.subspan(read);
		}
		return true;
	}

	std::optional<OfflineGraph::Analysis> AnalyzeSource(HANDLE file, const SourceStamp& stamp,
		OfflineGraph::Summary& summary, const std::atomic_bool& stop, HkxTiming::File& timing)
	{
		auto& totalRead = summary.sourceBytesRead;
		std::array<char, OfflineGraph::packfileHeaderBytes> header;
		if (header.size() > totalSourceBytes - totalRead) {
			++summary.deferred;
			return {};
		}
		if (!ReadSource(file, header, totalRead, stop, timing)) return {};
		if (const auto screened = OfflineGraph::ScreenHeader(header, stamp.size)) return OfflineGraph::Analysis{*screened, {}};

		const auto remaining = stamp.size - header.size();
		if (remaining > totalSourceBytes - totalRead) {
			++summary.deferred;
			return {};
		}
		if (stop.load(std::memory_order_relaxed)) return {};
		auto bytes = std::make_unique_for_overwrite<char[]>(static_cast<std::size_t>(stamp.size));
		std::memcpy(bytes.get(), header.data(), header.size());
		const std::span data{bytes.get(), static_cast<std::size_t>(stamp.size)};
		if (!ReadSource(file, data.subspan(header.size()), totalRead, stop, timing)) return {};
		const auto decodeStart = timing.Now();
		auto result = OfflineGraph::Analyze(data, stop);
		if (timing.Enabled()) summary.sourceDecode.Add(OfflineGraph::Clock::now() - decodeStart);
		return result;
	}

	template <class T>
	bool Read(std::istream& in, T& value)
	{
		return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
	}

	void Write(std::ostream& out, const auto& value)
	{
		out.write(reinterpret_cast<const char*>(&value), sizeof(value));
	}

	GraphPlan::Shape Shape(const GraphPlan::Entry& plan)
	{
		GraphPlan::Signature classification;
		for (const auto ordinal : plan.stateMachines) classification.Add(ordinal);
		return {plan.topology, classification.value, plan.nodes};
	}

	std::optional<OfflineGraph::Analysis> ReadCache(const Path& path, const SourceStamp& expected)
	{
		if (!Plain(GetFileAttributesW(path.c_str()), false)) return {};
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		const auto size = static_cast<std::streamoff>(in.tellg());
		constexpr auto header = 4 * sizeof(std::uint32_t) + sizeof(SourceStamp);
		if (size < static_cast<std::streamoff>(header) ||
			size > static_cast<std::streamoff>(header + 255 + GraphPlan::maxFileSize + InstanceTopology::Trace::maxPortableBytes) ||
			!in.seekg(0)) return {};
		std::uint32_t version{}, status{}, nameLength{}, planLength{};
		SourceStamp stamp{};
		if (!Read(in, version) || version != format || !Read(in, stamp) || stamp != expected ||
			!Read(in, status) || !Read(in, nameLength) || !Read(in, planLength)) return {};
		if (status <= static_cast<unsigned>(Status::belowLimit) && !nameLength && !planLength && size == static_cast<std::streamoff>(header)) {
			return OfflineGraph::Analysis{static_cast<Status>(status), {}};
		}
		if (status != static_cast<unsigned>(Status::planned) || !nameLength || nameLength > 255 ||
			planLength < GraphPlan::headerSize || planLength > GraphPlan::maxFileSize ||
			static_cast<std::size_t>(size) <= header + nameLength + planLength) return {};
		std::string name(nameLength, '\0');
		if (!in.read(name.data(), nameLength) || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }) ||
			name == "<unnamed>") return {};
		auto plan = GraphPlan::ReadEntryBytes([&](char* target, std::size_t count) {
			return static_cast<bool>(in.read(target, static_cast<std::streamsize>(count)));
		}, planLength);
		if (!plan) return {};
		auto links = InstanceTopology::Trace::ReadPortable(in, static_cast<std::size_t>(size) - header - nameLength - planLength, *plan);
		if (!links) return {};
		const auto shape = Shape(*plan);
		return OfflineGraph::Analysis{Status::planned, Candidate{std::move(name), shape,
			{std::move(*links), std::make_shared<const GraphPlan::Entry>(std::move(*plan))}}};
	}

	void SaveCache(const Path& path, const SourceStamp& stamp, const OfflineGraph::Analysis& analysis, const std::atomic_bool& stop)
	{
		if (stop.load(std::memory_order_relaxed) || analysis.status == Status::cancelled) return;
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return;
		auto temporary = path;
		temporary += std::format(".{}-{}.tmp", GetCurrentProcessId(), OfflineGraph::Clock::now().time_since_epoch().count());
		std::ofstream out(temporary, std::ios::binary | std::ios::noreplace);
		if (!out) return;
		Write(out, format);
		Write(out, stamp);
		Write(out, static_cast<std::uint32_t>(analysis.status));
		if (const auto& candidate = analysis.candidate) {
			const auto& plan = *candidate->payload.plan;
			const auto nameLength = static_cast<std::uint32_t>(candidate->name.size());
			const auto planLength = static_cast<std::uint32_t>(GraphPlan::headerSize + plan.stateMachines.size() * sizeof(std::uint32_t));
			Write(out, nameLength);
			Write(out, planLength);
			out.write(candidate->name.data(), nameLength);
			GraphPlan::WriteEntry(out, plan);
			if (!candidate->payload.links.WritePortable(out, plan)) out.setstate(std::ios::failbit);
		} else {
			Write(out, zero);
			Write(out, zero);
		}
		out.close();
		if (!out || stop.load(std::memory_order_relaxed) ||
			!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) std::filesystem::remove(temporary, error);
	}

	Path CacheName(const Path& directory, const Path& path, const Path& meshes)
	{
		GraphPlan::Signature key;
		for (const auto ch : path.lexically_relative(meshes).native()) key.Add(ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch);
		return directory / std::format("{:016x}.off", key.value);
	}
}

namespace OfflineGraph
{
	Summary Prepare(const Path& meshes, const Path& directory, const std::atomic_bool& stop, Publish publish, Progress progress,
		unsigned maxPlans, bool measureIO) noexcept
	{
		Summary summary;
		try {
			if (stop.load(std::memory_order_relaxed)) return {.interrupted = true};
			if (GetModuleHandleW(L"usvfs_x64.dll")) return {.virtualFilesystem = true};
			const auto actors = meshes / L"actors";
			if (!directory.is_absolute() || !actors.is_absolute() || !publish || !Plain(GetFileAttributesW(actors.c_str()), true)) return summary;
			std::vector<SourceFile> files;
			files.reserve(64);
			std::size_t remaining = maxEntries;
			const auto character = actors / L"character";
			const bool foundCharacter = Plain(GetFileAttributesW(character.c_str()), true);
			if (foundCharacter) ListActorBehaviors(character, files, stop, remaining, true);
			if (remaining && !stop.load(std::memory_order_relaxed)) {
				WIN32_FIND_DATAW item{};
				const auto search = FindFirstFileW((actors / L"*").c_str(), &item);
				if (search != INVALID_HANDLE_VALUE) {
					const std::unique_ptr<void, decltype(&FindClose)> close(search, FindClose);
					do {
						if (stop.load(std::memory_order_relaxed)) return {.interrupted = true};
						if (!remaining) break;
						--remaining;
						const std::wstring_view name(item.cFileName);
						if (name == L"." || name == L".." || !Plain(item.dwFileAttributes, true)) continue;
						const bool isCharacter = _wcsicmp(item.cFileName, L"character") == 0;
						if (foundCharacter && isCharacter) continue;
						ListActorBehaviors(actors / name, files, stop, remaining, isCharacter);
					} while (FindNextFileW(search, &item));
				}
			}
			if (stop.load(std::memory_order_relaxed)) return {.interrupted = true};
			summary.catalogLimited = remaining == 0;
			std::ranges::sort(files, Earlier);
			const auto count = static_cast<unsigned>(files.size());
			if (count && progress && !stop.load(std::memory_order_relaxed)) progress(0, count);
			for (const auto& source : files) {
				if (stop.load(std::memory_order_relaxed)) break;
				const auto& path = source.path;
				++summary.checked;
				try {
					HkxTiming::File timing(measureIO ? &summary.sourceIO : nullptr);
					const auto openStart = timing.Now();
					const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
					timing.Opened(openStart, handle != INVALID_HANDLE_VALUE);
					if (handle != INVALID_HANDLE_VALUE) {
						const std::unique_ptr<void, decltype(&CloseHandle)> close(handle, CloseHandle);
						const auto stamp = Identify(handle);
						if (stamp && stamp->size >= minSource && stamp->size <= maxSource && IsNtfs(handle)) {
							const auto cache = CacheName(directory, path, meshes);
							auto result = ReadCache(cache, *stamp);
							const bool reused = result.has_value();
							if (!result) result = AnalyzeSource(handle, *stamp, summary, stop, timing);
							if (result && result->status != Status::cancelled &&
								!stop.load(std::memory_order_relaxed) && Identify(handle) == stamp) {
								if (reused) ++summary.reused;
								else {
									if (result->candidate) ++summary.built;
									try {
										SaveCache(cache, *stamp, *result, stop);
									} catch (const std::exception&) {
										// A valid candidate remains usable even if persistence fails.
									}
								}
								if (stop.load(std::memory_order_relaxed)) break;
								if (result->status == Status::belowLimit) ++summary.belowLimit;
								if (result->status == Status::unsupported) ++summary.unsupported;
								if (result->candidate) {
									if (summary.admitted < maxPlans && publish(std::move(*result->candidate))) ++summary.admitted;
									else ++summary.notAdmitted;
								}
							}
						}
					}
				} catch (const std::exception&) {
					// The asset and the regular live path remain available.
				}
				if (progress && !stop.load(std::memory_order_relaxed)) progress(summary.checked, count);
			}
			summary.interrupted = stop.load(std::memory_order_relaxed);
		} catch (const std::exception&) {
			summary.interrupted = true;
		}
		return summary;
	}
}

