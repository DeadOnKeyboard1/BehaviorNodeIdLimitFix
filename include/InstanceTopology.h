#pragma once

#include "GraphPlan.h"
#include "ProcessedGraphCache.h"
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace InstanceTopology
{
	// The process-local record describes edges in first-discovery order.
	// It never owns or extends the lifetime of an engine node.
	class Trace
	{
	public:
		Trace() = default;
		explicit Trace(bool record) : recording(record), valid(record) {}

		Trace(const Trace& other) :
			rows(other.rows), edges(other.edges), packed(other.packed), nodeCount(other.nodeCount),
			recording(other.recording), valid(other.valid), edgeWidth(other.edgeWidth)
		{
			UpdateNodeBudget();
		}

		Trace(Trace&& other) noexcept { Swap(other); }

		Trace& operator=(const Trace& other)
		{
			if (this != &other) {
				Trace copy(other);
				Swap(copy);
			}
			return *this;
		}

		Trace& operator=(Trace&& other) noexcept
		{
			if (this != &other) {
				Trace moved(std::move(other));
				Swap(moved);
			}
			return *this;
		}

		void Reset(bool record = true) noexcept
		{
			nodeCount = 0;
			rows.clear();
			edges.clear();
			std::vector<std::uint8_t>{}.swap(packed);
			edgeWidth = 0;
			recording = valid = record;
			UpdateNodeBudget();
		}

		void RecordEmptyNode() noexcept { RecordEmptyNodes(1); }

		void RecordEmptyNodes(std::size_t count) noexcept
		{
			if (!recording) return;
			if (count > nodeBudget || nodeCount > nodeBudget - count) {
				Discard();
				return;
			}
			nodeCount += count;
		}

		[[nodiscard]] std::span<std::uint32_t> RecordChildren(std::size_t count) noexcept
		{
			if (!count) {
				RecordEmptyNode();
				return {};
			}
			if (!recording) return {};
			const auto ordinal = nodeCount++;
			if (nodeCount > nodeBudget || count > instanceBytes / sizeof(std::uint32_t) - edges.size()) {
				Discard();
				return {};
			}
			const auto estimate = nodeCount * sizeof(void*) + (rows.size() + 1) * sizeof(Row) +
				(edges.size() + count) * sizeof(std::uint32_t);
			if (estimate > instanceBytes) {
				Discard();
				return {};
			}
			try {
				const auto start = edges.size();
				const bool needsCapacity = start + count > edges.capacity() || rows.size() == rows.capacity();
				edges.resize(start + count);
				rows.push_back({static_cast<std::uint32_t>(ordinal), static_cast<std::uint32_t>(edges.size())});
				if (needsCapacity) {
					UpdateNodeBudget();
					if (nodeCount > nodeBudget) {
						Discard();
						return {};
					}
				}
				return std::span{edges}.subspan(start, count);
			} catch (const std::bad_alloc&) {
				Discard();
				return {};
			}
		}

		[[nodiscard]] bool Complete(std::size_t nodes) const noexcept
		{
			return valid && nodes != 0 && nodeCount == nodes;
		}

		[[nodiscard]] std::size_t CapacityBytes() const noexcept
		{
			return rows.capacity() * sizeof(Row) + edges.capacity() * sizeof(std::uint32_t) + packed.capacity();
		}

		[[nodiscard]] bool IsPacked() const noexcept { return edgeWidth != 0; }

		[[nodiscard]] bool PackOrdinals() noexcept
		{
			if (IsPacked()) return true;
			if (!Complete(nodeCount) || edges.empty()) return false;
			const std::uint8_t width = nodeCount <= 0xFFFFu ? 2 : 3;
			if (nodeCount > 0xFFFFFFu) return false;
			try {
				std::vector<std::uint8_t> compact(edges.size() * width);
				if (compact.capacity() >= edges.capacity() * sizeof(std::uint32_t)) return false;
				if (!(width == 2 ? PackEdges<2>(compact) : PackEdges<3>(compact))) return false;
				packed = std::move(compact);
				std::vector<std::uint32_t>{}.swap(edges);
				edgeWidth = width;
				recording = false;
				return true;
			} catch (const std::bad_alloc&) {
				return false;
			}
		}

		[[nodiscard]] bool SameLinks(const Trace& other) const noexcept
		{
			if (!Complete(nodeCount) || !other.Complete(nodeCount) || rows != other.rows) return false;
			if (edgeWidth == other.edgeWidth) return IsPacked() ? packed == other.packed : edges == other.edges;
			const auto count = rows.empty() ? 0 : rows.back().end;
			return WithOrdinals([&](auto left) {
				return other.WithOrdinals([&](auto right) {
					for (std::uint32_t index = 0; index < count; ++index) {
						if (left(index) != right(index)) return false;
					}
					return true;
				});
			});
		}

		template <class Node, class Inspect>
		[[nodiscard]] bool Matches(Node* root, std::span<Node* const> nodes, Inspect inspect) const
		{
			if (edgeWidth == 2) return MatchUsing<0xFFFFu>(root, nodes, inspect, [&](std::size_t i) { return PackedWord<2>(i); });
			if (edgeWidth == 3) return MatchUsing<0xFFFFFFu>(root, nodes, inspect, [&](std::size_t i) { return PackedWord<3>(i); });
			return MatchUsing<GraphPlan::noNode>(root, nodes, inspect, [&](std::size_t i) { return edges[i]; });
		}

		static constexpr std::size_t maxPortableBytes = 2 * 1024 * 1024;

		[[nodiscard]] std::size_t PortableBytes() const noexcept
		{
			return sizeof(std::uint32_t) * 5 + rows.size() * sizeof(Row) + edges.size() * sizeof(std::uint32_t) + packed.size();
		}

		bool WritePortable(std::ostream& output, const GraphPlan::Entry& plan) const
		{
			if (!Complete(plan.nodes) || PortableBytes() > maxPortableBytes || !ValidateTopology(plan.topology)) return false;
			const std::uint32_t header[]{
				portableVersion, static_cast<std::uint32_t>(nodeCount), static_cast<std::uint32_t>(rows.size()),
				rows.empty() ? 0u : rows.back().end, edgeWidth
			};
			output.write(reinterpret_cast<const char*>(header), sizeof(header));
			const auto write = [&output](const auto& values) {
				if (!values.empty()) output.write(reinterpret_cast<const char*>(values.data()),
					static_cast<std::streamsize>(values.size() * sizeof(values.front())));
			};
			write(rows);
			if (IsPacked()) write(packed);
			else write(edges);
			return static_cast<bool>(output);
		}

		[[nodiscard]] static std::optional<Trace> ReadPortable(std::istream& input, std::size_t bytes, const GraphPlan::Entry& plan)
		{
			auto trace = ReadPortableBytes(bytes, plan, [&](char* data, std::size_t count) {
				return static_cast<bool>(input.read(data, static_cast<std::streamsize>(count)));
			});
			if (!trace || input.peek() != std::char_traits<char>::eof() || input.bad()) return {};
			return trace;
		}

		// The caller supplies a bounded file length and an exact-read function.
		[[nodiscard]] static std::optional<Trace> ReadPortableBytes(std::size_t bytes, const GraphPlan::Entry& plan, auto readBytes)
		{
			std::uint32_t header[5]{};
			if (bytes > maxPortableBytes || bytes < sizeof(header) ||
				!readBytes(reinterpret_cast<char*>(header), sizeof(header))) return {};
			const auto [version, nodes, rowCount, edgeCount, width] = header;
			if (version != portableVersion || nodes != plan.nodes || nodes == 0 || rowCount > nodes ||
				rowCount > edgeCount || (width != 0 && width != 2 && width != 3) ||
				(width == 2 && nodes > 0xFFFFu) || (width == 3 && nodes > 0xFFFFFFu)) return {};
			if (bytes != sizeof(header) + std::size_t{rowCount} * sizeof(Row) +
				std::size_t{edgeCount} * (width ? width : sizeof(std::uint32_t))) return {};
			Trace trace;
			trace.rows.resize(rowCount);
			trace.nodeCount = nodes;
			trace.edgeWidth = static_cast<std::uint8_t>(width);
			if (width) trace.packed.resize(std::size_t{edgeCount} * width);
			else trace.edges.resize(edgeCount);
			const auto read = [&readBytes](auto& values) {
				return values.empty() || readBytes(reinterpret_cast<char*>(values.data()), values.size() * sizeof(values.front()));
			};
			if (!read(trace.rows) || !read(trace.edges) || !read(trace.packed)) return {};
			trace.valid = trace.ValidateTopology(plan.topology);
			if (!trace.valid) return {};
			return trace;
		}

		// Resolve only current pointers. The caller checks new ordinals with
		// its identity index so two cached nodes cannot alias one live node.
		template <class Node, class PublishNew, class Inspect>
		[[nodiscard]] bool Replay(Node* root, std::vector<Node*>& nodes, PublishNew publish, Inspect inspect,
			std::size_t* verified = nullptr) const
		{
			struct ReplayProgress
			{
				std::size_t* destination;
				std::size_t checked{};
				~ReplayProgress() { if (destination) *destination = checked; }
			} progress{verified};
			if (!Complete(nodeCount) || !root || !nodes.empty() || !publish(root, 0)) return false;
			if (edgeWidth == 2) return ReplayUsing<0xFFFFu>(nodes, publish, inspect, progress.checked,
				[this](std::size_t index) { return PackedWord<2>(index); });
			if (edgeWidth == 3) return ReplayUsing<0xFFFFFFu>(nodes, publish, inspect, progress.checked,
				[this](std::size_t index) { return PackedWord<3>(index); });
			return ReplayUsing<GraphPlan::noNode>(nodes, publish, inspect, progress.checked,
				[this](std::size_t index) { return edges[index]; });
		}

		// Reconstruct a validated discovery prefix without reading native nodes.
		void CopyPrefix(std::size_t verified, Trace& destination, GraphPlan::Signature& signature) const noexcept
		{
			verified = (std::min)(verified, nodeCount);
			(void)WithOrdinals([&](auto ordinalAt) {
				std::size_t next{};
				std::uint32_t begin{};
				for (const auto& row : rows) {
					if (row.ordinal >= verified) break;
					signature.AddEmptyNodes(row.ordinal - next);
					destination.RecordEmptyNodes(row.ordinal - next);
					const auto count = row.end - begin;
					signature.Add(count);
					auto children = destination.RecordChildren(count);
					for (std::size_t i = 0; i < count; ++i) {
						const auto index = ordinalAt(begin + i);
						signature.Add(index);
						if (!children.empty()) children[i] = index;
					}
					begin = row.end;
					next = row.ordinal + 1;
				}
				signature.AddEmptyNodes(verified - next);
				destination.RecordEmptyNodes(verified - next);
				return true;
			});
		}

	private:
		static constexpr std::uint32_t portableVersion = 0x31504F54u;

		[[nodiscard]] bool ValidateTopology(std::uint64_t expected) const noexcept
		{
			const auto edgeCount = IsPacked() ? packed.size() / edgeWidth : edges.size();
			return WithOrdinals([&](auto ordinalAt) {
				GraphPlan::Signature topology;
				std::uint32_t begin{}, nextNode{}, discovered{1};
				for (const auto& row : rows) {
					if (row.ordinal < nextNode || row.ordinal >= discovered || row.ordinal >= nodeCount ||
						row.end <= begin || row.end > edgeCount) return false;
					topology.AddEmptyNodes(row.ordinal - nextNode);
					topology.Add(row.end - begin);
					for (auto edge = begin; edge < row.end; ++edge) {
						const auto index = ordinalAt(edge);
						if (index != GraphPlan::noNode) {
							if (index > discovered || index >= nodeCount) return false;
							if (index == discovered) ++discovered;
						}
						topology.Add(index);
					}
					begin = row.end;
					nextNode = row.ordinal + 1;
				}
				if (begin != edgeCount || discovered != nodeCount) return false;
				topology.AddEmptyNodes(nodeCount - nextNode);
				return topology.value == expected;
			});
		}

		void Swap(Trace& other) noexcept
		{
			rows.swap(other.rows);
			edges.swap(other.edges);
			packed.swap(other.packed);
			std::swap(nodeCount, other.nodeCount);
			std::swap(nodeBudget, other.nodeBudget);
			std::swap(recording, other.recording);
			std::swap(valid, other.valid);
			std::swap(edgeWidth, other.edgeWidth);
		}

		void UpdateNodeBudget() noexcept
		{
			const auto bytes = CapacityBytes();
			nodeBudget = bytes >= instanceBytes ? 0 : (instanceBytes - bytes) / sizeof(void*);
		}

		void Discard() noexcept
		{
			recording = valid = false;
			std::vector<Row>{}.swap(rows);
			std::vector<std::uint32_t>{}.swap(edges);
		}

		template <std::size_t width>
		[[nodiscard]] std::uint32_t PackedWord(std::size_t index) const noexcept
		{
			static_assert(std::endian::native == std::endian::little);
			const auto* data = packed.data() + index * width;
			std::uint16_t low;
			std::memcpy(&low, data, sizeof(low));
			if constexpr (width == 3) {
				return low | (static_cast<std::uint32_t>(data[2]) << 16);
			} else {
				return low;
			}
		}

		template <std::size_t width>
		[[nodiscard]] bool PackEdges(std::span<std::uint8_t> output) const noexcept
		{
			static_assert(std::endian::native == std::endian::little);
			for (std::size_t i = 0; i < edges.size(); ++i) {
				const auto value = edges[i];
				if (value != GraphPlan::noNode && value >= nodeCount) return false;
				// The low bytes of noNode are the packed null sentinel at either width.
				std::memcpy(output.data() + i * width, &value, width);
			}
			return true;
		}

		template <std::size_t width>
		[[nodiscard]] std::uint32_t UnpackOrdinal(std::size_t index) const noexcept
		{
			const auto value = PackedWord<width>(index);
			constexpr auto sentinel = (1u << (width * 8)) - 1;
			return value == sentinel ? GraphPlan::noNode : value;
		}

		template <class Use>
		[[nodiscard]] bool WithOrdinals(Use use) const
		{
			if (edgeWidth == 2) return use([this](std::size_t i) { return UnpackOrdinal<2>(i); });
			if (edgeWidth == 3) return use([this](std::size_t i) { return UnpackOrdinal<3>(i); });
			return use([this](std::size_t i) { return edges[i]; });
		}

		template <std::uint32_t nullIndex, class Node, class PublishNew, class Inspect, class ReadOrdinal>
		[[nodiscard]] bool ReplayUsing(std::vector<Node*>& nodes, PublishNew& publish, Inspect& inspect,
			std::size_t& checked, ReadOrdinal readOrdinal) const
		{
			std::size_t row{};
			std::uint32_t begin{};
			for (; checked < nodeCount; ++checked) {
				if (checked >= nodes.size()) return false;
				const auto end = row < rows.size() && rows[row].ordinal == checked ? rows[row++].end : begin;
				if (!inspect(nodes[checked], checked, [&](std::size_t count, auto childAt) {
					if (count != end - begin) return false;
					for (std::size_t child = 0; child < count; ++child) {
						const auto expected = readOrdinal(begin + child);
						const auto live = childAt(child);
						if (expected == nullIndex) {
							if (live) return false;
						} else if (!live || (expected < nodes.size() ? nodes[expected] != live :
							expected != nodes.size() || !publish(live, expected))) return false;
					}
					return true;
				})) return false;
				begin = end;
			}
			return nodes.size() == nodeCount && row == rows.size();
		}

		// The root establishes liveness for ordinal zero. A matching live edge
		// establishes liveness for the next ordinal before inspect may read it.
		// A stale address is used only for pointer comparison until that point.
		template <std::uint32_t nullIndex, class Node, class Inspect, class ReadOrdinal>
		[[nodiscard]] bool MatchUsing(Node* root, std::span<Node* const> nodes, Inspect& inspect, ReadOrdinal readOrdinal) const
		{
			if (!Complete(nodes.size()) || !root || nodes.front() != root) return false;
			std::size_t discovered = 1;
			std::size_t row{};
			std::uint32_t begin{};
			for (std::size_t i = 0; i < nodes.size(); ++i) {
				if (i >= discovered) return false;
				const auto end = row < rows.size() && rows[row].ordinal == i ? rows[row++].end : begin;
				const auto check = [&](std::size_t count, auto childAt) {
					if (count != end - begin) return false;
					for (std::size_t child = 0; child < count; ++child) {
						const auto index = readOrdinal(begin + child);
						const auto live = childAt(child);
						if (index == nullIndex) {
							if (live) return false;
						} else {
							if (!live || index >= nodes.size() || index > discovered || live != nodes[index]) return false;
							if (index == discovered) ++discovered;
						}
					}
					return true;
				};
				if (!inspect(nodes[i], i, check)) return false;
				begin = end;
			}
			return discovered == nodes.size() && row == rows.size();
		}

		struct Row
		{
			std::uint32_t ordinal;
			std::uint32_t end;
			bool operator==(const Row&) const = default;
		};
		static_assert(sizeof(Row) == 2 * sizeof(std::uint32_t));

		std::vector<Row> rows;
		std::vector<std::uint32_t> edges;
		std::vector<std::uint8_t> packed;
		std::size_t nodeCount{};
		std::size_t nodeBudget{instanceBytes / sizeof(void*)};
		bool recording{};
		bool valid{};
		std::uint8_t edgeWidth{};
	};
}
