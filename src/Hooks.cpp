#include "Hooks.h"
#include "GraphPerformance.h"
#include "GraphCache.h"
#include "GraphLifecycle.h"
#include "GraphSlimming.h"
#include "InstanceTopology.h"
#include "TransitionMap.h"

#include "REX/W32/KERNEL32.h"
#include <hde64.h>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <format>
#include <mutex>
#include <new>
#include <utility>

namespace
{
	using hkMapBase = TransitionMap::Table;

	struct NodeChildInfo
	{
		RE::hkbNode* node;
		void*        nodeInfo;
	};
	static_assert(sizeof(NodeChildInfo) == 0x10);

	struct RawHkArray
	{
		NodeChildInfo* data;
		std::int32_t   size;
		std::int32_t   capacityAndFlags;
	};
	static_assert(sizeof(RawHkArray) == 0x10);

	struct NodeChildrenInfo
	{
		RawHkArray*   children;
		std::uint64_t flags;
	};
	static_assert(sizeof(NodeChildrenInfo) == 0x10);

	// Verified Havok / BShkb offsets across Skyrim SE 1.5.97 / AE 1.6.1170 / AE 1.7.104
	static_assert(offsetof(RE::hkbNode, id) == 0x40);
	static_assert(offsetof(RE::hkbBehaviorGraph, rootGenerator) == 0x80);
	static_assert(offsetof(RE::hkbBehaviorGraph, globalTransitionData) == 0xB0);
	static_assert(offsetof(RE::hkbBehaviorGraph, activeNodes) == 0x98);
	static_assert(offsetof(RE::hkbBehaviorGraph, numStaticNodes) == 0x128);
	static_assert(offsetof(RE::hkbBehaviorGraph, nextUniqueID) == 0x12A);
	static_assert(offsetof(RE::hkbBehaviorGraph, isActive) == 0x12C);
	static_assert(offsetof(RE::hkbBehaviorGraph, uniqueIDPool) == 0x50);
	static_assert(offsetof(RE::hkbCharacter, behaviorGraph) == 0x58);

	constexpr std::size_t   kGraphNodeThreshold = GraphPlan::nodeThreshold;
	constexpr auto kInvalidNodeId = GraphPlan::invalidId;
	constexpr std::uint32_t kDontDeallocateFlag = 0x80000000u;
	constexpr std::size_t   kGlobalTransitionMapOffset = 0x50;
	constexpr std::uint32_t kArrayCapacityMask = 0x3FFFFFFFu;
	constexpr std::int32_t  kMaxChildrenPerNode = 1'000'000;

	using StateMachineStaticClassFn = const RE::hkClass* (*)();
	using HkMapInsertFn = std::int32_t (*)(hkMapBase* a_map, void* a_alloc, std::uint64_t a_key, std::uint64_t a_val);
	using PreSetupBehaviorGraphFn = bool (*)(void* a_config, RE::hkbCharacter* a_character, void* a_callback);
	using InitNodeIdsFn = void (*)(RE::hkbBehaviorGraph* a_graph);

	const RE::hkClass*      g_stateMachineClass = nullptr;
	const TransitionMap::RuntimeProfile* g_transitionMapProfile = nullptr;
	HkMapInsertFn           g_hkMapInsert = nullptr;
	RE::hkContainerHeapAllocator::Allocator* g_heapAllocator = nullptr;
	PreSetupBehaviorGraphFn g_origPreSetup = nullptr;
	InitNodeIdsFn           g_origInitNodeIds = nullptr;
	std::mutex g_compactionMutex;

	[[nodiscard]] std::uintptr_t DecodeCallRel32(std::uintptr_t a_callSite) noexcept
	{
		std::int32_t rel{};
		std::memcpy(&rel, reinterpret_cast<const void*>(a_callSite + 1), sizeof(rel));
		const auto nextIp = static_cast<std::intptr_t>(a_callSite) + 5;
		return static_cast<std::uintptr_t>(nextIp + static_cast<std::ptrdiff_t>(rel));
	}

	[[nodiscard]] bool BytesEqual(const std::uint8_t* a_ptr, std::initializer_list<std::uint8_t> a_pattern) noexcept
	{
		std::size_t idx = 0;
		for (const std::uint8_t b : a_pattern) {
			if (a_ptr[idx++] != b) {
				return false;
			}
		}
		return true;
	}

	struct CallBoundaries
	{
		static constexpr std::size_t kMaxBytes = 0x120;
		std::bitset<kMaxBytes> calls;
		std::size_t size{};
	};

	[[nodiscard]] CallBoundaries DecodeCalls(const std::uint8_t* a_bytes, std::size_t a_maxBytes) noexcept
	{
		CallBoundaries result;
		while (result.size < a_maxBytes) {
			const auto offset = result.size;
			const auto* instruction = a_bytes + offset;
			const auto remaining = a_maxBytes - offset;
			if (remaining >= 4 && BytesEqual(instruction, { 0xCC, 0xCC, 0xCC, 0xCC })) break;
			// HDE64 reads up to 15 bytes. Give it a window that ends at a
			// validated memory boundary even at the end of the scan.
			std::array<std::uint8_t, 16> window{};
			std::memcpy(window.data(), instruction, std::min<std::size_t>(15, remaining));
			hde64s decoded{};
			const auto length = hde64_disasm(window.data(), &decoded);
			if (length == 0 || length > remaining || (decoded.flags & F_ERROR)) break;
			if (length == 5 && instruction[0] == 0xE8) result.calls.set(offset);
			result.size += length;
			if (instruction[0] == 0xC3 || instruction[0] == 0xC2) {
				break;
			}
		}
		return result;
	}

