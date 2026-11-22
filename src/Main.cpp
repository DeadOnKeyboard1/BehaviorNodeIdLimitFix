#include "Hooks.h"
#include "GraphPerformance.h"
#include "GraphSlimming.h"
#include "StartupPreload.h"

#include <filesystem>
#include <fstream>

// This plugin uses reviewed Havok layouts. The CommonLib CMake helper does
// not set the post-629 compatibility bit when declaring layout dependence.
SKSEPluginInfo(
	.Version = { BEHAVIOR_VERSION_MAJOR, BEHAVIOR_VERSION_MINOR, BEHAVIOR_VERSION_PATCH, 0 },
	.Name = "BehaviorNodeIdLimitFix",
	.Author = "DoK",
	.StructCompatibility = SKSE::StructCompatibility::Dependent,
	.RuntimeCompatibility = { SKSE::VersionIndependence::AddressLibrary, true }
);

namespace
{
	void InitializeLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}

		*path /= "BehaviorNodeIdLimitFix.log"sv;
		std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
		try {
			sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		} catch (const spdlog::spdlog_ex&) {
			return;
		}
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::warn);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v"s);
	}

	void LoadOptimizationSettings()
	{
		GraphSlimming::Settings settings;
		try {
			const auto directory = std::filesystem::path(REL::Module::get().filePath().data()).parent_path();
			if (directory.is_absolute()) {
				std::ifstream input(directory / "Data/SKSE/Plugins/BehaviorNodeIdLimitFix.ini", std::ios::binary);
				if (input) settings = GraphSlimming::ReadSettings(input);
			}
		} catch (const std::exception&) {
			logger::warn("Could not read BehaviorNodeIdLimitFix.ini. Using defaults.");
		}
		GraphSlimming::Configure(settings);
		logger::info("Node optimization enabled={} packing={} deduplication={} instanceFastPath={} persistentCache={} performanceLogging={} adaptivePreloading={}",
			settings.enabled, settings.Optimize(), settings.SharePlans(), settings.instanceFastPath,
			settings.persistentCache, settings.performanceLogging,
			settings.adaptivePreloading && (settings.persistentCache || settings.preloadAnimationFiles));
		if (settings.experimental) logger::info("Experimental optimization requested. This build has zero qualified experimental rules.");
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::InitInfo initInfo{};
	initInfo.log = false;
	initInfo.trampoline = true;
	initInfo.trampolineSize = 128;
	SKSE::Init(a_skse, initInfo);

	InitializeLogging();
	LoadOptimizationSettings();

	const auto pluginName = SKSE::GetPluginName();
	const auto pluginVersion = SKSE::GetPluginVersion();
	const auto runtimeVersion = REL::Module::get().version();
	logger::info("{} v{} loading on Skyrim {}.", pluginName, pluginVersion.string("."), runtimeVersion.string("."));

	if (SKSE::GetTrampoline().empty()) {
		SKSE::GetTrampoline().create(128);
	}

	if (!Hooks::Install()) {
		logger::critical("Failed to install BehaviorNodeIdLimitFix hooks.");
		if (auto* log = spdlog::default_logger_raw()) {
			log->flush();
		}
		return false;
	}

	if (GraphSlimming::Options().performanceLogging && !GraphPerformance::Start()) {
		logger::warn("Could not start periodic graph performance logging.");
	}
	StartupPreload::Install();
	logger::info("{} initialized successfully.", pluginName);
	if (auto* log = spdlog::default_logger_raw()) {
		log->flush();
	}
	return true;
}
