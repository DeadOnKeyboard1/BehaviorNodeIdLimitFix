#include "HkxDirectoryIndex.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

namespace
{
	using Path = std::filesystem::path;
	constexpr std::string_view magic = "BHVRHKX1";
	constexpr std::size_t headerBytes = magic.size() + sizeof(std::uint64_t) + sizeof(std::uint32_t);
	constexpr std::size_t maxFolders = 32768;
	constexpr std::size_t maxWorkingFolders = maxFolders + 1024;
	constexpr std::size_t maxEntries = 250000;
	constexpr std::size_t maxPathChars = 2048;
	constexpr std::size_t maxNameChars = 255;
	constexpr std::size_t ioChunkBytes = 256u * 1024u;

	bool PlainDirectory(DWORD flags) noexcept
	{
		return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) && !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
	}

	bool IsHkx(std::wstring_view name) noexcept
	{
		return name.size() > 4 && _wcsnicmp(name.data() + name.size() - 4, L".hkx", 4) == 0;
	}

	bool ValidName(std::wstring_view name) noexcept
	{
		return !name.empty() && name != L"." && name != L".." &&
			name.back() != L'.' && name.back() != L' ' &&
			name.find_first_of(L"/\\:*?\"<>|") == name.npos && name.find(L'\0') == name.npos;
	}

	bool ValidRelativePath(std::wstring_view path) noexcept
	{
		while (!path.empty()) {
			const auto separator = path.find_first_of(L"/\\");
			if (!ValidName(path.substr(0, separator))) return false;
			if (separator == path.npos) return true;
			path.remove_prefix(separator + 1);
		}
		return false;
	}

	// Only the real NTFS namespace is indexed. A virtualized or different
	// filesystem can still use the ordinary scoped enumeration.
	bool CanIndex(const Path& root)
	{
		if (GetModuleHandleW(L"usvfs_x64.dll")) return false;
		const HANDLE directory = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (directory == INVALID_HANDLE_VALUE) return false;
		const std::unique_ptr<void, decltype(&CloseHandle)> closeDirectory(directory, CloseHandle);
		wchar_t name[32]{};
		return GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, nullptr, name, 32) &&
			_wcsicmp(name, L"NTFS") == 0;
	}

	template <class T>
	bool Take(std::string_view& bytes, T& value)
	{
		if (bytes.size() < sizeof(value)) return false;
		std::memcpy(&value, bytes.data(), sizeof(value));
		bytes.remove_prefix(sizeof(value));
		return true;
	}

	bool TakeText(std::string_view& bytes, std::wstring& text, std::size_t limit)
	{
		std::uint16_t length{};
		if (!Take(bytes, length) || !length || length > limit || bytes.size() < length * sizeof(wchar_t)) return false;
		text.resize(length);
		const auto size = length * sizeof(wchar_t);
		std::memcpy(text.data(), bytes.data(), size);
		bytes.remove_prefix(size);
		return true;
	}

	std::size_t TextBytes(std::wstring_view text) noexcept
	{
		return sizeof(std::uint16_t) + text.size() * sizeof(wchar_t);
	}

	template <class T>
	void Put(std::string& bytes, const T& value)
	{
		bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
	}

	void PutText(std::string& bytes, std::wstring_view text)
	{
		Put(bytes, static_cast<std::uint16_t>(text.size()));
		bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
	}

	std::optional<std::uint64_t> Checksum(std::string_view bytes, auto stopped) noexcept
	{
		std::uint64_t hash = 14695981039346656037ULL;
		while (!bytes.empty()) {
			if (stopped()) return {};
			const auto chunk = bytes.substr(0, ioChunkBytes);
			for (const auto byte : chunk) {
				hash ^= static_cast<unsigned char>(byte);
				hash *= 1099511628211ULL;
			}
			bytes.remove_prefix(chunk.size());
		}
		return hash;
	}
}

namespace AnimationPreload
{
	DirectoryIndex::DirectoryIndex(Path a_root, Path a_file, const std::atomic_bool* stop, Clock::time_point deadline) :
		root(std::move(a_root)), file(std::move(a_file))
	{
		if ((stop && stop->load(std::memory_order_relaxed)) ||
			(deadline != (Clock::time_point::max)() && Clock::now() >= deadline) ||
			!file.is_absolute() || !CanIndex(root)) file.clear();
		if (!file.empty()) Load(stop, deadline);
	}

	DirectoryIndex::Stamp DirectoryIndex::Identify(const Path& path) noexcept
	{
		const HANDLE directory = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (directory == INVALID_HANDLE_VALUE) return {};
		const std::unique_ptr<void, decltype(&CloseHandle)> closeDirectory(directory, CloseHandle);
		FILE_BASIC_INFO times{};
		BY_HANDLE_FILE_INFORMATION id{};
		if (!GetFileInformationByHandleEx(directory, FileBasicInfo, &times, sizeof(times)) ||
			!PlainDirectory(times.FileAttributes) || !times.ChangeTime.QuadPart ||
			!GetFileInformationByHandle(directory, &id)) return {};
		return {id.dwVolumeSerialNumber, (std::uint64_t{id.nFileIndexHigh} << 32) | id.nFileIndexLow,
			times.CreationTime.QuadPart, times.LastWriteTime.QuadPart, times.ChangeTime.QuadPart};
	}