	[[nodiscard]] std::size_t ExecutableBytes(std::uintptr_t a_addr, bool a_gameTextOnly = false) noexcept
	{
		REX::W32::MEMORY_BASIC_INFORMATION mbi{};
		if (!a_addr || REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_addr), &mbi, sizeof(mbi)) == 0) return 0;
		constexpr std::uint32_t kMemCommit = 0x00001000u;
		constexpr std::uint32_t kPageNoAccess = 0x00000001u;
		constexpr std::uint32_t kPageExecuteRead = 0x00000020u;
		constexpr std::uint32_t kPageExecuteReadWrite = 0x00000040u;
		constexpr std::uint32_t kPageExecuteWriteCopy = 0x00000080u;
		constexpr std::uint32_t kPageGuard = 0x00000100u;
		constexpr std::uint32_t kExecuteMask =
			kPageExecuteRead |
			kPageExecuteReadWrite |
			kPageExecuteWriteCopy;

		if (mbi.state != kMemCommit || (mbi.protect & (kPageNoAccess | kPageGuard)) || !(mbi.protect & kExecuteMask)) return 0;
		const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.baseAddress);
		if (a_addr < regionBase || a_addr - regionBase >= mbi.regionSize) return 0;
		const auto remaining = mbi.regionSize - (a_addr - regionBase);
		if (!a_gameTextOnly) return remaining;
		const auto text = REL::Module::get().segment(REL::Segment::textx);
		const auto textStart = text.address();
		if (a_addr < textStart || a_addr - textStart >= text.size()) return 0;
		return (std::min)(remaining, text.size() - (a_addr - textStart));
	}

	[[nodiscard]] bool IsExecutableTextAddress(std::uintptr_t a_addr) noexcept { return ExecutableBytes(a_addr, true) != 0; }
	[[nodiscard]] bool IsLiveExecutableAddress(std::uintptr_t a_addr) noexcept { return ExecutableBytes(a_addr) != 0; }

	[[nodiscard]] bool HasExactSeHook1Epilogue(const std::uint8_t* a_bytes, std::size_t a_off, std::size_t a_scanSize) noexcept
	{
		if (a_off + 10 > a_scanSize) {
			return false;
		}
		const auto* tail = a_bytes + a_off + 5;
		const bool  hasNopEpilogue = (a_off + 11 <= a_scanSize) && BytesEqual(tail, { 0x90, 0x48, 0x83, 0xC4, 0x48, 0xC3 });
		const bool  hasDirectEpilogue = BytesEqual(tail, { 0x48, 0x83, 0xC4, 0x48, 0xC3 });
		return hasNopEpilogue || hasDirectEpilogue;
	}

	[[nodiscard]] std::optional<std::uintptr_t> FindPreSetupCallSite()
	{
		const std::uintptr_t parentAddr = REL::RelocationID(64209, 65241).address();
		if (!IsExecutableTextAddress(parentAddr)) {
			logger::error("Hook 1 parent relocation (SE 64209 / AE 65241) invalid: 0x{:X}.", parentAddr);
			return std::nullopt;
		}

		const auto*           bytes = reinterpret_cast<const std::uint8_t*>(parentAddr);
		constexpr std::size_t kMaxParentSize = 80;
		const auto            decoded = DecodeCalls(bytes, (std::min)(kMaxParentSize, ExecutableBytes(parentAddr, true)));
		const auto            scanSize = decoded.size;
		constexpr std::size_t kPreferredOffset = 0x3F;

		if (REL::Module::IsAE()) {
			const std::uintptr_t expectedTarget = REL::ID(65232).address();
			if (!IsExecutableTextAddress(expectedTarget)) {
				logger::error("Hook 1 target REL::ID(65232) invalid: 0x{:X}.", expectedTarget);
				return std::nullopt;
			}

			if (decoded.calls[kPreferredOffset]) {
				const std::uintptr_t site = parentAddr + kPreferredOffset;
				if (DecodeCallRel32(site) == expectedTarget) {
					return site;
				}
			}

			std::uintptr_t matchedSite = 0;
			std::size_t    matchCount = 0;
			for (std::size_t off = 0; off + 5 <= scanSize; ++off) {
				if (decoded.calls[off]) {
					const std::uintptr_t site = parentAddr + off;
					if (DecodeCallRel32(site) == expectedTarget) {
						matchedSite = site;
						++matchCount;
					}
				}
			}
			if (matchCount == 1) {
				return matchedSite;
			}

			// Co-load compatibility fallback: chain if another SKSE plugin already wrote a valid trampoline call at +0x3F.
			if (matchCount == 0 && kPreferredOffset >= 5 && kPreferredOffset + 11 <= scanSize &&
				decoded.calls[kPreferredOffset] &&
				BytesEqual(bytes + kPreferredOffset - 5, { 0x48, 0x8D, 0x4C, 0x24, 0x28 }) &&
				BytesEqual(bytes + kPreferredOffset + 5, { 0x90, 0x48, 0x83, 0xC4, 0x48, 0xC3 })) {
				const std::uintptr_t site = parentAddr + kPreferredOffset;
				const std::uintptr_t chainedTarget = DecodeCallRel32(site);
				if (IsLiveExecutableAddress(chainedTarget)) {
					logger::info("Hook 1 (AE): call site +0x{:X} already patched (-> 0x{:X}); chaining existing hook.",
						kPreferredOffset, chainedTarget);
					return site;
				}
			}

			logger::error("Hook 1 (AE) failed to locate unique E8 call to REL::ID(65232) inside REL::ID(65241); matches={}.", matchCount);
			return std::nullopt;
		}

		// SE 1.5.97 (REL::ID 64209):
		// Pass 1: exact `(nop)? add rsp, 0x48; ret` epilogue targeting .text.
		// Pass 2: generic `(nop)? add rsp, imm8; ret` frame epilogue targeting .text.
		// Pass 3: exact epilogue targeting a live trampoline/executable page (hook chaining).
		std::uintptr_t matchedSite = 0;
		std::size_t    matchCount = 0;
		for (std::size_t off = 0; off + 10 <= scanSize; ++off) {
			if (!decoded.calls[off] || !HasExactSeHook1Epilogue(bytes, off, scanSize)) {
				continue;
			}
			if (IsExecutableTextAddress(DecodeCallRel32(parentAddr + off))) {
				matchedSite = parentAddr + off;
				++matchCount;
			}
		}

		if (matchCount == 1) {
			return matchedSite;
		}

		if (matchCount == 0) {
			for (std::size_t off = 0x20; off + 10 <= scanSize; ++off) {
				if (!decoded.calls[off]) {
					continue;
				}
				const auto* tail = bytes + off + 5;
				const auto  checkGenericEpilogue = [](const std::uint8_t* a_ep) noexcept {
					return a_ep[0] == 0x48 && a_ep[1] == 0x83 && a_ep[2] == 0xC4 &&
					       a_ep[3] >= 0x20 && a_ep[3] <= 0x78 && (a_ep[3] % 8u) == 0u &&
					       a_ep[4] == 0xC3;
				};
				const bool hasGeneric =
					checkGenericEpilogue(tail) ||
					((off + 11 <= scanSize) && tail[0] == 0x90 && checkGenericEpilogue(tail + 1));
				if (!hasGeneric) {
					continue;
				}
				if (IsExecutableTextAddress(DecodeCallRel32(parentAddr + off))) {
					matchedSite = parentAddr + off;
					++matchCount;
				}
			}
			if (matchCount == 1) {
				logger::info("Hook 1 (SE) matched generic tail-epilogue call site at +0x{:X}.",
					matchedSite - parentAddr);
				return matchedSite;
			}
		}

		if (matchCount == 0) {
			for (std::size_t off = 0; off + 10 <= scanSize; ++off) {
				if (!decoded.calls[off] || !HasExactSeHook1Epilogue(bytes, off, scanSize)) {
					continue;
				}
				const std::uintptr_t chainedTarget = DecodeCallRel32(parentAddr + off);
				if (IsLiveExecutableAddress(chainedTarget)) {
					matchedSite = parentAddr + off;
					++matchCount;
				}
			}
			if (matchCount == 1) {
				logger::info("Hook 1 (SE): call site +0x{:X} already patched (-> 0x{:X}); chaining existing hook.",
					matchedSite - parentAddr, DecodeCallRel32(matchedSite));
				return matchedSite;
			}
		}

		logger::error("Hook 1 (SE) failed to locate unique tail E8 call inside REL::ID(64209); matches={}.", matchCount);
		return std::nullopt;
	}

	[[nodiscard]] std::optional<std::uintptr_t> FindInitNodeIdsCallSite()
	{
		const std::uintptr_t parentAddr = REL::RelocationID(57921, 58502).address();
		const std::uintptr_t expectedTarget = REL::RelocationID(57796, 58369).address();
		if (!IsExecutableTextAddress(parentAddr) || !IsExecutableTextAddress(expectedTarget)) {
			logger::error("Hook 2 relocation invalid: parent=0x{:X}, target=0x{:X}.", parentAddr, expectedTarget);
			return std::nullopt;
		}

		const auto*           bytes = reinterpret_cast<const std::uint8_t*>(parentAddr);
		const auto decoded = DecodeCalls(bytes, (std::min)(CallBoundaries::kMaxBytes, ExecutableBytes(parentAddr, true)));
		const auto scanLimit = decoded.size;
		constexpr std::size_t kPreferredOffset = 0x70;

		std::uintptr_t matchedSite = 0;
		std::size_t    matchCount = 0;
		for (std::size_t off = 0; off + 5 <= scanLimit; ++off) {
			if (decoded.calls[off]) {
				const std::uintptr_t site = parentAddr + off;
				if (DecodeCallRel32(site) == expectedTarget) {
					matchedSite = site;
					++matchCount;
				}
			}
		}

		if (matchCount == 1) {
			return matchedSite;
		}
		if (matchCount > 1 && decoded.calls[kPreferredOffset] &&
			DecodeCallRel32(parentAddr + kPreferredOffset) == expectedTarget) {
			return parentAddr + kPreferredOffset;
		}

		// Co-load compatibility fallback: chain if another plugin already patched the expected `mov rcx, rsi; call initNodeIds` slot.
		if (matchCount == 0 && decoded.calls[kPreferredOffset] && decoded.calls[0x68] &&
			BytesEqual(bytes + 0x6D, { 0x48, 0x8B, 0xCE })) {
			const std::uintptr_t site = parentAddr + kPreferredOffset;
			const std::uintptr_t chainedTarget = DecodeCallRel32(site);
			if (IsLiveExecutableAddress(chainedTarget)) {
				logger::info("Hook 2: call site +0x{:X} already patched (-> 0x{:X}); chaining existing hook.",
					kPreferredOffset, chainedTarget);
				return site;
			}
		}

		logger::error("Hook 2 failed to locate E8 call to initNodeIds inside parent function; matches={}.", matchCount);
		return std::nullopt;
	}

	struct TraversalBuffers
	{
		std::vector<RE::hkbNode*> nodes;
		std::vector<std::uint32_t> buckets;
		std::vector<NodeChildInfo> childScratch;
		InstanceTopology::Trace links;
	};

	TraversalBuffers& ThreadTraversalBuffers()
	{
		thread_local TraversalBuffers buffers;
		return buffers;
	}

	struct TraversalLease
	{
		// Give this traversal exclusive ownership. A nested traversal gets
		// independent buffers even when it runs on the same thread.
		TraversalBuffers buffers = std::move(ThreadTraversalBuffers());

		~TraversalLease()
		{
			constexpr std::size_t retainedBytes = 1u << 20;
			auto& cached = ThreadTraversalBuffers();
			const auto vectorBytes = []<class T>(const std::vector<T>& values) { return values.capacity() * sizeof(T); };
			const auto used = vectorBytes(cached.nodes) + vectorBytes(cached.buckets) +
				vectorBytes(cached.childScratch) + cached.links.CapacityBytes();
			auto remaining = retainedBytes - (std::min)(used, retainedBytes);
			const auto retain = [&](auto& target, auto& current, std::size_t oldBytes, std::size_t newBytes) {
				if (newBytes <= oldBytes || newBytes - oldBytes > remaining) return;
				remaining -= newBytes - oldBytes;
				target = std::move(current);
			};
			const auto retainVector = [&](auto& target, auto& current) {
				retain(target, current, vectorBytes(target), vectorBytes(current));
			};
			buffers.nodes.clear();
			buffers.buckets.clear();
			buffers.links.Reset(false);
			retainVector(cached.childScratch, buffers.childScratch);
			retainVector(cached.buckets, buffers.buckets);
			retainVector(cached.nodes, buffers.nodes);
			retain(cached.links, buffers.links, cached.links.CapacityBytes(), buffers.links.CapacityBytes());
		}
	};

	// Each bucket stores a four-byte discovery index into the traversal vector.
	class FlatNodeIndex
	{
	public:
		static constexpr std::size_t kMaxBuckets = 2 * GraphPlan::maxNodes;

		FlatNodeIndex(std::size_t a_initialBuckets, std::vector<std::uint32_t>& a_storage) : _buckets(a_storage)
		{
			const auto retained = a_initialBuckets >= 16384 ? (std::min)(_buckets.capacity(), std::size_t{1} << 17) : 0;
			const auto clamped = (std::min)((std::max)(a_initialBuckets, retained), kMaxBuckets);
			std::size_t       cap = 16;
			while (cap < clamped) {
				cap <<= 1;
			}
			_buckets.assign(cap, GraphPlan::noNode);
		}

		std::uint32_t FindOrAdd(RE::hkbNode* a_ptr, std::vector<RE::hkbNode*>& a_nodes)
		{
			auto slot = FindSlot(a_ptr, a_nodes);
			if (_buckets[slot] != GraphPlan::noNode) return _buckets[slot];
			if ((a_nodes.size() + 1) * 2 > _buckets.size()) {
				if (_buckets.size() == kMaxBuckets) return GraphPlan::noNode;
				Rehash(a_nodes);
				slot = FindSlot(a_ptr, a_nodes);
			}
			const auto index = static_cast<std::uint32_t>(a_nodes.size());
			a_nodes.push_back(a_ptr);
			_buckets[slot] = index;
			return index;
		}

	private:
		[[nodiscard]] static std::size_t hashPtr(const void* a_ptr) noexcept
		{
			std::uint64_t x = reinterpret_cast<std::uintptr_t>(a_ptr);
			x ^= x >> 30;
			x *= 0xbf58476d1ce4e5b9ULL;
			x ^= x >> 27;
			x *= 0x94d049bb133111ebULL;
			x ^= x >> 31;
			return static_cast<std::size_t>(x);
		}

		std::size_t FindSlot(RE::hkbNode* a_ptr, const std::vector<RE::hkbNode*>& a_nodes) const noexcept
		{
			const auto mask = _buckets.size() - 1;
			auto slot = hashPtr(a_ptr) & mask;
			while (_buckets[slot] != GraphPlan::noNode && a_nodes[_buckets[slot]] != a_ptr) {
				slot = (slot + 1) & mask;
			}
			return slot;
		}

		void Rehash(const std::vector<RE::hkbNode*>& a_nodes)
		{
			_buckets.assign(_buckets.size() * 2, GraphPlan::noNode);
			for (std::uint32_t index = 0; index < a_nodes.size(); ++index) {
				_buckets[FindSlot(a_nodes[index], a_nodes)] = index;
			}
		}

		std::vector<std::uint32_t>& _buckets;
	};

	[[nodiscard]] bool WritableSpan(std::uintptr_t address, std::size_t bytes, std::size_t alignment) noexcept;

	// The native getChildren implementations grow through the Havok heap
	// allocator. Its array reserve clears the non-owning bit on replacement.
	struct ChildBufferOwner
	{
		RawHkArray& array;
		const NodeChildInfo* borrowed;
		std::size_t borrowedBytes;
		NodeChildInfo* replacement{};
		std::int32_t replacementBytes{};

		void ValidateReplacement() noexcept
		{
			const auto flags = static_cast<std::uint32_t>(array.capacityAndFlags);
			const auto capacity = flags & kArrayCapacityMask;
			if ((flags & kDontDeallocateFlag) || !array.data || !capacity ||
				capacity > static_cast<std::uint32_t>(INT32_MAX / sizeof(NodeChildInfo))) return;
			const auto address = reinterpret_cast<std::uintptr_t>(array.data);
			const auto bytes = capacity * sizeof(NodeChildInfo);
			if (GraphPlan::BuffersOverlap(address, bytes, reinterpret_cast<std::uintptr_t>(borrowed), borrowedBytes) ||
				!WritableSpan(address, bytes, alignof(NodeChildInfo))) return;
			replacement = array.data;
			replacementBytes = static_cast<std::int32_t>(bytes);
		}

		~ChildBufferOwner()
		{
			if (replacement) g_heapAllocator->BufFree(replacement, replacementBytes);
		}
	};

	struct LeafMethods
	{
		std::uintptr_t bound{};
		std::uintptr_t children{};
		const std::uintptr_t* clipTable{};
	};

	LeafMethods VerifiedLeafMethods()
	{
		static const bool verifiedRuntime = REL::Module::get().version() == REL::Version{1, 7, 104, 0};
		if (!verifiedRuntime) return {};
		// The clip-generator pair is constant bound 2 and a no-op getChildren
		// on the primary EXE. Recheck the live bytes before skipping either call.
		const auto tableAddress = RE::VTABLE_hkbClipGenerator[0].address();
		const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
		if (tableAddress < rdata.address() || tableAddress - rdata.address() >= rdata.size() ||
			rdata.size() - (tableAddress - rdata.address()) < 10 * sizeof(std::uintptr_t)) return {};
		const auto* table = reinterpret_cast<const std::uintptr_t*>(tableAddress);
		const auto bound = table[8];
		const auto children = table[9];
		if (ExecutableBytes(bound, true) < 6 || ExecutableBytes(children, true) < 3 ||
			!BytesEqual(reinterpret_cast<const std::uint8_t*>(bound), {0xB8, 2, 0, 0, 0, 0xC3}) ||
			!BytesEqual(reinterpret_cast<const std::uint8_t*>(children), {0xC2, 0, 0})) return {};
		const auto getClass = table[1];
		if (ExecutableBytes(getClass, true) >= 5 &&
			*reinterpret_cast<const std::uint8_t*>(getClass) == 0xE9) {
			const auto target = DecodeCallRel32(getClass);  // E9 uses the same rel32 encoding.
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(target);
			if (ExecutableBytes(target, true) >= 8 && BytesEqual(bytes, {0x48, 0x8D, 0x05}) && bytes[7] == 0xC3) {
				std::int32_t displacement{};
				std::memcpy(&displacement, bytes + 3, sizeof(displacement));
				const auto type = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(target + 7) + displacement);
				if (type != reinterpret_cast<std::uintptr_t>(g_stateMachineClass)) return {bound, children, table};
			}
		}
		return {bound, children, nullptr};
	}

	bool IsStateMachine(RE::hkbNode* node, LeafMethods leaf, std::size_t& verifiedTypes)
	{
		const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(node);
		if (leaf.clipTable && vtable == leaf.clipTable) {
			++verifiedTypes;
			return false;
		}
		return node->GetClassType() == g_stateMachineClass;
	}

	class ChildEnumerator
	{
	public:
		ChildEnumerator(std::vector<NodeChildInfo>& scratch, const GraphLifecycle::Analyses::Ticket& ticket) :
			largeChildren(scratch), analysis(ticket) {}

		template <class Visit>
		bool Read(RE::hkbNode* node, LeafMethods leaf, std::size_t& verifiedLeaves, Visit visit)
		{
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(node);
			if (!vtable || !vtable[8] || !vtable[9]) return false;
			if (vtable[8] == leaf.bound && vtable[9] == leaf.children) {
				++verifiedLeaves;
				return visit(std::span<const NodeChildInfo>{});
			}
			using GetMaxNumChildren = std::int32_t (*)(RE::hkbNode*, std::int32_t);
			using GetChildren = void (*)(RE::hkbNode*, std::int32_t, NodeChildrenInfo&);
			const auto maxChildren = reinterpret_cast<GetMaxNumChildren>(vtable[8])(node, 0);
			if (!analysis.IsCurrent()) return false;
			if (maxChildren == 0) return visit(std::span<const NodeChildInfo>{});
			if (maxChildren < 0 || maxChildren > kMaxChildrenPerNode) return false;

			const std::size_t neededCapacity = static_cast<std::size_t>(maxChildren) + 16;
			if (childCount < neededCapacity) {
				if (largeChildren.size() < neededCapacity) largeChildren.resize(neededCapacity);
				childBuffer = largeChildren.data();
				childCount = largeChildren.size();
				childCapacity = largeChildren.capacity();
			}

			RawHkArray childArr{};
			childArr.data = childBuffer;
			childArr.size = 0;
			childArr.capacityAndFlags = static_cast<std::int32_t>(
				static_cast<std::uint32_t>(childCount) | kDontDeallocateFlag);

			ChildBufferOwner ownership{ childArr, childBuffer, childCapacity * sizeof(NodeChildInfo) };
			NodeChildrenInfo info{};
			info.children = &childArr;
			info.flags = 0;

			if (!analysis.IsCurrent()) return false;
			reinterpret_cast<GetChildren>(vtable[9])(node, 0, info);
			if (childArr.data != childBuffer) ownership.ValidateReplacement();
			if (!analysis.IsCurrent()) return false;

			const auto flags = static_cast<std::uint32_t>(childArr.capacityAndFlags);
			const auto capacity = flags & kArrayCapacityMask;
			if (info.children != &childArr || childArr.size > kMaxChildrenPerNode ||
				!GraphPlan::ValidChildArray(childArr.size, capacity, childArr.data != nullptr) ||
				(childArr.data == childBuffer && (capacity != childCount || !(flags & kDontDeallocateFlag))) ||
				(childArr.data != childBuffer && !ownership.replacement)) return false;
			return visit(std::span<const NodeChildInfo>{childArr.data, static_cast<std::size_t>(childArr.size)});
		}

	private:
		std::array<NodeChildInfo, 128> localChildren{};
		std::vector<NodeChildInfo>& largeChildren;
		const GraphLifecycle::Analyses::Ticket& analysis;
		NodeChildInfo* childBuffer = localChildren.data();
		std::size_t childCount = localChildren.size();
		std::size_t childCapacity = localChildren.size();
	};

	bool CollectReachableNodes(
		RE::hkbNode* root, TraversalBuffers& storage, bool largeHint, LeafMethods leaf,
		GraphPlan::Signature& signature, std::size_t& verifiedLeaves, bool foldEmptyNodes,
		GraphPlan::PreparedCheck& prepared, std::size_t& verifiedTypes, const GraphLifecycle::Analyses::Ticket& analysis)
	{
		auto& nodes = storage.nodes;
		FlatNodeIndex visited(largeHint ? 16384 : 1024, storage.buckets);
		ChildEnumerator children(storage.childScratch, analysis);
		if (visited.FindOrAdd(root, nodes) == GraphPlan::noNode) return false;
		for (std::size_t i = 0; i < nodes.size(); ++i) {
			if (!analysis.IsCurrent()) return false;
			prepared.Observe(i, [&] { return IsStateMachine(nodes[i], leaf, verifiedTypes); });
			if (!analysis.IsCurrent()) return false;
			const bool valid = children.Read(nodes[i], leaf, verifiedLeaves, [&](std::span<const NodeChildInfo> live) {
				if (live.empty()) {
					if (foldEmptyNodes) signature.AddEmptyNode();
					else signature.Add(0);
					storage.links.RecordEmptyNode();
					return true;
				}
				signature.Add(static_cast<std::uint32_t>(live.size()));
				auto saved = storage.links.RecordChildren(live.size());
				for (std::size_t j = 0; j < live.size(); ++j) {
					const auto child = live[j].node;
					const auto index = child ? visited.FindOrAdd(child, nodes) : GraphPlan::noNode;
					if (child && index == GraphPlan::noNode) return false;
					signature.Add(index);
					if (!saved.empty()) saved[j] = index;
				}
				return true;
			});
			if (!valid) return false;
		}
		return analysis.IsCurrent();
	}

	bool ReplayTopology(RE::hkbNode* root, TraversalBuffers& storage, const GraphCache::PreparedPlan& prepared,
		LeafMethods leaf, std::size_t& verifiedLeaves, std::size_t& verifiedTypes,
		const GraphLifecycle::Analyses::Ticket& analysis)
	{
		auto& nodes = storage.nodes;
		const auto ids = prepared.IDs();
		nodes.reserve(ids.size());
		FlatNodeIndex visited(ids.size() * 2, storage.buckets);
		ChildEnumerator children(storage.childScratch, analysis);
		return prepared.payload->links.Replay(root, nodes, [&](RE::hkbNode* child, std::size_t ordinal) {
			return visited.FindOrAdd(child, nodes) == ordinal;
		}, [&](RE::hkbNode* node, std::size_t ordinal, auto check) {
			if (!analysis.IsCurrent() || IsStateMachine(node, leaf, verifiedTypes) != (ids[ordinal] != kInvalidNodeId) ||
				!analysis.IsCurrent()) return false;
			return children.Read(node, leaf, verifiedLeaves, [&](std::span<const NodeChildInfo> live) {
				return check(live.size(), [&](std::size_t index) { return live[index].node; });
			});
		});
	}

	[[nodiscard]] bool WritableSpan(std::uintptr_t address, std::size_t bytes, std::size_t alignment) noexcept
	{
		if (!address || address % alignment) return false;
		while (bytes) {
			REX::W32::MEMORY_BASIC_INFORMATION info{};
			constexpr std::uint32_t writablePages = 0x04u | 0x08u | 0x40u | 0x80u;
			if (!REX::W32::VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
				info.state != 0x1000u || (info.protect & 0x100u) || !(info.protect & writablePages)) return false;
			const auto base = reinterpret_cast<std::uintptr_t>(info.baseAddress);
			if (address < base || address - base >= info.regionSize) return false;
			const auto checked = (std::min)(bytes, info.regionSize - (address - base));
			bytes -= checked;
			if (!bytes) return true;
			if (checked > (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
			address += checked;
		}
		return true;
	}

	[[nodiscard]] std::optional<std::size_t> ValidMapCapacity(const hkMapBase& map)
	{
		if (!map.elem) return TransitionMap::ValidHeader(map, 0) ? std::optional<std::size_t>{0} : std::nullopt;
		if (map.hashMod < 0 || map.hashMod >= (1 << 20)) return {};
		const auto capacity = static_cast<std::size_t>(map.hashMod) + 1;
		if (!TransitionMap::ValidHeader(map, capacity) || !WritableSpan(reinterpret_cast<std::uintptr_t>(map.elem),
			capacity * sizeof(TransitionMap::Bucket), alignof(TransitionMap::Bucket))) return {};
		return capacity;
	}

	bool IDsMatch(const RE::hkbBehaviorGraph& graph,
		const std::vector<RE::hkbNode*>& nodes, const std::vector<std::uint32_t>& stateMachines,
		std::span<const std::uint16_t> preparedIds)
	{
		const auto count = static_cast<std::int16_t>(stateMachines.size());
		if (graph.numStaticNodes != count || graph.nextUniqueID != count || graph.uniqueIDPool.size() != 0) return false;
		if (!preparedIds.empty()) {
			return std::ranges::equal(nodes, preparedIds, {}, &RE::hkbNode::id);
		}
		std::size_t next{};
		for (std::size_t i = 0; i < nodes.size(); ++i) {
			auto expected = kInvalidNodeId;
			if (next < stateMachines.size() && stateMachines[next] == i) expected = static_cast<std::uint16_t>(next++);
			if (nodes[i]->id != expected) return false;
		}
		return true;
	}

	struct ProcessedGraph
	{
		const RE::hkReferencedObject* transitionData{};
		std::vector<RE::hkbNode*> nodes;
		std::shared_ptr<const GraphSlimming::Payload> payload;
		GraphPlan::Shape shape;
		mutable std::atomic_bool persisted{};
		mutable std::atomic_bool topologyPersisted{};

		[[nodiscard]] std::size_t PrivateBytes() const noexcept
		{
			return sizeof(*this) + nodes.capacity() * sizeof(RE::hkbNode*);
		}
	};

	using InstanceCache = InstanceTopology::Cache<const RE::hkbBehaviorGraph*, ProcessedGraph>;
	InstanceCache processedGraphs;
	GraphLifecycle::Analyses graphAnalyses;
	GraphSlimming::Engine optimizer;
	thread_local std::uint64_t nativeInitSequence{};

	// Track only calls through the existing init hook. Native init stays
	// outside the lock, while this plugin's graph commits check this list.
	class NativeInitialization
	{
	public:
		explicit NativeInitialization(RE::hkbBehaviorGraph* graph) : graph(graph)
		{
			InstanceCache::Retired retired;
			std::scoped_lock lock(g_compactionMutex);
			transitionData = graph ? graph->globalTransitionData.get() : nullptr;
			retired = Invalidate(transitionData);
			next = head;
			head = this;
		}

		~NativeInitialization()
		{
			InstanceCache::Retired retired;
			std::scoped_lock lock(g_compactionMutex);
			const auto* current = completed && graph ? graph->globalTransitionData.get() : nullptr;
			if (current && current != transitionData) retired = Invalidate(current);
			auto** entry = &head;
			while (*entry != this) entry = &(*entry)->next;
			*entry = next;
		}

		NativeInitialization(const NativeInitialization&) = delete;
		NativeInitialization& operator=(const NativeInitialization&) = delete;

		void MarkCompleted() noexcept { completed = true; }

		// Hold g_compactionMutex.
		[[nodiscard]] static bool Contains(const RE::hkbBehaviorGraph* graph, const RE::hkReferencedObject* transition = nullptr) noexcept
		{
			for (const auto* initializing = head; initializing; initializing = initializing->next) {
				if (initializing->graph == graph || (transition && initializing->transitionData == transition)) return true;
			}
			return false;
		}

	private:
		// Hold g_compactionMutex. Destruction of the extracted snapshots is deferred to the caller.
		[[nodiscard]] InstanceCache::Retired Invalidate(const RE::hkReferencedObject* transition) const
		{
			if (!graph) return {};
			graphAnalyses.Invalidate(graph, transition);
			return processedGraphs.ExtractIf([&](const RE::hkbBehaviorGraph* cachedGraph, const ProcessedGraph& cached) {
				return cachedGraph == graph || (transition && cached.transitionData == transition);
			});
		}

		static inline NativeInitialization* head{};
		const RE::hkbBehaviorGraph* graph;
		const RE::hkReferencedObject* transitionData{};
		NativeInitialization* next{};
		bool completed{};
	};

	enum class ApplyResult { unchanged, rebuilt, graphChanged, idsInUse, invalidPlan, invalidMap, unexpectedMapCode, rebuildFailed };

	struct CommitMetrics
	{
		std::size_t mapBytesBefore{};
		std::size_t mapBytesAfter{};
		std::size_t idsWritten{};
		GraphPerformance::Duration lockWait{};
		bool mapRebuilt{};
		bool tableReused{};
	};

	struct GraphIdentity
	{
		RE::hkbGenerator* root{};
		const RE::hkReferencedObject* transitionData{};
		const GraphLifecycle::Analyses::Ticket* analysis{};
	};

	[[nodiscard]] bool IDsAreWritable(const RE::hkbBehaviorGraph& graph)
	{
		const auto activeCount = graph.activeNodes ? graph.activeNodes->size() : 0;
		return GraphPlan::CanRewriteIDs(graph.isActive, activeCount, graph.uniqueIDPool.size(),
			graph.numStaticNodes, graph.nextUniqueID);
	}

	[[nodiscard]] bool NativeInsertMatchesReference(const TransitionMap::RuntimeProfile& profile) noexcept
	{
		const auto address = reinterpret_cast<std::uintptr_t>(g_hkMapInsert);
		if (ExecutableBytes(address, true) < profile.insertBytes) return false;
		return TransitionMap::MatchesVerifiedInsert(profile, {reinterpret_cast<const std::uint8_t*>(address), profile.insertBytes});
	}

	ApplyResult ApplyGraphPlan(RE::hkbBehaviorGraph& graph, GraphIdentity identity, const std::vector<RE::hkbNode*>& nodes,
		const std::vector<std::uint32_t>& stateMachines, CommitMetrics& metrics, const ProcessedGraph* instance = nullptr,
		std::span<const std::uint16_t> preparedIds = {})
	{
		if (!identity.analysis->IsCurrent()) return ApplyResult::graphChanged;
		if (!GraphPlan::ValidAssignments(nodes.size(), stateMachines, preparedIds) || nodes.front() != identity.root) {
			return ApplyResult::invalidPlan;
		}
		// Traversal and disk I/O stay outside the publication lock.
		const bool measure = GraphSlimming::Options().performanceLogging;
		const auto lockStart = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		std::scoped_lock lock(g_compactionMutex);
		if (measure) metrics.lockWait = std::chrono::steady_clock::now() - lockStart;
		if (!identity.analysis->IsCurrent() || NativeInitialization::Contains(&graph, identity.transitionData) || graph.isActive ||
			graph.rootGenerator.get() != identity.root || graph.globalTransitionData.get() != identity.transitionData) {
			return ApplyResult::graphChanged;
		}
		if (instance && !processedGraphs.IsCurrent(&graph, instance)) return ApplyResult::graphChanged;
		if (!IDsAreWritable(graph)) return ApplyResult::idsInUse;
		const auto transition = reinterpret_cast<std::uintptr_t>(identity.transitionData);
		if (transition > (std::numeric_limits<std::uintptr_t>::max)() - kGlobalTransitionMapOffset) return ApplyResult::invalidMap;
		const auto mapAddress = transition + kGlobalTransitionMapOffset;
		if (!WritableSpan(mapAddress, sizeof(hkMapBase), alignof(hkMapBase))) return ApplyResult::invalidMap;
		auto& map = *reinterpret_cast<hkMapBase*>(mapAddress);
		const auto buckets = ValidMapCapacity(map);
		if (!buckets) return ApplyResult::invalidMap;
		metrics.mapBytesBefore = metrics.mapBytesAfter = *buckets * sizeof(TransitionMap::Bucket);
		const auto valueAt = [&](std::size_t id) { return reinterpret_cast<std::uintptr_t>(nodes[stateMachines[id]]); };
		const bool hasExpectedCount = (static_cast<std::uint32_t>(map.numElemsAndFlags) & ~kDontDeallocateFlag) == stateMachines.size();
		const auto* mapProfile = g_transitionMapProfile;
		const bool verifiedABI = mapProfile != nullptr;
		const bool reuseWithChangedIds = verifiedABI && GraphSlimming::Options().Optimize();
		// An optimization-enabled rebuild does not need a separate ID scan if the map differs.
		const bool mapMatches = hasExpectedCount && (reuseWithChangedIds || IDsMatch(graph, nodes, stateMachines, preparedIds)) &&
			TransitionMap::Matches(map, *buckets, stateMachines.size(), verifiedABI, valueAt);
		if (mapMatches && (!reuseWithChangedIds || IDsMatch(graph, nodes, stateMachines, preparedIds))) {
			return ApplyResult::unchanged;
		}

		const auto applyNodeIds = [&] {
			const auto writeId = [&](RE::hkbNode* node, auto id) {
				if (node->id == id) return;
				node->id = id;
				++metrics.idsWritten;
			};
			if (!preparedIds.empty()) {
				for (std::size_t i = 0; i < nodes.size(); ++i) writeId(nodes[i], preparedIds[i]);
				return;
			}
			std::uint16_t next{};
			for (std::size_t i = 0; i < nodes.size(); ++i) {
				const bool isStateMachine = next < stateMachines.size() && stateMachines[next] == i;
				writeId(nodes[i], isStateMachine ? next++ : kInvalidNodeId);
			}
		};
		if (mapMatches) {
			metrics.tableReused = true;
			applyNodeIds();
		} else if (mapProfile) {
			if (!NativeInsertMatchesReference(*mapProfile)) return ApplyResult::unexpectedMapCode;
			const auto insert = [](hkMapBase& staged, std::size_t id, std::uintptr_t value) {
				return g_hkMapInsert(&staged, g_heapAllocator, id, value) == 1;
			};
			if (!TransitionMap::Replace(map, *buckets, stateMachines.size(),
				static_cast<RE::hkMemoryAllocator&>(*g_heapAllocator), insert, valueAt)) return ApplyResult::rebuildFailed;
			applyNodeIds();
		} else {
			// A changed map requires a qualified allocator and map layout.
			return ApplyResult::unexpectedMapCode;
		}

		const auto count = static_cast<std::int16_t>(stateMachines.size());
		graph.numStaticNodes = count;
		graph.nextUniqueID = count;
		metrics.mapRebuilt = !mapMatches;
		metrics.mapBytesAfter = map.elem && map.hashMod >= 0 ?
			(static_cast<std::size_t>(map.hashMod) + 1) * sizeof(TransitionMap::Bucket) : 0;
		return ApplyResult::rebuilt;
	}

	bool ReportApplyResult(ApplyResult result, const char* source, const char* name)
	{
		if (result == ApplyResult::invalidPlan) {
			logger::warn("[{}] '{}' has an inconsistent node ID plan. Keeping engine IDs.", source, name);
		} else if (result == ApplyResult::invalidMap) {
			logger::warn("[{}] '{}' has invalid Havok map storage. Skipping compaction.", source, name);
		} else if (result == ApplyResult::rebuildFailed) {
			logger::warn("[{}] '{}' could not prepare the replacement Havok map. Keeping engine IDs.", source, name);
		} else if (result == ApplyResult::unexpectedMapCode) {
			logger::warn("[{}] '{}' cannot use the qualified Havok map writer. Keeping engine IDs.", source, name);
		} else if (result == ApplyResult::idsInUse) {
			logger::warn("[{}] '{}' still has active or allocated node IDs. Keeping engine IDs.", source, name);
		}
		return result == ApplyResult::unchanged || result == ApplyResult::rebuilt;
	}

	const char* CacheWriteLabel(GraphCache::WriteResult result) noexcept
	{
		switch (result) {
		case GraphCache::WriteResult::saved: return "saved";
		case GraphCache::WriteResult::current: return "already-saved";
		case GraphCache::WriteResult::pending: return "pending";
		case GraphCache::WriteResult::failed: return "failed";
		default: return "skipped";
		}
	}

	bool SavedOnDisk(GraphCache::WriteResult result) noexcept
	{
		return result == GraphCache::WriteResult::saved || result == GraphCache::WriteResult::current;
	}

	using GraphPerformance::Outcome;

	template <class... Args>
	void LogGraphDetail(std::format_string<Args...> message, Args&&... args) noexcept
	{
		try {
			const auto time = std::chrono::system_clock::now();
			GraphPerformance::QueueDetail(time, std::format(message, std::forward<Args>(args)...));
		} catch (const std::exception&) {
			// Optional diagnostics must not interfere with graph processing.
		}
	}

	std::optional<Outcome> TryReuseInstance(RE::hkbBehaviorGraph& graph, GraphIdentity identity, const InstanceCache::Snapshot& previous,
		std::vector<NodeChildInfo>& childScratch, const char* source, const char* name)
	{
		if (!identity.analysis->IsCurrent()) return Outcome::changed;
		if (previous->transitionData != identity.transitionData) return {};
		using Clock = std::chrono::steady_clock;
		const auto& options = GraphSlimming::Options();
		const auto initSequence = nativeInitSequence;
		const auto start = options.performanceLogging ? Clock::now() : Clock::time_point{};
		const auto leaf = VerifiedLeafMethods();
		ChildEnumerator children(childScratch, *identity.analysis);
		std::size_t verifiedLeaves{};
		std::size_t verifiedClipTypes{};
		std::size_t nextMachine{};
		const auto& plan = *previous->payload->plan;
		if (!previous->payload->links.Matches(static_cast<RE::hkbNode*>(identity.root),
			std::span<RE::hkbNode* const>{previous->nodes},
			[&](RE::hkbNode* node, std::size_t ordinal, auto checkLinks) {
				if (!identity.analysis->IsCurrent() || !children.Read(node, leaf, verifiedLeaves, [&](std::span<const NodeChildInfo> live) {
					return checkLinks(live.size(), [&](std::size_t index) { return live[index].node; });
				})) return false;
				const bool expectedMachine = nextMachine < plan.stateMachines.size() &&
					plan.stateMachines[nextMachine] == ordinal;
				if (!identity.analysis->IsCurrent() || IsStateMachine(node, leaf, verifiedClipTypes) != expectedMachine) return false;
				if (expectedMachine) ++nextMachine;
				return true;
			}) || nextMachine != plan.stateMachines.size() || initSequence != nativeInitSequence) return {};

		const auto applyStart = options.performanceLogging ? Clock::now() : Clock::time_point{};
		CommitMetrics commit;
		const auto result = ApplyGraphPlan(graph, identity, previous->nodes, plan.stateMachines, commit, previous.get());
		if (result == ApplyResult::graphChanged) return {};
		if (result == ApplyResult::idsInUse) return Outcome::idsInUse;
		if (!ReportApplyResult(result, source, name)) return Outcome::blocked;

		const auto applyEnd = options.performanceLogging ? Clock::now() : Clock::time_point{};
		// Keep the disk retry behavior even when the process-local record is
		// sufficient to avoid re-deriving the plan.
		const char* cacheWrite = "not-needed";
		if (options.persistentCache && !previous->persisted.load(std::memory_order_relaxed)) {
			const auto lookup = GraphCache::Read(name, previous->shape);
			const bool cacheMatches = lookup.entry && lookup.entry->topology == plan.topology &&
				lookup.entry->nodes == plan.nodes && lookup.entry->stateMachines == plan.stateMachines;
			if (cacheMatches && lookup.persisted) {
				previous->persisted.store(true, std::memory_order_relaxed);
			} else if (!cacheMatches || lookup.retryWrite || lookup.origin == GraphCache::Origin::legacy) {
				const auto write = GraphCache::Write(name, previous->shape, plan);
				cacheWrite = CacheWriteLabel(write);
				if (SavedOnDisk(write)) previous->persisted.store(true, std::memory_order_relaxed);
			}
		}
		if (options.persistentCache && options.adaptivePreloading &&
			!previous->topologyPersisted.load(std::memory_order_relaxed) &&
			GraphCache::QueueTopology(name, previous->shape, previous->payload)) {
			previous->topologyPersisted.store(true, std::memory_order_relaxed);
		}
		if (options.performanceLogging && GraphPerformance::WantsDetail()) {
			const auto analysisUs = std::chrono::duration_cast<std::chrono::microseconds>(applyStart - start).count();
			const auto applyUs = std::chrono::duration_cast<std::chrono::microseconds>(applyEnd - applyStart - commit.lockWait).count();
			LogGraphDetail("[{}] '{}' traversal=instance-fast cache=hit nodes={} optimizedNodes={} stateMachines={} leafCallsSkipped={} "
				"analysis={}us optimize=0us idCommit={}us commitLockWait={}ns cacheWrite={} mapRebuilt={} mapReused={} "
				"mapBytesBefore={} mapBytesAfter={} idsWritten={} nativeObjectsEliminated=0", source, name, plan.nodes, plan.nodes,
				plan.stateMachines.size(), verifiedLeaves * 2 + verifiedClipTypes, analysisUs, applyUs,
				std::chrono::duration_cast<std::chrono::nanoseconds>(commit.lockWait).count(),
				cacheWrite, commit.mapRebuilt, commit.tableReused, commit.mapBytesBefore, commit.mapBytesAfter, commit.idsWritten);
		}
		return Outcome::instance;
	}

	Outcome CompactBehaviorGraphNodeIds(RE::hkbBehaviorGraph* graph, const char* source)
	{
		if (!graph) return Outcome::noGraph;
		const auto& options = GraphSlimming::Options();
		GraphLifecycle::Analyses::Ticket analysis(graphAnalyses, g_compactionMutex);
		GraphIdentity identity;
		bool largeHint;
		{
			std::scoped_lock lock(g_compactionMutex);
			if (NativeInitialization::Contains(graph) || graph->isActive) return Outcome::active;
			identity = {graph->rootGenerator.get(), graph->globalTransitionData.get(), &analysis};
			if (!identity.root || !identity.transitionData) return Outcome::notReady;
			if (!IDsAreWritable(*graph)) return Outcome::idsInUse;
			if (NativeInitialization::Contains(graph, identity.transitionData) ||
				!analysis.TryBegin(graph, identity.transitionData)) return Outcome::analysisBusy;
			largeHint = static_cast<std::uint16_t>(graph->numStaticNodes) >= 4096;
		}
		const auto* transitionData = identity.transitionData;
		auto* root = identity.root;
		const auto graphName = graph->name.c_str();
		const char* name = graphName && *graphName ? graphName : "<unnamed>";
		TraversalLease traversal;
		if (options.instanceFastPath) {
			if (const auto previous = processedGraphs.Find(graph)) {
				if (const auto reused = TryReuseInstance(*graph, identity, previous, traversal.buffers.childScratch, source, name)) return *reused;
				processedGraphs.Invalidate(graph, previous.get());
			}
		}
		{
			std::scoped_lock lock(g_compactionMutex);
			if (!analysis.IsCurrent() || NativeInitialization::Contains(graph, identity.transitionData) || graph->isActive ||
				graph->rootGenerator.get() != root || graph->globalTransitionData.get() != transitionData) return Outcome::changed;
		}
		using Clock = std::chrono::steady_clock;
		const auto micros = [](auto start) {
			return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
		};
		const auto start = Clock::now();
		auto& nodes = traversal.buffers.nodes;
		auto& trace = traversal.buffers.links;
		const bool persistentTopology = options.persistentCache && options.adaptivePreloading;
		const auto startupPlan = persistentTopology ? GraphCache::FindPrepared(name) : nullptr;
		auto preparedIds = startupPlan ? startupPlan->IDs() : std::span<const std::uint16_t>{};
		const bool hasTopology = startupPlan && startupPlan->payload;
		const auto leaf = largeHint || hasTopology ? VerifiedLeafMethods() : LeafMethods{};
		GraphPlan::Signature signature;
		std::size_t verifiedLeaves{};
		std::size_t verifiedClipTypes{};
		std::optional<bool> replay;
		std::size_t verifiedPrefix{};
		if (hasTopology) {
			try {
				replay = ReplayTopology(root, traversal.buffers, *startupPlan, leaf, verifiedLeaves, verifiedClipTypes, analysis);
			} catch (const std::bad_alloc&) {
				// A failed cache allocation still allows the ordinary path.
			}
		}
		if (!analysis.IsCurrent()) return Outcome::changed;
		const bool topologyHit = replay.value_or(false);
		bool startupHit = topologyHit;
		if (!topologyHit) {
			nodes.clear();
			nodes.reserve(largeHint ? 16384 : 512);
			trace.Reset(options.instanceFastPath || persistentTopology);
			verifiedLeaves = verifiedClipTypes = 0;
			// An offline candidate has never been observed in Skyrim. Require
			// exact replay before letting it supply any IDs.
			GraphPlan::PreparedCheck startupCheck(startupPlan && !startupPlan->offline ? preparedIds : std::span<const std::uint16_t>{});
			if (!CollectReachableNodes(root, traversal.buffers, largeHint, leaf, signature, verifiedLeaves, options.Optimize(),
				startupCheck, verifiedClipTypes, analysis)) {
				if (!analysis.IsCurrent()) return Outcome::changed;
				logger::warn("[{}] '{}' could not be traversed completely. Skipping compaction.", source, name);
				return Outcome::blocked;
			}
			verifiedPrefix = startupCheck.VerifiedPrefix();
			startupHit = startupPlan && startupCheck.Matches(nodes.size(), signature.value, startupPlan->shape.topology);
		}
		if (nodes.size() <= kGraphNodeThreshold) return Outcome::belowLimit;
		const auto traversalUs = micros(start);
		const auto nodeCount = nodes.size();

		GraphPlan::Shape shape{};
		GraphCache::Lookup lookup;
		GraphPlan::Entry fresh;
		bool hit = startupHit;
		bool matchedOfflineLinks{};
		std::int64_t classificationUs{}, cacheReadUs{}, validationUs{};
		if (startupHit) {
			shape = startupPlan->shape;
			lookup = {GraphCache::Result::hit, startupPlan->entry, GraphCache::Origin::preloaded, false, !startupPlan->offline};
		} else {
			const auto classificationStart = Clock::now();
			GraphPlan::StateMachines machines(static_cast<std::uint32_t>(nodes.size()));
			if (verifiedPrefix) {
				for (const auto ordinal : startupPlan->entry->stateMachines) {
					if (ordinal >= verifiedPrefix) break;
					machines.Add(ordinal);
				}
			}
			for (auto i = static_cast<std::uint32_t>(verifiedPrefix); i < nodes.size(); ++i) {
				if (!analysis.IsCurrent()) return Outcome::changed;
				if (IsStateMachine(nodes[i], leaf, verifiedClipTypes)) machines.Add(i);
			}
			if (machines.Count() > kGraphNodeThreshold) {
				logger::error("[{}] '{}' exceeds the {} state-machine ID limit.", source, name, kGraphNodeThreshold);
				return Outcome::blocked;
			}
			shape = machines.Describe(signature.value);
			classificationUs = micros(classificationStart);
			const auto cacheReadStart = Clock::now();
			if (options.persistentCache) lookup = GraphCache::Read(name, shape);
			cacheReadUs = micros(cacheReadStart);
			const auto validationStart = Clock::now();
			hit = lookup.entry && machines.Matches(*lookup.entry, shape.topology);
			if (persistentTopology && trace.Complete(nodes.size()) && (!hit || !lookup.prepared || !lookup.prepared->payload)) {
				auto offline = GraphCache::FindOffline(name, shape);
				if (offline && machines.Matches(*offline->entry, shape.topology) &&
					trace.SameLinks(offline->payload->links)) {
					if (!hit) lookup = {GraphCache::Result::hit, offline->entry, GraphCache::Origin::preloaded};
					lookup.prepared = std::move(offline);
					matchedOfflineLinks = true;
					hit = true;
				}
			}
			validationUs = micros(validationStart);
			if (!hit) {
				const auto analysisStart = Clock::now();
				fresh = machines.Build(shape.topology);
				fresh.analysisMicros = static_cast<std::uint32_t>(std::min<std::int64_t>(micros(analysisStart), 60'000'000));
			}
		}
		const auto* verifiedAlternative = !startupHit && hit ? lookup.prepared.get() : nullptr;
		const bool changedShape = startupPlan && !startupHit &&
			(shape.nodes != startupPlan->shape.nodes || shape.topology != startupPlan->shape.topology ||
				shape.classification != startupPlan->shape.classification);
		// A temporary allocation failure during replay is not a mismatch.
		if (startupPlan && (changedShape || replay == false || (!hasTopology && !startupHit))) {
			{
				std::scoped_lock lock(g_compactionMutex);
				if (!analysis.IsCurrent() || NativeInitialization::Contains(graph, identity.transitionData) || graph->isActive ||
					graph->rootGenerator.get() != root || graph->globalTransitionData.get() != transitionData) return Outcome::changed;
				using GraphCache::PreparedMismatch;
				auto mismatch = PreparedMismatch::plan;
				if (changedShape) mismatch = PreparedMismatch::differentShape;
				else if (replay == false && (startupHit || verifiedAlternative == startupPlan.get())) {
					mismatch = PreparedMismatch::topology;
				}
				GraphCache::ResolvePreparedMismatch(name, *startupPlan, mismatch);
			}
			if (options.performanceLogging && GraphPerformance::WantsDetail()) {
				LogGraphDetail("[{}] '{}' startup candidate mismatch, differentShape={}, matchedAlternate={}, reusedPreparedIds={}.",
					source, name, changedShape, verifiedAlternative != nullptr, startupHit || verifiedAlternative != nullptr);
			}
		}
		if (verifiedAlternative) preparedIds = verifiedAlternative->IDs();
		else if (!startupHit) preparedIds = {};
		const auto& plan = hit ? *lookup.entry : fresh;
		const char* cacheState = !options.persistentCache ? "disabled" : (hit ? "hit" : "miss");
		if (!hit && (lookup.entry || lookup.result == GraphCache::Result::invalid)) cacheState = "invalidated";
		if (!hit && lookup.result == GraphCache::Result::unavailable) cacheState = "unavailable";
		const bool legacy = lookup.origin == GraphCache::Origin::legacy;
		const char* cacheSource = "none";
		if (hit) {
			switch (lookup.origin) {
			case GraphCache::Origin::memory: cacheSource = "memory"; break;
			case GraphCache::Origin::disk: cacheSource = "disk"; break;
			case GraphCache::Origin::legacy: cacheSource = "legacy"; break;
			case GraphCache::Origin::preloaded: cacheSource = "preloaded"; break;
			default: break;
			}
		}

		const auto analysisUs = micros(start);
		const auto applyStart = Clock::now();
		CommitMetrics commit;
		const auto applied = ApplyGraphPlan(*graph, identity, nodes, plan.stateMachines, commit, nullptr, preparedIds);
		const auto applyUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - applyStart - commit.lockWait).count();
		if (!ReportApplyResult(applied, source, name)) {
			if (applied == ApplyResult::graphChanged) return Outcome::changed;
			if (applied == ApplyResult::idsInUse) return Outcome::idsInUse;
			return Outcome::blocked;
		}
		const auto cacheWriteStart = Clock::now();
		auto writeResult = GraphCache::WriteResult::skipped;
		const bool attemptWrite = options.persistentCache &&
			(!hit || legacy || lookup.retryWrite || (!lookup.persisted &&
				((topologyHit && startupPlan->offline) || (verifiedAlternative && verifiedAlternative->offline))));
		if (hit && legacy) {
			// The older measurement covered different work. Keep its valid plan,
			// but start the timing baseline anew for this schema.
			auto migrated = plan;
			migrated.analysisMicros = 0;
			writeResult = GraphCache::Write(name, shape, migrated);
		} else if (attemptWrite) {
			writeResult = GraphCache::Write(name, shape, plan);
		}
		const auto writeUs = micros(cacheWriteStart);
		const auto machineCount = plan.stateMachines.size();
		const auto planUs = fresh.analysisMicros;
		const auto optimizationStart = Clock::now();
		GraphSlimming::Preparation prepared;
		bool resident{};
		if (topologyHit || trace.Complete(nodeCount)) {
			try {
				const auto* reusable = topologyHit ? startupPlan.get() : verifiedAlternative;
				if (reusable && reusable->payload &&
					(topologyHit || matchedOfflineLinks || trace.SameLinks(reusable->payload->links))) {
					const auto& payload = reusable->payload;
					const auto bytes = payload->CapacityBytes();
					prepared = {payload, bytes, bytes, true, payload->links.IsPacked()};
				} else {
					std::shared_ptr<const GraphPlan::Entry> verifiedPlan = lookup.entry;
					if (!hit) verifiedPlan = std::make_shared<const GraphPlan::Entry>(std::move(fresh));
					else if (legacy) {
						auto migrated = plan;
						migrated.analysisMicros = 0;
						verifiedPlan = std::make_shared<const GraphPlan::Entry>(std::move(migrated));
					}
					prepared = optimizer.Prepare(shape, std::move(trace), std::move(verifiedPlan), options);
				}
				const bool topologyPersisted = persistentTopology && GraphCache::QueueTopology(name, shape, prepared.payload);
				if (prepared.payload && options.instanceFastPath) {
					auto saved = std::make_shared<ProcessedGraph>();
					saved->transitionData = transitionData;
					saved->nodes = std::move(nodes);
					saved->payload = prepared.payload;
					saved->shape = shape;
					saved->persisted.store((hit && lookup.persisted) || SavedOnDisk(writeResult), std::memory_order_relaxed);
					saved->topologyPersisted.store(topologyPersisted, std::memory_order_relaxed);
					const auto bytes = saved->PrivateBytes();
					const InstanceCache::SharedStorage shared{prepared.payload.get(), prepared.bytesAfter};
					InstanceCache::Admission admission;
					{
						std::scoped_lock lock(g_compactionMutex);
						if (analysis.IsCurrent()) admission = processedGraphs.Admit(graph, saved, bytes, shared);
					}
					resident = admission.stored;
				}
			} catch (const std::bad_alloc&) {
				// The already-applied ID plan does not depend on the optional instance snapshot.
			}
		}
		if (options.performanceLogging && GraphPerformance::WantsDetail()) {
			const char* cacheWrite = attemptWrite ? CacheWriteLabel(writeResult) : "not-needed";
			const char* traversalMode = topologyHit ? (startupPlan->offline ? "offline-topology" : "disk-topology") :
				(startupHit ? "preload-fast" : (verifiedAlternative && verifiedAlternative->offline ? "offline-verified" : "full"));
			LogGraphDetail("[{}] '{}' traversal={} cache={} cacheSource={} cacheWrite={} nodes={} optimizedNodes={} "
				"stateMachines={} leafCallsSkipped={} analysis={}us traverse={}us classify={}us readCache={}us "
				"validate={}us rebuildPlan={}us writeCache={}us optimize={}us idCommit={}us commitLockWait={}ns nativeObjectsEliminated=0 "
				"sharedPayloads={} payloadBytesBefore={} payloadBytesAfter={} newPayloadBytes={} packedOrdinals={} "
				"instanceStored={} mapRebuilt={} mapReused={} mapBytesBefore={} mapBytesAfter={} idsWritten={}",
				source, name, traversalMode, cacheState, cacheSource, cacheWrite, nodeCount, nodeCount, machineCount,
				verifiedLeaves * 2 + verifiedClipTypes, analysisUs, traversalUs, classificationUs, cacheReadUs, validationUs,
				planUs, writeUs, micros(optimizationStart), applyUs,
				std::chrono::duration_cast<std::chrono::nanoseconds>(commit.lockWait).count(),
				resident && prepared.shared ? 1 : 0, prepared.bytesBefore,
				prepared.bytesAfter, resident && !prepared.shared ? prepared.bytesAfter : 0, prepared.packed, resident,
				commit.mapRebuilt, commit.tableReused, commit.mapBytesBefore, commit.mapBytesAfter, commit.idsWritten);
		}
		if (topologyHit) return startupPlan->offline ? Outcome::offlineTopology : Outcome::diskTopology;
		if (verifiedAlternative && verifiedAlternative->offline) return Outcome::offlineVerified;
		return startupHit ? Outcome::prepared : Outcome::full;
	}

	Outcome TryCompact(RE::hkbBehaviorGraph* graph, const char* source)
	{
		try {
			return CompactBehaviorGraphNodeIds(graph, source);
		} catch (const std::bad_alloc&) {
			logger::error("[{}] Not enough memory to complete BehaviorNodeIdLimitFix processing.", source);
			return Outcome::blocked;
		}
	}

	bool Thunk_PreSetupBehaviorGraph(void* a_config, RE::hkbCharacter* a_character, void* a_callback)
	{
		using Clock = std::chrono::steady_clock;
		const bool logging = GraphSlimming::Options().performanceLogging;
		const auto start = logging ? Clock::now() : Clock::time_point{};
		const auto outcome = TryCompact(a_character ? a_character->behaviorGraph.get() : nullptr, "BShkbLink");
		const auto nativeStart = logging ? Clock::now() : Clock::time_point{};
		const bool result = g_origPreSetup(a_config, a_character, a_callback);
		if (logging) GraphPerformance::Record(GraphPerformance::Hook::preSetup, outcome, Clock::now() - nativeStart, nativeStart - start);
		return result;
	}

	void Thunk_InitNodeIds(RE::hkbBehaviorGraph* a_graph)
	{
		using Clock = std::chrono::steady_clock;
		const bool logging = GraphSlimming::Options().performanceLogging;
		const auto start = logging ? Clock::now() : Clock::time_point{};
		Clock::time_point nativeStart{}, nativeEnd{};
		++nativeInitSequence;
		{
			NativeInitialization initializing(a_graph);
			if (logging) nativeStart = Clock::now();
			g_origInitNodeIds(a_graph);
			if (logging) nativeEnd = Clock::now();
			initializing.MarkCompleted();
		}
		const auto outcome = TryCompact(a_graph, "initNodeIds");
		if (logging) GraphPerformance::Record(GraphPerformance::Hook::initNodeIds, outcome,
			nativeEnd - nativeStart, (nativeStart - start) + (Clock::now() - nativeEnd));
	}
}

