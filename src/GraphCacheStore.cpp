#include "GraphCacheStore.h"
#include "GraphSlimming.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{
	constexpr unsigned scanLimit = 4096;
	constexpr unsigned extraPlanChecks = 32;
	constexpr std::size_t preloadByteLimit = 16 * 1024 * 1024;
	constexpr std::size_t preparedByteLimit = 32 * 1024 * 1024;
	constexpr std::size_t preparedPlanLimit = 4 * 1024 * 1024;
	constexpr std::size_t topologyByteLimit = 24 * 1024 * 1024;
	constexpr std::size_t offlineByteLimit = 64 * 1024 * 1024;
	constexpr unsigned offlinePlanLimit = 256;
	constexpr std::uint64_t graphStorageVersion = 0x31454C444E425247ULL;
	constexpr std::size_t graphHeaderBytes = 3 * sizeof(std::uint64_t) + sizeof(std::uint32_t);
	constexpr std::size_t minGraphFileBytes = graphHeaderBytes + GraphPlan::headerSize;
	constexpr std::size_t maxGraphFileBytes = graphHeaderBytes + GraphPlan::maxFileSize + InstanceTopology::Trace::maxPortableBytes;
	constexpr auto retryDelay = std::chrono::seconds(30);
	std::atomic<std::uint64_t> writeSequence{};

	// Shared delete keeps an open reader on its original file while an
	// atomic replacement publishes the new cache file at the same path.
	class CacheReader
	{
	public:
		explicit CacheReader(const std::filesystem::path& path) noexcept :
			file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr), CloseHandle)
		{
			if (file.get() == INVALID_HANDLE_VALUE) {
				const auto error = GetLastError();
				status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
					GraphCache::Result::missing : GraphCache::Result::unavailable;
				(void)file.release();
				return;
			}
			BY_HANDLE_FILE_INFORMATION info{};
			if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
				(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
			length = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
			status = GraphCache::Result::hit;
		}

		bool Read(void* bytes, std::size_t size) noexcept
		{
			// Cache formats bound every read well below the Win32 I/O limit.
			const auto count = static_cast<DWORD>(size);
			DWORD read{};
			if (!ReadFile(file.get(), bytes, count, &read, nullptr)) {
				status = GraphCache::Result::unavailable;
				return false;
			}
			if (read != count) status = GraphCache::Result::invalid;
			return read == count;
		}

		std::uint64_t length{};
		GraphCache::Result status{GraphCache::Result::unavailable};

	private:
		std::unique_ptr<void, decltype(&CloseHandle)> file;
	};

	std::optional<GraphPlan::Entry> ReadPlan(CacheReader& file, std::size_t size)
	{
		if (size < GraphPlan::headerSize || size > GraphPlan::maxFileSize) return {};
		// Keep small plans to one file read. Large ordinal arrays go straight into their final storage.
		std::array<char, 512> scratch;
		if (size <= scratch.size()) {
			if (!file.Read(scratch.data(), size)) return {};
			std::span<const char> bytes(scratch.data(), size);
			return GraphPlan::ReadEntryBytes([&](char* target, std::size_t count) {
				if (count > bytes.size()) return false;
				std::memcpy(target, bytes.data(), count);
				bytes = bytes.subspan(count);
				return true;
			}, size);
		}
		return GraphPlan::ReadEntryBytes([&](char* target, std::size_t count) { return file.Read(target, count); }, size);
	}

	GraphCache::Lookup ReadPlanFile(const std::filesystem::path& path)
	{
		using GraphCache::Result;
		CacheReader file(path);
		if (file.status != Result::hit) return {file.status, {}};
		auto entry = ReadPlan(file, file.length);
		if (!entry) return {file.status == Result::hit ? Result::invalid : file.status, {}};
		return {Result::hit, std::make_shared<const GraphPlan::Entry>(std::move(*entry)), GraphCache::Origin::disk, false, true};
	}

	std::optional<GraphCache::Key> ParseVariantName(std::wstring_view name) noexcept
	{
		if ((name.size() != 37 || !name.ends_with(L".bin")) &&
			(name.size() != 39 || !name.ends_with(L".graph"))) return {};
		if (name[16] != L'-') return {};
		const auto parse = [](std::wstring_view digits, std::uint64_t& value) {
			for (auto digit : digits) {
				if (digit >= L'A' && digit <= L'F') digit += L'a' - L'A';
				value <<= 4;
				if (digit >= L'0' && digit <= L'9') value |= static_cast<unsigned>(digit - L'0');
				else if (digit >= L'a' && digit <= L'f') value |= static_cast<unsigned>(digit - L'a' + 10);
				else return false;
			}
			return true;
		};
		GraphCache::Key key{};
		if (!parse(name.substr(0, 16), key.graph) || !parse(name.substr(17, 16), key.shape)) return {};
		return key;
	}

	struct PreloadCandidate
	{
		GraphCache::Key key;
		std::uint64_t modified;
		std::size_t bytes;
		bool hasBundle;
	};

	GraphPlan::Shape CachedShape(const GraphPlan::Entry& plan) noexcept
	{
		GraphPlan::Signature classification;
		for (const auto ordinal : plan.stateMachines) classification.Add(ordinal);
		return {plan.topology, classification.value, plan.nodes};
	}

	// Remove an uncommitted temporary file after its stream closes.
	struct RemoveTemporary
	{
		const std::filesystem::path& path;
		bool ownsFile{};

		~RemoveTemporary()
		{
			if (!ownsFile) return;
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	};

	std::optional<InstanceTopology::Trace> ReadTopology(CacheReader& file, std::size_t size, const GraphPlan::Entry& plan)
	{
		if (!size || size > InstanceTopology::Trace::maxPortableBytes) return {};
		return InstanceTopology::Trace::ReadPortableBytes(size, plan,
			[&](char* bytes, std::size_t count) { return file.Read(bytes, count); });
	}

	std::optional<InstanceTopology::Trace> ReadTopologyFile(const std::filesystem::path& path, const GraphPlan::Entry& plan)
	{
		CacheReader file(path);
		return file.status == GraphCache::Result::hit ? ReadTopology(file, file.length, plan) : std::nullopt;
	}

	bool PublishCacheFile(const std::filesystem::path& temporary, const std::filesystem::path& path)
	{
		if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
		const auto error = GetLastError();
		if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) return false;

		// On a filesystem supporting FileRenameInfoEx, existing readers with
		// delete sharing can finish reading the old file after replacement.
		const HANDLE file = CreateFileW(temporary.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		const std::unique_ptr<void, decltype(&CloseHandle)> closeFile(file, CloseHandle);
		const auto target = std::filesystem::absolute(path);
		const auto nameBytes = target.native().size() * sizeof(wchar_t);
		const auto size = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
		auto storage = std::make_unique<std::byte[]>(size);
		auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
		info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
		info->FileNameLength = static_cast<DWORD>(nameBytes);
		std::memcpy(storage.get() + offsetof(FILE_RENAME_INFO, FileName), target.c_str(), nameBytes + sizeof(wchar_t));
		// The SDK hides this class when targeting older Windows versions.
		constexpr auto renameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
#if NTDDI_VERSION >= NTDDI_WIN10_RS1
		static_assert(renameInfoEx == FileRenameInfoEx);
#endif
		return SetFileInformationByHandle(file, renameInfoEx, info, static_cast<DWORD>(size)) != FALSE;
	}

	bool WriteCacheFile(const std::filesystem::path& path, auto write)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return false;
		auto temporary = path;
		static const auto started = GraphCache::Store::Clock::now().time_since_epoch().count();
		temporary += std::format(".{}-{}-{}.tmp", GetCurrentProcessId(), started,
			writeSequence.fetch_add(1, std::memory_order_relaxed));
		RemoveTemporary cleanup{temporary};
		std::ofstream output(temporary, std::ios::binary | std::ios::noreplace);
		if (!output) return false;
		cleanup.ownsFile = true;
		if (!write(output)) return false;
		output.close();
		if (!output) return false;
		const auto saved = PublishCacheFile(temporary, path);
		cleanup.ownsFile = !saved;
		return saved;
	}

	bool WritePlanFile(const std::filesystem::path& path, const GraphPlan::Entry& entry)
	{
		return WriteCacheFile(path, [&](auto& output) {
			GraphPlan::WriteEntry(output, entry);
			return static_cast<bool>(output);
		});
	}

	bool SamePlan(const GraphPlan::Entry& a, const GraphPlan::Entry& b)
	{
		return a.topology == b.topology && a.nodes == b.nodes && a.stateMachines == b.stateMachines;
	}

	struct GraphFile
	{
		std::shared_ptr<const GraphPlan::Entry> entry;
		std::optional<InstanceTopology::Trace> links;
		GraphPlan::Shape shape{};
	};

	GraphFile ReadGraphFile(const std::filesystem::path& path, GraphCache::Key key, bool withLinks)
	{
		CacheReader file(path);
		if (file.status != GraphCache::Result::hit || file.length < minGraphFileBytes || file.length > maxGraphFileBytes) return {};
		std::uint64_t header[3]{};
		std::uint32_t planLength{};
		if (!file.Read(header, sizeof(header)) || header[0] != graphStorageVersion || header[1] != key.graph ||
			header[2] != key.shape || !file.Read(&planLength, sizeof(planLength))) return {};
		const auto prefix = graphHeaderBytes + planLength;
		// A complete embedded plan can still be used when its optional topology was lost.
		if (file.length < prefix || file.length - prefix > InstanceTopology::Trace::maxPortableBytes) return {};
		auto plan = ReadPlan(file, planLength);
		if (!plan) return {};
		const auto shape = CachedShape(*plan);
		if (shape.CacheVariant() != key.shape) return {};
		auto links = withLinks && plan->nodes * sizeof(std::uint16_t) <= preparedPlanLimit ?
			ReadTopology(file, file.length - prefix, *plan) : std::nullopt;
		return {std::make_shared<const GraphPlan::Entry>(std::move(*plan)), std::move(links), shape};
	}

	bool WriteGraphFile(const std::filesystem::path& path, GraphCache::Key key,
		const GraphPlan::Entry& plan, const InstanceTopology::Trace& links)
	{
		return WriteCacheFile(path, [&](auto& output) {
			const std::uint64_t header[]{graphStorageVersion, key.graph, key.shape};
			const auto planLength = static_cast<std::uint32_t>(GraphPlan::headerSize + plan.stateMachines.size() * sizeof(std::uint32_t));
			output.write(reinterpret_cast<const char*>(header), sizeof(header));
			output.write(reinterpret_cast<const char*>(&planLength), sizeof(planLength));
			GraphPlan::WriteEntry(output, plan);
			return links.WritePortable(output, plan);
		});
	}
}

