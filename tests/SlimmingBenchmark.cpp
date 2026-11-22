#include "GraphPlan.h"
#include "InstanceTopology.h"
#ifndef BEHAVIOR_REFERENCE_107
#	include "GraphSlimming.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
	struct Node
	{
		std::uint32_t first{};
		std::uint32_t size{};
		bool machine{};
	};

	struct Fixture
	{
		explicit Fixture(std::uint32_t count) : nodes(count)
		{
			order.reserve(count);
			for (auto& node : nodes) order.push_back(&node);
			edges.reserve(count + count / 10);
			nodes[0] = {0, count, false};
			edges.push_back(GraphPlan::noNode);
			for (std::uint32_t i = 1; i < count; ++i) edges.push_back(i);
			for (std::uint32_t i = 1; i < count; ++i) {
				auto& node = nodes[i];
				node.machine = (i % 101 == 0);
				if (i % 23 != 0) continue;
				node.first = static_cast<std::uint32_t>(edges.size());
				node.size = 2;
				edges.push_back(i - 1);
				edges.push_back(i);
			}
		}

		std::vector<Node> nodes;
		std::vector<Node*> order;
		std::vector<std::uint32_t> edges;
	};

	struct Capture
	{
		InstanceTopology::Trace links{true};
		GraphPlan::Shape shape;
		GraphPlan::Entry plan;
	};

	Capture Analyze(const Fixture& fixture, [[maybe_unused]] bool fold)
	{
		Capture captured;
		GraphPlan::Signature topology;
		GraphPlan::StateMachines stateMachines(static_cast<std::uint32_t>(fixture.nodes.size()));
		for (std::uint32_t i = 0; i < fixture.nodes.size(); ++i) {
			const auto& node = fixture.nodes[i];
#ifndef BEHAVIOR_REFERENCE_107
			if (!node.size) {
				if (fold) topology.AddEmptyNode();
				else topology.Add(0);
				captured.links.RecordEmptyNode();
				if (node.machine) stateMachines.Add(i);
				continue;
			}
#endif
			topology.Add(node.size);
			auto ordinals = captured.links.RecordChildren(node.size);
			for (std::uint32_t child = 0; child < node.size; ++child) {
				const auto ordinal = fixture.edges[node.first + child];
				topology.Add(ordinal);
				ordinals[child] = ordinal;
			}
			if (node.machine) stateMachines.Add(i);
		}
		captured.shape = stateMachines.Describe(topology.value);
		captured.plan = stateMachines.Build(topology.value);
		return captured;
	}

	bool Replay(const Fixture& fixture, const InstanceTopology::Trace& links, const GraphPlan::Entry& plan)
	{
		std::size_t nextMachine{};
		const auto& machines = plan.stateMachines;
		return links.Matches(fixture.order.front(), std::span<Node* const>{fixture.order},
			[&](const Node* node, std::size_t ordinal, auto check) {
				const bool expected = nextMachine < machines.size() && machines[nextMachine] == ordinal;
				if (node->machine != expected) return false;
				if (expected) ++nextMachine;
				return check(node->size, [&](std::size_t edge) {
					const auto child = fixture.edges[node->first + edge];
					return child == GraphPlan::noNode ? nullptr : fixture.order[child];
				});
			}) && nextMachine == machines.size();
	}

	template <class Work>
	void Measure(const char* label, Work work)
	{
		using Clock = std::chrono::steady_clock;
		std::array<double, 120> times{};
		std::uint64_t digest{};
		std::size_t memory{};
		for (unsigned i = 0; i < times.size() + 8; ++i) {
			const auto start = Clock::now();
			const auto [bytes, value] = work(i);
			const auto time = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
			digest = (digest ^ value) * 1099511628211ULL + i;
			memory = bytes;
			if (i >= 8) times[i - 8] = time;
		}
		std::sort(times.begin(), times.end());
		std::cout << label << ',' << times[times.size() / 2] << ',' << times[times.size() * 95 / 100]
			<< ',' << memory << ',' << digest << '\n';
	}
}

int main()
{
	constexpr std::uint32_t count = 40'000;
	std::array fixtures{Fixture(count), Fixture(count), Fixture(count), Fixture(count)};
	std::cout << "mode,median_us,p95_us,resident_payload_bytes,checksum\n";
#ifdef BEHAVIOR_REFERENCE_107
	std::array<std::shared_ptr<const Capture>, 4> live;
	const auto payloadBytes = [](const Capture& memo) {
		return sizeof(memo.plan) + sizeof(memo.links) + memo.links.CapacityBytes() +
			memo.plan.stateMachines.capacity() * sizeof(std::uint32_t);
	};
	std::size_t warmBytes{};
	Measure("1.0.7 reference full analysis", [&](unsigned i) {
		const auto actor = i % fixtures.size();
		live[actor] = std::make_shared<const Capture>(Analyze(fixtures[actor], false));
		warmBytes = 0;
		for (const auto& memo : live) if (memo) warmBytes += payloadBytes(*memo);
		return std::pair{warmBytes, live[actor]->shape.topology};
	});
	InstanceTopology::Cache<std::size_t, Capture> cache;
	for (unsigned i = 0; i < live.size(); ++i) {
		if (!cache.Remember(i, live[i], payloadBytes(*live[i]))) return 3;
	}
	Measure("1.0.7 reference warm instance cache", [&](unsigned i) {
		const auto actor = i % fixtures.size();
		const auto memo = cache.Find(actor);
		if (!memo || !Replay(fixtures[actor], memo->links, memo->plan)) std::exit(4);
		return std::pair{warmBytes, memo->plan.topology};
	});
#else
	GraphSlimming::Settings safe;
	GraphSlimming::Settings disabled = safe;
	disabled.enabled = false;
	GraphSlimming::Engine engine;
	std::array<std::shared_ptr<const GraphSlimming::Payload>, 4> live;
	const auto retained = [&] {
		std::size_t bytes{};
		for (unsigned i = 0; i < live.size(); ++i) {
			if (live[i] && std::find(live.begin(), live.begin() + i, live[i]) == live.begin() + i) {
				bytes += live[i]->CapacityBytes();
			}
		}
		return bytes;
	};
	const auto rebuild = [&](unsigned i, const auto& settings) {
		const auto actor = i % fixtures.size();
		auto capture = Analyze(fixtures[actor], settings.Optimize());
		auto result = engine.Prepare(capture.shape, std::move(capture.links),
			std::make_shared<const GraphPlan::Entry>(std::move(capture.plan)), settings);
		if (!result.payload) std::exit(2);
		live[actor] = std::move(result.payload);
		return capture.shape.topology;
	};
	const auto build = [&](unsigned i, const auto& settings) {
		const auto key = rebuild(i, settings);
		return std::pair{retained(), key};
	};
	Measure("new with slimming disabled", [&](unsigned i) { return build(i, disabled); });
	live.fill(nullptr);
	Measure("new safe slimming full analysis", [&](unsigned i) { return build(i, safe); });
	InstanceTopology::Cache<std::size_t, GraphSlimming::Payload> cache;
	for (unsigned i = 0; i < live.size(); ++i) {
		if (!cache.Remember(i, live[i], 0, {live[i].get(), live[i]->CapacityBytes()})) return 3;
	}
	const auto warmBytes = retained();
	Measure("new safe slimming warm instance cache", [&](unsigned i) {
		const auto actor = i % fixtures.size();
		const auto memo = cache.Find(actor);
		const auto& fixture = fixtures[actor];
		if (!memo || !Replay(fixture, memo->links, *memo->plan)) std::exit(4);
		return std::pair{warmBytes, memo->plan->topology};
	});
#endif
}
