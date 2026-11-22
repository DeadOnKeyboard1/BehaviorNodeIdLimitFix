#include "OfflineGraph.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
	namespace fs = std::filesystem;
	using OfflineGraph::Status;
	constexpr std::uint32_t stateCount = 32769;
	constexpr std::uint32_t array = 0x320;
	constexpr std::uint32_t graph = 0x60;
	constexpr std::uint32_t machine = 0x1b0;
	constexpr std::size_t headerBytes = 208;
	std::uint32_t unknownClass{};
	std::size_t firstClipClass{};
	unsigned published{};
	unsigned checks{};

	void Require(bool value, const char* message)
	{
		++checks;
		if (!value) throw std::runtime_error(message);
	}

	template <class T>
	void Put(std::vector<char>& data, std::size_t offset, const T& value)
	{
		std::memcpy(data.data() + offset, &value, sizeof(T));
	}

	void Push(std::vector<char>& data, const auto& value)
	{
		const auto start = data.size();
		data.resize(start + sizeof(value));
		Put(data, start, value);
	}

	std::vector<char> Fixture(std::uint32_t count = stateCount)
	{
		const auto stateBase = array + count * 8 + 8;
		const auto clipBase = stateBase + count * 128;
		// Keep a low-node fixture large enough to enter the offline scanner.
		std::vector<char> names, data((std::max)(3u << 20, clipBase + count * 256)), local, global, objects;
		const auto addClass = [&](const char* name, std::uint32_t signature) {
			Push(names, signature);
			names.push_back(9);
			const auto nameAt = static_cast<std::uint32_t>(names.size());
			names.insert(names.end(), name, name + std::strlen(name) + 1);
			return nameAt;
		};
		const auto containerClass = addClass("hkRootLevelContainer", 0x2772c11e);
		const auto graphClass = addClass("hkbBehaviorGraph", 0xb1218f86);
		const auto machineClass = addClass("hkbStateMachine", 0x816c1dcb);
		const auto stateClass = addClass("hkbStateMachineStateInfo", 0x0ed7f9d0);
		const auto clipClass = addClass("hkbClipGenerator", 0x333b85b9);
		unknownClass = addClass("hkbBehaviorReferenceGenerator", 0xfcb5423);
		const auto mark = [&](std::uint32_t offset, std::uint32_t type) {
			Push(objects, offset);
			Push(objects, 0u);
			Push(objects, type);
		};
		const auto pointer = [&](std::uint32_t offset, std::uint32_t value) {
			Push(global, offset);
			Push(global, 2u);
			Push(global, value);
		};
		const auto localPointer = [&](std::uint32_t offset, std::uint32_t value) {
			Push(local, offset);
			Push(local, value);
		};
		mark(0, containerClass);
		localPointer(0, 16);
		Put(data, 8, 1u);
		pointer(32, graph);
		mark(graph, graphClass);
		localPointer(graph + 0x38, 0x190);
		constexpr char logicalName[] = "IndependentExample.hkb";
		std::memcpy(data.data() + 0x190, logicalName, sizeof(logicalName));
		pointer(graph + 0x80, machine);
		mark(machine, machineClass);
		Put(data, machine + 0x98, count);
		localPointer(machine + 0x90, array);
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto state = stateBase + i * 128;
			const auto clip = clipBase + i * 256;
			mark(state, stateClass);
			pointer(array + 8 * i, state);
			pointer(state + 0x58, clip);
		}
		const auto firstClass = objects.size() + 8;
		for (std::uint32_t i = 0; i < count; ++i) mark(clipBase + i * 256, clipClass);
		std::vector<char> file(headerBytes);
		Put(file, 0, 0x57e0e057u);
		Put(file, 4, 0x10c0c010u);
		Put(file, 12, 8u);
		Put(file, 16, 0x01000108u);
		Put(file, 20, 3u);
		Put(file, 24, 2u);
		Put(file, 36, containerClass);
		constexpr char version[] = "hk_2010.2.0-r1";
		std::memcpy(file.data() + 40, version, sizeof(version));
		const auto section = [&](std::size_t offset, const char* tag, const auto& fields) {
			std::memcpy(file.data() + offset, tag, std::strlen(tag));
			for (std::size_t i = 0; i < fields.size(); ++i) Put(file, offset + 20 + i * 4, fields[i]);
		};
		const auto ns = static_cast<std::uint32_t>(names.size());
		const auto start = static_cast<std::uint32_t>(headerBytes) + ns;
		const auto loc = static_cast<std::uint32_t>(data.size());
		const auto glob = loc + static_cast<std::uint32_t>(local.size());
		const auto virt = glob + static_cast<std::uint32_t>(global.size());
		const auto end = virt + static_cast<std::uint32_t>(objects.size());
		section(64, "__classnames__", std::array<std::uint32_t, 7>{headerBytes, ns, ns, ns, ns, ns, ns});
		section(112, "__types__", std::array{start, 0u, 0u, 0u, 0u, 0u, 0u});
		section(160, "__data__", std::array{start, loc, glob, virt, end, end, end});
		file.reserve(start + end);
		for (const auto* segment : {&names, &data, &local, &global, &objects}) file.insert(file.end(), segment->begin(), segment->end());
		if (count == stateCount) firstClipClass = start + virt + firstClass;
		return file;
	}
}