namespace GraphCache
{
	Store::Store(std::filesystem::path a_directory) : directory(std::move(a_directory)) {}

	std::filesystem::path Store::PathFor(Key key, bool legacy) const
	{
		return directory / (legacy ? std::format("{:016x}.bin", key.graph) :
			std::format("{:016x}-{:016x}.bin", key.graph, key.shape));
	}

	Lookup Store::FindLoaded(Key key, Clock::time_point now)
	{
		const auto found = loaded.find(key);
		if (found == loaded.end()) return {};
		auto& record = found->second;
		record.lastUse = ++useSequence;
		const auto origin = record.preloaded ? Origin::preloaded : Origin::memory;
		record.preloaded = false;
		const bool persisted = record.topologySaved || (!record.writing && !record.retryAfter);
		return {Result::hit, record.entry, origin,
			!persisted && !record.writing && record.retryAfter && now >= *record.retryAfter,
			persisted, record.prepared};
	}

	bool Store::MakeRoom()
	{
		if (loaded.size() < memoryLimit) return true;
		auto oldest = loaded.end();
		for (auto it = loaded.begin(); it != loaded.end(); ++it) {
			if (!it->second.writing && !it->second.topologyTicket &&
				(oldest == loaded.end() || it->second.lastUse < oldest->second.lastUse)) oldest = it;
		}
		if (oldest == loaded.end()) return false;
		loaded.erase(oldest);
		return true;
	}

