#pragma once

// Shared lifecycle policy for hooks injected into Cascadeur.
// 1. Wait for QCoreApplication without touching Qt objects from DllMain.
// 2. Install currently resolvable hooks on the GUI thread.
// 3. Use Qt lifecycle events for objects created later.
// 4. Optionally poll for UI that loads late and bypasses those events.
namespace CascadeurHookLifecycle {

constexpr int kApplicationWaitAttempts = 3000;
constexpr int kApplicationWaitIntervalMs = 100;
constexpr int kDeferredScanIntervalMs = 2000;
constexpr int kDeferredScanWindowMs = 60000;

// Phase 3 compatibility fallback. Disabled by default because the initial
// GUI-thread installation and Qt lifecycle events cover normal Cascadeur use.
// Enable only for a version or plug-in that creates UI without useful events.
// The fallback implementation is intentionally retained at its call site.
constexpr bool kEnableDeferredPolling = false;

} // namespace CascadeurHookLifecycle
