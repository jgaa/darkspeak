# DarkSpeak — Application Metrics

**Status:** Design specification
**Scope:** Application-wide C++ measurement infrastructure; independent of the FileShare dashboard, UI and MCP.

## 1. Purpose and ownership

Implement one application-owned `Metrics` class wrapping `yahat::Metrics`, following the established pattern in NextApp's `src/backend/include/nextapp/Metrics.h`. Metrics measure DarkSpeak operations whether or not a dashboard, MCP or HTTP server is active. They are **observability**, never an authoritative store of configuration, authorization, credits, transfer progress, storage accounting or application state.

The `Metrics` object has one well-defined lifetime, initialized before instrumented services and destroyed after them. Services receive a reference to it through existing dependency wiring; do not create per-component registries, global mutable metric factories, or QML-owned measurements. Metric declarations, descriptions, units, approved fixed labels, and typed accessors live in this single class; group accessors by subsystem if useful without splitting registry ownership.

Use the yahat-cpp `yahat/Metrics.h` types and registration API, e.g. `Metrics::Counter<uint64_t>`, `Gauge<uint64_t>`, `Histogram<double>`, `Summary<double>`, `AddCounter`, `AddGauge`, `AddHistogram`, and `AddSummary`. Scoped gauges (`.scoped()`) count active operations; scoped histogram timers measure durations when suitable. RAII handles must survive coroutine suspension correctly and must not outlive the registry. Counters are monotonic **per process lifetime**; the UI must not assume they persist across restart. Use monotonic elapsed time for duration and rate calculations.

## 2. Measurements and resource discipline

Choose metric types according to meaning:
- **Counters:** completed operations, bytes processed, failures, denied requests, cache hits/misses. Counters are process-lifetime totals.
- **Gauges:** current workers, in-flight transfers, queued tasks, active indexers, cached bytes, temporary artifacts, reservations and mount counts. Scoped gauges for lifetimes, explicit `set` for reconciled state snapshots.
- **Histograms or summaries:** request/operation latencies and other measured durations. Prefer bounded histograms for stable dashboards/export; use yahat summaries only where measured overhead and semantic requirements justify them.
- **Derived rates:** compute from successive counter samples in C++ dashboard/test models, **not** by separately incrementing a second per-second rate metric at every event.

Provide a small, meaningful core set initially, expanding instrumented operations when the corresponding subsystems are implemented:

| Area | Initial instruments |
| --- | --- |
| Application | uptime via steady-clock start, application warnings/errors totals, active async workers |
| Generic transfers | active, queued, paused transfers; sent/received payload bytes; completed/failed/resumed totals; failure categories; checkpoint wait and verification durations |
| Storage/FileManager | active indexing/reconciliation/hash work, indexed objects as a bounded aggregate, read/write bytes, temporary/cache bytes, outstanding physical reservations, failures by fixed category |
| VFS/FileShare | configured mounts and identity resources as aggregate gauges, browse/catalog generation duration, authorization denials, active catalog builds and completed/failed exports |
| MCP/HTTP (if enabled) | active authenticated requests, completed/error counts and latency without request arguments or sensitive labels |

Metrics do not replace feature-specific models. Credit balances, individual transfers, pending approvals, resource configurations, mount conflicts, and warnings with actionable details are read through their **authoritative C++ services/models**. A FileShare dashboard combines model data and metric snapshots; it does not rebuild operational state from counters.

**Performance:** instrumentation must be lightweight, bounded and safe from worker threads. No filesystem access, database writes, allocations proportional to peer data, or blocking UI work on the metric hot path. Pre-register metrics at startup, with a finite documented family/label budget; avoid dynamic metric creation per contact, identity, file, transfer, path, error string or request. Prefer fixed error categories and a bounded `other` category. Histograms/summaries must be chosen with attention to their synchronization and retention costs. Use sampled or aggregated measurements for extremely hot paths; no per-byte events. Registry/export snapshot reading must not significantly block worker operations.

## 3. Privacy and exposure