	Lookup Store::Read(Key key, Clock::time_point now) noexcept
	{
		try {
			if (directory.empty()) return {Result::unavailable, {}};
			{
				std::scoped_lock lock(memoryMutex);
				if (auto hit = FindLoaded(key, now); hit.entry) return hit;
			}
			// Keep two on-demand reads from opening the same cold record.
			std::scoped_lock diskLock(fileMutexes[key.graph % fileMutexes.size()]);
			{
				std::scoped_lock lock(memoryMutex);
				if (auto hit = FindLoaded(key, now); hit.entry) return hit;
			}
			auto path = PathFor(key);
			auto result = ReadPlanFile(path);
			if (result.entry && CachedShape(*result.entry).CacheVariant() != key.shape) {
				result = {Result::invalid, {}};
			}
			if (!result.entry) {
				path.replace_extension(".graph");
				if (auto graph = ReadGraphFile(path, key, false); graph.entry) {
					result = {Result::hit, std::move(graph.entry), Origin::disk, false, true};
				}
			}
			if (result.result == Result::missing || result.result == Result::invalid) {
				// The engine must validate this against the live graph before migrating it.
				auto legacy = ReadPlanFile(PathFor(key, true));
				if (legacy.entry && CachedShape(*legacy.entry).CacheVariant() == key.shape) {
					legacy.origin = Origin::legacy;
					legacy.persisted = false;
					result = std::move(legacy);
				}
			}
			std::scoped_lock lock(memoryMutex);
			if (auto hit = FindLoaded(key, now); hit.entry) return hit;
			if (result.origin == Origin::disk && MakeRoom()) {
				loaded.emplace(key, Record{result.entry, {}, ++useSequence});
			}
			return result;
		} catch (const std::exception&) {
			return {Result::unavailable, {}};
		}
	}

