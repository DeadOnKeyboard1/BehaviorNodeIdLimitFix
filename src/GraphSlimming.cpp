#include "GraphSlimming.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

namespace
{
	GraphSlimming::Settings activeSettings;

	std::string_view Trim(std::string_view value)
	{
		constexpr std::string_view space = " \t\r\n";
		const auto begin = value.find_first_not_of(space);
		return begin == value.npos ? std::string_view{} : value.substr(begin, value.find_last_not_of(space) - begin + 1);
	}

	bool SameText(std::string_view lhs, std::string_view rhs)
	{
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
		if (lhs.size() != rhs.size()) return false;
		for (std::size_t i = 0; i < lhs.size(); ++i) if (lower(lhs[i]) != lower(rhs[i])) return false;
		return true;
	}

	bool SamePayload(const GraphSlimming::Payload& candidate, const InstanceTopology::Trace& links,
		const GraphPlan::Entry& plan)
	{
		const auto& saved = *candidate.plan;
		return (&saved == &plan || (saved.topology == plan.topology && saved.nodes == plan.nodes &&
			saved.stateMachines == plan.stateMachines)) && candidate.links.SameLinks(links);
	}
}

namespace GraphSlimming
{
	Settings ReadSettings(std::istream& input)
	{
		Settings result;
		constexpr std::pair<std::string_view, bool Settings::*> keys[] = {
			{"bEnableNodeOptimization", &Settings::enabled},
			{"bEnableSafeDeduplication", &Settings::safeDeduplication},
			{"bEnableGraphOptimization", &Settings::graphOptimization},
			{"bEnableInstanceFastPath", &Settings::instanceFastPath},
			{"bEnablePersistentOptimizationCache", &Settings::persistentCache},
			{"bEnableExperimentalOptimizations", &Settings::experimental},
			{"bEnablePerformanceLogging", &Settings::performanceLogging},
			{"bEnableAdaptivePreloading", &Settings::adaptivePreloading},
			{"bShowPreloadProgress", &Settings::showPreloadProgress},
			{"bPreloadAnimationFiles", &Settings::preloadAnimationFiles},
			{"bPrecomputeOfflineGraphs", &Settings::offlineGraphPreparation},
		};
		struct NumericSetting
		{
			std::string_view name;
			unsigned Settings::*member;
			unsigned minimum;
			unsigned maximum;
		};
		constexpr NumericSetting numericKeys[] = {
			{"iPreloadMaxPlans", &Settings::preloadMaxPlans, 1, 256},
			{"iPreloadAssetMaxMiB", &Settings::preloadAssetMaxMiB, 64, 8192},
		};
		bool section{};
		std::string line;
		while (std::getline(input, line)) {
			std::string_view row(line);
			if (row.starts_with("\xEF\xBB\xBF")) row.remove_prefix(3);
			row = Trim(row.substr(0, row.find_first_of(";#")));
			if (row.empty()) continue;
			if (row.front() == '[' && row.back() == ']') {
				section = SameText(Trim(row.substr(1, row.size() - 2)), "NodeOptimization");
				continue;
			}
			const auto equals = row.find('=');
			if (!section || equals == row.npos) continue;
			const auto key = Trim(row.substr(0, equals));
			const auto value = Trim(row.substr(equals + 1));
			if (value.empty()) continue;
			const auto numericKey = std::ranges::find_if(numericKeys, [&](const auto& item) { return SameText(key, item.name); });
			if (numericKey != std::end(numericKeys)) {
				unsigned numeric{};
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), numeric);
				if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
					result.*(numericKey->member) = std::clamp(numeric, numericKey->minimum, numericKey->maximum);
				}
				continue;
			}
			const bool on = SameText(value, "true") || value == "1";
			const bool off = SameText(value, "false") || value == "0";
			if (!on && !off) continue;
			for (const auto& [name, member] : keys) {
				if (SameText(key, name)) {
					result.*member = on;
					break;
				}
			}
		}
		return result;
	}

	const Settings& Options() noexcept { return activeSettings; }

	void Configure(Settings settings) noexcept { activeSettings = settings; }

	Preparation Engine::Prepare(GraphPlan::Shape shape, InstanceTopology::Trace links,
		std::shared_ptr<const GraphPlan::Entry> plan, const Settings& settings)
	{
		Preparation result;
		if (!plan || !links.Complete(plan->nodes) || shape.nodes != plan->nodes ||
			shape.topology != plan->topology || !plan->WellFormed()) return result;
		result.bytesBefore = sizeof(Payload) + links.CapacityBytes() + sizeof(GraphPlan::Entry) +
			plan->stateMachines.capacity() * sizeof(std::uint32_t);
		const bool share = settings.SharePlans();
		const bool packing = settings.Optimize();
		const auto key = share ? shape.CacheVariant() : 0;
		auto* bucket = share ? &index[key % index.size()] : nullptr;
		using Candidates = std::array<std::shared_ptr<const Payload>, candidatesPerBucket>;
		const auto candidatesUnderLock = [&] {
			Candidates found;
			for (std::size_t i = 0; i < found.size(); ++i) {
				const auto& entry = bucket->candidates[i];
				if (entry.key == key && entry.packing == packing) found[i] = entry.payload.lock();
			}
			return found;
		};
		const auto tryReuse = [&](const Candidates& candidates, const Candidates& checked,
			const InstanceTopology::Trace& wantedLinks, const GraphPlan::Entry& wantedPlan) {
			for (const auto& payload : candidates) {
				if (!payload || std::ranges::find(checked, payload) != checked.end() ||
					!SamePayload(*payload, wantedLinks, wantedPlan)) continue;
				result.bytesAfter = payload->CapacityBytes();
				result.shared = true;
				result.packed = payload->links.IsPacked();
				result.payload = payload;
				return true;
			}
			return false;
		};
		Candidates checked;
		std::uint64_t revision{};
		if (bucket) {
			{
				std::scoped_lock lock(bucket->mutex);
				revision = bucket->revision;
				checked = candidatesUnderLock();
			}
			if (tryReuse(checked, {}, links, *plan)) return result;
		}
		result.packed = packing && links.PackOrdinals();
		result.payload = std::make_shared<const Payload>(Payload{std::move(links), std::move(plan)});
		result.bytesAfter = result.payload->CapacityBytes();
		for (int attempt = 0; bucket && attempt < 2; ++attempt) {
			Candidates latest;
			{
				std::scoped_lock lock(bucket->mutex);
				if (revision == bucket->revision) {
					auto* target = &bucket->candidates.front();
					for (auto& entry : bucket->candidates) {
						if (entry.payload.expired()) {
							target = &entry;
							break;
						}
						if (entry.published < target->published) target = &entry;
					}
					*target = {key, packing, result.payload, ++bucket->revision};
					break;
				}
				revision = bucket->revision;
				latest = candidatesUnderLock();
			}
			// Compare only candidates published since the last check.
			if (tryReuse(latest, checked, result.payload->links, *result.payload->plan)) break;
			checked = std::move(latest);
		}
		return result;
	}
}
