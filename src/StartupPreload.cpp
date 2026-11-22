#include "StartupPreload.h"
#include "AnimationPreload.h"
#include "GraphCache.h"
#include "GraphSlimming.h"
#include "OfflineGraph.h"

#include <Windows.h>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <new>
#include <utility>

namespace
{
	using Clock = std::chrono::steady_clock;
	using Messages = SKSE::MessagingInterface;
	using namespace std::chrono_literals;

	constexpr wchar_t windowClass[] = L"BehaviorNodeIdLimitFix.StartupProgress";
	constexpr UINT_PTR refreshTimer = 1;
	constexpr int cancelButton = 1;
	constexpr std::uint64_t countMask = 0xFFFFu;
	enum class Phase : std::uint64_t { initial, plans, offline, catalog, behaviors, animations };
	std::atomic_flag started{};
	std::atomic_bool startupEnded{};
	std::atomic_bool finished{};
	std::atomic<HANDLE> startupWorker{};
	std::atomic<std::uint64_t> progress{};
	HMODULE module{};
	HANDLE windowThread{};

	struct StartupEvents
	{
		StartupEvents() = default;
		StartupEvents(const StartupEvents&) = delete;
		StartupEvents& operator=(const StartupEvents&) = delete;

		~StartupEvents()
		{
			if (cancelled) CloseHandle(cancelled);
			if (ready) CloseHandle(ready);
		}

		const HANDLE cancelled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
		const HANDLE ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
	};

	using EventOwner = std::shared_ptr<StartupEvents>;
	EventOwner startupEvents;

	HANDLE StartThread(LPTHREAD_START_ROUTINE work, const EventOwner& events) noexcept
	{
		// Transfer a separate owner to the callback before it can start.
		auto* owner = events ? new (std::nothrow) EventOwner(events) : nullptr;
		if (events && !owner) return nullptr;
		const auto thread = CreateThread(nullptr, 0, work, owner, 0, nullptr);
		if (!thread) delete owner;
		return thread;
	}

	struct WindowState
	{
		const StartupEvents* events;
		std::uint64_t lastProgress{};
		UINT dpi{96};
	};

	void RequestCancel(const StartupEvents* events) noexcept
	{
		startupEnded.store(true, std::memory_order_release);
		if (events) SetEvent(events->cancelled);
	}