namespace Hooks
{
	bool Install()
	{
		static bool s_installed = false;
		if (s_installed) {
			return true;
		}

		const auto runtime = REL::Module::get().version();
		const auto* profile = TransitionMap::FindRuntimeProfile({runtime[0], runtime[1], runtime[2], runtime[3]});
		g_transitionMapProfile = profile;
		if (profile) {
			logger::info("Skyrim runtime {} | Havok map rebuild profile selected | Address Library ID {} | guard {} bytes",
				runtime.string("."), profile->insertId, profile->insertBytes);
		} else {
			logger::info("Skyrim runtime {} | Havok map rebuild profile unavailable", runtime.string("."));
		}

		const auto getStateMachineClass = reinterpret_cast<StateMachineStaticClassFn>(
			REL::RelocationID(58262, 58901).address());
		if (!IsLiveExecutableAddress(reinterpret_cast<std::uintptr_t>(getStateMachineClass))) {
			logger::error("hkbStateMachine::staticClass relocation invalid: 0x{:X}.",
				reinterpret_cast<std::uintptr_t>(getStateMachineClass));
			return false;
		}

		g_stateMachineClass = getStateMachineClass();
		if (!g_stateMachineClass || !g_stateMachineClass->name ||
			std::strcmp(g_stateMachineClass->name, "hkbStateMachine") != 0) {
			logger::error("hkbStateMachine::staticClass verification failed (name='{}').",
				(g_stateMachineClass && g_stateMachineClass->name) ? g_stateMachineClass->name : "<null>");
			return false;
		}
		logger::info("Verified hkbStateMachine hkClass at 0x{:X} (name='{}').",
			reinterpret_cast<std::uintptr_t>(g_stateMachineClass), g_stateMachineClass->name);

		g_heapAllocator = RE::hkContainerHeapAllocator::GetSingleton();
		if (!g_heapAllocator) {
			logger::error("Havok heap allocator is unavailable.");
			return false;
		}
		if (profile) {
			const auto address = REL::ID(profile->insertId).address();
			if (!IsLiveExecutableAddress(address)) {
				logger::error("Havok map insert relocation {} is invalid at 0x{:X}.", profile->insertId, address);
				return false;
			}
			g_hkMapInsert = reinterpret_cast<HkMapInsertFn>(address);
		}

		const auto hook1Site = FindPreSetupCallSite();
		if (!hook1Site) {
			return false;
		}

		const auto hook2Site = FindInitNodeIdsCallSite();
		if (!hook2Site) {
			return false;
		}

		auto& trampoline = SKSE::GetTrampoline();
		constexpr std::size_t kRequiredTrampolineBytes = 32;
		if (trampoline.free_size() < kRequiredTrampolineBytes) {
			logger::error("Insufficient SKSE trampoline space: free={}, required={}.",
				trampoline.free_size(), kRequiredTrampolineBytes);
			return false;
		}

		const auto validPredecessor = [&](std::uintptr_t site) -> std::uintptr_t {
			if (ExecutableBytes(site, true) < 5 || *reinterpret_cast<const std::uint8_t*>(site) != 0xE8) return 0;
			const auto target = DecodeCallRel32(site);
			if (!IsLiveExecutableAddress(target) || target == *hook1Site || target == *hook2Site ||
				target == reinterpret_cast<std::uintptr_t>(&Thunk_PreSetupBehaviorGraph) ||
				target == reinterpret_cast<std::uintptr_t>(&Thunk_InitNodeIds)) return 0;
			return target;
		};
		const auto predecessor1 = validPredecessor(*hook1Site);
		const auto predecessor2 = validPredecessor(*hook2Site);
		if (!predecessor1 || !predecessor2 || *hook1Site == *hook2Site || predecessor1 == predecessor2) {
			logger::error("BehaviorNodeIdLimitFix could not validate independent hook predecessors.");
			return false;
		}
		g_origPreSetup = reinterpret_cast<PreSetupBehaviorGraphFn>(predecessor1);
		g_origInitNodeIds = reinterpret_cast<InitNodeIdsFn>(predecessor2);
		(void)trampoline.write_call<5>(*hook1Site, Thunk_PreSetupBehaviorGraph);
		(void)trampoline.write_call<5>(*hook2Site, Thunk_InitNodeIds);

		s_installed = true;
		logger::info("Installed Hook 1 (BShkbLink_PostNodeIds_PreSetup) at 0x{:X} -> orig 0x{:X}.",
			*hook1Site, reinterpret_cast<std::uintptr_t>(g_origPreSetup));
		logger::info("Installed Hook 2 (hkbBehaviorGraph_InitNodeIds) at 0x{:X} -> orig 0x{:X}.",
			*hook2Site, reinterpret_cast<std::uintptr_t>(g_origInitNodeIds));

		return true;
	}
}
