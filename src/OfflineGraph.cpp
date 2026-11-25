#include "OfflineGraph.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace
{
	using Bytes = std::span<const char>;
	using OfflineGraph::Clock;
	using OfflineGraph::Status;
	constexpr auto noNode = GraphPlan::noNode;
	constexpr std::size_t maxNodes = 240'000;
	constexpr std::size_t maxEdges = 550'000;
	constexpr std::size_t maxFixups = 900'000;
	constexpr std::size_t maxReviewedRows = 1'000'000;

	template <class T>
	T Value(Bytes data, std::size_t offset)
	{
		if (offset > data.size() || sizeof(T) > data.size() - offset) throw Status::unsupported;
		T value;
		std::memcpy(&value, data.data() + offset, sizeof(T));
		return value;
	}

	std::string_view Text(Bytes data, std::size_t offset)
	{
		if (offset >= data.size()) throw Status::unsupported;
		const auto* start = data.data() + offset;
		const auto* end = static_cast<const char*>(std::memchr(start, 0, (std::min)(data.size() - offset, std::size_t{256})));
		if (!end) throw Status::unsupported;
		return {start, static_cast<std::size_t>(end - start)};
	}

	struct Section
	{
		std::uint32_t start, local, global, object, exports, imports, end;
	};

	Section Header(Bytes file, std::size_t offset, std::size_t fileSize, std::string_view required)
	{
		if (Text(file, offset) != required) throw Status::unsupported;
		const auto section = Value<Section>(file, offset + 20);
		if (section.start < OfflineGraph::packfileHeaderBytes || section.start > fileSize || section.end > fileSize - section.start ||
			section.local > section.global || section.global > section.object || section.object > section.exports ||
			section.exports != section.imports || section.imports != section.end) throw Status::unsupported;
		return section;
	}

	struct Layout
	{
		Section names;
		Section contents;
	};

	Layout ReadLayout(Bytes header, std::size_t fileSize)
	{
		if (header.size() < OfflineGraph::packfileHeaderBytes || fileSize < OfflineGraph::packfileHeaderBytes || fileSize > (64u << 20) ||
			Value<std::uint32_t>(header, 0) != 0x57e0e057 || Value<std::uint32_t>(header, 4) != 0x10c0c010 ||
			Value<std::uint32_t>(header, 12) != 8 || Value<std::uint32_t>(header, 16) != 0x01000108 ||
			Value<std::uint32_t>(header, 20) != 3 || Value<std::uint32_t>(header, 24) != 2 ||
			Value<std::uint32_t>(header, 32) != 0 || Text(header, 40) != "hk_2010.2.0-r1") throw Status::unsupported;
		const auto names = Header(header, 64, fileSize, "__classnames__");
		const auto types = Header(header, 112, fileSize, "__types__");
		const auto contents = Header(header, 160, fileSize, "__data__");
		if (names.local != names.end || types.end || names.start + names.end > types.start || types.start > contents.start ||
			contents.start + contents.end != fileSize) throw Status::unsupported;
		const auto root = Value<std::uint32_t>(header, 28);
		const auto rootClass = Value<std::uint32_t>(header, 36);
		// The root needs its variants pointer and count. Class names follow a signature and tag byte.
		if ((root & 7u) || root > contents.local || contents.local - root < 12 ||
			rootClass < 5 || rootClass >= names.local) throw Status::unsupported;
		if ((contents.exports - contents.object) / 12 <= GraphPlan::nodeThreshold) throw Status::belowLimit;
		return {names, contents};
	}

	enum class Kind : std::uint8_t { unknown, container, graph, stateMachine, state, transitionArray,
		clip, selector, modifierGenerator };

	Kind Identify(std::string_view name, std::uint32_t signature)
	{
		// HKX2's Skyrim SE reflection data and the installed version-8 class table
		// identify these layouts independently of the C++ runtime objects.
		struct Class { std::string_view name; std::uint32_t signature; Kind kind; };
		constexpr Class supported[]{
			{"hkRootLevelContainer", 0x2772c11e, Kind::container},
			{"hkbBehaviorGraph", 0xb1218f86, Kind::graph},
			{"hkbStateMachine", 0x816c1dcb, Kind::stateMachine},
			{"hkbStateMachineStateInfo", 0x0ed7f9d0, Kind::state},
			{"hkbStateMachineTransitionInfoArray", 0xe397b11e, Kind::transitionArray},
			{"hkbClipGenerator", 0x333b85b9, Kind::clip},
			{"hkbManualSelectorGenerator", 0xd932fab8, Kind::selector},
			{"hkbModifierGenerator", 0x1f81fae6, Kind::modifierGenerator}
		};
		for (const auto& item : supported) if (item.signature == signature && item.name == name) return item.kind;
		return Kind::unknown;
	}

	struct Array
	{
		std::uint32_t offset;
		std::uint32_t count;
	};

	class Packfile
	{
	public:
		Packfile(Bytes file, const std::atomic_bool& a_stop, Clock::time_point a_deadline) : stop(a_stop), deadline(a_deadline)
		{
			const auto [names, contents] = ReadLayout(file, file.size());
			data = file.subspan(contents.start, contents.local);
			const auto namesData = file.subspan(names.start, names.local);
			std::unordered_map<std::uint32_t, Kind> classes;
			for (std::size_t pos = 0; pos < namesData.size() && !Padding(namesData.subspan(pos));) {
				const auto signature = Value<std::uint32_t>(namesData, pos);
				if (Value<std::uint8_t>(namesData, pos + 4) != 9 || classes.size() >= 1024) throw Status::unsupported;
				const auto name = Text(namesData, pos + 5);
				if (name.empty()) throw Status::unsupported;
				classes.emplace(static_cast<std::uint32_t>(pos + 5), Identify(name, signature));
				pos += 6 + name.size();
			}
			const auto rootClass = classes.find(Value<std::uint32_t>(file, 36));
			if (rootClass == classes.end() || rootClass->second != Kind::container) throw Status::unsupported;
			root = Value<std::uint32_t>(file, 28);

			const auto section = file.subspan(contents.start, contents.end);
			const auto localBytes = contents.global - contents.local;
			const auto globalBytes = contents.object - contents.global;
			const auto objectBytes = contents.exports - contents.object;
			objects.reserve((std::min)(std::size_t{objectBytes} / 12, maxFixups));
			ForTable(section.subspan(contents.object, objectBytes), 12, [&](Bytes row) {
				const auto offset = Value<std::uint32_t>(row, 0);
				const auto name = classes.find(Value<std::uint32_t>(row, 8));
				if ((offset & 7) || offset >= data.size() || Value<std::uint32_t>(row, 4) != 0 || name == classes.end() ||
					!objects.emplace(offset, name->second).second) throw Status::unsupported;
			});
			if (Type(root) != Kind::container) throw Status::unsupported;
			CheckStop();
			pointers.reserve((std::min)(std::size_t{localBytes} / 8, maxFixups) + (std::min)(std::size_t{globalBytes} / 12, maxFixups));
			ForTable(section.subspan(contents.local, localBytes), 8,
				[&](Bytes row) { AddPointer(Value<std::uint32_t>(row, 0), Value<std::uint32_t>(row, 4)); });
			ForTable(section.subspan(contents.global, globalBytes), 12, [&](Bytes row) {
				if (Value<std::uint32_t>(row, 4) != 2) throw Status::unsupported;
				AddPointer(Value<std::uint32_t>(row, 0), Value<std::uint32_t>(row, 8));
			});
		}

		void CheckStop() const
		{
			if (stop.load(std::memory_order_relaxed) ||
				(deadline != (Clock::time_point::max)() && Clock::now() >= deadline)) throw Status::cancelled;
		}

		std::uint32_t RootGraph() const
		{
			const auto variants = Members(root, 24);
			if (variants.count != 1) throw Status::unsupported;
			const auto graph = Pointer(variants.offset + 16);
			if (Type(graph) != Kind::graph) throw Status::unsupported;
			return graph;
		}

		std::string Name(std::uint32_t graph) const
		{
			const auto name = Text(data, Pointer(graph + 0x38));
			if (name.empty() || name == "<unnamed>" || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; })) {
				throw Status::unsupported;
			}
			return std::string(name);
		}

		std::uint32_t Generator(std::uint32_t graph) const
		{
			const auto generator = Pointer(graph + 0x80);
			if (generator == noNode) throw Status::unsupported;
			return generator;
		}

		Kind Type(std::uint32_t offset) const
		{
			const auto item = objects.find(offset);
			return item == objects.end() ? Kind::unknown : item->second;
		}

		void Children(std::uint32_t node, Kind type, std::vector<std::uint32_t>& edges)
		{
			edges.clear();
			switch (type) {
			case Kind::clip:
				return;
			case Kind::selector: {
				const auto list = Members(node + 0x48, 8);
				Charge(list.count);
				for (std::uint32_t i = 0; i < list.count; ++i) {
					if ((i & 1023u) == 0) CheckStop();
					edges.push_back(Pointer(list.offset + i * 8));
				}
				return;
			}
			case Kind::modifierGenerator:
				if (Pointer(node + 0x48) != noNode) break;
				edges.push_back(Pointer(node + 0x50));
				return;
			case Kind::stateMachine: {
				const auto states = Members(node + 0x90, 8);
				Charge(states.count);
				for (std::uint32_t i = 0; i < states.count; ++i) {
					if ((i & 255u) == 0) CheckStop();
					const auto state = Pointer(states.offset + i * 8);
					if (Type(state) != Kind::state) throw Status::unsupported;
					edges.push_back(Pointer(state + 0x58));
					CheckTransitions(Pointer(state + 0x50));
				}
				CheckTransitions(Pointer(node + 0xa0));
				return;
			}
			default:
				break;
			}
			// In particular, a reference generator requires Skyrim's project
			// resolver. Its target cannot be inferred from a filename here.
			throw Status::unsupported;
		}

	private:
		bool Padding(Bytes bytes) const
		{
			while (!bytes.empty()) {
				const auto chunk = bytes.first((std::min)(bytes.size(), std::size_t{4096}));
				if (!std::ranges::all_of(chunk, [](char ch) { return static_cast<unsigned char>(ch) == 255; })) return false;
				bytes = bytes.subspan(chunk.size());
				if (!bytes.empty()) CheckStop();
			}
			return true;
		}

		void ForTable(Bytes dataTable, std::size_t stride, auto consume)
		{
			std::size_t pos{};
			unsigned records{};
			while (dataTable.size() - pos >= stride && Value<std::uint32_t>(dataTable, pos) != noNode) {
				if (records++ >= maxFixups) throw Status::unsupported;
				if ((records & 1023u) == 0) CheckStop();
				consume(dataTable.subspan(pos, stride));
				pos += stride;
			}
			if (!Padding(dataTable.subspan(pos))) throw Status::unsupported;
		}

		void AddPointer(std::uint32_t from, std::uint32_t to)
		{
			if (from > data.size() || data.size() - from < 8 ||
				to >= data.size() || !pointers.emplace(from, to).second) throw Status::unsupported;
		}

		std::uint32_t Pointer(std::uint32_t at) const
		{
			if (Value<std::uint64_t>(data, at) != 0) throw Status::unsupported;
			const auto item = pointers.find(at);
			return item == pointers.end() ? noNode : item->second;
		}

		Array Members(std::uint32_t at, unsigned stride) const
		{
			const auto count = Value<std::uint32_t>(data, at + 8);
			const auto target = Pointer(at);
			if (count > maxEdges || (count && (target == noNode || (target & 7) || count > (data.size() - target) / stride))) {
				throw Status::unsupported;
			}
			return {target, count};
		}

		void Charge(std::size_t rows)
		{
			if (rows > remainingRows) throw Status::unsupported;
			remainingRows -= rows;
		}

		void CheckTransitions(std::uint32_t target)
		{
			if (target == noNode) return;
			if (Type(target) != Kind::transitionArray) throw Status::unsupported;
			const auto transitions = Members(target + 0x10, 0x48);
			Charge(transitions.count);
			for (std::uint32_t i = 0; i < transitions.count; ++i) {
				if ((i & 1023u) == 0) CheckStop();
				const auto effect = Pointer(transitions.offset + i * 0x48 + 0x20);
				// Effects may acquire linked generators at runtime.
				if (effect != noNode) throw Status::unsupported;
			}
		}

		const std::atomic_bool& stop;
		Clock::time_point deadline;
		Bytes data;
		std::uint32_t root{};
		std::size_t remainingRows{maxReviewedRows};
		std::unordered_map<std::uint32_t, std::uint32_t> pointers;
		std::unordered_map<std::uint32_t, Kind> objects;
	};
}

