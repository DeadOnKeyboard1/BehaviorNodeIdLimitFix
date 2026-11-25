# Changelog

## 2.5.3

- Harden Havok graph and transition-map commits by verifying the native table header, complete writable bucket span, live native insert function fingerprint, and active/recycled node-ID invariants before publishing replacements or ID assignments.
- Improve cached instance replay and shared payload safety: guard child array pointers against buffer overlap and native reinitialization, retain shared transition invalidation tracking across aliases, and support multi-width ordinal encoding cleanly (`16-bit` / `24-bit`).
- Add bounded startup offline HKX topology precomputation (`Cache/Offline`) with stamped invalidation, NTFS directory index budgeting (`HKXIndex.bin`), combined `.graph` background caching, and live-verified `traversal=offline-topology` / `traversal=offline-verified` paths.
- Expand per-hook and HKX timing telemetry with lock-wait metrics, clipped leaf fast-paths on Skyrim 1.7.104.0, and ship default production INI (`bEnablePerformanceLogging = false`).
- Extend unit/benchmark coverage across malformed dense IDs, concurrent invalidations, offline admission budgets, and packed replay resumption; statically reverify hook sites and Havok transition insert spans for Skyrim `1.5.97.0`, `1.6.1170.0`, and `1.7.104.0`.

## 2.5.0

- Precompute topology and ID plans from supported loose behavior HKX files at startup, persist across restarts, and validate against live graphs before applying.
- Reuse valid dense ID plans when cached topologies rebuild and check additional startup candidates when records cannot be used.
- Prioritize character, first-person, and OAR assets during optional HKX file warming and avoid rewriting unchanged HKX directory indexes.
- Streamline compact topology traversal, reuse thread-local scratch buffers, and share identical immutable cached payloads.
- Invalidate shared-transition instances on native reinitialization, validate Havok transition map commits, add `bPrecomputeOfflineGraphs`, and separate offline cache metrics in performance logs.
