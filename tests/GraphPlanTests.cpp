#include "GraphCacheStore.h"
#include "GraphPerformance.h"
#include "GraphSlimming.h"
#include "AnimationPreload.h"
#include "HkxDirectoryIndex.h"
#include "InstanceTopology.h"
#include "TransitionMap.h"

#include <Windows.h>
#include <algorithm>
#include <barrier>
#include <format>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <thread>

int main()
{
	std::size_t checks{};
	const auto require = [&checks](bool ok, const char* message) {
		++checks;
		if (!ok) { std::cerr << message << '\n'; std::exit(1); }
	};
	using namespace GraphPlan;
	{
		using namespace std::chrono_literals;
		using GraphPerformance::Outcome;
		GraphPerformance::Counters perf;
		perf.Add(Outcome::belowLimit, {10us, 2us});
		perf.Add(Outcome::idsInUse, {30us, 1us});
		perf.Add(Outcome::diskTopology, {4us, 7us});
		require(perf.calls == 3 && perf.Count(Outcome::belowLimit) == 1 &&
			perf.Count(Outcome::idsInUse) == 1 && perf.Count(Outcome::diskTopology) == 1 &&
			perf.Count(Outcome::full) == 0, "Performance counters omitted a short, skipped or cached graph");
		require(perf.nativeTotal == 44us && perf.nativeMax == 30us &&
			perf.pluginTotal == 10us && perf.pluginMax == 7us, "Native and plugin durations were mixed");
		require(perf.nativeLatency.P95UpperMicros(perf.calls, perf.nativeMax) == 30 &&
			perf.pluginLatency.P95UpperMicros(perf.calls, perf.pluginMax) == 7,
			"The P95 upper estimate exceeded the measured maximum");
		GraphPerformance::Counters mixedLatency;
		for (unsigned i = 0; i < 19; ++i) mixedLatency.Add(Outcome::offlineVerified, {126us, 1us});
		mixedLatency.Add(Outcome::offlineTopology, {30s, 999us});
		require(mixedLatency.nativeLatency.P95UpperMicros(mixedLatency.calls, mixedLatency.nativeMax) == 250 &&
			mixedLatency.pluginLatency.P95UpperMicros(mixedLatency.calls, mixedLatency.pluginMax) == 125 &&
			mixedLatency.Count(Outcome::offlineTopology) == 1, "Latency percentiles misclassified the tail");
		GraphPerformance::Counters longLatency;
		longLatency.Add(Outcome::analysisBusy, {30s, 0us});
		require(longLatency.nativeLatency.P95UpperMicros(1, longLatency.nativeMax) == 30'000'000 &&
			longLatency.pluginLatency.P95UpperMicros(1, longLatency.pluginMax) == 0,
			"The histogram overflow bucket understated a long stall");
		GraphPerformance::Counters initialization;
		initialization.Add(Outcome::full, {20ms, 3ms, 9ms, 2, true});
		initialization.Add(Outcome::instance, {65ms, 2ms});
		require(initialization.nativeTotal == 85ms && initialization.nativeExclusive.total == 76ms &&
			initialization.nativeExclusive.Mean() == 38ms && initialization.nativeExclusive.maximum == 65ms &&
			initialization.nestedCalls == 2 && initialization.nestedTime == 9ms && initialization.externalTargets == 1 &&
			initialization.slow16ms == 2 && initialization.slow50ms == 1,
			"Nested or external Havok calls distorted timing or slow-call counts");
		GraphPerformance::HookTiming outer(true);
		outer.BeginNative();
		{
			GraphPerformance::HookTiming inner(true);
			inner.BeginNative();
			{
				GraphPerformance::HookTiming deepest(true);
				deepest.BeginNative();
				deepest.EndNative();
			}
			inner.EndNative();
			require(inner.Finish(false).nestedCalls == 1, "Innermost Havok hook was not attributed to its direct parent");
		}
		outer.EndNative();
		{
			GraphPerformance::HookTiming outside(true);
			outside.BeginNative();
			outside.EndNative();
		}
		const auto measured = outer.Finish(false);
		require(measured.nestedCalls == 1 && measured.native >= measured.nestedTime &&
			measured.NativeExclusive() + measured.nestedTime == measured.native,
			"Nested work was charged twice or attributed outside the native call");
	}
	Signature shape;
	for (const auto ordinal : { 2u, 1u, 2u, 0u, 1u, 0u }) shape.Add(ordinal);
	Signature changed;
	for (const auto ordinal : { 2u, 2u, 1u, 0u, 1u, 0u }) changed.Add(ordinal);
	const Entry original{ shape.value, 80'000, 300, { 1, 30'000, 79'999 } };
	auto dense = PrepareIds(original);
	require(ValidAssignments(original.nodes, original.stateMachines, dense) &&
		ValidAssignments(maxNodes, original.stateMachines) && !ValidAssignments(maxNodes + 1ull, original.stateMachines),
		"ID validation rejected valid limits or accepted an oversized graph");
	dense[0] = 0;
	const std::array<std::uint32_t, 2> duplicate{1, 1};
	require(!ValidAssignments(original.nodes, original.stateMachines, dense) &&
		!ValidAssignments(original.nodes, duplicate) &&
		!ValidAssignments(original.nodes, original.stateMachines, std::span{dense}.first(32)),
		"Corrupt or truncated ID assignments reached the map writer");
	const auto validDense = PrepareIds(original);
	PreparedCheck prefix(validDense);
	prefix.Observe(0, [] { return false; });
	prefix.Observe(1, [] { return true; });
	prefix.Observe(3, [] { return false; });
	require(prefix.VerifiedPrefix() == 2 && !prefix.Matches(original.nodes, original.topology, original.topology),
		"A gap in live classification authorized the prepared plan");
	StateMachines live(original.nodes);
	for (const auto index : original.stateMachines) live.Add(index);
	require(live.Matches(original, shape.value), "Unchanged large graph failed validation");
	require(!live.Matches(original, changed.value), "Changed ordered topology reused old analysis");
	require(live.Describe(shape.value).CacheVariant() != live.Describe(changed.value).CacheVariant(),
		"Different topologies have the same cache identity");
	StateMachines resized(original.nodes + 64);
	for (const auto index : original.stateMachines) resized.Add(index);
	require(!resized.Matches(original, shape.value), "Different node count reused old analysis");
	StateMachines replaced(original.nodes);
	for (const auto index : { 1u, 64u, 79'999u }) replaced.Add(index);
	require(!replaced.Matches(original, shape.value), "Same count with changed state-machine positions reused the cache");
	require(replaced.Describe(shape.value).CacheVariant() != live.Describe(shape.value).CacheVariant(),
		"Same topology with changed class positions would overwrite a cache variant");
	live.Add(1);
	live.Add(original.nodes);
	const auto rebuilt = live.Build(shape.value);
	require(rebuilt.stateMachines == original.stateMachines && live.Count() == 3,
		"Enumerating the mask lost nodes or accepted duplicates or out-of-bounds indices");
	StateMachines edgeCase(original.nodes);
	for (const auto index : { 0u, 63u, 64u, 65u, 79'999u }) edgeCase.Add(index);
	require(edgeCase.Build(shape.value).stateMachines == std::vector<std::uint32_t>{ 0, 63, 64, 65, 79'999 },
		"State-machine mask lost positions across word boundaries");
	auto wrongCountMatch = original;
	wrongCountMatch.stateMachines = { 1, 1, 79'999 };
	require(!live.Matches(wrongCountMatch, shape.value), "Repeated live index hid a missing state machine");
	wrongCountMatch.stateMachines = { 30'000, 1, 79'999 };
	require(!live.Matches(wrongCountMatch, shape.value), "An unsorted cache entry validated");
	wrongCountMatch.stateMachines = { 1, 30'000, original.nodes };
	require(!live.Matches(wrongCountMatch, shape.value), "An out-of-range live index validated");
	std::ostringstream output;
	WriteEntry(output, original);
	const auto bytes = output.str();
	std::istringstream restarted(bytes);
	const auto loaded = ReadEntry(restarted);
	require(loaded && loaded->stateMachines == original.stateMachines &&
		loaded->analysisMicros == original.analysisMicros && live.Matches(*loaded, shape.value),
		"Persisted entry failed to round-trip");
	for (std::size_t bytesRead = 0; bytesRead < bytes.size(); ++bytesRead) {
		std::istringstream partial(bytes.substr(0, bytesRead));
		require(!ReadEntry(partial), "A partially written file validated");
	}
	std::istringstream trailing(bytes + "x");
	require(!ReadEntry(trailing), "Trailing bytes were accepted");
	auto corrupted = bytes;
	corrupted[0] ^= 1;
	std::istringstream oldVersion(corrupted);
	require(!ReadEntry(oldVersion), "Unsupported cache format was accepted");
	for (const auto invalid : { std::vector<std::uint32_t>{ 3, 3 }, { 2, 1 }, { 80'000 } }) {
		auto wrong = original;
		wrong.stateMachines = invalid;
		std::ostringstream bad;
		WriteEntry(bad, wrong);
		std::istringstream input(bad.str());
		require(!ReadEntry(input), "Corrupt state-machine ordinals were accepted");
	}
	require(ValidChildArray(128, 128, true) && ValidChildArray(0, 128, true), "Valid native arrays were rejected");
	require(!ValidChildArray(-1, 128, true) && !ValidChildArray(129, 128, true) &&
		!ValidChildArray(1, 0, true) && !ValidChildArray(1, 128, false), "Invalid array bounds reached traversal");
	require(InBorrowedBuffer(0x1000, 0x1000, 0x200) && InBorrowedBuffer(0x11FF, 0x1000, 0x200) &&
		!InBorrowedBuffer(0x1200, 0x1000, 0x200) && !InBorrowedBuffer(0x0FFF, 0x1000, 0x200),
		"Interior Havok scratch pointer was classified as a separate allocation");
	require(BuffersOverlap(0x0ff0, 17, 0x1000, 0x200) && BuffersOverlap(0x11ff, 8, 0x1000, 0x200) &&
		!BuffersOverlap(0x0ff0, 16, 0x1000, 0x200) && !BuffersOverlap(0x1200, 8, 0x1000, 0x200) &&
		!BuffersOverlap(0x1000, 0, 0x1000, 0x200), "Scratch buffer overlap did not respect both bounds");
	require(ReachableMapSlot(0, 0, 7, 7), "Havok key at home was unreachable");
	require(ReachableMapSlot(1, 2, 7, 7), "Havok key after collisions was unreachable");
	require(!ReachableMapSlot(1, 2, 1, 7), "Havok lookup crossed an empty bucket");
	require(!ReachableMapSlot(1, 2, 0, 7), "Havok lookup started in an empty bucket");
	require(ReachableMapSlot(7 * 16, 1, 6, 7), "Havok probing failed across the end of the table");
	require(!ReachableMapSlot(7 * 16, 1, 7, 7), "Wrapped Havok probing started in an empty bucket");

	struct MapAllocator
	{
		bool fail{};
		int allocations{};
		int frees{};
		std::int32_t lastFreedBytes{};
		void* BlockAlloc(std::int32_t size)
		{
			if (fail) return nullptr;
			auto* block = ::operator new(static_cast<std::size_t>(size), std::nothrow);
			if (block) ++allocations;
			return block;
		}
		void BlockFree(void* block, std::int32_t size)
		{
			++frees;
			lastFreedBytes = size;
			::operator delete(block);
		}
	} allocator;
	const auto valueAt = [](std::size_t id) -> std::uint64_t { return id + 100; };
	const auto insert = [](TransitionMap::Table& table, std::size_t id, std::uint64_t value) {
		const auto count = static_cast<std::uint32_t>(table.numElemsAndFlags) & ~TransitionMap::borrowed;
		if (count * 2 > static_cast<std::uint32_t>(table.hashMod)) return false;
		const auto mask = static_cast<std::size_t>(table.hashMod);
		auto slot = (static_cast<std::uint32_t>(id >> 4) * 0x9e3779b1u) & mask;
		while (table.elem[slot].key != TransitionMap::emptyKey) slot = (slot + 1) & mask;
		table.elem[slot] = {id, value};
		++table.numElemsAndFlags;
		return true;
	};
	constexpr auto* seProfile = TransitionMap::FindRuntimeProfile({1, 5, 97, 0});
	constexpr auto* aeProfile = TransitionMap::FindRuntimeProfile({1, 6, 1170, 0});
	constexpr auto* primaryProfile = TransitionMap::FindRuntimeProfile({1, 7, 104, 0});
	static_assert(seProfile->insertId == 56771 && aeProfile->insertId == 57198 && primaryProfile->insertId == 57198);
	static_assert(aeProfile != primaryProfile &&
		!TransitionMap::FindRuntimeProfile({0, 0, 0, 0}) &&
		!TransitionMap::FindRuntimeProfile({1, 7, 104, 1}));
	std::array<std::uint8_t, primaryProfile->insertBytes> unqualifiedInsert{};
	require(!TransitionMap::MatchesVerifiedInsert(*primaryProfile, unqualifiedInsert) &&
		!TransitionMap::MatchesVerifiedInsert(*primaryProfile, std::span{unqualifiedInsert}.first(42)),
		"The map writer accepted an unqualified or truncated function body");
	constexpr TransitionMap::RuntimeProfile shortGuard{{1, 7, 104, 0}, 57198, 1, 0xAF63BD4C8601B7DFULL};
	constexpr std::array<std::uint8_t, 1> zeroByte{};
	require(TransitionMap::MatchesVerifiedInsert(shortGuard, zeroByte) &&
		!TransitionMap::MatchesVerifiedInsert(*primaryProfile, zeroByte),
		"The insert guard ignored the selected profile's length or fingerprint");
	require(TransitionMap::RequiredCapacity(0) == 0 && TransitionMap::RequiredCapacity(1) == 8 &&
		TransitionMap::RequiredCapacity(9) == 32 && TransitionMap::RequiredCapacity(nodeThreshold) == 65'536,
		"Unexpected Havok bucket reservation");
	std::array<TransitionMap::Bucket, 8> borrowedBuckets{};
	borrowedBuckets[0] = {0, 45};
	TransitionMap::Table liveMap{borrowedBuckets.data(), static_cast<std::int32_t>(TransitionMap::borrowed | 1u), 7};
	allocator.fail = true;
	require(!TransitionMap::Replace(liveMap, 8, 17, allocator, insert, valueAt) &&
		liveMap.elem == borrowedBuckets.data() && borrowedBuckets[0].val == 45 && allocator.frees == 0,
		"Allocation failure changed the live map");
	allocator.fail = false;
	const auto failThird = [&](TransitionMap::Table& table, std::size_t id, std::uint64_t value) {
		return id != 2 && insert(table, id, value);
	};
	require(!TransitionMap::Replace(liveMap, 8, 17, allocator, failThird, valueAt) &&
		liveMap.elem == borrowedBuckets.data() && borrowedBuckets[0].val == 45 && allocator.frees == 1,
		"Failed staged insertion changed the live map or leaked the staged buckets");
	require(TransitionMap::Replace(liveMap, 8, 33, allocator, insert, valueAt) &&
		liveMap.elem != borrowedBuckets.data() && liveMap.hashMod == 127 && liveMap.numElemsAndFlags == 33 &&
		allocator.frees == 1 && borrowedBuckets[0].val == 45, "Staged rebuild freed or overwrote borrowed buckets");
	std::size_t populated{};
	for (int i = 0; i <= liveMap.hashMod; ++i) {
		const auto [key, value] = liveMap.elem[i];
		if (key != TransitionMap::emptyKey && key < 33 && value == valueAt(key)) ++populated;
	}
	require(populated == 33, "A committed transition map lost an ID mapping");
	auto* oldOwned = liveMap.elem;
	require(TransitionMap::Replace(liveMap, 128, 1, allocator, insert, valueAt) && liveMap.elem != oldOwned &&
		allocator.frees == 2 && allocator.lastFreedBytes == 128 * sizeof(TransitionMap::Bucket),
		"Owned native table was not released with its original capacity");
	require(TransitionMap::Replace(liveMap, 8, 0, allocator, insert, valueAt) && liveMap.elem == nullptr &&
		liveMap.hashMod == -1 && liveMap.numElemsAndFlags == 0 && allocator.allocations == allocator.frees,
		"An empty graph did not publish Havok's empty layout or release its storage");
	const auto wrongValue = [&](TransitionMap::Table& table, std::size_t id, std::uint64_t value) {
		return insert(table, id, value + 1);
	};
	require(!TransitionMap::Replace(liveMap, 0, 2, allocator, wrongValue, valueAt) &&
		liveMap.elem == nullptr && liveMap.hashMod == -1 && allocator.allocations == allocator.frees,
		"A complete-looking table with incorrect values was published");
	TransitionMap::Table badHeader{borrowedBuckets.data(), 1, 15};
	require(!TransitionMap::Matches(badHeader, 8, 1, true, valueAt) &&
		!TransitionMap::Replace(badHeader, 8, 2, allocator, insert, valueAt),
		"Mismatched old table bounds reached the allocator");
	const auto unexpectedResize = [&](TransitionMap::Table& staged, std::size_t, std::uint64_t) {
		staged.elem = borrowedBuckets.data();
		return true;
	};
	require(!TransitionMap::Replace(liveMap, 0, 2, allocator, unexpectedResize, valueAt) &&
		!liveMap.elem && allocator.allocations == allocator.frees,
		"An unexpected move of staged storage changed the live map or leaked plugin storage");
	require(TransitionMap::Replace(liveMap, 0, nodeThreshold, allocator, insert, valueAt) &&
		TransitionMap::Matches(liveMap, 65'536, nodeThreshold, true, valueAt) &&
		TransitionMap::Replace(liveMap, 65'536, 0, allocator, insert, valueAt) &&
		allocator.allocations == allocator.frees, "The largest legal state-machine map failed the transactional path");

	struct Node
	{
		std::vector<Node*> children;
		bool machine{};
	};
	Node root, left, right, shared, replacement;
	root.children = {&left, nullptr, &right};
	left.children = {&shared, &root};
	right.children = {&shared};
	left.machine = true;
	const std::array<Node*, 4> orderedNodes{&root, &left, &right, &shared};
	InstanceTopology::Trace instance(true);
	const auto record = [&](std::initializer_list<std::uint32_t> ordinals) {
		auto edges = instance.RecordChildren(ordinals.size());
		std::copy(ordinals.begin(), ordinals.end(), edges.begin());
	};
	record({1, noNode, 2});
	record({3, 0});
	record({3});
	record({});
	std::size_t visits{};
	const auto inspect = [&](Node* node, std::size_t ordinal, auto check) {
		++visits;
		return node->machine == (ordinal == 1) &&
			check(node->children.size(), [&](std::size_t i) { return node->children[i]; });
	};
	const auto matches = [&](Node* entry) {
		visits = 0;
		return instance.Matches(entry, std::span<Node* const>{orderedNodes}, inspect);
	};
	require(matches(&root) && visits == 4, "Live topology with a cycle and shared child did not validate");
	require(!matches(&replacement) && visits == 0, "Different root was inspected as a cached root");
	root.children[0] = &replacement;
	require(!matches(&root) && visits == 1, "Detached cached address was inspected after a replaced edge");
	root.children[0] = &left;
	root.children[1] = &right;
	require(!matches(&root) && visits == 1, "A formerly null edge was accepted");
	root.children[1] = nullptr;
	std::swap(root.children.front(), root.children.back());
	require(!matches(&root) && visits == 1, "Reordered live edges were accepted");
	std::swap(root.children.front(), root.children.back());
	right.children.push_back(&replacement);
	require(!matches(&root) && visits == 3, "An added edge was accepted despite the same root and child addresses");
	right.children.pop_back();
	left.machine = false;
	require(!matches(&root) && visits == 2, "Changed state-machine classification reused the plan");
	left.machine = true;
	require(matches(&root), "The unchanged graph no longer validated after earlier misses");
	InstanceTopology::Trace unconnected(true);
	for (std::size_t i = 0; i < orderedNodes.size(); ++i) (void)unconnected.RecordChildren(0);
	visits = 0;
	const auto withoutChildren = [&](Node*, std::size_t, auto check) {
		++visits;
		return check(0, [](std::size_t) -> Node* { return nullptr; });
	};
	require(!unconnected.Matches(&root, std::span<Node* const>{orderedNodes}, withoutChildren) && visits == 1,
		"Cached node was inspected before a live preceding edge established its identity");
	InstanceTopology::Trace oversizedTrace(true);
	require(oversizedTrace.RecordChildren(InstanceTopology::instanceBytes / sizeof(std::uint32_t) + 1).empty() &&
		!oversizedTrace.Complete(1),
		"An out-of-budget instance was retained");
	require(CanRewriteIDs(false, 0, 0, -12, -12), "Fresh native counters with pending overflow were rejected");
	require(!CanRewriteIDs(true, 0, 0, 2, 2), "An active graph could change its IDs");
	require(!CanRewriteIDs(false, 1, 0, 2, 2), "An inactive graph with a live node could change its IDs");
	require(!CanRewriteIDs(false, 0, 1, 2, 2), "An existing dynamic ID pool could be discarded");
	require(!CanRewriteIDs(false, 0, 0, 2, 3), "A graph with issued dynamic IDs could change them");

	// The topology exceeds both old limits, including a long tail of leaves.
	constexpr std::uint32_t largeNodes = 140'000;
	constexpr std::size_t largeEdges = 600'000;
	std::vector<int> largeStorage(largeNodes);
	std::vector<int*> largeOrder;
	largeOrder.reserve(largeNodes);
	for (auto& value : largeStorage) largeOrder.push_back(&value);
	InstanceTopology::Trace largeTrace(true);
	auto largeRootEdges = largeTrace.RecordChildren(largeEdges);
	require(largeRootEdges.size() == largeEdges, "The former fixed edge limit still disables capture");
	for (std::size_t i = 0; i < largeEdges; ++i) {
		largeRootEdges[i] = i < largeNodes - 1 ? static_cast<std::uint32_t>(i + 1) : 1u;
	}
	for (std::size_t i = 1; i < largeNodes; ++i) (void)largeTrace.RecordChildren(0);
	require(largeTrace.Complete(largeNodes), "The former fixed node limit still disables capture");
	require(largeTrace.Matches(largeOrder.front(), std::span<int* const>{largeOrder}, [&](int*, std::size_t ordinal, auto check) {
		return check(ordinal == 0 ? largeEdges : 0, [&](std::size_t edge) {
			return largeOrder[edge < largeNodes - 1 ? edge + 1 : 1];
		});
	}), "Large sparse trace did not match its live topology");
	require(largeTrace.CapacityBytes() < 3'000'000, "Leaf rows consumed unexpected topology memory");
	largeTrace.Reset();
	(void)largeTrace.RecordChildren(0);
	require(largeTrace.Complete(1) && !largeTrace.Complete(largeNodes), "Reused capture retained prior node counts");

	InstanceTopology::Cache<int, int, 3> instanceCache(100);
	const auto first = std::make_shared<int>(10);
	const auto second = std::make_shared<int>(20);
	const auto third = std::make_shared<int>(30);
	const auto fourth = std::make_shared<int>(40);
	require(instanceCache.Remember(1, first, 30) && instanceCache.Remember(2, second, 30) &&
		instanceCache.Remember(3, third, 30), "Multiple instances could not be recorded together");
	require(instanceCache.Find(3) == third && instanceCache.Find(1) == first,
		"Alternating across live instances missed their cached data");
	const auto held = instanceCache.Find(2);
	require(held == second && instanceCache.Find(3) == third && instanceCache.Find(1) == first,
		"Instance lookup lost a snapshot");
	require(!instanceCache.Remember(4, fourth, 101) && instanceCache.IsCurrent(1, first.get()),
		"An oversized record displaced existing graphs");
	require(instanceCache.Remember(4, fourth, 40) && !instanceCache.Find(2) && *held == 20 &&
		instanceCache.IsCurrent(1, first.get()) && instanceCache.IsCurrent(3, third.get()),
		"Resident cache did not evict the least recently used instance");
	{
		std::jthread writer([&] { instanceCache.Invalidate(1); });
	}
	require(!instanceCache.IsCurrent(1, first.get()) && !instanceCache.Find(1),
		"A reader could commit with a snapshot another thread invalidated");
	require(instanceCache.Remember(1, second, 30), "Could not remember an initialized replacement");
	instanceCache.Invalidate(1, first.get());
	require(instanceCache.IsCurrent(1, second.get()), "A stale reader evicted a newer record");

	std::istringstream wrongFileLength(bytes);
	require(!ReadEntry(wrongFileLength, headerSize), "File length inconsistent with ordinal count validated");
	std::istringstream exactFileLength(bytes);
	require(ReadEntry(exactFileLength, bytes.size()).has_value(), "A valid length-aware cache file was rejected");
	{
		std::istringstream combined(bytes + "t", std::ios::binary);
		const auto bounded = ReadEntryBytes([&](char* target, std::size_t count) {
			return static_cast<bool>(combined.read(target, static_cast<std::streamsize>(count)));
		}, bytes.size());
		require(bounded && bounded->stateMachines == original.stateMachines && combined.peek() == 't',
			"Reading an embedded plan consumed the following topology");
		auto wrongCount = bytes;
		const std::uint32_t advertised = static_cast<std::uint32_t>(nodeThreshold);
		std::memcpy(wrongCount.data() + headerSize - sizeof(advertised), &advertised, sizeof(advertised));
		std::istringstream corrupt(wrongCount, std::ios::binary);
		unsigned reads{};
		const auto mismatchedSize = ReadEntryBytes([&](char* target, std::size_t count) {
			++reads;
			return static_cast<bool>(corrupt.read(target, static_cast<std::streamsize>(count)));
		}, wrongCount.size());
		require(!mismatchedSize && reads == 1, "A mismatched ordinal count triggered a second cache read");
	}

	using namespace std::chrono_literals;
	using GraphCache::Key;
	using GraphCache::Origin;
	using GraphCache::PreparedMismatch;
	using GraphCache::Result;
	using GraphCache::Store;
	using GraphCache::WriteResult;
	const Store::Clock::time_point start{};
	const auto directory = std::filesystem::temp_directory_path() /
		std::format("behavior-cache-tests-{}-{}", GetCurrentProcessId(), Store::Clock::now().time_since_epoch().count());
	require(std::filesystem::create_directory(directory), "Could not create a new cache test directory");
	const auto planFile = [](const std::filesystem::path& root, Key key) {
		return root / std::format("{:016x}-{:016x}.bin", key.graph, key.shape);
	};
	const auto updated = replaced.Build(shape.value);
	const Key key{7, replaced.Describe(updated.topology).CacheVariant()};
	const auto target = planFile(directory, key);
	const auto readTargetBytes = [&] {
		std::ifstream input(target, std::ios::binary);
		return ReadEntry(input);
	};
	Store cache(directory);
	require(cache.Read(key).result == Result::missing, "A missing cache file was misclassified");
	// Seed the mismatched variant to exercise atomic replacement of bad cache contents.
	require(cache.Write(key, original, start) == WriteResult::saved, "Initial cache file could not be committed");
	require(cache.Read(key, start).origin == Origin::memory && cache.Read(key, start).persisted,
		"Committed plan did not stay in memory with confirmed persistence");
	auto changedTiming = original;
	changedTiming.analysisMicros += 100;
	require(cache.Write(key, changedTiming, start) == WriteResult::current, "A timing-only change rewrote the plan");

	const HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	require(locked != INVALID_HANDLE_VALUE, "Could not acquire the replacement test lock");
	require(cache.Write(key, updated, start) == WriteResult::failed, "A locked destination unexpectedly allowed replacement");
	auto hit = cache.Read(key, start + 29s);
	require(hit.entry && hit.entry->stateMachines == updated.stateMachines && !hit.retryWrite && !hit.persisted,
		"A write error discarded the rebuilt in-memory plan");
	require(cache.Write(key, updated, start + 29s) == WriteResult::pending, "Failed write retried early");
	CloseHandle(locked);
	const auto oldDisk = readTargetBytes();
	require(oldDisk && oldDisk->stateMachines == original.stateMachines && Store(directory).Read(key).result == Result::invalid,
		"Failed replacement damaged the previous committed plan");
	require(std::distance(std::filesystem::directory_iterator(directory), {}) == 1,
		"Failed replacement left a temporary file behind");
	require(cache.Read(key, start + 30s).retryWrite, "Failed write was not eligible for retry");

	constexpr std::size_t workerCount = 12;
	std::array<WriteResult, workerCount> outcomes{};
	std::array<std::jthread, workerCount> writers;
	std::barrier ready(static_cast<std::ptrdiff_t>(workerCount));
	for (std::size_t i = 0; i < writers.size(); ++i) {
		writers[i] = std::jthread([&, i] {
			ready.arrive_and_wait();
			outcomes[i] = cache.Write(key, updated, start + 30s);
		});
	}
	for (auto& writer : writers) writer.join();
	require(std::count(outcomes.begin(), outcomes.end(), WriteResult::saved) == 1,
		"Concurrent retries performed zero or multiple disk commits");
	require(std::all_of(outcomes.begin(), outcomes.end(), [](WriteResult result) {
		return result == WriteResult::saved || result == WriteResult::pending || result == WriteResult::current;
	}), "A coalesced cache write reported an unexpected error");
	const auto newDisk = Store(directory).Read(key);
	require(newDisk.origin == Origin::disk && newDisk.entry->stateMachines == updated.stateMachines,
		"Restart did not load the complete retried file");
	require(!cache.Read(key, start + 60s).retryWrite, "Successful retry remained pending");

	const HANDLE sharedReader = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	require(sharedReader != INVALID_HANDLE_VALUE, "Could not open the cache with delete sharing");
	const auto sharedWrite = cache.Write(key, original, start + 60s);
	std::string heldBytes(headerSize + updated.stateMachines.size() * sizeof(std::uint32_t), '\0');
	DWORD heldSize{};
	const auto readHeld = ReadFile(sharedReader, heldBytes.data(), static_cast<DWORD>(heldBytes.size()), &heldSize, nullptr);
	CloseHandle(sharedReader);
	require(sharedWrite == WriteResult::saved, "A cache reader permitting deletion blocked publication");
	std::istringstream heldInput(heldBytes, std::ios::binary);
	const auto oldEntry = ReadEntry(heldInput, heldSize);
	const auto newEntry = readTargetBytes();
	require(readHeld && oldEntry && oldEntry->stateMachines == updated.stateMachines &&
		newEntry && newEntry->stateMachines == original.stateMachines,
		"An atomic replacement did not preserve the previous data for the open handle");
	require(cache.Write(key, updated, start + 60s) == WriteResult::saved, "Could not restore the current plan for subsequent tests");

	const Key oversized{7, live.Describe(original.topology).CacheVariant()};
	const auto invalidPath = planFile(directory, oversized);
	{
		std::ofstream file(invalidPath, std::ios::binary);
		file.seekp(maxFileSize);
		file.put('x');
	}
	require(cache.Read(oversized).result == Result::invalid, "An oversized cache file was accepted");
	require(cache.Write(oversized, original) == WriteResult::saved, "Oversized cache entry was not replaced");
	require(Store(directory).Read(oversized).origin == Origin::disk, "Repaired cache file was unusable");
	{
		auto interrupted = target;
		interrupted += L".interrupted.tmp";
		std::ofstream partialFile(interrupted, std::ios::binary);
		partialFile << "partial data";
	}
	const auto afterInterruption = Store(directory).Read(key);
	require(afterInterruption.origin == Origin::disk && afterInterruption.entry->stateMachines == updated.stateMachines,
		"An interrupted temporary write affected a committed entry");
	{
		std::ofstream legacyFile(directory / "0000000000000008.bin", std::ios::binary);
		WriteEntry(legacyFile, original);
	}
	const Key legacyKey{8, oversized.shape};
	const auto legacy = Store(directory).Read(legacyKey);
	require(legacy.origin == Origin::legacy && legacy.entry->stateMachines == original.stateMachines,
		"Existing legacy cache file stopped loading");
	{
		const Key unrelated{legacyKey.graph, key.shape};
		require(!Store(directory).Read(unrelated).entry, "A legacy plan was returned for a different graph variant");
		{
			std::ofstream corrupt(planFile(directory, legacyKey), std::ios::binary);
			corrupt.put('x');
		}
		Store repair(directory);
		const auto fallback = repair.Read(legacyKey);
		require(fallback.entry && fallback.origin == Origin::legacy && !fallback.persisted &&
			live.Matches(*fallback.entry, original.topology), "An invalid modern file hid the valid legacy plan");
		require(repair.Write(legacyKey, *fallback.entry) == WriteResult::saved &&
			Store(directory).Read(legacyKey).origin == Origin::disk, "The verified fallback could not replace the damaged file");
	}
	{
		StateMachines manyMachines(original.nodes);
		for (std::uint32_t ordinal = 0; ordinal < 512; ordinal += 2) manyMachines.Add(ordinal);
		const auto largePlan = manyMachines.Build(original.topology);
		const Key largePlanKey{9, manyMachines.Describe(original.topology).CacheVariant()};
		require(cache.Write(largePlanKey, largePlan) == WriteResult::saved, "Could not write a plan above the stack-read threshold");
		const auto loadedPlan = Store(directory).Read(largePlanKey);
		require(loadedPlan.entry && loadedPlan.origin == Origin::disk && loadedPlan.entry->stateMachines == largePlan.stateMachines,
			"Reading the large ordinal array directly lost data");
	}

	Store eviction(directory);
	require(eviction.Read(key).origin == Origin::disk, "Could not load the first eviction candidate");
	require(eviction.Read(oversized).origin == Origin::disk, "Could not load the second eviction candidate");
	for (std::uint64_t graph = 10; graph < 72; ++graph) {
		require(eviction.Write({graph, oversized.shape}, original) == WriteResult::saved, "Filling the memory cache failed");
	}
	require(eviction.Read(key).origin == Origin::memory, "Recently needed entry was evicted too early");
	require(eviction.Write({72, oversized.shape}, original) == WriteResult::saved, "Cache admission at capacity failed");
	require(eviction.Read(key).origin == Origin::memory && eviction.Read(oversized).origin == Origin::disk,
		"Memory cache did not evict the least recently used plan");

	const auto startupDirectory = directory / "startup";
	require(std::filesystem::create_directory(startupDirectory), "Could not create the startup test directory");
	const auto startupFile = [&](Key entry) {
		return planFile(startupDirectory, entry);
	};
	const Key newest{42, replaced.Describe(updated.topology).CacheVariant()};
	const Key previous{42, live.Describe(original.topology).CacheVariant()};
	const Key another{43, previous.shape};
	const Key misnamed{44, newest.shape};
	Store seed(startupDirectory);
	require(seed.Write(newest, updated) == WriteResult::saved &&
		seed.Write(previous, original) == WriteResult::saved &&
		seed.Write(another, original) == WriteResult::saved &&
		seed.Write(misnamed, original) == WriteResult::saved, "Could not seed startup plans");
	const auto stamp = std::filesystem::file_time_type::clock::now();
	std::filesystem::last_write_time(startupFile(newest), stamp);
	std::filesystem::last_write_time(startupFile(previous), stamp - 1h);
	std::filesystem::last_write_time(startupFile(another), stamp - 2h);
	std::filesystem::last_write_time(startupFile(misnamed), stamp - 3h);

	static std::atomic_bool cancelled{};
	static std::array<std::pair<unsigned, unsigned>, 4> progress{};
	static unsigned updates{};
	Store warm(startupDirectory);
	const auto startup = warm.Preload(2, Store::Clock::now() + 5s, cancelled, [](unsigned checked, unsigned total) noexcept {
		if (updates < progress.size()) progress[updates] = {checked, total};
		++updates;
	});
	require(startup.selected == 2 && startup.checked == 2 && startup.warmed == 2 && startup.prepared == 2 &&
		!startup.interrupted && startup.preparedBytes == (original.nodes + updated.nodes) * sizeof(std::uint16_t),
		"Preload did not prepare the newest variant and the next distinct graph");
	require(updates == 3 && progress[0] == std::pair{0u, 2u} && progress[1] == std::pair{1u, 2u} &&
		progress[2] == std::pair{2u, 2u}, "Preload progress did not count actual attempts");
	{
		std::filesystem::last_write_time(startupFile(misnamed), stamp + 1h);
		Store backfilled(startupDirectory);
		const auto recovered = backfilled.Preload(2, Store::Clock::now() + 5s, cancelled, nullptr);
		require(recovered.selected == 3 && recovered.checked == 3 && recovered.unusable == 1 &&
			recovered.warmed == 2 && recovered.prepared == 2 && !recovered.interrupted &&
			backfilled.FindPrepared(newest.graph) && backfilled.FindPrepared(another.graph) &&
			!backfilled.FindPrepared(misnamed.graph), "An invalid cache file occupied a usable startup plan slot");
		std::filesystem::last_write_time(startupFile(misnamed), stamp - 3h);
	}
	const auto preparation = warm.FindPrepared(newest.graph);
	require(preparation && preparation->shape.CacheVariant() == newest.shape && preparation->entry->stateMachines == updated.stateMachines,
		"The prepared plan resolved to the wrong variant");
	const auto ids = preparation->IDs();
	PreparedCheck valid(ids), wrongType(ids), extraNode(ids);
	for (std::size_t index = 0; index < ids.size(); ++index) {
		const auto machine = std::ranges::binary_search(updated.stateMachines, static_cast<std::uint32_t>(index));
		valid.Observe(index, [machine] { return machine; });
		wrongType.Observe(index, [machine, index] { return machine != (index == 0); });
		extraNode.Observe(index, [machine] { return machine; });
	}
	extraNode.Observe(ids.size(), [] { return false; });
	require(valid.Matches(ids.size(), updated.topology, preparation->shape.topology) &&
		!valid.Matches(ids.size() - 1, updated.topology, preparation->shape.topology) &&
		!valid.Matches(ids.size(), updated.topology ^ 1, preparation->shape.topology) &&
		!wrongType.Matches(ids.size(), updated.topology, preparation->shape.topology) &&
		!extraNode.Matches(ids.size() + 1, updated.topology, preparation->shape.topology),
		"Fused verification accepted a changed graph or rejected the unchanged one");
	require(ids.size() == updated.nodes && ids[updated.stateMachines.front()] == 0 &&
		ids[updated.stateMachines.back()] == updated.stateMachines.size() - 1,
		"Prepared IDs do not preserve ascending state-machine order");
	require(warm.Read(newest).origin == Origin::preloaded && warm.Read(newest).origin == Origin::memory &&
		warm.Read(previous).origin == Origin::disk, "Preloading did not preserve separate on-demand variants");
	warm.ResolvePreparedMismatch(newest.graph, *preparation, PreparedMismatch::differentShape);
	require(!warm.FindPrepared(newest.graph) && warm.Read(newest).prepared == preparation && !warm.Read(previous).prepared,
		"An unprepared live variant under the same name discarded the existing exact preparation");
	warm.ResolvePreparedMismatch(newest.graph, *preparation);
	require(!warm.FindPrepared(newest.graph) && warm.FindPrepared(another.graph), "Rejecting a changed graph erased an unrelated preparation");
	require(warm.Read(previous).entry->stateMachines == original.stateMachines, "Rejecting a startup plan damaged the disk fallback");

	const auto evictionDirectory = directory / "preload-eviction";
	std::filesystem::create_directory(evictionDirectory);
	std::filesystem::copy_file(startupFile(newest), evictionDirectory / startupFile(newest).filename());
	std::filesystem::copy_file(startupFile(another), evictionDirectory / startupFile(another).filename());
	Store warmEviction(evictionDirectory);
	const auto evictionPreload = warmEviction.Preload(2, Store::Clock::now() + 5s, cancelled, nullptr);
	const auto activePreparation = warmEviction.FindPrepared(newest.graph);
	require(evictionPreload.warmed == 2 && activePreparation, "Could not set up prepared-plan eviction");
	bool addedAll = true;
	for (std::uint64_t i = 0; i < 65; ++i) {
		addedAll &= warmEviction.Write({1024 + i, previous.shape}, original) == WriteResult::saved;
	}
	require(addedAll && warmEviction.FindPrepared(newest.graph) == activePreparation && !warmEviction.FindPrepared(another.graph),
		"Used prepared plan was evicted ahead of the untouched prepared plan");
	for (const auto& item : std::filesystem::directory_iterator(evictionDirectory)) std::filesystem::remove(item.path());
	require(std::filesystem::remove(evictionDirectory), "Could not clean the prepared eviction fixture");

	Store mixed(startupDirectory);
	require(mixed.Read(newest).origin == Origin::disk, "Could not seed an on-demand memory entry");
	const auto mixedStats = mixed.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
	const auto mixedLatest = mixed.FindPrepared(newest.graph);
	require(mixedStats.selected == 4 && mixedStats.checked == 4 && mixedStats.warmed == 2 && mixedStats.prepared == 3 &&
		!mixed.FindPrepared(misnamed.graph) && mixed.Read(newest).origin == Origin::memory &&
		mixed.Read(previous).origin == Origin::preloaded,
		"Startup did not warm additional variants or preserve an active entry");
	require(mixedLatest && mixedLatest->shape.CacheVariant() == newest.shape,
		"An on-demand hit prevented startup preparation of the same variant");
	mixed.ResolvePreparedMismatch(newest.graph, *mixedLatest);
	const auto mixedOlder = mixed.FindPrepared(previous.graph);
	require(mixedOlder && mixedOlder->shape.CacheVariant() == previous.shape, "Rejection lost the next prepared variant");
	mixed.ResolvePreparedMismatch(newest.graph, *mixedLatest);
	require(mixed.FindPrepared(previous.graph) == mixedOlder && mixed.FindPrepared(another.graph),
		"A repeated stale rejection discarded a valid preparation");

	Store alternating(startupDirectory);
	const auto readyVariants = alternating.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
	const auto expectedVariant = alternating.FindPrepared(newest.graph);
	const auto actualVariant = alternating.Read(previous);
	require(readyVariants.prepared == 3 && expectedVariant && actualVariant.prepared &&
		live.Matches(*actualVariant.prepared->entry, original.topology) &&
		!replaced.Matches(*actualVariant.prepared->entry, updated.topology),
		"By-shape lookup did not return the verified alternate plan");
	alternating.ResolvePreparedMismatch(newest.graph, *expectedVariant, PreparedMismatch::differentShape);
	require(!alternating.FindPrepared(newest.graph) && alternating.FindPrepared(another.graph),
		"Multiple live variants did not disable speculative lookup only for the ambiguous name");
	require(alternating.Read(newest).prepared == expectedVariant && alternating.Read(previous).prepared == actualVariant.prepared,
		"Learning about a second live shape discarded a valid preparation");
	alternating.ResolvePreparedMismatch(newest.graph, *expectedVariant);
	require(!alternating.Read(newest).prepared && alternating.Read(previous).prepared == actualVariant.prepared,
		"Invalidating a single bundle removed the valid alternate shape");
	Store interruptedStartup(startupDirectory);
	const auto cancelledStats = interruptedStartup.Preload(16, Store::Clock::now() + 5s, cancelled,
		[](unsigned checked, unsigned) noexcept { if (checked == 1) cancelled.store(true); });
	require(cancelledStats.checked == 1 && cancelledStats.prepared == 1 && cancelledStats.interrupted &&
		!interruptedStartup.FindPrepared(another.graph), "Startup cancellation admitted another plan");
	cancelled.store(false);
	const auto expiredStats = interruptedStartup.Preload(16, Store::Clock::now(), cancelled, nullptr);
	require(expiredStats.interrupted && expiredStats.checked == 0, "Expired startup work performed file reads");
	Store onDemand(startupDirectory);
	const auto rejected = onDemand.Read(misnamed);
	require(rejected.result == Result::invalid && !rejected.entry && !rejected.persisted,
		"An unrelated plan was admitted from a variant filename without a matching bundle");
	std::filesystem::copy_file(startupFile(newest), startupFile(misnamed), std::filesystem::copy_options::overwrite_existing);
	const auto corrected = onDemand.Read(misnamed);
	require(corrected.origin == Origin::disk && corrected.entry && corrected.entry->stateMachines == updated.stateMachines,
		"The invalid plan remained in memory after its disk contents were replaced");
	for (const auto& file : std::filesystem::directory_iterator(startupDirectory)) std::filesystem::remove(file.path());
	require(std::filesystem::remove(startupDirectory), "Could not clean the preload test directory");

	const auto bakedDirectory = directory / "portable";
	constexpr unsigned bakedNodes = 40000;
	InstanceTopology::Trace bakedLinks(true);
	Signature bakedSignature;
	bakedSignature.Add(bakedNodes - 1);
	auto children = bakedLinks.RecordChildren(bakedNodes - 1);
	for (unsigned ordinal = 1; ordinal < bakedNodes; ++ordinal) {
		children[ordinal - 1] = ordinal;
		bakedSignature.Add(ordinal);
	}
	for (unsigned ordinal = 1; ordinal < bakedNodes; ++ordinal) {
		bakedLinks.RecordEmptyNode();
		bakedSignature.Add(0);
	}
	StateMachines bakedClasses(bakedNodes);
	bakedClasses.Add(0);
	bakedClasses.Add(21);
	const auto bakedShape = bakedClasses.Describe(bakedSignature.value);
	const auto bakedPlan = std::make_shared<const Entry>(bakedClasses.Build(bakedSignature.value));
	require(bakedLinks.PackOrdinals(), "Could not pack the persistent replay fixture");
	const GraphSlimming::Payload bakedPayload{std::move(bakedLinks), bakedPlan};
	const Key bakedKey{0xA, bakedShape.CacheVariant()};
	Store writer(bakedDirectory);
	require(writer.Write(bakedKey, *bakedPlan) == WriteResult::saved, "Could not commit the topology's plan");
	const auto lookupOnly = writer.ClaimTopology(bakedKey, *bakedPlan, start, false);
	require(!lookupOnly.ticket && !lookupOnly.saved, "A full queue created a new topology write ticket");
	const auto firstClaim = writer.ClaimTopology(bakedKey, *bakedPlan);
	require(firstClaim.ticket && !writer.ClaimTopology(bakedKey, *bakedPlan).ticket, "Topology saves did not coalesce");
	require(writer.Write(bakedKey, original) == WriteResult::pending && writer.Read(bakedKey).entry->topology == bakedPlan->topology,
		"A different plan replaced a record with an active topology job");
	writer.CancelTopology(bakedKey, firstClaim.ticket);
	const auto nextClaim = writer.ClaimTopology(bakedKey, *bakedPlan);
	require(nextClaim.ticket && nextClaim.ticket != firstClaim.ticket, "A deferred topology could not be scheduled on its next use");
	writer.CancelTopology(bakedKey, firstClaim.ticket);
	require(!writer.SaveTopology(bakedKey, firstClaim.ticket, bakedPayload) && !writer.ClaimTopology(bakedKey, *bakedPlan).ticket,
		"An old topology request interfered with the replacement request");
	require(writer.SaveTopology(bakedKey, nextClaim.ticket, bakedPayload) && writer.ClaimTopology(bakedKey, *bakedPlan).saved,
		"Topology persistence was not recorded after publication");
	const auto savedWhileFull = writer.ClaimTopology(bakedKey, *bakedPlan, start, false);
	require(savedWhileFull.saved && !savedWhileFull.ticket &&
		!writer.ClaimTopology(bakedKey, original, start, false).saved,
		"A full queue hid a committed topology or confirmed the wrong plan");
	const Key incompleteTopologyKey{bakedKey.graph + 1, bakedKey.shape};
	require(writer.Write(incompleteTopologyKey, *bakedPlan) == WriteResult::saved, "Could not prepare incomplete-topology fixture");
	const auto pendingTopology = writer.ClaimTopology(incompleteTopologyKey, *bakedPlan);
	const GraphSlimming::Payload incompleteTopology{{}, bakedPlan};
	require(pendingTopology.ticket && !writer.SaveTopology(incompleteTopologyKey, pendingTopology.ticket, incompleteTopology) &&
		!writer.ClaimTopology(incompleteTopologyKey, *bakedPlan).ticket &&
		!std::filesystem::exists(bakedDirectory / std::format("{:016x}-{:016x}.graph", incompleteTopologyKey.graph, incompleteTopologyKey.shape)),
		"An incomplete topology was written or immediately requeued");
	std::filesystem::remove(bakedDirectory / std::format("{:016x}-{:016x}.bin", incompleteTopologyKey.graph, incompleteTopologyKey.shape));

	{
		const Key invalidLinksKey{bakedKey.graph + 3, bakedKey.shape};
		InstanceTopology::Trace wrongLinks(true);
		auto rootLinks = wrongLinks.RecordChildren(bakedNodes);
		require(rootLinks.size() == bakedNodes, "Could not capture the wrong-topology fixture");
		for (unsigned ordinal = 1; ordinal < bakedNodes; ++ordinal) rootLinks[ordinal - 1] = ordinal;
		rootLinks.back() = GraphPlan::noNode;
		for (unsigned ordinal = 1; ordinal < bakedNodes; ++ordinal) wrongLinks.RecordEmptyNode();
		const GraphSlimming::Payload mismatched{std::move(wrongLinks), bakedPlan};
		const auto stem = std::format("{:016x}-{:016x}", invalidLinksKey.graph, invalidLinksKey.shape);
		const auto graphFile = bakedDirectory / (stem + ".graph");
		require(writer.Write(invalidLinksKey, *bakedPlan) == WriteResult::saved, "Could not seed a plan for the wrong-topology fixture");
		const auto wrong = writer.ClaimTopology(invalidLinksKey, *bakedPlan);
		require(wrong.ticket && !writer.SaveTopology(invalidLinksKey, wrong.ticket, mismatched) &&
			!std::filesystem::exists(graphFile), "A different reachable topology was published as a valid cache record");
		const auto retry = writer.ClaimTopology(invalidLinksKey, *bakedPlan, Store::Clock::now() + 31s);
		require(retry.ticket && writer.SaveTopology(invalidLinksKey, retry.ticket, bakedPayload),
			"A rejected topology prevented a later valid publication");
		std::filesystem::remove(graphFile);
		std::filesystem::remove(bakedDirectory / (stem + ".bin"));
	}

	const Key combinedOnlyKey{bakedKey.graph + 2, bakedKey.shape};
	const auto combinedPlan = bakedDirectory / std::format("{:016x}-{:016x}.bin", combinedOnlyKey.graph, combinedOnlyKey.shape);
	const auto combinedGraph = bakedDirectory / std::format("{:016x}-{:016x}.graph", combinedOnlyKey.graph, combinedOnlyKey.shape);
	{
		std::ofstream blocked(combinedPlan, std::ios::binary);
		blocked.put('x');
	}
	const HANDLE restrictedPlan = CreateFileW(combinedPlan.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	require(restrictedPlan != INVALID_HANDLE_VALUE, "Could not lock the companion plan");
	const auto failedPlanWrite = writer.Write(combinedOnlyKey, *bakedPlan, start);
	const auto combinedClaim = writer.ClaimTopology(combinedOnlyKey, *bakedPlan, start);
	const auto savedCombined = writer.SaveTopology(combinedOnlyKey, combinedClaim.ticket, bakedPayload);
	CloseHandle(restrictedPlan);
	require(failedPlanWrite == WriteResult::failed && combinedClaim.ticket && savedCombined,
		"Combined graph did not persist independently of its blocked companion plan");
	const auto storedCombined = writer.Read(combinedOnlyKey, start + 60s);
	const auto reloadedCombined = Store(bakedDirectory).Read(combinedOnlyKey);
	require(storedCombined.persisted && !storedCombined.retryWrite && reloadedCombined.entry &&
		reloadedCombined.entry->stateMachines == bakedPlan->stateMachines &&
		writer.Write(combinedOnlyKey, *bakedPlan, start + 60s) == WriteResult::current &&
		std::filesystem::file_size(combinedPlan) == 1,
		"A durable combined graph was treated as an uncommitted plan and rewritten");
	std::filesystem::remove(combinedGraph);
	std::filesystem::remove(combinedPlan);
	Store bakedRestart(bakedDirectory);
	const auto bakedStats = bakedRestart.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
	const auto bakedCandidate = bakedRestart.FindPrepared(bakedKey.graph);
	require(bakedStats.baked == 1 && bakedCandidate && bakedCandidate->payload &&
		bakedCandidate->payload->links.SameLinks(bakedPayload.links) && bakedRestart.ClaimTopology(bakedKey, *bakedPlan).saved,
		"Preload did not prepare the persistent topology for the stored graph");
	bakedRestart.ResolvePreparedMismatch(bakedKey.graph, *bakedCandidate, PreparedMismatch::topology);
	const auto idsOnly = bakedRestart.FindPrepared(bakedKey.graph);
	require(idsOnly && !idsOnly->payload && idsOnly->IDs().data() == bakedCandidate->IDs().data() &&
		idsOnly->IDs().size() == bakedCandidate->IDs().size() && bakedRestart.Read(bakedKey).prepared == idsOnly,
		"A topology-only failure discarded or copied the validated ID table");
	bakedRestart.ResolvePreparedMismatch(bakedKey.graph, *bakedCandidate);
	const auto afterRejection = bakedRestart.ClaimTopology(bakedKey, *bakedPlan);
	require(bakedRestart.FindPrepared(bakedKey.graph) == idsOnly && afterRejection.ticket && !afterRejection.saved,
		"A stale topology rejection erased the retained IDs or prevented a fresh topology write");
	bakedRestart.CancelTopology(bakedKey, afterRejection.ticket);
	const Key offlineKey{0x1234, bakedShape.CacheVariant()};
	Store offlineOnly(bakedDirectory);
	require(offlineOnly.AdmitOffline(offlineKey, bakedShape, GraphSlimming::Payload{bakedPayload}) &&
		!offlineOnly.Read(offlineKey).entry, "An offline plan leaked into the ordinary plan lookup");
	const auto offlineCandidate = offlineOnly.FindPrepared(offlineKey.graph);
	require(offlineCandidate && offlineCandidate->offline && offlineCandidate->payload &&
		offlineCandidate->payload->links.SameLinks(bakedPayload.links), "The offline replay candidate was lost");
	offlineOnly.ResolvePreparedMismatch(offlineKey.graph, *offlineCandidate);
	require(!offlineOnly.FindPrepared(offlineKey.graph) && !offlineOnly.Read(offlineKey).entry &&
		offlineOnly.FindOffline(offlineKey) == offlineCandidate,
		"A failed offline replay stopped exact lookup or leaked into the ordinary cache");
	{
		const auto publication = bakedDirectory / "offline-publication";
		Store publisher(publication);
		require(publisher.AdmitOffline(offlineKey, bakedShape, GraphSlimming::Payload{bakedPayload}),
			"Could not admit the offline publication fixture");
		const auto candidate = publisher.FindOffline(offlineKey);
		require(candidate && !publisher.ClaimTopology(offlineKey, *candidate->entry, start, false).ticket &&
			!publisher.Read(offlineKey).entry, "A full queue published an uncommitted offline plan");
		const auto job = publisher.ClaimTopology(offlineKey, *candidate->entry);
		require(job.ticket && !publisher.Read(offlineKey).persisted &&
			publisher.SaveTopology(offlineKey, job.ticket, *candidate->payload) &&
			publisher.Read(offlineKey).persisted, "Live-confirmed offline topology required a standalone plan write");
		require(Store(publication).Preload(1, Store::Clock::now() + 5s, cancelled, nullptr).baked == 1,
			"A restart could not preload the published offline topology");
		for (const auto& file : std::filesystem::directory_iterator(publication)) std::filesystem::remove(file.path());
		std::filesystem::remove(publication);
	}
	StateMachines offlineOther(bakedPlan->nodes);
	offlineOther.Add(0);
	offlineOther.Add(5);
	const auto otherShape = offlineOther.Describe(bakedPlan->topology);
	const Key otherOfflineKey{offlineKey.graph, otherShape.CacheVariant()};
	const auto otherPlan = std::make_shared<const Entry>(offlineOther.Build(bakedPlan->topology));
	require(offlineOnly.AdmitOffline(otherOfflineKey, otherShape, {bakedPayload.links, otherPlan}) &&
		!offlineOnly.AdmitOffline(otherOfflineKey, otherShape, {bakedPayload.links, otherPlan}) &&
		!offlineOnly.FindPrepared(offlineKey.graph) && offlineOnly.FindOffline(otherOfflineKey) &&
		offlineOnly.FindOffline(offlineKey) == offlineCandidate,
		"A second offline shape displaced an existing exact candidate or remained speculative");
	bool admittedLimit = true;
	for (unsigned i = 0; i < 254; ++i) {
		admittedLimit &= offlineOnly.AdmitOffline({offlineKey.graph + i + 1, offlineKey.shape},
			bakedShape, GraphSlimming::Payload{bakedPayload});
	}
	require(admittedLimit && !offlineOnly.AdmitOffline({offlineKey.graph + 300, offlineKey.shape},
		bakedShape, GraphSlimming::Payload{bakedPayload}), "Offline admission exceeded the resident plan limit");
	{
		Store simultaneous(bakedDirectory);
		constexpr std::size_t workers = 4;
		std::array<bool, workers> admitted{};
		std::array<std::jthread, workers> tasks;
		std::barrier readyToAdmit(static_cast<std::ptrdiff_t>(workers));
		for (std::size_t i = 0; i < tasks.size(); ++i) {
			tasks[i] = std::jthread([&, i] {
				GraphSlimming::Payload payload(bakedPayload);
				readyToAdmit.arrive_and_wait();
				admitted[i] = simultaneous.AdmitOffline(offlineKey, bakedShape, std::move(payload));
			});
		}
		for (auto& task : tasks) task.join();
		require(std::count(admitted.begin(), admitted.end(), true) == 1 && simultaneous.FindOffline(offlineKey),
			"Simultaneous admissions published duplicate prepared offline plans");
	}
	const auto bakedFile = bakedDirectory / std::format("{:016x}-{:016x}.graph", bakedKey.graph, bakedKey.shape);
	const auto originalPlanFile = bakedDirectory / std::format("{:016x}-{:016x}.bin", bakedKey.graph, bakedKey.shape);
	require(std::filesystem::remove(originalPlanFile), "Could not stage cache loss of the companion plan");
	{
		Store withoutPlan(bakedDirectory);
		const auto restored = withoutPlan.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
		require(restored.selected == 1 && restored.baked == 1 && withoutPlan.Read(bakedKey).entry->stateMachines == bakedPlan->stateMachines,
			"The combined graph was discarded when the companion plan disappeared");
	}
	require(Store(bakedDirectory).Read(bakedKey).entry != nullptr, "On-demand lookup could not recover the embedded plan");
	{
		const auto prefix = 3 * sizeof(std::uint64_t) + sizeof(std::uint32_t) +
			GraphPlan::headerSize + bakedPlan->stateMachines.size() * sizeof(std::uint32_t);
		std::filesystem::resize_file(bakedFile, prefix);
		Store planOnly(bakedDirectory);
		const auto embedded = planOnly.Read(bakedKey);
		const auto preloaded = planOnly.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
		const auto preparation = planOnly.FindPrepared(bakedKey.graph);
		const auto repair = planOnly.ClaimTopology(bakedKey, *bakedPlan);
		require(embedded.entry && embedded.entry->stateMachines == bakedPlan->stateMachines &&
			preloaded.prepared == 1 && preloaded.baked == 0 && preloaded.unusable == 0 &&
			preparation && !preparation->payload && repair.ticket && !repair.saved,
			"Losing only the topology discarded the valid embedded plan or prevented repair");
		require(planOnly.SaveTopology(bakedKey, repair.ticket, bakedPayload) &&
			std::filesystem::file_size(bakedFile) > prefix, "The missing topology could not be rebuilt");
	}
	require(Store(bakedDirectory).Write(bakedKey, original) == WriteResult::saved, "Could not seed a misidentified companion plan");
	const auto recovered = Store(bakedDirectory).Read(bakedKey);
	require(recovered.entry && recovered.entry->topology == bakedPlan->topology &&
		recovered.entry->stateMachines == bakedPlan->stateMachines, "A valid but unrelated plan masked the correct embedded plan");
	require(Store(bakedDirectory).Write(bakedKey, *bakedPlan) == WriteResult::saved, "Could not restore the companion fixture for migration");
	const auto legacyBakedFile = bakedDirectory / std::format("{:016x}-{:016x}.topo", bakedKey.graph, bakedKey.shape);
	require(std::filesystem::remove(bakedFile), "Could not stage an old topology cache fixture");
	{
		std::ofstream legacyTopology(legacyBakedFile, std::ios::binary);
		require(bakedPayload.links.WritePortable(legacyTopology, *bakedPlan), "Could not seed the old topology cache format");
	}
	require(Store(bakedDirectory).Preload(16, Store::Clock::now() + 5s, cancelled, nullptr).baked == 1 &&
		std::filesystem::is_regular_file(bakedFile), "Legacy topology was not incorporated into the versioned graph cache");
	require(std::filesystem::remove(legacyBakedFile), "Could not isolate the integrated cache fixture");
	{
		std::ofstream corruptedFile(bakedFile, std::ios::binary | std::ios::app);
		corruptedFile.put('x');
	}
	Store corruptedRestart(bakedDirectory);
	const auto fallbackStats = corruptedRestart.Preload(16, Store::Clock::now() + 5s, cancelled, nullptr);
	const auto ordinaryCandidate = corruptedRestart.FindPrepared(bakedKey.graph);
	require(fallbackStats.prepared == 1 && fallbackStats.baked == 0 && ordinaryCandidate && !ordinaryCandidate->payload,
		"Corrupt topology broke the ordinary cached plan");
	const auto repairClaim = corruptedRestart.ClaimTopology(bakedKey, *bakedPlan);
	require(repairClaim.ticket, "Corrupt topology did not become repairable");
	const auto oldGraph = CreateFileW(bakedFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	require(oldGraph != INVALID_HANDLE_VALUE, "Could not hold a cache destination open");
	const auto blockedRepair = corruptedRestart.SaveTopology(bakedKey, repairClaim.ticket, bakedPayload);
	CloseHandle(oldGraph);
	const auto afterFailure = Store::Clock::now();
	require(!blockedRepair && !corruptedRestart.ClaimTopology(bakedKey, *bakedPlan, afterFailure).ticket,
		"A failed background write could retry continuously on the fast path");
	const auto retry = corruptedRestart.ClaimTopology(bakedKey, *bakedPlan, afterFailure + 31s);
	require(retry.ticket && corruptedRestart.SaveTopology(bakedKey, retry.ticket, bakedPayload),
		"The in-memory topology did not save once the disk became writable");
	require(Store(bakedDirectory).Preload(16, Store::Clock::now() + 5s, cancelled, nullptr).baked == 1,
		"Rebuilt topology could not be preloaded");
	for (const auto& file : std::filesystem::directory_iterator(bakedDirectory)) std::filesystem::remove(file.path());
	require(std::filesystem::remove(bakedDirectory), "Could not clean the portable replay test directory");

	constexpr std::uint64_t gibibyte = std::uint64_t{1} << 30;
	require(AnimationPreload::BudgetBytes(2 * gibibyte, 4096) == 0 &&
		AnimationPreload::BudgetBytes(2 * gibibyte + 4096, 4096) == 4096 &&
		AnimationPreload::BudgetBytes(8 * gibibyte, 4096) == 2 * gibibyte &&
		AnimationPreload::BudgetBytes(64 * gibibyte, 4096) == 4 * gibibyte,
		"File preload did not preserve RAM headroom or respect its cap");
	const auto meshes = directory / "Meshes";
	std::vector<std::filesystem::path> assetFolders;
	const auto makeAsset = [&](const char* relative, std::size_t bytesToWrite) {
		const auto path = meshes / relative;
		for (auto parent = path.parent_path(); parent != directory; parent = parent.parent_path()) assetFolders.push_back(parent);
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary);
		output << std::string(bytesToWrite, 'x');
		require(static_cast<bool>(output), "Could not seed a file-cache fixture");
	};
	makeAsset("actors/character/behaviors/runtime.hkx", 128);
	makeAsset("actors/character/animations/pack/attacks.HKX", 96);
	makeAsset("OpenAnimationReplacer/pack/idle.hkx", 64);
	makeAsset("actors/character/animations/optional.txt", 32);
	makeAsset("clutter/ignored.hkx", 16);
	MEMORYSTATUSEX memory{};
	memory.dwLength = sizeof(memory);
	if (GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys > 2 * gibibyte) {
		static std::array<unsigned, 2> selectedAssets{};
		static std::array<unsigned, 2> checkedAssets{};
		const auto scan = AnimationPreload::Warm(meshes, 512, Store::Clock::now() + 5s, cancelled,
			[](bool behavior, unsigned checked, unsigned selected) noexcept {
				const std::size_t phase = behavior ? 0 : 1;
				selectedAssets[phase] = selected;
				checkedAssets[phase] = checked;
			}, {}, true);
		require(!scan.interrupted && !scan.catalogLimited && scan.behaviorFiles == 1 && scan.animationFiles == 2 && scan.bytes == 288 &&
			scan.selected == 3 && scan.checked == 3, "File preload missed active HKX or scanned unrelated assets");
		require(scan.behaviorIO.open.samples == 1 && scan.behaviorIO.fileRead.samples == 1 &&
			scan.behaviorIO.bytes == 128 && scan.behaviorIO.readCalls == 1 &&
			scan.animationIO.open.samples == 2 && scan.animationIO.fileRead.samples == 2 &&
			scan.animationIO.bytes == 160 && scan.animationIO.readCalls == 2 &&
			!scan.animationIO.failedReads && !scan.behaviorIO.shortReads,
			"HKX timing included unrelated files or mixed behavior and animation reads");
		require(scan.scannedDirectories == 7, "An already covered OAR directory was enumerated again without a persistent index");
		require(selectedAssets == std::array{1u, 2u} && checkedAssets == selectedAssets,
			"File progress did not report real behavior and animation work");
		const auto constrained = AnimationPreload::Warm(meshes, 128, Store::Clock::now() + 5s, cancelled, nullptr);
		require(constrained.behaviorFiles == 1 && constrained.animationFiles == 0 && constrained.bytes == 128 &&
			!constrained.behaviorIO.readCalls && !constrained.behaviorIO.open.samples,
			"File preload failed to prioritize behavior data within its byte budget");
		const auto prioritized = AnimationPreload::Warm(meshes, 224, Store::Clock::now() + 5s, cancelled, nullptr);
		require(prioritized.behaviorFiles == 1 && prioritized.animationFiles == 1 && prioritized.bytes == 192 &&
			prioritized.selected == 3, "OAR priority files were skipped or warmed twice");

		const auto indexFile = directory / "HKXIndex.bin";
		const auto scanIndexed = [&](std::uint64_t budget = 512) {
			return AnimationPreload::Warm(meshes, budget, Store::Clock::now() + 5s, cancelled, nullptr, indexFile);
		};
		const auto limitedIndexFile = directory / "limited-index.bin";
		const auto singleIndexFile = directory / "single-index.bin";
		const auto behaviorFolder = meshes / "actors/character/behaviors";
		const auto conditionFolder = meshes / "OpenAnimationReplacer/pack";
		{
			AnimationPreload::DirectoryIndex seed(meshes, limitedIndexFile);
			unsigned remaining = 32;
			(void)seed.Read(conditionFolder, Store::Clock::now() + 5s, cancelled, remaining);
			seed.Save(false, Store::Clock::now() + 5s, cancelled);
			require(seed.saved, "Could not seed an older index listing");
			AnimationPreload::DirectoryIndex single(meshes, singleIndexFile);
			(void)single.Read(behaviorFolder, Store::Clock::now() + 5s, cancelled, remaining);
			single.Save(false, Store::Clock::now() + 5s, cancelled);
			require(single.saved, "Could not measure a single index record");
		}
		const auto oneRecord = static_cast<std::size_t>(std::filesystem::file_size(singleIndexFile));
		{
			AnimationPreload::DirectoryIndex constrainedIndex(meshes, limitedIndexFile);
			unsigned remaining = 32;
			bool fullListing{};
			const auto data = constrainedIndex.Read(behaviorFolder, Store::Clock::now() + 5s, cancelled, remaining, &fullListing);
			constrainedIndex.Save(false, Store::Clock::now() + 5s, cancelled, oneRecord);
			require(data && fullListing && constrainedIndex.saved && constrainedIndex.limited &&
				std::filesystem::file_size(limitedIndexFile) == oneRecord, "A full index discarded the verified update");
		}
		{
			AnimationPreload::DirectoryIndex resumed(meshes, limitedIndexFile);
			unsigned remaining = 32;
			(void)resumed.Read(behaviorFolder, Store::Clock::now() + 5s, cancelled, remaining);
			(void)resumed.Read(conditionFolder, Store::Clock::now() + 5s, cancelled, remaining);
			resumed.Save(false, Store::Clock::now() + 5s, cancelled, oneRecord);
			require(resumed.reused == 1 && resumed.scanned == 1 && resumed.limited && !resumed.saved,
				"An identical bounded index was rewritten or lost its current listing");
		}
		std::filesystem::remove(limitedIndexFile);
		std::filesystem::remove(singleIndexFile);
		const auto firstScan = scanIndexed();
		require(firstScan.indexSaved && firstScan.indexedDirectories == 0 && firstScan.scannedDirectories > 0 &&
			std::filesystem::is_regular_file(indexFile), "Cold startup did not save a persistent HKX index");
		const auto reused = scanIndexed();
		require(reused.scannedDirectories == 0 && reused.indexedDirectories == firstScan.scannedDirectories &&
			!reused.indexSaved && reused.bytes == firstScan.bytes, "Clean restart rescanned or rewrote the HKX index");
		{
			std::atomic_bool stopIndex{true};
			AnimationPreload::DirectoryIndex cancelledIndex(meshes, indexFile, &stopIndex);
			stopIndex.store(false);
			AnimationPreload::DirectoryIndex expiredIndex(meshes, indexFile, &stopIndex, Store::Clock::now());
			unsigned remaining = 32;
			const auto deadline = Store::Clock::now() + 5s;
			const auto afterCancel = cancelledIndex.Read(behaviorFolder, deadline, stopIndex, remaining);
			const auto afterDeadline = expiredIndex.Read(behaviorFolder, deadline, stopIndex, remaining);
			require(afterCancel && afterDeadline && cancelledIndex.reused == 0 && expiredIndex.reused == 0 &&
				cancelledIndex.scanned == 1 && expiredIndex.scanned == 1,
				"An already cancelled or expired index load restored directory records");
		}

		makeAsset("actors/character/behaviors/runtime.hkx", 192);
		const auto edited = scanIndexed();
		require(edited.behaviorFiles == 1 && edited.animationFiles == 2 && edited.bytes == 352,
			"Indexed startup did not read the current contents of a modified file");

		const auto oldFile = meshes / "OpenAnimationReplacer/pack/idle.hkx";
		std::filesystem::rename(oldFile, oldFile.parent_path() / "replaced.hkx");
		makeAsset("actors/character/animations/pack/new.hkx", 48);
		{
			AnimationPreload::DirectoryIndex partial(meshes, indexFile);
			unsigned remaining = 1;
			bool fullListing{true};
			(void)partial.Read(meshes / "actors/character/animations/pack", Store::Clock::now() + 5s,
				cancelled, remaining, &fullListing);
			partial.Save(true, Store::Clock::now() + 5s, cancelled);
			require(remaining == 0 && !fullListing && !partial.saved, "A partial read accepted pruning of unchecked index entries");
		}
		const auto refreshed = scanIndexed();
		require(refreshed.scannedDirectories == 2 && refreshed.indexedDirectories + 2 == firstScan.scannedDirectories &&
			refreshed.indexSaved && refreshed.behaviorFiles == 1 && refreshed.animationFiles == 3 && refreshed.bytes == 400,
			"Changed HKX folders did not invalidate just their directory listings");
		require(scanIndexed().scannedDirectories == 0, "The refreshed index did not persist");
		std::filesystem::remove(oldFile.parent_path() / "replaced.hkx");
		std::filesystem::remove(oldFile.parent_path());
		const auto deleted = scanIndexed();
		require(deleted.indexSaved && deleted.animationFiles == 2 && deleted.bytes == 336,
			"Deleting a directory did not remove its HKX from the index");

		{
			std::ofstream damaged(indexFile, std::ios::binary | std::ios::app);
			damaged.put('x');
		}
		const auto repaired = scanIndexed();
		require(repaired.indexSaved && repaired.indexedDirectories == 0 && repaired.scannedDirectories > 0 &&
			repaired.bytes == deleted.bytes, "Corrupt index did not rebuild from the current deployment");
		require(scanIndexed().scannedDirectories == 0, "Rebuilt HKX index was not reusable");

		std::string deepRelative = "actors/character/animations";
		for (unsigned depth = 0; depth < 26; ++depth) deepRelative += "/d";
		const auto deepFolder = meshes / deepRelative;
		makeAsset((deepRelative + "/deep.hkx").c_str(), 16);
		const auto deadline = Store::Clock::now() + 5s;
		{
			AnimationPreload::DirectoryIndex seedDeep(meshes, indexFile);
			unsigned remaining = 8;
			const auto seeded = seedDeep.Read(deepFolder, deadline, cancelled, remaining);
			seedDeep.Save(false, deadline, cancelled);
			require(seeded && seeded->size() == 1 && seedDeep.saved, "Could not seed a deeper existing HKX index entry");
		}
		const auto limited = scanIndexed();
		require(limited.catalogLimited && limited.bytes == deleted.bytes, "The bounded catalog did not report its depth limit");
		{
			AnimationPreload::DirectoryIndex verifyDeep(meshes, indexFile);
			unsigned remaining = 8;
			const auto deep = verifyDeep.Read(deepFolder, deadline, cancelled, remaining);
			require(deep && deep->size() == 1 && verifyDeep.reused == 1, "A depth-limited catalog purged an unvisited listing");
		}
		std::filesystem::remove(deepFolder / "deep.hkx");

		const std::array firstPersonAssets{
			"actors/character/_1stperson/firstperson.hkx",
			"actors/character/_1stperson/character assets/skeleton.hkx",
			"actors/character/_1stperson/animations/attack.hkx",
			"actors/character/character assets female/skeleton_female.hkx"
		};
		for (const auto* asset : firstPersonAssets) makeAsset(asset, 17);
		const auto additional = scanIndexed(1024);
		require(additional.behaviorFiles == deleted.behaviorFiles + 3 && additional.animationFiles == deleted.animationFiles + 1 &&
			additional.bytes == deleted.bytes + firstPersonAssets.size() * 17 && additional.indexSaved,
			"First-person project files, skeletons or clips were skipped");
		const auto repeated = scanIndexed(1024);
		require(repeated.scannedDirectories == 0 && repeated.bytes == additional.bytes,
			"First-person files and skeletons did not reuse the persistent directory index");
		const auto cancelledAfterRead = AnimationPreload::Warm(meshes, 1024, Store::Clock::now() + 5s, cancelled,
			[](bool behavior, unsigned checked, unsigned) noexcept {
				if (behavior && checked == 1) cancelled.store(true);
			}, indexFile);
		require(cancelledAfterRead.interrupted && cancelledAfterRead.checked == 1 && cancelledAfterRead.behaviorFiles == 1 &&
			cancelledAfterRead.animationFiles == 0 && !cancelledAfterRead.indexSaved,
			"A startup cancellation between HKX reads continued the batch or published the index");
		cancelled.store(false);
		for (const auto* asset : firstPersonAssets) std::filesystem::remove(meshes / asset);
		std::filesystem::remove(meshes / "actors/character/animations/pack/new.hkx");

		constexpr auto nestedOar = "actors/character/animations/openanimationreplacer/pack/priority.hkx";
		makeAsset(nestedOar, 17);
		const auto loosePriority = AnimationPreload::Warm(meshes, 300, Store::Clock::now() + 5s, cancelled, nullptr);
		require(loosePriority.behaviorFiles == 1 && loosePriority.animationFiles == 1 &&
			loosePriority.bytes == 209 && loosePriority.selected == 3, "Nested OAR layout was missed or counted twice");
		std::filesystem::remove(meshes / nestedOar);
	}
	cancelled.store(true);
	const auto stoppedAssets = AnimationPreload::Warm(meshes, 512, Store::Clock::now() + 5s, cancelled, nullptr);
	require(stoppedAssets.interrupted && stoppedAssets.bytes == 0, "File preload read data after the main menu signal");
	for (const auto* asset : {"actors/character/behaviors/runtime.hkx", "actors/character/animations/pack/attacks.HKX",
		"OpenAnimationReplacer/pack/idle.hkx", "actors/character/animations/optional.txt", "clutter/ignored.hkx"}) {
		std::filesystem::remove(meshes / asset);
	}
	std::ranges::sort(assetFolders, [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
	for (const auto& folder : assetFolders) std::filesystem::remove(folder);

	for (const auto& file : std::filesystem::directory_iterator(directory)) std::filesystem::remove(file.path());
	require(std::filesystem::remove(directory), "Cache test directory could not be cleaned up");
	std::cout << checks << " graph-policy checks passed\n";
}