namespace OfflineGraph
{
	std::optional<Status> ScreenHeader(std::span<const char> header, std::uint64_t size) noexcept
	{
		try {
			(void)ReadLayout(header, size);
			return std::nullopt;
		} catch (Status status) {
			return status;
		}
	}

	Analysis Analyze(std::span<const char> bytes, const std::atomic_bool& stop, Clock::time_point deadline)
	{
		try {
			if (stop.load(std::memory_order_relaxed) || Clock::now() >= deadline) return {Status::cancelled, {}};
			Packfile file(bytes, stop, deadline);
			const auto graph = file.RootGraph();
			auto name = file.Name(graph);
			const auto root = file.Generator(graph);
			std::vector<std::uint32_t> nodes{root}, edges, stateMachines;
			std::unordered_map<std::uint32_t, std::uint32_t> ordinals{{root, 0}};
			GraphPlan::Signature topology;
			InstanceTopology::Trace links(true);
			std::size_t edgeCount{};
			std::size_t emptyNodes{};
			const auto flushEmptyNodes = [&] {
				topology.AddEmptyNodes(emptyNodes);
				links.RecordEmptyNodes(emptyNodes);
				emptyNodes = 0;
			};
			for (std::size_t i = 0; i < nodes.size(); ++i) {
				if ((i & 255) == 0) file.CheckStop();
				const auto type = file.Type(nodes[i]);
				if (type == Kind::stateMachine) {
					if (stateMachines.size() == GraphPlan::nodeThreshold) return {};
					stateMachines.push_back(static_cast<std::uint32_t>(i));
				}
				file.Children(nodes[i], type, edges);
				if (edges.empty()) {
					++emptyNodes;
					continue;
				}
				if (emptyNodes) flushEmptyNodes();
				if (edges.size() > maxEdges - edgeCount) return {};
				edgeCount += edges.size();
				topology.Add(static_cast<std::uint32_t>(edges.size()));
				auto row = links.RecordChildren(edges.size());
				if (row.size() != edges.size()) return {};
				for (std::size_t j = 0; j < edges.size(); ++j) {
					if (((j + 1) & 1023u) == 0) file.CheckStop();
					if (edges[j] == noNode) {
						row[j] = noNode;
					} else {
						const auto [position, added] = ordinals.try_emplace(edges[j], static_cast<std::uint32_t>(nodes.size()));
						if (added) {
							if (nodes.size() >= maxNodes) return {};
							nodes.push_back(edges[j]);
						}
						row[j] = position->second;
					}
					topology.Add(row[j]);
				}
			}
			file.CheckStop();
			if (emptyNodes) flushEmptyNodes();
			if (nodes.size() <= GraphPlan::nodeThreshold) return {Status::belowLimit, {}};
			auto plan = std::make_shared<const GraphPlan::Entry>(GraphPlan::Entry{
				topology.value, static_cast<std::uint32_t>(nodes.size()), 0, std::move(stateMachines)});
			if (!plan->WellFormed() || !links.Complete(nodes.size())) return {};
			(void)links.PackOrdinals();
			if (links.PortableBytes() > InstanceTopology::Trace::maxPortableBytes) return {};
			GraphPlan::Signature classification;
			for (const auto ordinal : plan->stateMachines) classification.Add(ordinal);
			file.CheckStop();
			return {Status::planned, Candidate{std::move(name), {topology.value, classification.value, plan->nodes},
				{std::move(links), std::move(plan)}}};
		} catch (Status status) {
			return {status, {}};
		}
	}
}