	void PaintProgress(HWND window, const WindowState& state) noexcept
	{
		PAINTSTRUCT paint{};
		const HDC dc = BeginPaint(window, &paint);
		if (!dc) return;
		const auto scale = [&](int value) { return MulDiv(value, static_cast<int>(state.dpi), 96); };
		const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
		RECT bounds{};
		GetClientRect(window, &bounds);
		SetDCBrushColor(dc, RGB(26, 30, 34));
		FillRect(dc, &bounds, brush);

		const int padding = scale(20);
		const RECT track{padding, scale(69), bounds.right - padding, scale(78)};
		SetDCBrushColor(dc, RGB(68, 74, 80));
		FillRect(dc, &track, brush);
		const auto phase = static_cast<Phase>(state.lastProgress >> 32);
		const auto total = static_cast<int>((state.lastProgress >> 16) & countMask);
		const auto checked = static_cast<int>(state.lastProgress & countMask);
		if (total) {
			const RECT fill{track.left, track.top, track.left + MulDiv(track.right - track.left, checked, total), track.bottom};
			SetDCBrushColor(dc, RGB(228, 186, 107));
			FillRect(dc, &fill, brush);
		}

		const auto font = GetStockObject(DEFAULT_GUI_FONT);
		const auto previous = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(241, 243, 245));
		constexpr wchar_t title[] = L"BehaviorNodeIdLimitFix";
		TextOutW(dc, padding, scale(15), title, static_cast<int>(std::size(title) - 1));
		SetTextColor(dc, RGB(196, 203, 210));
		wchar_t count[64]{};
		const wchar_t* label = phase == Phase::plans ? L"Behavior plans checked" :
			(phase == Phase::offline ? L"Offline graphs checked" :
			(phase == Phase::behaviors ? L"Behavior files checked" : L"Animation files checked"));
		const auto length = phase == Phase::initial ? swprintf_s(count, std::size(count), L"Preparing startup cache...") :
			(phase == Phase::catalog ? swprintf_s(count, std::size(count), L"Finding behavior and animation files...") :
				swprintf_s(count, std::size(count), L"%ls  %d / %d", label, checked, total));
		if (length > 0) TextOutW(dc, padding, scale(40), count, length);
		SelectObject(dc, previous);
		EndPaint(window, &paint);
	}

	LRESULT CALLBACK ProgressWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
	{
		if (message == WM_NCCREATE) {
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}
		auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		switch (message) {
		case WM_COMMAND:
			if (LOWORD(wparam) == cancelButton && HIWORD(wparam) == BN_CLICKED) {
				RequestCancel(state ? state->events : nullptr);
				DestroyWindow(window);
				return 0;
			}
			break;
		case WM_CLOSE:
			RequestCancel(state ? state->events : nullptr);
			DestroyWindow(window);
			return 0;
		case WM_PAINT:
			if (state) PaintProgress(window, *state);
			return 0;
		case WM_TIMER: {
			if (!state) return 0;
			if (startupEnded.load(std::memory_order_acquire) || finished.load(std::memory_order_acquire)) {
				DestroyWindow(window);
				return 0;
			}
			const auto latest = progress.load(std::memory_order_acquire);
			if (latest != state->lastProgress) {
				state->lastProgress = latest;
				InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, wparam, lparam);
	}

	DWORD WINAPI ShowProgress(void* context) noexcept
	{
		const std::unique_ptr<EventOwner> events(static_cast<EventOwner*>(context));
		if (!events || startupEnded.load(std::memory_order_acquire) || finished.load(std::memory_order_acquire)) return 0;
		WNDCLASSW type{};
		type.lpfnWndProc = ProgressWindow;
		type.hInstance = module;
		type.lpszClassName = windowClass;
		if (!RegisterClassW(&type)) return 0;

		WindowState state{events->get(), progress.load(std::memory_order_acquire)};
		const auto foreground = GetForegroundWindow();
		MONITORINFO monitor{};
		monitor.cbSize = sizeof(monitor);
		const auto screen = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
		if (GetMonitorInfoW(screen, &monitor)) {
			if (const auto dc = GetDC(nullptr)) {
				const auto dpi = GetDeviceCaps(dc, LOGPIXELSX);
				ReleaseDC(nullptr, dc);
				if (dpi > 0) state.dpi = static_cast<UINT>(dpi);
			}
			const auto scale = [&](int value) { return MulDiv(value, static_cast<int>(state.dpi), 96); };
			const int width = scale(450);
			const int height = scale(161);
			const auto& area = monitor.rcWork;
			const int x = area.left + (area.right - area.left - width) / 2;
			const int y = area.top + (area.bottom - area.top - height) / 2;
			const auto window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, L"Skyrim startup",
				WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height,
				nullptr, nullptr, module, &state);
			if (window) {
				RECT client{};
				GetClientRect(window, &client);
				const auto button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
					client.right - scale(125), scale(88), scale(105), scale(27), window,
					reinterpret_cast<HMENU>(static_cast<INT_PTR>(cancelButton)), module, nullptr);
				if (button && SetTimer(window, refreshTimer, 33, nullptr) &&
					SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
					SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
					MSG message{};
					while (GetMessageW(&message, nullptr, 0, 0) > 0) {
						if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
							SendMessageW(window, WM_CLOSE, 0, 0);
							continue;
						}
						TranslateMessage(&message);
						DispatchMessageW(&message);
					}
				}
				if (IsWindow(window)) DestroyWindow(window);
			}
		}
		UnregisterClassW(windowClass, module);
		return 0;
	}

	void ReportProgress(Phase phase, unsigned checked, unsigned selected) noexcept
	{
		selected = (std::min)(selected, static_cast<unsigned>(countMask));
		checked = (std::min)(checked, selected);
		progress.store((static_cast<std::uint64_t>(phase) << 32) | (std::uint64_t{selected} << 16) | checked,
			std::memory_order_release);
	}

	DWORD WINAPI WarmCache(void* context) noexcept
	{
		std::unique_ptr<EventOwner> events(static_cast<EventOwner*>(context));
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		const auto start = Clock::now();
		const auto& settings = GraphSlimming::Options();
		const auto deadline = (Clock::time_point::max)();
		const auto stats = settings.persistentCache ? GraphCache::Preload(settings.preloadMaxPlans, deadline, startupEnded,
			[](unsigned checked, unsigned selected) noexcept { ReportProgress(Phase::plans, checked, selected); }) :
			GraphCache::PreloadStats{};
		const auto plansDone = Clock::now();
		OfflineGraph::Summary offline;
		try {
			const auto runtime = REL::Module::get().version();
			const bool verified = runtime == REL::Version{1, 5, 97, 0} || runtime == REL::Version{1, 6, 1170, 0} ||
				runtime == REL::Version{1, 7, 104, 0};
			if (verified && settings.persistentCache && settings.offlineGraphPreparation &&
				!startupEnded.load(std::memory_order_relaxed)) {
				const auto game = std::filesystem::path(REL::Module::get().filePath().data()).parent_path();
				offline = OfflineGraph::Prepare(game / L"Data/meshes", game / L"Data/SKSE/Plugins/BehaviorNodeIdLimitFix/Cache/Offline",
					startupEnded, GraphCache::AdmitOffline,
					[](unsigned checked, unsigned selected) noexcept { ReportProgress(Phase::offline, checked, selected); });
			}
		} catch (const std::exception&) {
			offline.interrupted = true;
		}
		const auto offlineDone = Clock::now();

		AnimationPreload::Summary animationStats;
		std::uint64_t assetBudget{};
		if (settings.preloadAnimationFiles && !startupEnded.load(std::memory_order_relaxed) && plansDone < deadline) {
			try {
				MEMORYSTATUSEX memory{};
				memory.dwLength = sizeof(memory);
				if (GlobalMemoryStatusEx(&memory)) assetBudget = AnimationPreload::BudgetBytes(memory.ullAvailPhys, settings.preloadAssetMaxMiB);
				if (assetBudget) {
					const auto game = std::filesystem::path(REL::Module::get().filePath().data()).parent_path();
					const auto meshes = game / L"Data/meshes";
					const auto indexFile = game / L"Data/SKSE/Plugins/BehaviorNodeIdLimitFix/Cache/HKXIndex.bin";
					const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
					animationStats = AnimationPreload::Warm(meshes, assetBudget, deadline, startupEnded,
						[](bool behavior, unsigned checked, unsigned selected) noexcept {
							const auto phase = behavior ? Phase::behaviors : Phase::animations;
							ReportProgress(selected ? phase : Phase::catalog, checked, selected);
						}, indexFile);
					if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
				}
			} catch (const std::exception&) {
				animationStats.interrupted = true;
			}
		}
		const auto animationDone = Clock::now();
		// The gate protects cache preparation. A delayed log write must not
		// hold the main menu once all prepared data has been published.
		finished.store(true, std::memory_order_release);
		if (events) {
			SetEvent((*events)->ready);
			events.reset();
		}
		if (settings.performanceLogging) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(plansDone - start).count();
			logger::info("Startup preload: selected={} checked={} unusable={} warmed={} prepared={} topologies={} planBytes={} idBytes={} "
				"topologyBytes={} elapsed={}us interrupted={}", stats.selected, stats.checked, stats.unusable, stats.warmed, stats.prepared,
				stats.baked, stats.bytes, stats.preparedBytes, stats.topologyBytes, elapsed, stats.interrupted);
			if (settings.offlineGraphPreparation && settings.persistentCache) {
				const auto offlineElapsed = std::chrono::duration_cast<std::chrono::microseconds>(offlineDone - plansDone).count();
				logger::info("Startup offline graphs: checked={} reused={} built={} admitted={} belowLimit={} unsupported={} "
					"sourceBytesRead={} elapsed={}us interrupted={}", offline.checked, offline.reused, offline.built, offline.admitted,
					offline.belowLimit, offline.unsupported, offline.sourceBytesRead, offlineElapsed, offline.interrupted);
			}
			if (settings.preloadAnimationFiles) {
				const auto animationElapsed = std::chrono::duration_cast<std::chrono::microseconds>(animationDone - offlineDone).count();
				logger::info("Startup HKX preload: selected={} checked={} behaviors={} animations={} bytes={} budget={} elapsed={}us "
					"indexHits={} directoriesScanned={} indexSaved={} indexLimited={} catalogLimited={} interrupted={}",
					animationStats.selected, animationStats.checked, animationStats.behaviorFiles, animationStats.animationFiles,
					animationStats.bytes, assetBudget, animationElapsed, animationStats.indexedDirectories, animationStats.scannedDirectories,
					animationStats.indexSaved, animationStats.indexLimited, animationStats.catalogLimited, animationStats.interrupted);
			}
			if (auto* log = spdlog::default_logger_raw()) log->flush();
		}
		return 0;
	}

	bool AwaitWork(HANDLE ready, HANDLE cancelled, HANDLE worker, HANDLE window) noexcept
	{
		const HANDLE signals[]{ready, cancelled, worker, window};
		constexpr auto count = static_cast<DWORD>(std::size(signals));
		for (;;) {
			const auto result = MsgWaitForMultipleObjectsEx(count, signals, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
			if (result == WAIT_OBJECT_0) return true;
			if (result != WAIT_OBJECT_0 + count) return false;
			MSG message{};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
	}

	void OnSKSEMessage(Messages::Message* message)
	{
		if (message->type == Messages::kInputLoaded || message->type == Messages::kPreLoadGame || message->type == Messages::kNewGame) {
			const auto worker = startupWorker.exchange(nullptr, std::memory_order_acq_rel);
			if (!worker) return;
			const auto window = std::exchange(windowThread, nullptr);
			const auto events = std::move(startupEvents);
			const bool completed = message->type == Messages::kInputLoaded && window && events &&
				AwaitWork(events->ready, events->cancelled, worker, window);
			if (!completed && !finished.load(std::memory_order_acquire)) {
				RequestCancel(events.get());
				CancelSynchronousIo(worker);
			}
			CloseHandle(worker);
			if (window) CloseHandle(window);
			return;
		}
		if (message->type != Messages::kPostPostLoad || startupEnded.load(std::memory_order_relaxed) ||
			started.test_and_set(std::memory_order_relaxed)) return;

		// The worker touches only portable cache data and files, so completing
		// the menu's SKSE callback never depends on a native Havok loader.
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&progress), &module)) {
			logger::warn("Could not start startup preload.");
			return;
		}
		if (GraphSlimming::Options().showPreloadProgress) {
			try {
				startupEvents = std::make_shared<StartupEvents>();
				if (startupEvents->cancelled && startupEvents->ready) windowThread = StartThread(ShowProgress, startupEvents);
			} catch (const std::bad_alloc&) {
			}
			if (!windowThread) {
				startupEvents.reset();
				logger::warn("Startup preload window could not start. Continuing in the background until the main menu.");
			}
		}
		if (const HANDLE thread = StartThread(WarmCache, startupEvents)) {
			startupWorker.store(thread, std::memory_order_release);
		} else {
			RequestCancel(startupEvents.get());
			if (windowThread) CloseHandle(std::exchange(windowThread, nullptr));
			startupEvents.reset();
			logger::warn("Could not create the startup preload worker.");
		}
	}
}

namespace StartupPreload
{
	void Install()
	{
		const auto& settings = GraphSlimming::Options();
		if (!settings.adaptivePreloading || (!settings.persistentCache && !settings.preloadAnimationFiles)) return;
		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) logger::warn("Could not register startup preload messages.");
	}
}
