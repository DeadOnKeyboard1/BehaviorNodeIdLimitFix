#include "GraphSlimming.h"
#include "GraphLifecycle.h"
#include "TransitionMap.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <thread>

namespace
{
	InstanceTopology::Trace MakeTrace(std::uint32_t nodes, bool edited = false)
	{
		InstanceTopology::Trace trace(true);
		auto root = trace.RecordChildren(nodes);
		root[0] = GraphPlan::noNode;
		for (std::uint32_t i = 1; i < nodes; ++i) root[i] = i;
		for (std::uint32_t i = 1; i < nodes; ++i) {
			if (i == 1) trace.RecordChildren(1).front() = edited ? 1 : 0;
			else (void)trace.RecordChildren(0);
		}
		return trace;
	}
}

int main()
{
	std::size_t checks{};
	const auto require = [&](bool passed, const char* message) {
		++checks;
		if (!passed) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	};
	std::istringstream vanillaIni;
	const auto defaults = GraphSlimming::ReadSettings(vanillaIni);
	require(defaults.Optimize() && defaults.SharePlans() && defaults.instanceFastPath && defaults.persistentCache &&
		defaults.performanceLogging && !defaults.experimental, "Default settings differ from the INI");
	std::istringstream changedIni(
		"\xef\xbb\xbf[NodeOptimization]\r\n bEnableNodeOptimization = FALSE ; disable new features\n"
		"bEnableInstanceFastPath = 0\n bEnablePersistentOptimizationCache=0\n bEnablePerformanceLogging=0\n"
		"bEnableExperimentalOptimizations=true\n bEnableSafeDeduplication=typo\n [Other]\n bEnableInstanceFastPath=1\n");
	const auto disabled = GraphSlimming::ReadSettings(changedIni);
	require(!disabled.Optimize() && !disabled.SharePlans() && !disabled.instanceFastPath && !disabled.persistentCache &&
		!disabled.performanceLogging && disabled.experimental && disabled.safeDeduplication, "INI parsing changed a setting unexpectedly");
	std::istringstream independentIni("[NodeOptimization]\nbEnableSafeDeduplication=0\nbEnableGraphOptimization=0\n");
	const auto independent = GraphSlimming::ReadSettings(independentIni);
	require(independent.enabled && !independent.SharePlans() && !independent.Optimize() && independent.instanceFastPath &&
		independent.persistentCache, "Independent switches disabled the baseline graph caches");
	require(defaults.adaptivePreloading && defaults.showPreloadProgress && !defaults.preloadAnimationFiles &&
		defaults.preloadMaxPlans == 128 &&
		defaults.preloadAssetMaxMiB == 4096, "Default startup settings differ from the INI");
	std::istringstream preloadIni("[NodeOptimization]\nbEnableAdaptivePreloading=0\nbShowPreloadProgress=false\n"
		"bPreloadAnimationFiles=true\niPreloadAssetMaxMiB=9000\n"
		"iPreloadMaxPlans=1000\niPreloadBudgetMs=1\niPreloadBudgetMs=abc\niPreloadMaxPlans=\n");
	const auto preload = GraphSlimming::ReadSettings(preloadIni);
	require(!preload.adaptivePreloading && !preload.showPreloadProgress && preload.preloadAnimationFiles &&
		preload.preloadMaxPlans == 256 && preload.preloadAssetMaxMiB == 8192,
		"Startup INI values did not honor switches and bounds");

	GraphPlan::Signature expected, folded;
	for (const auto word : {0u, 1u, 0u, GraphPlan::noNode, 0u, 0u, 9001u}) {
		expected.Add(word);
		if (word == 0) folded.AddEmptyNode();
		else folded.Add(word);
	}
	require(expected.value == folded.value, "Folding leaf signatures invalidates old persistent keys");
	for (const auto run : {0u, 1u, 2u, 7u, 32u, 65535u, 65536u}) {
		auto sequential = expected;
		auto grouped = expected;
		for (unsigned i = 0; i < run; ++i) sequential.Add(0);
		grouped.AddEmptyNodes(run);
		require(sequential.value == grouped.value, "A grouped leaf run changed the topology fingerprint");
	}
	auto small = MakeTrace(64);
	InstanceTopology::Trace batched(true);
	auto batchedRoot = batched.RecordChildren(64);
	batchedRoot[0] = GraphPlan::noNode;
	for (std::uint32_t i = 1; i < 64; ++i) batchedRoot[i] = i;
	batched.RecordChildren(1)[0] = 0;
	batched.RecordEmptyNodes(62);
	require(batched.SameLinks(small), "Batching consecutive leaves changed discovery order");
	batched.RecordEmptyNodes((std::numeric_limits<std::size_t>::max)());
	require(!batched.Complete(64) && !batched.CapacityBytes(), "An oversized leaf run wrapped or retained a partial trace");
	batched.Reset();
	batched.RecordEmptyNodes(1);
	require(batched.Complete(1), "A discarded batched trace could not be reused");
	auto packedSmall = small;
	require(packedSmall.PackOrdinals() && small.SameLinks(packedSmall) && packedSmall.SameLinks(small),
		"Small graph changed meaning after 16-bit packing");
	require(!packedSmall.SameLinks(MakeTrace(64, true)), "A changed small graph matched a packed record");
	GraphPlan::Signature portableShape;
	portableShape.Add(64);
	portableShape.Add(GraphPlan::noNode);
	for (unsigned i = 1; i < 64; ++i) portableShape.Add(i);
	portableShape.Add(1);
	portableShape.Add(0);
	for (unsigned i = 2; i < 64; ++i) portableShape.Add(0);
	const GraphPlan::Entry portablePlan{portableShape.value, 64, 0, {1, 3}};
	std::ostringstream portableOutput(std::ios::binary);
	require(packedSmall.WritePortable(portableOutput, portablePlan), "Packed topology could not be written to a portable file");
	{
		auto differentPlan = portablePlan;
		differentPlan.topology ^= 1;
		std::ostringstream wrongOutput;
		require(!packedSmall.WritePortable(wrongOutput, differentPlan) && wrongOutput.str().empty(),
			"A topology was serialized for the wrong plan");
		InstanceTopology::Trace disconnected(true);
		GraphPlan::Signature disconnectedSignature;
		disconnectedSignature.AddEmptyNodes(portablePlan.nodes);
		differentPlan.topology = disconnectedSignature.value;
		for (unsigned i = 0; i < portablePlan.nodes; ++i) disconnected.RecordEmptyNode();
		require(!disconnected.WritePortable(wrongOutput, differentPlan) && wrongOutput.str().empty(),
			"Matching fingerprints let unreachable nodes into the cache");
	}
	const auto cachedTopology = portableOutput.str();
	std::istringstream portableInput(cachedTopology, std::ios::binary);
	const auto reloaded = InstanceTopology::Trace::ReadPortable(portableInput, cachedTopology.size(), portablePlan);
	require(reloaded && reloaded->SameLinks(packedSmall), "Saved topology did not survive restart");
	auto extraLeafRow = cachedTopology;
	std::uint32_t storedRows{}, edgeCount{};
	std::memcpy(&storedRows, extraLeafRow.data() + 2 * sizeof(std::uint32_t), sizeof(storedRows));
	std::memcpy(&edgeCount, extraLeafRow.data() + 3 * sizeof(std::uint32_t), sizeof(edgeCount));
	const std::uint32_t leafRow[]{2, edgeCount};
	extraLeafRow.insert((5 + storedRows * 2) * sizeof(std::uint32_t), reinterpret_cast<const char*>(leafRow), sizeof(leafRow));
	++storedRows;
	std::memcpy(extraLeafRow.data() + 2 * sizeof(std::uint32_t), &storedRows, sizeof(storedRows));
	std::istringstream redundantLeaf(extraLeafRow, std::ios::binary);
	require(!InstanceTopology::Trace::ReadPortable(redundantLeaf, extraLeafRow.size(), portablePlan),
		"A redundant empty row was accepted as a canonical cache entry");
	{
		auto rawSource = small;
		auto packedSource = packedSmall;
		InstanceTopology::Trace moved(std::move(rawSource));
		require(moved.SameLinks(small) && !rawSource.Complete(64) && !rawSource.SameLinks(small),
			"A moved-from raw trace remained valid");
		moved = std::move(packedSource);
		std::ostringstream stale;
		require(moved.SameLinks(packedSmall) && !packedSource.IsPacked() &&
			!packedSource.PackOrdinals() && !packedSource.WritePortable(stale, portablePlan),
			"A moved-from packed trace could still be packed or serialized");
		packedSource.Reset();
		packedSource.RecordEmptyNode();
		require(packedSource.Complete(1), "A moved-from trace could not be reused as a fresh capture");
	}
	auto corruptedTopology = cachedTopology;
	corruptedTopology.back() ^= 1;
	std::istringstream damagedTopology(corruptedTopology, std::ios::binary);
	require(!InstanceTopology::Trace::ReadPortable(damagedTopology, corruptedTopology.size(), portablePlan),
		"Invalid on-disk edges passed topology verification");
	std::istringstream shortenedTopology(cachedTopology.substr(0, cachedTopology.size() - 1), std::ios::binary);
	require(!InstanceTopology::Trace::ReadPortable(shortenedTopology, cachedTopology.size() - 1, portablePlan),
		"A partial topology file validated");
	std::array<unsigned, 64> nativeNodes{};
	std::vector<unsigned*> resolved;
	std::size_t checked{};
	const auto replay = [&](bool alias, bool interrupt = false) {
		resolved.clear();
		return reloaded->Replay(&nativeNodes[0], resolved, [&](auto* node, std::size_t expected) {
			if (std::ranges::find(resolved, node) != resolved.end() || expected != resolved.size()) return false;
			resolved.push_back(node);
			return true;
		}, [&](auto* node, std::size_t i, auto check) {
			if (interrupt && i == 17) throw std::bad_alloc{};
			return node == &nativeNodes[i] && check(i == 0 ? 64u : (i == 1 ? 1u : 0u), [&](auto edge) -> unsigned* {
				if (i == 1) return &nativeNodes[0];
				if (edge == 0) return nullptr;
				return &nativeNodes[alias && edge == 2 ? 1 : edge];
			});
		}, &checked);
	};
	require(replay(false) && resolved.size() == nativeNodes.size() && checked == nativeNodes.size(),
		"Saved edges failed to resolve freshly loaded nodes");
	require(!replay(true) && checked == 0 && resolved.size() == 2,
		"Replay accepted aliases or dropped valid discoveries before a mismatch inside a row");
	bool interrupted{};
	try {
		(void)replay(false, true);
	} catch (const std::bad_alloc&) {
		interrupted = true;
	}
	require(interrupted && checked == 17 && resolved.size() == nativeNodes.size(),
		"Interrupted replay lost its last confirmed row or its discovered nodes");
	packedSmall.RecordEmptyNode();
	require(packedSmall.Complete(64), "A sealed trace accepted an extra leaf");
	packedSmall.Reset();
	packedSmall.RecordEmptyNode();
	require(!packedSmall.IsPacked() && packedSmall.Complete(1), "Reset did not clear packed capture state");
	small = MakeTrace(64);
	require(small.CapacityBytes() != 0 &&
		small.RecordChildren(InstanceTopology::instanceBytes / sizeof(std::uint32_t)).empty() &&
		!small.Complete(65) && small.CapacityBytes() == 0, "An abandoned capture retained its large buffers");

	GraphSlimming::Engine engine;
	for (const auto count : {40'000u, 65'535u, 65'536u, 140'000u}) {
		const GraphPlan::Shape shape{4242u, 73u, count};
		auto plan = std::make_shared<const GraphPlan::Entry>(GraphPlan::Entry{shape.topology, count, 30, {1, count - 1}});
		auto a = engine.Prepare(shape, MakeTrace(count), plan, defaults);
		require(a.payload && a.packed && a.bytesAfter < a.bytesBefore && !a.shared, "Initial capture was not slimmed");
		auto b = engine.Prepare(shape, MakeTrace(count), plan, defaults);
		require(b.shared && b.payload == a.payload, "Exactly equal immutable graph data was not shared");
		auto changed = engine.Prepare(shape, MakeTrace(count, true), plan, defaults);
		require(changed.payload && !changed.shared && changed.payload != a.payload,
			"A colliding topology key shared a different child or transition");
		if (count == 40'000u) {
			const auto sameKey = engine.Prepare(shape, MakeTrace(count), plan, defaults);
			require(sameKey.shared && sameKey.payload == a.payload,
				"An exact candidate was lost when a different graph used the same key");
		}
		auto alternatePlan = std::make_shared<const GraphPlan::Entry>(GraphPlan::Entry{shape.topology, count, 30, {2, count - 1}});
		auto changedTypes = engine.Prepare(shape, MakeTrace(count, true), alternatePlan, defaults);
		require(changedTypes.payload && !changedTypes.shared && changedTypes.payload != changed.payload,
			"Different state-machine ordinals reused an immutable plan");
		require(!engine.Prepare(shape, MakeTrace(count - 1), plan, defaults).payload,
			"A partial snapshot was admitted");
		auto off = engine.Prepare(shape, MakeTrace(count), plan, disabled);
		require(off.payload && !off.packed && !off.shared && off.bytesAfter == off.bytesBefore,
			"Disabling slimming still transformed a snapshot");
		require(a.payload->links.SameLinks(off.payload->links) && off.payload->links.SameLinks(a.payload->links),
			"Comparing raw and packed links changed their meaning");

		struct Node { std::uint32_t ordinal; float time; };
		std::vector<Node> firstActor, secondActor;
		firstActor.reserve(count);
		secondActor.reserve(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			firstActor.push_back({i, 1.0f});
			secondActor.push_back({i, 3.0f});
		}
		std::vector<Node*> live;
		live.reserve(count);
		for (auto& node : secondActor) live.push_back(&node);
		bool addTransition{};
		const auto inspect = [&](Node* node, std::size_t ordinal, auto check) {
			if (node->ordinal != ordinal) return false;
			const auto children = ordinal == 0 ? count : (ordinal == 1 ? 1u : 0u);
			return check(children, [&](std::size_t i) -> Node* {
				if (ordinal == 0) return i == 0 ? nullptr : live[i];
				return addTransition ? live[1] : live[0];
			});
		};
		const auto matches = [&] {
			return a.payload->links.Matches(live.front(), std::span<Node* const>{live}, inspect);
		};
		const auto external = std::make_shared<Node>(Node{31, 42.0f});
		require(matches() && firstActor[1].time == 1.0f && secondActor[1].time == 3.0f && external->time == 42.0f,
			"Cloned or external fixture state changed while validating shared immutable data");
		addTransition = true;
		require(!matches(), "A modified live transition reused a shared pack");
		addTransition = false;
		require(matches(), "Recreating the original state could not validate the original topology");
		if (count == 65'536u) {
			std::vector<Node*> replayed;
			replayed.reserve(count);
			std::size_t verified{};
			const auto replayTrace = [&](const InstanceTopology::Trace& graph) {
				replayed.clear();
				return graph.Replay(live.front(), replayed, [&](Node* node, std::size_t expectedOrdinal) {
					if (expectedOrdinal >= live.size() || expectedOrdinal != replayed.size() ||
						live[expectedOrdinal] != node) return false;
					replayed.push_back(node);
					return true;
				}, inspect, &verified);
			};
			require(replayTrace(a.payload->links) && replayed == live &&
				replayTrace(off.payload->links) && replayed == live,
				"Raw or 24-bit replay lost the last ordinal or confused it with a null edge");
			addTransition = true;
			require(!replayTrace(a.payload->links) && verified == 1 && replayed == live &&
				!replayTrace(off.payload->links) && verified == 1 && replayed == live,
				"Replay lost the discovered pointers or the verified prefix after a changed edge");
			InstanceTopology::Trace resumed(true);
			GraphPlan::Signature signature;
			a.payload->links.CopyPrefix(verified, resumed, signature);
			signature.Add(1);
			signature.Add(1);
			resumed.RecordChildren(1)[0] = 1;
			signature.AddEmptyNodes(count - 2);
			for (std::uint32_t i = 2; i < count; ++i) resumed.RecordEmptyNode();
			require(resumed.Complete(count) && !resumed.SameLinks(off.payload->links) &&
				signature.value != shape.topology, "Resuming from the confirmed prefix reused a changed topology");
		}
		a.payload.reset();
		require(b.payload->plan->stateMachines.front() == 1, "One actor invalidated another actor's plan lifetime");
	}
	const GraphPlan::Shape concurrentShape{7734, 82, 40'000};
	auto concurrentPlan = std::make_shared<const GraphPlan::Entry>(
		GraphPlan::Entry{concurrentShape.topology, concurrentShape.nodes, 0, {20, 6000}});
	GraphSlimming::Engine concurrentEngine;
	constexpr std::size_t workers = 8;
	std::array<GraphSlimming::Preparation, workers> captures;
	std::array<std::jthread, workers> threads;
	std::barrier allReady(static_cast<std::ptrdiff_t>(workers));
	for (std::size_t i = 0; i < workers; ++i) {
		threads[i] = std::jthread([&, i] {
			auto candidate = MakeTrace(concurrentShape.nodes);
			allReady.arrive_and_wait();
			captures[i] = concurrentEngine.Prepare(concurrentShape, std::move(candidate), concurrentPlan, defaults);
		});
	}
	for (auto& thread : threads) thread.join();
	require(captures.front().payload && std::all_of(captures.begin(), captures.end(), [&](const auto& capture) {
		return capture.payload == captures.front().payload;
	}), "Concurrent identical capture retained duplicate payloads");
	auto rawOnly = defaults;
	rawOnly.graphOptimization = false;
	auto raw = concurrentEngine.Prepare(concurrentShape, MakeTrace(concurrentShape.nodes), concurrentPlan, rawOnly);
	require(raw.payload && !raw.packed && raw.payload != captures.front().payload,
		"Disabled packing reused a packed payload");
	const auto packedAgain = concurrentEngine.Prepare(concurrentShape, MakeTrace(concurrentShape.nodes), concurrentPlan, defaults);
	require(packedAgain.shared && packedAgain.payload == captures.front().payload,
		"A raw candidate displaced a live packed candidate");

	GraphSlimming::Engine collidingEngine;
	auto otherShape = concurrentShape;
	otherShape.topology ^= 0x20u;
	require(concurrentShape.CacheVariant() != otherShape.CacheVariant() &&
		concurrentShape.CacheVariant() % 32 == otherShape.CacheVariant() % 32,
		"Collision fixtures do not share an index bucket");
	auto otherPlan = std::make_shared<const GraphPlan::Entry>(
		GraphPlan::Entry{otherShape.topology, otherShape.nodes, 0, {20, 6000}});
	const auto firstPayload = collidingEngine.Prepare(concurrentShape, MakeTrace(concurrentShape.nodes), concurrentPlan, defaults);
	const auto otherPayload = collidingEngine.Prepare(otherShape, MakeTrace(otherShape.nodes, true), otherPlan, defaults);
	const auto firstAgain = collidingEngine.Prepare(concurrentShape, MakeTrace(concurrentShape.nodes), concurrentPlan, defaults);
	const auto otherAgain = collidingEngine.Prepare(otherShape, MakeTrace(otherShape.nodes, true), otherPlan, defaults);
	require(firstAgain.shared && firstAgain.payload == firstPayload.payload &&
		otherAgain.shared && otherAgain.payload == otherPayload.payload,
		"Alternating live graphs in one bucket repacked their payloads");

	InstanceTopology::Cache<int, int, 3> budget(100);
	const auto firstOwner = std::make_shared<int>(1);
	const auto secondOwner = std::make_shared<int>(2);
	const auto independentOwner = std::make_shared<int>(3);
	const InstanceTopology::Cache<int, int, 3>::SharedStorage shared{firstOwner.get(), 60};
	require(budget.Remember(1, firstOwner, 10, shared) && budget.Remember(2, secondOwner, 10, shared) &&
		budget.IsCurrent(1, firstOwner.get()) && budget.IsCurrent(2, secondOwner.get()),
		"Shared storage was charged twice");
	require(!budget.Remember(3, independentOwner, 10, {firstOwner.get(), 59}),
		"A conflicting byte count changed shared storage accounting");
	budget.Invalidate(1);
	require(budget.Remember(3, independentOwner, 30) && budget.IsCurrent(2, secondOwner.get()),
		"Removing one owner did not preserve the other");
	budget.Invalidate(2);
	require(budget.Remember(4, independentOwner, 70) && budget.IsCurrent(3, independentOwner.get()),
		"The last owner did not release the shared byte charge");
	{
		struct Capture { unsigned transition; };
		using AliasCache = InstanceTopology::Cache<int, Capture, 3>;
		AliasCache aliases(100);
		const auto transition = std::make_shared<unsigned>(11);
		const auto unrelated = std::make_shared<const Capture>(Capture{22});
		const AliasCache::SharedStorage sharedData{transition.get(), 60};
		require(aliases.Remember(1, std::make_shared<const Capture>(Capture{*transition}), 10, sharedData) &&
			aliases.Remember(2, std::make_shared<const Capture>(Capture{*transition}), 10, sharedData) &&
			aliases.Remember(3, unrelated, 10), "Could not seed graph snapshots with a shared transition");
		const std::weak_ptr<const Capture> firstAlias = aliases.Find(1);
		auto extracted = aliases.ExtractIf([&](int key, const Capture& captured) {
			return key == 1 || captured.transition == *transition;
		});
		require(!aliases.Find(1) && !aliases.Find(2) && aliases.Find(3) == unrelated && !firstAlias.expired(),
			"Shared-transition invalidation missed an alias or destroyed a snapshot before release");
		extracted = {};
		require(firstAlias.expired() && aliases.Remember(4, unrelated, 90) && aliases.Find(3) == unrelated,
			"Removing all aliases did not release their capacity");
	}
	{
		InstanceTopology::Cache<int, int, 1> eviction(100);
		auto old = std::make_shared<const int>(7);
		const std::weak_ptr<const int> retired = old;
		require(eviction.Remember(1, std::move(old), 10), "Could not seed deferred cache eviction");
		auto admission = eviction.Admit(2, std::make_shared<const int>(9), 10);
		require(admission.stored && !eviction.Find(1) && eviction.Find(2) && !retired.expired(),
			"Deferred admission released an evicted object before the caller's lock could be released");
		admission.retired = {};
		require(retired.expired(), "A replaced instance was retained after retirement");
	}

	std::array<TransitionMap::Bucket, 8> buckets;
	buckets.fill({TransitionMap::emptyKey, 0});
	buckets[0] = {0, 100};
	buckets[1] = {1, 200};
	const TransitionMap::Table table{buckets.data(), 2, 7};
	const auto lookup = [](auto id) { return (id + 1) * 100; };
	require(TransitionMap::Matches(table, 8, 2, true, lookup), "A verified live transition map could not be reused");
	buckets[1].val = 300;
	require(!TransitionMap::Matches(table, 8, 2, true, lookup), "A map entry for another actor was accepted");
	buckets[2] = {1, 200};
	buckets[1] = {TransitionMap::emptyKey, 0};
	require(!TransitionMap::Matches(table, 8, 2, true, lookup), "A map with an unreachable key was accepted");
	buckets[1] = {0, 100};
	require(!TransitionMap::Matches(table, 8, 2, true, lookup), "A map with duplicate IDs was accepted");

	GraphLifecycle::Analyses analyses;
	std::mutex commitLock;
	const auto* firstGraph = reinterpret_cast<const void*>(std::uintptr_t{0x1000});
	const auto* secondGraph = reinterpret_cast<const void*>(std::uintptr_t{0x1010});
	const auto* transitions = reinterpret_cast<const void*>(std::uintptr_t{0x2000});
	GraphLifecycle::Analyses::Ticket secondAnalysis(analyses, commitLock);
	GraphLifecycle::Analyses::Ticket latestAnalysis(analyses, commitLock);
	{
		GraphLifecycle::Analyses::Ticket firstAnalysis(analyses, commitLock);
		{
			std::scoped_lock lock(commitLock);
			require(firstAnalysis.TryBegin(firstGraph, transitions) && secondAnalysis.TryBegin(secondGraph),
				"Independent graph analyses were blocked");
			require(!latestAnalysis.TryBegin(firstGraph), "Duplicate analysis of a graph was admitted");
		}
		{
			std::jthread relink([&] {
				std::scoped_lock lock(commitLock);
				// A native relink with shared transition data must invalidate this analysis too.
				analyses.Invalidate(nullptr, transitions);
			});
		}
		require(!firstAnalysis.IsCurrent(), "Old snapshot remained current after native reinitialization");
		require(secondAnalysis.IsCurrent(), "Native graph invalidated an unrelated analysis");
		{
			std::scoped_lock lock(commitLock);
			require(!latestAnalysis.TryBegin(firstGraph) &&
				!latestAnalysis.TryBegin(reinterpret_cast<const void*>(std::uintptr_t{0x1020}), transitions),
				"A new reader overlapped an invalidated but unfinished reader of the same graph or map");
		}
	}
	{
		std::scoped_lock lock(commitLock);
		require(latestAnalysis.TryBegin(firstGraph, transitions) && latestAnalysis.IsCurrent(),
			"A drained graph analysis kept the address reserved");
	}
	std::cout << checks << " slimming checks passed\n";
}
