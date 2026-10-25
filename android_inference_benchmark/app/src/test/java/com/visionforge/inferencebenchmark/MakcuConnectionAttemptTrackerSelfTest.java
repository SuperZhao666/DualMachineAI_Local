package com.visionforge.inferencebenchmark;

/** Dependency-free contract for stale USB permission and reconnect rejection. */
final class MakcuConnectionAttemptTrackerSelfTest {
    static void run() {
        repeatedHealthChecksDoNotCancelNegotiation();
        permissionAndHealthCheckHaveOneOwner();
        MakcuConnectionAttemptTracker tracker = new MakcuConnectionAttemptTracker();
        MakcuConnectionAttemptTracker.Attempt first = tracker.begin("device-a", true);
        require(tracker.isPermissionPendingFor("device-a"));
        require(!tracker.consumePermission(first.generation, "device-b"));
        require(tracker.consumePermission(first.generation, "device-a"));
        require(!tracker.consumePermission(first.generation, "device-a"));

        MakcuConnectionAttemptTracker.Attempt second = tracker.begin("device-b", false);
        require(!tracker.isCurrent(first.generation, "device-a"));
        require(tracker.isCurrent(second.generation, "device-b"));
        require(tracker.isForDevice("device-b"));
        tracker.cancel();
        require(!tracker.isCurrent(second.generation, "device-b"));

        MakcuConnectionAttemptTracker.Attempt third = tracker.begin("device-c", false);
        tracker.clearIfCurrent(third.generation, "device-c");
        require(!tracker.isForDevice("device-c"));
        MakcuConnectionAttemptTracker.Snapshot snapshot = tracker.snapshot();
        require(snapshot.deviceName == null);
        require(!snapshot.permissionPending);
    }

    private static void repeatedHealthChecksDoNotCancelNegotiation() {
        MakcuConnectionAttemptTracker tracker = new MakcuConnectionAttemptTracker();
        MakcuConnectionAttemptTracker.Attempt first = tracker.beginConnectionIfNeeded("device-a");
        for (int i = 0; i < 20; ++i) require(tracker.beginConnectionIfNeeded("device-a") == null);
        require(tracker.isCurrent(first.generation, "device-a"));
        tracker.clearIfCurrent(first.generation, "device-a");
        require(tracker.beginConnectionIfNeeded("device-a") != null);
    }

    private static void permissionAndHealthCheckHaveOneOwner() {
        MakcuConnectionAttemptTracker tracker = new MakcuConnectionAttemptTracker();
        MakcuConnectionAttemptTracker.Attempt permission = tracker.begin("device-a", true);
        require(tracker.consumePermission(permission.generation, "device-a"));
        require(tracker.beginConnectionIfNeeded("device-a") == null);
        require(tracker.isCurrent(permission.generation, "device-a"));
        tracker.cancel();
        permission = tracker.begin("device-a", true);
        MakcuConnectionAttemptTracker.Attempt health = tracker.beginConnectionIfNeeded("device-a");
        require(health != null);
        require(!tracker.consumePermission(permission.generation, "device-a"));
        require(tracker.isCurrent(health.generation, "device-a"));
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("MAKCU connection attempt contract failed");
    }
}