	PreloadStats Store::Preload(unsigned maxPlans, Clock::time_point deadline, const std::atomic_bool& stop,
		PreloadProgress progress) noexcept
	{
		PreloadStats stats;
		const auto stopped = [&] { return stop.load(std::memory_order_relaxed) ||
			(deadline != (Clock::time_point::max)() && Clock::now() >= deadline); };
		try {
			if (directory.empty() || !maxPlans) return stats;
			if (stopped()) return {.interrupted = true};
			maxPlans = (std::min)(maxPlans, 256u);
			const auto attemptLimit = maxPlans + extraPlanChecks;
			std::unordered_map<Key, PreloadCandidate, KeyHash> uniqueCandidates;
			{
				WIN32_FIND_DATAW data{};
				const HANDLE find = FindFirstFileW((directory / L"*").c_str(), &data);
				if (find == INVALID_HANDLE_VALUE) return stats;
				const std::unique_ptr<void, decltype(&FindClose)> closeFind(find, FindClose);
				unsigned scanned{};
				do {
					if (stopped()) return {.interrupted = true};
					const auto size = (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
					if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
					const std::wstring_view name(data.cFileName);
					const bool bundle = name.ends_with(L".graph");
					const auto minSize = bundle ? minGraphFileBytes : GraphPlan::headerSize;
					const auto maxSize = bundle ? maxGraphFileBytes : GraphPlan::maxFileSize;
					if (size < minSize || size > maxSize) continue;
					if (const auto key = ParseVariantName(name)) {
						const PreloadCandidate candidate{*key, (std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) |
							data.ftLastWriteTime.dwLowDateTime, static_cast<std::size_t>((std::min)(size,
								static_cast<std::uint64_t>(GraphPlan::maxFileSize))), bundle};
						const auto [it, inserted] = uniqueCandidates.try_emplace(*key, candidate);
						if (!inserted) {
							it->second.modified = (std::max)(it->second.modified, candidate.modified);
							it->second.bytes = (std::min)(it->second.bytes, candidate.bytes);
							it->second.hasBundle |= bundle;
						}
					}
				} while (++scanned < scanLimit && FindNextFileW(find, &data));
			}
			std::vector<PreloadCandidate> candidates;
			candidates.reserve(uniqueCandidates.size());
			for (const auto& item : uniqueCandidates) candidates.push_back(item.second);
			std::ranges::sort(candidates, [](const PreloadCandidate& first, const PreloadCandidate& second) {
				if (first.modified != second.modified) return first.modified > second.modified;
				if (first.bytes != second.bytes) return first.bytes < second.bytes;
				if (first.key.graph != second.key.graph) return first.key.graph < second.key.graph;
				return first.key.shape < second.key.shape;
			});
			std::vector<PreloadCandidate> selected;
			std::vector<PreloadCandidate> olderVariants;
			selected.reserve((std::min)(candidates.size(), static_cast<std::size_t>(attemptLimit)));
			std::unordered_set<std::uint64_t> graphKeys;
			// A combined file's full length says little about its plan
			// length. Enforce the memory budget using the decoded plan size.
			const auto select = [&](const PreloadCandidate& candidate) {
				if (selected.size() < attemptLimit) selected.push_back(candidate);
			};
			for (const auto& candidate : candidates) {
				if (selected.size() >= attemptLimit) break;
				if (graphKeys.insert(candidate.key.graph).second) select(candidate);
				else if (olderVariants.size() < attemptLimit) olderVariants.push_back(candidate);
			}
			for (const auto& candidate : olderVariants) select(candidate);
			const auto available = static_cast<unsigned>(selected.size());
			stats.selected = (std::min)(maxPlans, available);
			{
				std::scoped_lock lock(memoryMutex);
				// Keep room for normal lookups in addition to the optional startup set.
				memoryLimit = (std::max)(memoryLimit, std::size_t{64} + stats.selected);
			}
			if (stats.selected && !stopped() && progress) progress(0, stats.selected);
			for (const auto& candidate : selected) {
				if (stats.checked == stats.selected || stopped()) break;
				const bool prepareIds = stats.preparedBytes < preparedByteLimit;
				// No file mutex across optional I/O. Gameplay can load or save
				// this key independently, and its memory record takes precedence.
				const auto path = PathFor(candidate.key);
				auto graphPath = path;
				graphPath.replace_extension(".graph");
				GraphFile graph;
				if (candidate.hasBundle) graph = ReadGraphFile(graphPath, candidate.key,
					prepareIds && stats.topologyBytes < topologyByteLimit);
				auto entry = std::move(graph.entry);
				auto shape = graph.shape;
				if (!entry && !stopped()) {
					entry = ReadPlanFile(path).entry;
					if (entry) {
						shape = CachedShape(*entry);
						if (shape.CacheVariant() != candidate.key.shape) entry.reset();
					}
				}
				if (stopped()) break;
				if (entry) {
					const auto bytes = GraphPlan::headerSize + entry->stateMachines.size() * sizeof(std::uint32_t);
					const auto idBytes = static_cast<std::size_t>(shape.nodes) * sizeof(std::uint16_t);
					std::shared_ptr<const PreparedPlan> prepared;
					bool topologyOnDisk = graph.links.has_value();
					if (prepareIds && idBytes <= preparedPlanLimit && idBytes <= preparedByteLimit - stats.preparedBytes) {
						std::shared_ptr<const GraphSlimming::Payload> payload;
						if (stats.topologyBytes < topologyByteLimit) {
							auto links = std::move(graph.links);
							if (!links) {
								auto legacy = path;
								legacy.replace_extension(".topo");
								links = ReadTopologyFile(legacy, *entry);
								if (links && !stopped()) {
									try {
										std::unique_lock lock(topologyFileMutexes[KeyHash{}(candidate.key) % topologyFileMutexes.size()],
											std::try_to_lock);
										if (lock && !stopped()) {
											const auto existing = ReadGraphFile(graphPath, candidate.key, true);
											topologyOnDisk = (existing.links && SamePlan(*existing.entry, *entry) &&
												existing.links->SameLinks(*links)) || WriteGraphFile(graphPath, candidate.key, *entry, *links);
										}
									} catch (const std::exception&) {
										// The validated legacy topology is usable even if migration cannot write.
									}
								}
							}
							if (links && links->CapacityBytes() <= topologyByteLimit - stats.topologyBytes) {
								payload = std::make_shared<const GraphSlimming::Payload>(
									GraphSlimming::Payload{std::move(*links), entry});
							}
						}
						prepared = std::make_shared<const PreparedPlan>(PreparedPlan{entry, shape,
							std::make_shared<const std::vector<std::uint16_t>>(GraphPlan::PrepareIds(*entry)), std::move(payload)});
					}
					if (stopped()) break;
					std::scoped_lock lock(memoryMutex);
					auto record = loaded.find(candidate.key);
					if (record == loaded.end() && bytes <= preloadByteLimit - stats.bytes && loaded.size() < memoryLimit) {
						record = loaded.try_emplace(candidate.key, Record{entry, {}, 0, false, true}).first;
						++stats.warmed;
						stats.bytes += bytes;
					}
					// An on-demand lookup may have published this plan before
					// preloading reached it. Add only matching, committed data.
					if (prepared && record != loaded.end() && !record->second.prepared && !record->second.writing &&
						!record->second.retryAfter && SamePlan(*entry, *record->second.entry)) {
						auto& variants = preparedGraphs[candidate.key.graph].shapes;
						if (std::ranges::find(variants, candidate.key.shape) == variants.end()) variants.push_back(candidate.key.shape);
						record->second.prepared = prepared;
						record->second.topologySaved |= topologyOnDisk;
						++stats.prepared;
						stats.preparedBytes += idBytes;
						if (prepared->payload) {
							++stats.baked;
							stats.topologyBytes += prepared->payload->links.CapacityBytes();
						}
					}
				} else {
					++stats.unusable;
					stats.selected = (std::min)(available, maxPlans + stats.unusable);
				}
				++stats.checked;
				if (progress) progress(stats.checked, stats.selected);
			}
			stats.interrupted = stats.checked != stats.selected;
		} catch (const std::exception&) {
			stats.interrupted = true;
		}
		return stats;
	}

	bool Store::AdmitOffline(Key key, GraphPlan::Shape shape, GraphSlimming::Payload&& payload)
	{
		const auto entry = payload.plan;
		if (!entry || !entry->WellFormed() || !payload.links.Complete(entry->nodes) ||
			CachedShape(*entry).CacheVariant() != key.shape || shape.CacheVariant() != key.shape ||
			entry->nodes * sizeof(std::uint16_t) > preparedPlanLimit) return false;
		const auto canAdmit = [&](std::size_t bytes) {
			if (offlinePlanCount >= offlinePlanLimit || bytes > offlineByteLimit - offlineBytes) return false;
			if (const auto live = loaded.find(key); live != loaded.end() && live->second.prepared && live->second.prepared->payload) {
				return false;
			}
			const auto found = offlineGraphs.find(key.graph);
			return found == offlineGraphs.end() || std::ranges::none_of(found->second.plans, [&](const auto& other) {
				return other->shape.CacheVariant() == key.shape;
			});
		};
		const auto fixedBytes = sizeof(PreparedPlan) + sizeof(std::vector<std::uint16_t>) + payload.CapacityBytes();
		{
			std::scoped_lock lock(memoryMutex);
			if (!canAdmit(fixedBytes + entry->nodes * sizeof(std::uint16_t))) return false;
		}
		auto ids = std::make_shared<const std::vector<std::uint16_t>>(GraphPlan::PrepareIds(*entry));
		const auto bytes = fixedBytes + ids->capacity() * sizeof(std::uint16_t);
		auto plan = std::make_shared<const PreparedPlan>(PreparedPlan{entry, shape, std::move(ids),
			std::make_shared<const GraphSlimming::Payload>(std::move(payload)), true});
		std::scoped_lock lock(memoryMutex);
		if (!canAdmit(bytes)) return false;
		offlineGraphs[key.graph].plans.push_back(std::move(plan));
		++offlinePlanCount;
		offlineBytes += bytes;
		return true;
	}

	std::shared_ptr<const PreparedPlan> Store::FindOffline(Key key)
	{
		std::scoped_lock lock(memoryMutex);
		const auto found = offlineGraphs.find(key.graph);
		if (found == offlineGraphs.end()) return {};
		const auto& variants = found->second.plans;
		const auto match = std::ranges::find_if(variants, [&](const auto& entry) {
			return entry->shape.CacheVariant() == key.shape;
		});
		return match != variants.end() ? *match : nullptr;
	}

	std::shared_ptr<const PreparedPlan> Store::FindPrepared(std::uint64_t graph)
	{
		std::scoped_lock lock(memoryMutex);
		if (const auto found = preparedGraphs.find(graph); found != preparedGraphs.end()) {
			if (found->second.requiresExactLookup) return {};
			for (const auto shape : found->second.shapes) {
				const auto record = loaded.find({graph, shape});
				if (record != loaded.end() && record->second.prepared) {
					record->second.lastUse = ++useSequence;
					return record->second.prepared;
				}
			}
			preparedGraphs.erase(found);
		}
		const auto offline = offlineGraphs.find(graph);
		if (offline == offlineGraphs.end() || offline->second.exactOnly || offline->second.plans.size() != 1) return {};
		return offline->second.plans.front();
	}

	void Store::ResolvePreparedMismatch(std::uint64_t graph, const PreparedPlan& expected, PreparedMismatch mismatch)
	{
		std::shared_ptr<const PreparedPlan> idsOnly;
		if (mismatch == PreparedMismatch::topology && expected.payload && !expected.offline) {
			try {
				idsOnly = std::make_shared<const PreparedPlan>(PreparedPlan{expected.entry, expected.shape, expected.ids, {}});
			} catch (const std::bad_alloc&) {
				// Retire the invalid topology even when a new preparation cannot be allocated.
			}
		}
		std::scoped_lock lock(memoryMutex);
		if (expected.offline) {
			const auto item = offlineGraphs.find(graph);
			if (item != offlineGraphs.end() && std::ranges::any_of(item->second.plans, [&](const auto& plan) {
				return plan.get() == &expected;
			})) item->second.exactOnly = true;
			return;
		}
		const auto found = preparedGraphs.find(graph);
		if (found == preparedGraphs.end()) return;
		const auto shape = expected.shape.CacheVariant();
		const auto record = loaded.find({graph, shape});
		if (record == loaded.end() || record->second.prepared.get() != &expected) return;
		if (mismatch == PreparedMismatch::differentShape) {
			found->second.requiresExactLookup = true;
			return;
		}
		record->second.topologySaved = false;
		record->second.prepared = std::move(idsOnly);
		if (record->second.prepared) return;
		std::erase(found->second.shapes, shape);
		if (found->second.shapes.empty()) preparedGraphs.erase(found);
	}

	Store::TopologyClaim Store::ClaimTopology(Key key, const GraphPlan::Entry& plan, Clock::time_point now, bool allowNewJob)
	{
		std::scoped_lock lock(memoryMutex);
		auto item = loaded.find(key);
		if (item == loaded.end()) {
			if (!allowNewJob || directory.empty()) return {};
			const auto offline = offlineGraphs.find(key.graph);
			if (offline == offlineGraphs.end()) return {};
			const auto& variants = offline->second.plans;
			const auto match = std::ranges::find_if(variants, [&](const auto& candidate) {
				return candidate->entry.get() == &plan && candidate->shape.CacheVariant() == key.shape;
			});
			// Only a live-confirmed payload submitted by the commit path gets a ticket.
			if (match == variants.end() || !MakeRoom()) return {};
			item = loaded.emplace(key, Record{.entry = (*match)->entry, .retryAfter = now + retryDelay}).first;
		}
		auto& record = item->second;
		if (!record.topologySaved && (!allowNewJob || record.topologyTicket ||
			(record.topologyRetryAfter && now < *record.topologyRetryAfter))) return {};
		if (record.entry.get() != &plan && !SamePlan(*record.entry, plan)) return {};
		record.lastUse = ++useSequence;
		if (record.topologySaved) return {.saved = true};
		record.topologyTicket = ++topologySequence;
		return {record.topologyTicket, false};
	}

	void Store::FinishTopology(Key key, std::uint64_t ticket, bool saved, std::optional<Clock::time_point> retry) noexcept
	{
		if (!ticket) return;
		std::scoped_lock lock(memoryMutex);
		const auto record = loaded.find(key);
		if (record == loaded.end() || record->second.topologyTicket != ticket) return;
		record->second.topologyTicket = 0;
		record->second.topologySaved |= saved;
		record->second.topologyRetryAfter = retry;
	}

	void Store::CancelTopology(Key key, std::uint64_t ticket) noexcept
	{
		FinishTopology(key, ticket, false, {});
	}

	bool Store::SaveTopology(Key key, std::uint64_t ticket, const GraphSlimming::Payload& payload) noexcept
	{
		if (!ticket) return false;
		bool saved{};
		try {
			if (payload.plan && payload.plan->WellFormed() && payload.links.Complete(payload.plan->nodes) &&
				payload.links.PortableBytes() <= InstanceTopology::Trace::maxPortableBytes &&
				CachedShape(*payload.plan).CacheVariant() == key.shape) {
				std::scoped_lock lock(topologyFileMutexes[KeyHash{}(key) % topologyFileMutexes.size()]);
				bool matchesClaim{};
				{
					std::scoped_lock cacheLock(memoryMutex);
					const auto record = loaded.find(key);
					if (record == loaded.end() || record->second.topologyTicket != ticket) return false;
					matchesClaim = record->second.entry.get() == payload.plan.get() ||
						SamePlan(*record->second.entry, *payload.plan);
				}
				if (!matchesClaim) {
					FinishTopology(key, ticket, false, Clock::now() + retryDelay);
					return false;
				}
				auto path = PathFor(key);
				path.replace_extension(".graph");
				const auto existing = ReadGraphFile(path, key, true);
				saved = (existing.links && SamePlan(*existing.entry, *payload.plan) &&
					existing.links->SameLinks(payload.links)) || WriteGraphFile(path, key, *payload.plan, payload.links);
				// Complete the request before another topology writer can take its place.
				FinishTopology(key, ticket, saved, saved ? std::nullopt : std::optional{Clock::now() + retryDelay});
				return saved;
			}
		} catch (const std::exception&) {
		}
		FinishTopology(key, ticket, false, Clock::now() + retryDelay);
		return false;
	}

	WriteResult Store::Write(Key key, const GraphPlan::Entry& entry, Clock::time_point now) noexcept
	{
		try {
			if (directory.empty() || !entry.WellFormed()) return WriteResult::skipped;
			{
				std::scoped_lock lock(memoryMutex);
				const auto found = loaded.find(key);
				if (found != loaded.end()) {
					auto& current = found->second;
					current.lastUse = ++useSequence;
					if (current.writing) return WriteResult::pending;
					if (SamePlan(*current.entry, entry)) {
						if (current.topologySaved || !current.retryAfter) return WriteResult::current;
						if (now < *current.retryAfter) return WriteResult::pending;
						// Retrying the same plan must preserve the topology request.
						current.writing = true;
					} else {
						if (current.topologyTicket) return WriteResult::pending;
						auto plan = std::make_shared<const GraphPlan::Entry>(entry);
						current = Record{std::move(plan), {}, current.lastUse, true};
					}
				} else {
					auto plan = std::make_shared<const GraphPlan::Entry>(entry);
					if (!MakeRoom()) return WriteResult::skipped;
					loaded.emplace(key, Record{std::move(plan), {}, ++useSequence, true});
				}
			}
			bool saved{};
			try {
				std::scoped_lock diskLock(fileMutexes[key.graph % fileMutexes.size()]);
				saved = WritePlanFile(PathFor(key), entry);
			} catch (const std::exception&) {
				// Finish the pending write state below.
			}
			std::scoped_lock lock(memoryMutex);
			auto& record = loaded.at(key);  // Active writers cannot be evicted or replaced.
			record.writing = false;
			record.retryAfter = saved ? std::nullopt : std::optional{now + retryDelay};
			return saved ? WriteResult::saved : WriteResult::failed;
		} catch (const std::exception&) {
			return WriteResult::failed;
		}
	}
}