int main()
{
	try {
		std::atomic_bool stop{};
		auto file = Fixture();
		auto expected = OfflineGraph::Analyze(file, stop);
		Require(expected.status == Status::planned && expected.candidate, "A valid standalone packfile produced no offline plan");
		const auto& candidate = *expected.candidate;
		Require(candidate.name == "IndependentExample.hkb" && candidate.shape.nodes == stateCount + 1 &&
			candidate.payload.plan->stateMachines == std::vector<std::uint32_t>{0}, "Incorrect graph identity or state machines");
		GraphPlan::Signature independent;
		independent.Add(stateCount);
		for (unsigned i = 1; i <= stateCount; ++i) independent.Add(i);
		for (unsigned i = 0; i < stateCount; ++i) independent.Add(0);
		Require(candidate.shape.topology == independent.value && candidate.payload.links.IsPacked(),
			"Offline graph ordering differs from the expected live traversal");
		{
			std::vector<char> header(file.begin(), file.begin() + headerBytes);
			Require(!OfflineGraph::ScreenHeader(header, file.size()), "The valid root was rejected by the header screen");
			Put(header, 28, 1u);
			Require(OfflineGraph::ScreenHeader(header, file.size()) == Status::unsupported,
				"The header screen accepted a misaligned root");
			std::uint32_t dataBytes{};
			std::memcpy(&dataBytes, header.data() + 184, sizeof(dataBytes));
			Put(header, 28, dataBytes & ~7u);
			Require(OfflineGraph::ScreenHeader(header, file.size()) == Status::unsupported,
				"The header screen accepted a root without enough room for its variants");
			Put(header, 28, 0u);
			Put(header, 36, 4u);
			Require(OfflineGraph::ScreenHeader(header, file.size()) == Status::unsupported,
				"The header screen accepted a root class inside the class signature");
		}
		{
			std::uint32_t dataStart{};
			std::memcpy(&dataStart, file.data() + 180, sizeof(dataStart));
			const auto firstStateTransitions = dataStart + array + stateCount * 8 + 8 + 0x50;
			Put(file, firstStateTransitions, std::uint64_t{1});
			Require(OfflineGraph::Analyze(file, stop).status == Status::unsupported,
				"A non-null serialized transition pointer reached the offline plan");
			Put(file, firstStateTransitions, std::uint64_t{0});
		}

		const auto root = fs::temp_directory_path() / (L"offline-plan-test-" + std::to_wstring(GetCurrentProcessId()));
		const auto meshes = root / L"meshes";
		const auto sourceFolder = meshes / L"actors/character/behaviors";
		const auto cache = root / L"cache";
		Require(fs::create_directories(sourceFolder), "Could not create test assets");
		const auto source = sourceFolder / L"Example.hkx";
		const auto write = [&] {
			std::ofstream output(source, std::ios::binary);
			output.write(file.data(), static_cast<std::streamsize>(file.size()));
			Require(static_cast<bool>(output), "Fixture could not be written");
		};
		const auto publish = [](OfflineGraph::Candidate&& data) noexcept {
			if (data.shape.nodes != stateCount + 1) return false;
			++published;
			return true;
		};
		const auto prepare = [&] {
			return OfflineGraph::Prepare(meshes, cache, stop, publish, nullptr);
		};
		write();
		const auto first = prepare();
		Require(first.checked == 1 && first.built == 1 && first.admitted == 1 && !first.reused &&
			first.sourceBytesRead == file.size(),
			"Cold startup did not bake the graph");
		const auto second = prepare();
		Require(second.reused == 1 && !second.built && second.admitted == 1 && published == 2 && second.sourceBytesRead == 0,
			"Restart rebuilt an unchanged graph or lost its plan");

		const auto cacheFile = fs::directory_iterator(cache)->path();
		{
			std::ofstream broken(cacheFile, std::ios::binary | std::ios::app);
			broken.put('x');
		}
		const auto repaired = prepare();
		Require(repaired.built == 1 && repaired.admitted == 1 && !repaired.reused, "A damaged candidate was not rebuilt");
		const auto originalTime = fs::last_write_time(source);
		Put(file, firstClipClass, unknownClass);
		Require(OfflineGraph::Analyze(file, stop).status == Status::unsupported, "An external reference was assigned local children");
		write();
		fs::last_write_time(source, originalTime);
		const auto changed = prepare();
		Require(changed.unsupported == 1 && !changed.reused && !changed.admitted, "A changed file reused its previous plan");
		const auto skipped = prepare();
		Require(skipped.unsupported == 1 && skipped.reused == 1, "An unchanged unsupported file was reanalyzed");
		Require(OfflineGraph::Analyze(std::span<const char>{file}.first(file.size() - 1), stop).status == Status::unsupported,
			"Truncated packfile was accepted");
		std::vector<char> invalidSections(file.begin(), file.begin() + headerBytes);
		Require(OfflineGraph::Analyze(invalidSections, stop).status == Status::unsupported, "Invalid section range was accepted");

		file = Fixture(12);
		write();
		const auto shortGraph = prepare();
		Require(shortGraph.belowLimit == 1 && shortGraph.sourceBytesRead == OfflineGraph::packfileHeaderBytes && !shortGraph.reused,
			"Below-limit graph performed more than a header read");
		Require(OfflineGraph::Analyze(file, stop).status == Status::belowLimit,
			"Header screen and full analysis disagree about the node limit");
		Put(file, 12, 9u);
		write();
		const auto badHeader = prepare();
		Require(badHeader.unsupported == 1 && badHeader.sourceBytesRead == OfflineGraph::packfileHeaderBytes && !badHeader.reused,
			"Invalid packfile header performed more than a header read");
		Put(file, 12, 8u);
		Put(file, 28, 0xfffffff8u);
		write();
		const auto badRoot = prepare();
		Require(badRoot.unsupported == 1 && badRoot.sourceBytesRead == OfflineGraph::packfileHeaderBytes && !badRoot.reused,
			"An out-of-range root triggered a full packfile read");
		{
			const auto creatures = meshes / L"actors/aaa-creature/behaviors";
			const auto firstPerson = meshes / L"actors/character/_1stperson/behaviors";
			Require(fs::create_directories(creatures) && fs::create_directories(firstPerson), "Could not create priority fixtures");
			std::vector<fs::path> fillers;
			for (unsigned i = 0; i < 36; ++i) {
				const auto path = creatures / (std::to_wstring(i) + L".hkx");
				fs::create_hard_link(source, path);
				fillers.push_back(path);
			}
			const auto playerGraph = firstPerson / L"Player.hkx";
			const auto playable = Fixture();
			{
				std::ofstream output(playerGraph, std::ios::binary);
				output.write(playable.data(), static_cast<std::streamsize>(playable.size()));
				Require(static_cast<bool>(output), "Could not write the first-person graph");
			}
			const auto priorityCache = root / L"priority-cache";
			const auto firstPriority = OfflineGraph::Prepare(meshes, priorityCache, stop, publish, nullptr);
			Require(firstPriority.checked == 32 && firstPriority.built == 1 && firstPriority.admitted == 1 &&
				firstPriority.unsupported == 31 && firstPriority.sourceBytesRead == playable.size() + 31 * OfflineGraph::packfileHeaderBytes,
				"Creature files crowded the first-person character graph out of the bounded startup catalog");
			const auto warmPriority = OfflineGraph::Prepare(meshes, priorityCache, stop, publish, nullptr);
			Require(warmPriority.checked == firstPriority.checked && warmPriority.reused == firstPriority.checked &&
				warmPriority.admitted == 1 && warmPriority.sourceBytesRead == 0,
				"A restart reanalyzed unchanged graphs in the bounded catalog");
			std::vector<fs::path> cacheFiles;
			for (const auto& item : fs::directory_iterator(priorityCache)) cacheFiles.push_back(item.path());
			for (const auto& path : cacheFiles) fs::remove(path);
			fs::remove(priorityCache);
			for (const auto& path : fillers) fs::remove(path);
			fs::remove(creatures);
			fs::remove(creatures.parent_path());
			fs::remove(playerGraph);
			fs::remove(firstPerson);
			fs::remove(firstPerson.parent_path());
		}
		stop.store(true);
		Require(OfflineGraph::Analyze(file, stop).status == Status::cancelled && prepare().interrupted,
			"Cancellation did not stop offline preparation");
		fs::remove(cacheFile);
		fs::remove(cache);
		fs::remove(source);
		for (auto path = sourceFolder; path != root; path = path.parent_path()) fs::remove(path);
		fs::remove(root);
		std::cout << checks << " offline graph checks passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}

