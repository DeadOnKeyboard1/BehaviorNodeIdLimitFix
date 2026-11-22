# Changelog

## 2.5.0

- Precompute topology and ID plans from supported loose behavior HKX files at startup. Persist them across restarts and invalidate them when the source changes, including edits that preserve file size. Validate each plan against the live graph before applying it.
- Reuse valid dense ID plans when the corresponding cached topology needs rebuilding. Check additional startup candidates when a cache record cannot be used.
- Prioritize character, first-person and OAR assets during optional HKX file warming. Skip OAR directories already covered by the priority pass and write the persistent HKX index only when it changes.
- Streamline traversal and validation of compact topology data, reuse scratch buffers per thread and improve sharing of identical cached payloads.
- Invalidate cached graph instances that share transition data when the native graph is reinitialized. Retire cached buffers outside the commit lock.
- Validate node assignments, map memory and the native insert routine before publishing a replacement Havok transition map. Select and log the map rebuild profile for the detected Skyrim runtime.
- Add `bPrecomputeOfflineGraphs`, enabled by default. Optional HKX file warming is controlled separately by `bPreloadAnimationFiles`, which defaults to `false`.
- Add per-hook P95 estimates, lock wait timings and separate offline cache results. Write large-graph detail rows through the background reporter.

Validation includes 467 automated checks and static hook and map writer verification for Skyrim 1.5.97.0, 1.6.1170.0 and 1.7.104.0.
