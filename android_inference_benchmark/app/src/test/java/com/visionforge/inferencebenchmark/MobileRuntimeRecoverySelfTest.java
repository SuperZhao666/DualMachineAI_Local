package com.visionforge.inferencebenchmark;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Pure contracts used by the Service, with controlled lifecycle interleavings. */
final class MobileRuntimeRecoverySelfTest {
    static void run() throws Exception {
        verifiesTerminalAndCompletionBasedRetry();
        verifiesPackageCapabilitiesAndAssets();
        verifiesFatalVersusIdle();
        verifiesSnapshotIsNonBlockingAndGenerationOwned();
        verifiesConfigurationHandoffOwnership();
    }

    private static void verifiesTerminalAndCompletionBasedRetry() {
        MobileRuntimeRetryPolicy policy = new MobileRuntimeRetryPolicy();
        require(policy.mayAttempt(0));
        policy.failed(20_000, MobilePipelineCoordinator.FailureDisposition.RETRYABLE, "socket_fatal");
        require(!policy.mayAttempt(24_999) && policy.mayAttempt(25_000));
        policy.failed(40_000, MobilePipelineCoordinator.FailureDisposition.RETRYABLE, "socket_fatal");
        require(policy.nextAttempt() == 50_000);
        policy.failed(60_000, MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE,
                "qnn_htp_architecture_not_packaged");
        for (long time : new long[]{100_000, 1_000_000, Long.MAX_VALUE})
            require(!policy.mayAttempt(time));
        require("qnn_htp_architecture_not_packaged".equals(policy.failureCode()));
        policy.failed(90_000, MobilePipelineCoordinator.FailureDisposition.RETRYABLE, "unrelated_network_error");
        require(!policy.mayAttempt(Long.MAX_VALUE)
                && "qnn_htp_architecture_not_packaged".equals(policy.failureCode()));
        policy.reset(); // Explicit model/configuration retry, not a network notification.
        require(policy.mayAttempt(60_001) && "none".equals(policy.failureCode()));
    }

    private static void verifiesPackageCapabilitiesAndAssets() {
        MobileInferenceBackendPolicy.Plan qnnOnly = MobileInferenceBackendPolicy.evaluate(
                MobileSocCompatibilityPolicy.evaluate("SM8650", "QTI", "qcom"), true, false);
        require(qnnOnly.candidates.length == 1 && qnnOnly.candidates[0] == MobileInferenceBackend.QNN_HTP);
        MobileInferenceBackendPolicy.Plan unavailable = MobileInferenceBackendPolicy.evaluate(
                MobileSocCompatibilityPolicy.evaluate("MT6989", "MediaTek", "mt6989"), true, false);
        require(unavailable.candidates.length == 0);
        require(new MobileInferenceFailurePolicy.Accumulator().resolve().disposition
                == MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE);
        require(MobileInferencePreparationAssets.failure("package missing", new FileNotFoundException())
                .disposition == MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE);
        require(MobileInferencePreparationAssets.failure("temporary disk I/O", new IOException())
                .disposition == MobilePipelineCoordinator.FailureDisposition.RETRYABLE);
        require(MobileInferencePreparationAssets.failure("package integrity", new MobilePackagedAssetException("hash"))
                .disposition == MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE);
    }

    private static void verifiesFatalVersusIdle() {
        require(MobileRuntimeSnapshot.from("running=0", "configured=1", "ready=1", "")
                .requiresFullRecovery);
        require(MobileRuntimeSnapshot.from("running=1", "decoder_failed=1", "ready=1", "")
                .requiresFullRecovery);
        require(!MobileRuntimeSnapshot.from("running=1 last_completed_access_unit_age_ms=9000",
                "configured=1 qnn_executions=0", "ready=1", "").requiresFullRecovery);
        require(!MobileRuntimeSnapshot.from("report_unavailable", "configured=0", "ready=0", "")
                .requiresFullRecovery);
    }

    private static void verifiesSnapshotIsNonBlockingAndGenerationOwned() throws Exception {
        long first = MobileRuntimeReportStore.begin();
        CountDownLatch executing = new CountDownLatch(1), finish = new CountDownLatch(1);
        AtomicBoolean staleCommitted = new AtomicBoolean(true);
        Thread nativeReportWorker = new Thread(() -> {
            executing.countDown();
            try { require(finish.await(3, TimeUnit.SECONDS)); }
            catch (InterruptedException failure) { throw new AssertionError(failure); }
            staleCommitted.set(MobileRuntimeReportStore.publish(first, 20,
                    MobileRuntimeSnapshot.from("running=1", "configured=1", "ready=1", "")));
        });
        nativeReportWorker.start();
        require(executing.await(3, TimeUnit.SECONDS));
        long second = MobileRuntimeReportStore.begin();
        // UI read completes while the fake vendor call remains blocked.
        require(MobileRuntimeReportStore.snapshot().generation == second);
        finish.countDown(); nativeReportWorker.join(3000);
        require(!nativeReportWorker.isAlive() && !staleCommitted.get());
        require(MobileRuntimeReportStore.publish(second, 30,
                MobileRuntimeSnapshot.from("running=1", "configured=1", "ready=1", "")));
        require(MobileRuntimeReportStore.snapshot().sampledAtMillis == 30);
    }

    private static void verifiesConfigurationHandoffOwnership() {
        MobileActivityOwnership activity = new MobileActivityOwnership();
        long old = activity.claim();
        activity.beginHandoff(old);
        require(activity.takeHandoff() == old);
        long replacement = activity.claim();
        AtomicBoolean revoked = new AtomicBoolean();
        require(!activity.ifCurrent(old, () -> revoked.set(true)) && !revoked.get());
        require(activity.ifCurrent(replacement, () -> revoked.set(true)) && revoked.get());
        // An ordinary last unbind has no handoff token and retains fail-close.
        require(activity.takeHandoff() == 0);
        activity.beginHandoff(replacement);
        require(activity.takeHandoff() == replacement && activity.current(replacement));
    }

    private static void require(boolean condition) { if (!condition) throw new AssertionError(); }
}