	void DirectoryIndex::Load(const std::atomic_bool* stop, Clock::time_point deadline) noexcept
	{
		const auto stopped = [&] { return (stop && stop->load(std::memory_order_relaxed)) ||
			(deadline != (Clock::time_point::max)() && Clock::now() >= deadline); };
		try {
			if (stopped()) return;
			std::error_code error;
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, error)) || error) return;
			std::ifstream input(file, std::ios::binary | std::ios::ate);
			const auto length = static_cast<std::streamoff>(input.tellg());
			if (length < static_cast<std::streamoff>(headerBytes) || length > static_cast<std::streamoff>(maxEncodedBytes)) return;
			if (!input.seekg(0) || stopped()) return;
			const auto size = static_cast<std::size_t>(length);
			const auto buffer = std::make_unique_for_overwrite<char[]>(size);
			for (std::size_t offset = 0; offset < size;) {
				if (stopped()) return;
				const auto chunk = (std::min)(ioChunkBytes, size - offset);
				if (!input.read(buffer.get() + offset, static_cast<std::streamsize>(chunk))) return;
				offset += chunk;
			}
			std::string_view bytes(buffer.get(), size);
			if (!bytes.starts_with(magic)) return;
			bytes.remove_prefix(magic.size());
			std::uint64_t checksum{};
			std::uint32_t count{};
			if (!Take(bytes, checksum)) return;
			const auto checksummedBytes = bytes;
			if (!Take(bytes, count) || count > maxFolders || Checksum(checksummedBytes, stopped) != checksum) return;
			decltype(snapshots) restored;
			std::size_t entries{};
			for (std::uint32_t i = 0; i < count; ++i) {
				if (stopped()) return;
				const auto bytesBefore = bytes.size();
				std::wstring path;
				Stamp stamp{};
				std::uint32_t itemCount{};
				if (!TakeText(bytes, path, maxPathChars) || !ValidRelativePath(path) ||
					!Take(bytes, stamp) || !stamp.change || !Take(bytes, itemCount) || itemCount > maxEntries - entries ||
					itemCount > bytes.size() / (sizeof(std::uint16_t) + sizeof(wchar_t) + sizeof(std::uint8_t))) return;
				auto items = std::make_shared<List>();
				items->reserve(itemCount);
				for (std::uint32_t j = 0; j < itemCount; ++j) {
					if ((j & 255u) == 0 && stopped()) return;
					std::wstring name;
					std::uint8_t directory{};
					if (!TakeText(bytes, name, maxNameChars) || !ValidName(name) ||
						!Take(bytes, directory) || directory > 1 || (!directory && !IsHkx(name))) return;
					items->push_back({std::move(name), directory != 0});
				}
				if (!restored.try_emplace(std::move(path), Snapshot{stamp, std::move(items), bytesBefore - bytes.size()}).second) return;
				entries += itemCount;
			}
			if (bytes.empty() && !stopped()) {
				snapshots = std::move(restored);
				savedChecksum = checksum;
				savedBytes = size;
			}
		} catch (const std::exception&) {
			snapshots.clear();
		}
	}

	std::shared_ptr<const DirectoryIndex::List> DirectoryIndex::Read(const Path& path, Clock::time_point deadline,
		const std::atomic_bool& stop, unsigned& remaining, bool* fullyRead)
	{
		const auto stopped = [&] { return stop.load(std::memory_order_relaxed) ||
			(deadline != (Clock::time_point::max)() && Clock::now() >= deadline); };
		if (fullyRead) *fullyRead = false;
		if (!remaining || stopped()) {
			incompleteRead = true;
			return {};
		}
		const auto stamp = file.empty() ? Stamp{} : Identify(path);
		auto key = stamp.change ? path.lexically_relative(root).native() : std::wstring{};
		const bool cacheable = key.size() <= maxPathChars && ValidRelativePath(key);
		const auto known = cacheable ? snapshots.find(key) : snapshots.end();
		if (known != snapshots.end() && known->second.stamp == stamp) {
			if (!known->second.visitOrder) {
				++reused;
				known->second.visitOrder = ++visitSequence;
			}
			if (fullyRead) *fullyRead = true;
			return known->second.items;
		}
		if (!stamp.change) {
			const auto attributes = GetFileAttributesW(path.c_str());
			if (!PlainDirectory(attributes)) {
				if (attributes == INVALID_FILE_ATTRIBUTES) {
					const auto error = GetLastError();
					incompleteRead |= error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
				}
				return {};
			}
			if (!file.empty()) incompleteRead = true;
		}
		WIN32_FIND_DATAW item{};
		const HANDLE handle = FindFirstFileW((path / L"*").c_str(), &item);
		if (handle == INVALID_HANDLE_VALUE) {
			incompleteRead |= GetLastError() != ERROR_FILE_NOT_FOUND;
			return {};
		}
		const std::unique_ptr<void, decltype(&FindClose)> closeDirectory(handle, FindClose);
		auto items = std::make_shared<List>();
		auto encodedBytes = TextBytes(key) + sizeof(Stamp) + sizeof(std::uint32_t);
		++scanned;
		do {
			if (!remaining || stopped()) {
				incompleteRead = true;
				return items;
			}
			const std::wstring_view name(item.cFileName);
			if (!ValidName(name)) continue;
			--remaining;
			if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			const bool directory = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (directory || IsHkx(name)) {
				encodedBytes += TextBytes(name) + sizeof(std::uint8_t);
				items->push_back({std::wstring(name), directory});
			}
		} while (FindNextFileW(handle, &item));
		if (GetLastError() != ERROR_NO_MORE_FILES || !remaining || stopped()) {
			incompleteRead = true;
			return items;
		}
		if (stamp.change && (cacheable || fullyRead) && Identify(path) != stamp) {
			incompleteRead = true;
			return items;
		}
		if (fullyRead) *fullyRead = file.empty() || stamp.change;
		if (cacheable && (snapshots.size() < maxWorkingFolders || known != snapshots.end())) {
			const auto order = known != snapshots.end() && known->second.visitOrder ? known->second.visitOrder : ++visitSequence;
			snapshots.insert_or_assign(std::move(key), Snapshot{stamp, items, encodedBytes, order});
			dirty = true;
		} else if (cacheable) {
			limited = true;
		}
		return items;
	}

	void DirectoryIndex::Save(bool complete, Clock::time_point deadline, const std::atomic_bool& stop, std::size_t byteLimit) noexcept
	{
		const auto stopped = [&] { return stop.load(std::memory_order_relaxed) ||
			(deadline != (Clock::time_point::max)() && Clock::now() >= deadline); };
		try {
			byteLimit = (std::min)(byteLimit, maxEncodedBytes);
			if (file.empty() || byteLimit < headerBytes || stopped()) return;
			if (complete && !incompleteRead) {
				dirty |= std::erase_if(snapshots, [](const auto& entry) { return !entry.second.visitOrder; }) != 0;
			}
			if (!dirty) return;
			using ItemOnDisk = decltype(snapshots)::value_type;
			std::vector<const ItemOnDisk*> ordered;
			ordered.reserve(snapshots.size());
			for (const auto& item : snapshots) ordered.push_back(&item);
			// Keep the directories verified by this startup first, in discovery
			// order. A partial scan can carry over older entries when they fit.
			std::ranges::sort(ordered, [](const auto* a, const auto* b) {
				const auto first = a->second.visitOrder;
				const auto second = b->second.visitOrder;
				if (first && second) return first < second;
				if (first || second) return first != 0;
				return a->first < b->first;
			});
			std::size_t entries{}, bytes = headerBytes, selected{};
			for (const auto* item : ordered) {
				if (stopped()) return;
				if (selected == maxFolders) {
					limited = true;
					break;
				}
				const auto& snapshot = item->second;
				if (snapshot.items->size() > maxEntries - entries || snapshot.encodedBytes > byteLimit - bytes) {
					limited = true;
					continue;
				}
				entries += snapshot.items->size();
				bytes += snapshot.encodedBytes;
				ordered[selected++] = item;
			}
			if (!selected && !ordered.empty()) return;
			std::string data;
			data.reserve(bytes);
			data.append(magic);
			Put(data, std::uint64_t{});
			Put(data, static_cast<std::uint32_t>(selected));
			for (std::size_t i = 0; i < selected; ++i) {
				if (stopped()) return;
				const auto& [path, snapshot] = *ordered[i];
				PutText(data, path);
				Put(data, snapshot.stamp);
				Put(data, static_cast<std::uint32_t>(snapshot.items->size()));
				std::size_t encodedItems{};
				for (const auto& item : *snapshot.items) {
					if ((encodedItems++ & 255u) == 0 && stopped()) return;
					PutText(data, item.name);
					Put(data, static_cast<std::uint8_t>(item.directory));
				}
			}
			if (data.size() != bytes || stopped()) return;
			const auto checksum = Checksum(std::string_view(data).substr(magic.size() + sizeof(std::uint64_t)), stopped);
			if (!checksum || stopped()) return;
			if (savedChecksum == checksum && savedBytes == data.size()) {
				dirty = false;
				return;
			}
			std::memcpy(data.data() + magic.size(), &*checksum, sizeof(*checksum));
			std::error_code error;
			std::filesystem::create_directories(file.parent_path(), error);
			if (error) return;
			auto temporary = file;
			temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
				std::to_wstring(Clock::now().time_since_epoch().count()) + L".tmp";
			std::ofstream output(temporary, std::ios::binary | std::ios::noreplace);
			if (!output) return;
			output.write(data.data(), static_cast<std::streamsize>(data.size()));
			output.close();
			if (output && !stopped()) {
				std::filesystem::rename(temporary, file, error);
				if (!error) {
					saved = true;
					savedChecksum = checksum;
					savedBytes = data.size();
					dirty = false;
					return;
				}
			}
			std::filesystem::remove(temporary, error);
		} catch (const std::exception&) {
		}
	}
}
