#pragma once

#include "GraphPlan.h"
#include "ProcessedGraphCache.h"
#include <new>
#include <span>

namespace InstanceTopology
{
	// The process-local record describes edges in first-discovery order.
	// It never owns or extends the lifetime of an engine node.
	class Trace
	{
	public:
		Trace() = default;
		explicit Trace(bool record) : recording(record) {}

		void Reset() noexcept
		{
			nodeCount = 0;
			rows.clear();
			edges.clear();
			recording = true;
		}

		[[nodiscard]] std::span<std::uint32_t> RecordChildren(std::size_t count) noexcept
		{
			if (!recording) return {};
			const auto ordinal = nodeCount++;
			if (nodeCount > GraphPlan::maxNodes || count > instanceBytes / sizeof(std::uint32_t) - edges.size()) {
				recording = false;
				return {};
			}
			if (!count) return {};
			const auto estimate = nodeCount * sizeof(void*) + (rows.size() + 1) * sizeof(Row) +
				(edges.size() + count) * sizeof(std::uint32_t);
			if (estimate > instanceBytes) {
				recording = false;
				return {};
			}
			try {
				const auto start = edges.size();
				edges.resize(start + count);
				rows.push_back({static_cast<std::uint32_t>(ordinal), static_cast<std::uint32_t>(edges.size())});
				return std::span{edges}.subspan(start, count);
			} catch (const std::bad_alloc&) {
				recording = false;
				return {};
			}
		}

		[[nodiscard]] bool Complete(std::size_t nodes) const noexcept
		{
			return recording && nodes != 0 && nodeCount == nodes;
		}

		[[nodiscard]] std::size_t CapacityBytes() const noexcept
		{
			return rows.capacity() * sizeof(Row) + edges.capacity() * sizeof(std::uint32_t);
		}

		// The root establishes liveness for ordinal zero. A matching live edge
		// establishes liveness for the next ordinal before inspect may read it.
		// A stale address is used only for pointer comparison until that point.
		template <class Node, class Inspect>
		[[nodiscard]] bool Matches(Node* root, std::span<Node* const> nodes, Inspect inspect) const
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
						const auto index = edges[begin + child];
						const auto live = childAt(child);
						if (index == GraphPlan::noNode) {
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

	private:
		struct Row
		{
			std::uint32_t ordinal;
			std::uint32_t end;
		};

		std::vector<Row> rows;
		std::vector<std::uint32_t> edges;
		std::size_t nodeCount{};
		bool recording{};
	};
}