Metric names, descriptions and all labels must be reviewed as **potentially public data**. Never attach identities, contacts, onion addresses, IPs, paths, filenames, file hashes, message contents, keys, tokens, resource IDs, peer-controlled text or arbitrary error messages. Avoid labels whose combinations indirectly identify individuals. Aggregate numeric totals can still reveal activity patterns: dashboard metrics are local administrative data, not remotely accessible through the messaging protocol. The OpenMetrics exporter is explicitly opt-in and off by default.

If HTTP export is enabled, expose one `/metrics/` route using yahat's OpenMetrics rendering from the same registry. The route is independent of dashboard/MCP enablement. Listener configuration must be explicit: default loopback only; never bind it to a Tor onion or peer-facing listener or reuse externally exposed routes accidentally. Follow the existing app HTTP authentication/security design where applicable; if export has no authentication, require a dedicated loopback-only listener with explicit local opt-in. No unbounded HTTP output, scrape concurrency or serialization work. Never install a second registry for HTTP. Document that local metrics may disclose usage patterns to other local processes/users with access to the listening endpoint; do not claim loopback is a confidentiality boundary.

Do not enable a new HTTP listener merely to obtain local dashboard data. Enable/disable the HTTP exporter at runtime safely if the existing server supports it; otherwise use an explicit restart-required configuration, not a silent partial state.

## 4. C++ consumers and QML

Expose a bounded, thread-safe, coherent-enough **typed C++ snapshot API** for locally needed metrics. Dashboard models obtain metric values directly from the registry or its snapshot adapter; they must **not parse the OpenMetrics text representation**. The dashboard model owns sampling cadence, rate deltas, invalidation after process restart, rounding, and user-visible presentation values. Values can be sampled more slowly when hidden; no QML timers performing metric arithmetic. UI layout/focus/animation stay in QML, while all application state and nontrivial calculations stay in C++.

A metric snapshot is not a transactional cross-subsystem state capture. Consumers must tolerate readings taken at slightly different times, and zero/unknown/not-applicable states. Snapshot and subscriber APIs must be bounded and must not require constructing a dashboard view to be usable.

## 5. MCP and regression testing

The **test MCP server** provides a bounded `metrics_snapshot`-style read operation exposing the same typed and sanitized counters, gauges and aggregate distribution data as the C++ snapshot API. Test-only commands may orchestrate application operations and failure scenarios using the existing testing infrastructure, but must not forge metrics as a substitute for actually exercising the application. Metric snapshots are read-only and require controller authorization; production MCP, if it later exposes sanitized metrics, needs its own `read_diagnostics` capability and policy review. A metrics URL is **not** a backdoor into test MCP.

CTest regressions must verify: counter increments and process-restart reset semantics; balanced scoped gauges on normal completion, errors and coroutine cancellation; stable naming/units/fixed labels; worker-thread concurrency; bounded snapshot/export output; exporter disabled by default; listener cannot expose on Tor/non-loopback; no secret/peer-supplied values in labels or output; and that metrics/dashboard absence never changes transfer/storage correctness. Use `tests/run-all-tests.sh` and register new scenarios with CTest in line with `agents.md`.

## 6. Delivery and integration

1. Add the one `Metrics` C++ class and yahat metrics build dependency, enabling `YAHAT_ENABLE_METRICS` as required by the inspected dependency version. Reuse the project's existing pinned `yahat-cpp` integration when available. **Do not require MCP to be enabled to collect application metrics.** When MCP is disabled, pull/build only the needed metrics support if the existing library permits it, without unnecessarily enabling its HTTP/MCP stack.
2. Instrument existing application and transfer/storage operations with typed accessors and scoped counters/timers, preserving established threading and cancellation paths.
3. Expose typed snapshots through test MCP and add deterministic tests before dashboard work.
4. Add optional `/metrics/` export independently; implement the dashboard and its extra metrics later under `fileshare_dashboard.md`.

Do not redesign services solely to accommodate instrumentation, and do not add dependencies from backend processing to QML, HTTP or FileShare dashboard components. Metrics initialization or exporter failure must not disable the application's core messaging functionality.
