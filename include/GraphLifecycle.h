#pragma once

#include <atomic>
#include <mutex>

namespace GraphLifecycle
{
	// The existing graph commit lock protects this list. Tickets stay on
	// the stack and track only analyses currently running in this process.
	class Analyses
	{
	public:
		class Ticket
		{
		public:
			Ticket(Analyses& analyses, std::mutex& mutex) noexcept : analyses(analyses), mutex(mutex) {}
			Ticket(const Ticket&) = delete;
			Ticket& operator=(const Ticket&) = delete;

			// Hold the commit lock for admission. Traversal can check its
			// ticket without blocking engine callbacks.
			[[nodiscard]] bool TryBegin(const void* value, const void* transition = nullptr) noexcept
			{
				if (!value || graph) return false;
				for (const auto* active = analyses.first; active; active = active->next) {
					if (active->graph == value || (transition && active->transitionData == transition)) return false;
				}
				graph = value;
				transitionData = transition;
				next = analyses.first;
				analyses.first = this;
				current.store(true, std::memory_order_relaxed);
				return true;
			}
			[[nodiscard]] bool IsCurrent() const noexcept { return current.load(std::memory_order_acquire); }

			~Ticket()
			{
				if (!graph) return;
				std::scoped_lock lock(mutex);
				auto** item = &analyses.first;
				while (*item != this) item = &(*item)->next;
				*item = next;
			}

		private:
			friend class Analyses;
			Analyses& analyses;
			std::mutex& mutex;
			Ticket* next{};
			const void* graph{};
			const void* transitionData{};
			std::atomic_bool current{};
		};

		// Hold the commit lock.
		void Invalidate(const void* graph, const void* transition = nullptr) noexcept
		{
			for (auto* item = first; item; item = item->next) {
				if (item->graph == graph || (transition && item->transitionData == transition)) {
					item->current.store(false, std::memory_order_release);
				}
			}
		}

	private:
		Ticket* first{};
	};
}
