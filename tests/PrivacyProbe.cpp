#include <source_location>
#include <spdlog/spdlog.h>

extern "C" __declspec(dllexport) const char* behavior_privacy_probe()
{
	spdlog::debug("BehaviorNodeIdLimitFix build qualification");
	return std::source_location::current().file_name();
}
