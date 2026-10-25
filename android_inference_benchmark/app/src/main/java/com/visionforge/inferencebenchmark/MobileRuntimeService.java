package com.visionforge.inferencebenchmark;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import com.visionforge.inferencebenchmark.runtime.*;
import java.io.File;
import java.util.List;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/** Local LAN runtime. A single worker owns network, model and pipeline transitions. */
public final class MobileRuntimeService extends Service {
    private static final String CHANNEL = "visionforge_local_runtime";
    private static final int NOTIFICATION_ID = 320;
    private static final String ACTION_STOP = "com.visionforge.mobile.local.STOP";
    private static final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private java.util.concurrent.ScheduledFuture<?> healthTask;
    private final java.util.concurrent.atomic.AtomicLong topologyRevision = new java.util.concurrent.atomic.AtomicLong();
    private final Object modelPreparationLock = new Object();
    private final MobileRuntimeAuthorization authorization = new MobileRuntimeAuthorization();
    private final MobileRuntimeRetryPolicy retry = new MobileRuntimeRetryPolicy();
    private final MobileActivityOwnership activityOwnership = new MobileActivityOwnership();
    private long reportGeneration;
    private final MobileRuntimeReportStore.Cursor reportCursor = new MobileRuntimeReportStore.Cursor();
    private final java.util.concurrent.ExecutorService reportWorker = Executors.newSingleThreadExecutor();
    private final java.util.concurrent.atomic.AtomicBoolean reportPending = new java.util.concurrent.atomic.AtomicBoolean();
    private final MobileTransportCatalog transportCatalog = new MobileTransportCatalog();
    private final LocalBinder binder = new LocalBinder();
    private final MobileRuntimeCompositionRoot composition = new MobileRuntimeCompositionRoot(binder);
    private volatile boolean destroying;
    private volatile boolean foreground;
    private boolean running;
    private long revision;
    private volatile MobileTransportEndpoint activeEndpoint;
    private MobileEventLogger events;
    private MobileResponseCalibrationStore responseCalibration;
    private final DiagnosticSessionManifest diagnosticManifest = new DiagnosticSessionManifest();
    private volatile MobileControlRuntime controlRuntime;
    private MobilePipelineCoordinator pipeline;
    private Cat6ReadyLifecycleCoordinator ready;
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback networkCallback;
    private ConnectivityManager.NetworkCallback ethernetDemand;
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private MobileInferenceBackend lastPreparedInferenceBackend;
    private String lastSkeletonDirectory;
    private final MobileRuntimeMetricsSampler serviceMetrics = new MobileRuntimeMetricsSampler();
    private long nextControlDiagnostic;
    private static volatile String readyReport = "local_runtime_not_started";

    static void ensureRunning(Context context) {
        context.startForegroundService(new Intent(context, MobileRuntimeService.class)
                .setAction(MobileRuntimeCommandProtocol.ACTION_ENSURE));
    }
    static void requestPowerPolicyRefresh(Context context) { ensureRunning(context); }
    static MobileRuntimeReadModel status() { return MobileRuntimeReadModelStore.snapshot(); }
    static String ethernetDiagnostics() { return MobileRuntimeReadModelStore.ethernetDiagnostics(); }
    static String cat6ReadyDiagnostics() { return readyReport; }

    public final class LocalBinder extends Binder implements MobileRuntimeBinding {
        @Override public void setActivityForeground(boolean value) {
            setForeground(value, "activity_hidden");
        }
        @Override public long claimActivity() { return activityOwnership.claim(); }
        @Override public void setActivityForeground(boolean value, long owner) {
            activityOwnership.ifCurrent(owner, () -> setForeground(value, "activity_hidden"));
        }
        @Override public void beginActivityConfigurationChange(long owner) {
            activityOwnership.beginHandoff(owner);
        }
        @Override public void selectGameModel(String token) {
            submit(() -> {
                MobileModelCatalog.Profile model = MobileModelCatalog.forProductionToken(token);
                if (model == null) return;
                responseCalibration.save(controlRuntime);
                stopPipeline("model_changed");
                controlRuntime.selectModel(model);
                responseCalibration.restore(controlRuntime);
                retry.reset();
            });
        }
        @Override public void selectOutputRoute(String token) {
            submit(() -> {
                responseCalibration.save(controlRuntime);
                controlRuntime.selectOutputRoute(ControlOutputRoute.fromStorageToken(token));
                responseCalibration.restore(controlRuntime);
                controlRuntime.ensureSelectedOutputConnected();
            });
        }
        @Override public String outputRouteReport() {
            return controlRuntime == null ? "runtime_not_ready" : controlRuntime.outputRouteReport();
        }
        @Override public void runDebugBluetoothHidMoveProbe(int x,int y,int reports,int interval) {
            submit(() -> controlRuntime.runDebugBluetoothHidMoveProbe(x,y,reports,interval));
        }
    }

    private void setForeground(boolean value, String reason) {
        final long generation = authorization.updateForeground(value, () -> {
            foreground = false;
            if (controlRuntime != null) controlRuntime.failClosed(reason);
        });
        foreground = authorization.foreground();
        submit(() -> {
            // A delayed hidden callback must not stop a newly visible generation.
            if (!value && authorization.generation() == generation) stopPipeline(reason);
        });
    }

    @Override public void onCreate() {
        super.onCreate();
        events = new MobileEventLogger(this);
        submit(() -> {
            controlRuntime = MobileControlRuntime.get(this);
            responseCalibration = new MobileResponseCalibrationStore(this,events);
            responseCalibration.restore(controlRuntime);
            pipeline = new MobilePipelineCoordinator(new QnnNativeVideoInferencePipeline(), events);
            ready = new Cat6ReadyLifecycleCoordinator(new NativeCat6ReadyAgent());
        });
        NotificationManager manager=getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,"本地双机运行",NotificationManager.IMPORTANCE_LOW));
        startForeground(NOTIFICATION_ID, notification());
        connectivity=getSystemService(ConnectivityManager.class);
        networkCallback=new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) { changed(n,false); }
            @Override public void onLinkPropertiesChanged(Network n,LinkProperties p) { changed(n,false); }
            @Override public void onCapabilitiesChanged(Network n,NetworkCapabilities c) { changed(n,false); }
            @Override public void onLost(Network n) {
                MobileTransportEndpoint active = activeEndpoint;
                long lostHandle = n.getNetworkHandle();
                boolean activeRouteAvailable = active != null
                        && lostHandle != active.networkHandle && routeStillAvailable(active);
                // Remove immediately: a health tick can already be queued ahead
                // of updateNetwork and must not reopen the lost candidate.
                transportCatalog.remove(lostHandle);
                if (controlRuntime != null) synchronized (controlRuntime) {
                    if (MobileNetworkLossPolicy.shouldFailClosed(lostHandle, active,
                            activeRouteAvailable && activeEndpoint == active)) {
                        controlRuntime.failClosed("network_lost");
                    }
                }
                changed(n,true);
            }
        };
        connectivity.registerNetworkCallback(new NetworkRequest.Builder()
                .clearCapabilities().addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build(),networkCallback);
        ethernetDemand=new ConnectivityManager.NetworkCallback() {};
        try {
            // Keep NOT_RESTRICTED: clearing it asks Android 16 for a privileged
            // restricted network and crashes an ordinary local application.
            connectivity.requestNetwork(new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build(),ethernetDemand);
        } catch (SecurityException unavailable) {
            ethernetDemand = null;
            events.write("ethernet_request_unavailable", unavailable.toString());
        }
        submit(() -> {
            for(Network n: connectivity.getAllNetworks()) updateNetwork(n,false);
            controlRuntime.ensureSelectedOutputConnected();
            publish(MobileRuntimePhase.READY,"本地模式：等待电脑连接");
        });
        healthTask=worker.scheduleWithFixedDelay(this::tickSafely,250,250,TimeUnit.MILLISECONDS);
    }
    @Override public int onStartCommand(Intent intent,int flags,int id) {
        if(intent!=null && ACTION_STOP.equals(intent.getAction())) {
            setForeground(false, "user_stopped");
            stopSelf();
        }
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public boolean onUnbind(Intent intent) {
        final long handoff = activityOwnership.takeHandoff();
        if (handoff == 0L) binder.setActivityForeground(false);
        else worker.schedule(() -> {
            // A configuration handoff is bounded if the replacement never binds.
            if (!destroying) activityOwnership.ifCurrent(handoff,
                    () -> setForeground(false, "configuration_handoff_expired"));
        }, 2, TimeUnit.SECONDS);
        return true;
    }
    @Override public void onTaskRemoved(Intent intent) {
        setForeground(false, "task_removed"); stopSelf();
    }
    private void changed(Network network,boolean lost) { topologyRevision.incrementAndGet(); submit(() -> updateNetwork(network,lost)); }
    private void updateNetwork(Network network,boolean lost) {
        MobileTransportEndpoint previous = transportCatalog.selected();
        if(lost) transportCatalog.remove(network.getNetworkHandle());
        else {
            EthernetNetworkDiagnostics.Evaluation e=EthernetNetworkDiagnostics.evaluate(
                    connectivity,network,connectivity.getLinkProperties(network));
            composition.publishEthernetDiagnostics(e.detail());
            if(e.accepted) transportCatalog.upsert(e.endpoint);
            else transportCatalog.remove(network.getNetworkHandle());
        }
        MobileTransportEndpoint selected = transportCatalog.selected();
        events.write("local_network_update", "lost=" + lost + " network=" + network.getNetworkHandle()
                + " previous={" + (previous == null ? "none" : previous.detail()) + "} selected={"
                + (selected == null ? "none" : selected.detail()) + "}");
        // Capability/link callbacks for the same route cannot erase terminal
        // model failures or bypass a transient completion-based backoff.
    }
    private void submit(Runnable action) {
        if(destroying) return;
        try { worker.execute(() -> { if(!destroying) try { action.run(); }
            catch(RuntimeException|LinkageError e) { failed(e); } }); }
        catch(java.util.concurrent.RejectedExecutionException ignored) { /* Service has stopped. */ }
    }
    private void tickSafely() {
        if(destroying || controlRuntime==null || ready==null || pipeline==null) return;
        try { tick(); } catch(RuntimeException|LinkageError e) { failed(e); }
    }
    private void tick() {
        MobileTransportEndpoint endpoint=transportCatalog.selected();
        ready.update(endpoint);
        readyReport=QnnHtpBridge.getNativeCat6ReadyAgentReport();
        if(endpoint!=null && !endpoint.isCat6() && !endpoint.hostDiscovered) {
            String host=QnnHtpBridge.getNativeCat6ReadyAgentRequesterIpv4();
            if(MobileTransportEndpoint.isUsableUnicastIpv4(host)) {
                endpoint=transportCatalog.upsert(endpoint.withDiscoveredHost(host));
            }
        }
        if(running && (!foreground || endpoint==null || !activeEndpoint.hasSameDataPlaneRoute(endpoint)))
            stopPipeline("local_route_changed");
        controlRuntime.updateTransportEndpoint(endpoint);
        if(!foreground || endpoint==null || !endpoint.isReadyForDataPlane()) return;
        if(!running && retry.mayAttempt(SystemClock.elapsedRealtime())) startPipeline(endpoint);
        if(!running) return;
        final long sampledLifecycle = authorization.generation();
        final long sampledPipeline = reportGeneration;
        requestReportSample(sampledLifecycle, sampledPipeline);
        consumeCurrentSample();
    }

    private void consumeCurrentSample() {
        if (!running || destroying) return;
        final long sampledLifecycle = authorization.generation();
        final long sampledPipeline = reportGeneration;
        MobileRuntimeReportStore.Sample sampled = MobileRuntimeReportStore.snapshot();
        if (sampled.generation != sampledPipeline || sampled.lifecycleGeneration != sampledLifecycle
                || sampled.sampledAtMillis == 0 || SystemClock.elapsedRealtime() - sampled.sampledAtMillis
                > MobileRuntimeSnapshot.LIVE_VIDEO_MAX_AGE_MILLIS) {
            controlRuntime.failClosed("runtime_report_not_current");
            return;
        }
        MobileRuntimeSnapshot snapshot = sampled.snapshot;
        if (retry.recoverFatal(snapshot, () -> stopPipeline("native_full_recovery_required"),
                SystemClock::elapsedRealtime)) {
            publish(MobileRuntimePhase.FAILED, "接收或解码已终止，等待重试", retry.failureCode());
            return;
        }
        boolean healthy=snapshot.videoLinkLive && snapshot.decoderReady
                && snapshot.qnnReady && snapshot.qnnExecutionLive;
        authorization.ifCurrent(sampledLifecycle, () -> { synchronized (controlRuntime) {
            // Serialize this final reopen check with onLost's immediate close.
            // The catalog may have changed while native reports were sampled.
            if (!foreground || destroying || !running || sampledPipeline != reportGeneration
                    || activeEndpoint == null
                    || !activeEndpoint.hasSameDataPlaneRoute(transportCatalog.selected())) {
                controlRuntime.failClosed("local_route_changed_during_health");
                return;
            }
            reportCursor.consume(sampled, () -> controlRuntime.reconcile(healthy,snapshot.nativeControlOutputRequestedKnown,
                    snapshot.nativeControlOutputRequested,snapshot.nativeControlOutputEffective,
                    snapshot.nativeControlOutputRecoverySuspended));
        }});
        recordDiagnostics(snapshot);
    }

    private void requestReportSample(long lifecycle, long pipelineGeneration) {
        if (!reportPending.compareAndSet(false, true)) return;
        try {
            reportWorker.execute(() -> {
                boolean consumeQueued = false;
                try {
                    final long sampledAt = SystemClock.elapsedRealtime();
                    MobileRuntimeSnapshot snapshot = MobileRuntimeSnapshot.from(
                            QnnHtpBridge.getNativeVideoReceiverReport(), QnnHtpBridge.getNativeH264DecoderReport(),
                            QnnHtpBridge.getNativeQnnRealtimeReport(), controlRuntime.makcuReport());
                    boolean current = authorization.ifCurrent(lifecycle, () -> MobileRuntimeReportStore.publish(
                            pipelineGeneration, lifecycle, sampledAt, snapshot));
                    // Consume the completed sample promptly, without another
                    // sampling request or a backlog on the lifecycle worker.
                    if (current) {
                        consumeQueued = true;
                        submit(() -> {
                            try { consumeCurrentSample(); }
                            finally { reportPending.set(false); }
                        });
                    }
                } catch (RuntimeException | LinkageError failure) {
                    events.write("runtime_report_unavailable", MobileThrowableDiagnostics.format(failure));
                } finally {
                    // Do not sample another pre-enable native snapshot while
                    // this result still awaits its serialized gate decision.
                    if (!consumeQueued) reportPending.set(false);
                }
            });
        } catch (java.util.concurrent.RejectedExecutionException stopped) { reportPending.set(false); }
    }

    private void recordDiagnostics(MobileRuntimeSnapshot snapshot) {
        long now = SystemClock.elapsedRealtime();
        String sampled = serviceMetrics.sampleIfDue(snapshot, now);
        if (sampled != null) {
            events.write("runtime_session_manifest", diagnosticManifest.sample(this));
            responseCalibration.save(controlRuntime);
            events.write("runtime_pipeline_metrics", controlRuntime.inferenceAuditDetail()
                    + " route=" + controlRuntime.activeOutputRoute().storageToken + " " + sampled
                    + " qnn_p95=" + snapshot.qnnP95 + " " + events.status()
                    + " thermal_status=" + getSystemService(PowerManager.class).getCurrentThermalStatus());
            // Full bounded native reports retain queue/drop counters, quantiles,
            // control suppression reasons and transport completion semantics.
            events.write("runtime_pipeline_detail", "receiver={" + snapshot.videoReport
                    + "} decoder_control={" + snapshot.decoderReport + "} qnn={"
                    + snapshot.qnnReport + "} transport={" + controlRuntime.outputRouteReport()
                    + "} usb={" + snapshot.makcuReport + "}");
            long traceBegin = System.nanoTime();
            long elapsedAnchor = SystemClock.elapsedRealtimeNanos();
            long unixAnchor = System.currentTimeMillis();
            long traceAnchorEnd = System.nanoTime();
            events.write("runtime_clock_anchor", "monotonic_before_us=" + traceBegin / 1000
                    + " elapsed_realtime_us=" + elapsedAnchor / 1000 + " unix_ms=" + unixAnchor
                    + " monotonic_after_us=" + traceAnchorEnd / 1000);
            String trace = QnnHtpBridge.getNativeControlTraceReport();
            String pipelineTrace = QnnHtpBridge.getNativePipelineTraceReport();
            long nativeDone = System.nanoTime();
            DiagnosticTraceBatch.write(trace, events::write);
            DiagnosticTraceBatch.write(pipelineTrace, events::write);
            String delivery = controlRuntime.drainMoveTrace();
            DiagnosticTraceBatch.write(delivery, events::write);
            events.write("runtime_trace_cost", "native_snapshot_format_us=" + (nativeDone - traceAnchorEnd) / 1000
                    + " total_snapshot_enqueue_us=" + (System.nanoTime() - traceBegin) / 1000
                    + " characters=" + (trace.length() + pipelineTrace.length() + delivery.length()));
        }
        if (now < nextControlDiagnostic) return;
        nextControlDiagnostic = now + 1000;
        StringBuilder detail = new StringBuilder("route=")
                .append(controlRuntime.activeOutputRoute().storageToken);
        for (String field : CONTROL_DIAGNOSTIC_FIELDS) detail.append(' ').append(field)
                .append('=').append(MobileDiagnosticFieldParser.valueOr(snapshot.decoderReport, field, "unknown"));
        events.write("runtime_control_sample", detail.toString());
    }

    private static final String[] CONTROL_DIAGNOSTIC_FIELDS = {
            "output_requested", "output_enabled", "control_blocker", "processed_frames",
            "java_offer_acceptances", "java_offer_rejections", "native_last_offer_interval_us",
            "native_move_completion_pending_suppressed", "post_completion_visibility_suppressed",
            "native_bluetooth_hid_api_acceptances", "native_last_bluetooth_hid_api_acceptance_latency_us",
            "native_device_ack_completions", "native_last_device_ack_latency_us",
            "no_targets", "no_move_suppressed", "response_guard_suppressed", "settle_guard_suppressed",
            "direction_flip_suppressed", "motion_invalid_suppressed", "output_disabled_suppressed"
    };
    private void startPipeline(MobileTransportEndpoint endpoint) {
        final long topology=topologyRevision.get();
        final long lifecycle = authorization.generation();
        publish(MobileRuntimePhase.STARTING,"正在加载本地模型");
        MobileModelCatalog.Profile model=controlRuntime.activeModel();
        MobilePipelineCoordinator.PrepareResult prepared=prepareModelDataPlaneClosed(
                getApplicationInfo().nativeLibraryDir,lastSkeletonDirectory,model,
                MobileInferenceBackendPolicy.evaluate(currentSocCompatibility(), true,
                        hasPackagedPortableModel(model)));
        if(!prepared.prepared) { stopPipeline("model_prepare_failed");
            retry.failed(SystemClock.elapsedRealtime(), prepared.disposition, prepared.failureCode);
            publish(MobileRuntimePhase.FAILED,prepared.message,prepared.failureCode); return; }
        // Activity may have stopped while the native model was loading.
        // Android emits capability/link callbacks for the same usable route
        // while QNN loads. Callback count alone must not cancel and reload the
        // graph forever. Re-read OS route state before opening the data plane.
        if(destroying || !foreground || !authorization.current(lifecycle)
                || !endpoint.hasSameDataPlaneRoute(transportCatalog.selected()) ||
                (topology!=topologyRevision.get() && !routeStillAvailable(endpoint))) {
            stopPipeline("start_cancelled"); return;
        }
        controlRuntime.selectModel(model);
        MobilePipelineCoordinator.StartResult started=pipeline.openPreparedDataPlane(endpoint,model);
        if(!started.started) { stopPipeline("udp_start_failed");
            retry.failed(SystemClock.elapsedRealtime(), started.disposition, started.failureCode);
            publish(MobileRuntimePhase.FAILED,started.message,retry.failureCode()); return; }
        if (destroying || !foreground || !authorization.current(lifecycle)
                || !endpoint.hasSameDataPlaneRoute(transportCatalog.selected())) {
            stopPipeline("start_cancelled"); return;
        }
        activeEndpoint=endpoint; running=true;
        retry.reset();
        reportGeneration = MobileRuntimeReportStore.begin();
        acquireLocks();
        publish(MobileRuntimePhase.RUNNING,"本地直连运行："+endpoint.localIpv4+" ↔ "+endpoint.hostIpv4);
        events.write("local_pipeline_started",endpoint.detail());
    }
    private boolean hasPackagedPortableModel(MobileModelCatalog.Profile model) {
        if (model == null) return false;
        try {
            String[] names = getAssets().list("portable_models");
            if (names != null) for (String name : names)
                if (name.equals(model.portableModelAsset)) return true;
            return false;
        } catch (java.io.IOException unavailable) {
            events.write("portable_package_capability_unavailable", unavailable.toString());
            // An I/O failure is different from a successfully enumerated
            // package with no portable model; let the Service back off.
            throw new java.io.UncheckedIOException(unavailable);
        }
    }
    private boolean routeStillAvailable(MobileTransportEndpoint endpoint) {
        if (endpoint == null || connectivity == null) return false;
        try {
            for (Network network : connectivity.getAllNetworks()) {
                if (network.getNetworkHandle() != endpoint.networkHandle) continue;
                EthernetNetworkDiagnostics.Evaluation current = EthernetNetworkDiagnostics.evaluate(
                        connectivity, network, connectivity.getLinkProperties(network));
                return current.accepted && endpoint.hasSameNetworkRoute(current.endpoint);
            }
        } catch (RuntimeException unavailable) {
            // A loss racing the OS query is absent evidence, so keep output closed.
            return false;
        }
        return false;
    }
    private void stopPipeline(String reason) {
        events.write("local_pipeline_stopping", "reason=" + reason + " running=" + running
                + " foreground=" + foreground + " endpoint={"
                + (activeEndpoint == null ? "none" : activeEndpoint.detail()) + "}");
        running=false; activeEndpoint=null;
        reportGeneration = MobileRuntimeReportStore.begin();
        if(controlRuntime!=null) controlRuntime.failClosed(reason);
        try { if(pipeline!=null) pipeline.stop(); }
        finally { releaseLocks(); }
        if (retry.terminal()) publish(MobileRuntimePhase.FAILED,
                "模型或包能力不可用：" + retry.failureCode(), retry.failureCode());
        else publish(MobileRuntimePhase.READY,"本地模式：等待连接或返回应用");
    }
    private void failed(Throwable e) {
        try { stopPipeline("local_runtime_failure"); } catch(RuntimeException|LinkageError ignored) {}
        retry.failed(SystemClock.elapsedRealtime(), MobilePipelineCoordinator.FailureDisposition.RETRYABLE,
                "transient_runtime_failure");
        events.write("local_runtime_failed",MobileThrowableDiagnostics.format(e));
        publish(MobileRuntimePhase.FAILED,e.getClass().getSimpleName()+": "+e.getMessage(),retry.failureCode());
    }
    private void publish(MobileRuntimePhase phase,String detail) {
        publish(phase,detail,"none");
    }
    private void publish(MobileRuntimePhase phase,String detail,String code) {
        composition.publish(new MobileRuntimeReadModel(phase,detail,code,++revision));
    }
    private void acquireLocks() {
        if(wakeLock==null) {
            wakeLock=getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"VisionForge:Local");
            wakeLock.setReferenceCounted(false);
        }
        if(!wakeLock.isHeld()) wakeLock.acquire();
        if(wifiLock==null) {
            wifiLock=getSystemService(WifiManager.class).createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF,"VisionForge:LocalWifi");
            wifiLock.setReferenceCounted(false);
        }
        if(!wifiLock.isHeld()) wifiLock.acquire();
    }
    private void releaseLocks() {
        if(wakeLock!=null && wakeLock.isHeld()) wakeLock.release();
        if(wifiLock!=null && wifiLock.isHeld()) wifiLock.release();
    }
    private Notification notification() {
        PendingIntent open=PendingIntent.getActivity(this,0,new Intent(this,MainActivity.class),PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop=PendingIntent.getService(this,1,new Intent(this,MobileRuntimeService.class).setAction(ACTION_STOP),PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this,CHANNEL).setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle("VisionForge 本地版").setContentText("本地局域网连接；退出应用后停止控制输出")
                .setContentIntent(open).setOngoing(true).addAction(0,"停止",stop).build();
    }
    @Override public void onDestroy() {
        authorization.destroy(() -> {
            destroying=true; foreground=false;
            if(controlRuntime!=null) controlRuntime.failClosed("service_destroyed");
        });
        if(healthTask!=null) healthTask.cancel(false);
        reportWorker.shutdownNow();
        if(connectivity!=null) {
            if(networkCallback!=null) connectivity.unregisterNetworkCallback(networkCallback);
            if(ethernetDemand!=null) connectivity.unregisterNetworkCallback(ethernetDemand);
        }
        // All native teardown follows any in-flight preparation on the same worker.
        worker.execute(() -> {
            try { stopPipeline("service_destroyed"); }
            finally {
                try { if(ready!=null) ready.stop(); }
                finally { if(controlRuntime!=null) controlRuntime.closeForServiceStop("service_destroyed");
                    publish(MobileRuntimePhase.STOPPED,"本地运行已停止"); }
            }
        });
        // Shared process worker serializes teardown before a replacement Service starts.
        super.onDestroy();
    }

    // Existing device/backend compatibility and model loading methods follow.

    private MobilePipelineCoordinator.PrepareResult
            prepareModelDataPlaneClosed(
            String nativeDirectory,
            String skeletonDirectory,
            MobileModelCatalog.Profile model,
            MobileInferenceBackendPolicy.Plan plan) {
        synchronized (modelPreparationLock) {
            MobileInferenceFailurePolicy.Accumulator failures =
                    new MobileInferenceFailurePolicy.Accumulator();
            MobileInferencePreparationAssets portableAssets = null;
            for (MobileInferenceBackend backend : plan.candidates) {
                MobileInferencePreparationAssets assets;
                if (backend.requiresQnnRuntime) {
                    assets = resolveQnnInferenceAssets(skeletonDirectory);
                } else {
                    if (portableAssets == null) {
                        portableAssets = resolvePortableInferenceAssets(model);
                    }
                    assets = portableAssets;
                }
                if (!assets.ready) {
                    MobilePipelineCoordinator.PrepareResult assetFailure =
                            assetResolutionFailure(backend, assets);
                    failures.record(backend, assetFailure);
                    logInferenceBackendAttempt(
                            backend, "asset_resolution_failed", assets.detail);
                    continue;
                }
                MobilePipelineCoordinator.PrepareResult prepared =
                        pipeline.prepareDataPlaneClosed(
                                backend,
                                nativeDirectory,
                                assets.vendorRuntimeDirectory,
                                assets.portableModelPath,
                                model);
                if (prepared.prepared) {
                    lastPreparedInferenceBackend = backend;
                    if (backend.requiresQnnRuntime) {
                        lastSkeletonDirectory =
                                assets.vendorRuntimeDirectory;
                    }
                    logInferenceBackendAttempt(
                            backend, "prepared", assets.detail);
                    return prepared;
                }
                failures.record(backend, prepared);
                logInferenceBackendAttempt(
                        backend, "prepare_failed", prepared.message);
            }
            lastPreparedInferenceBackend = null;
            return failures.resolve();
        }
    }

    private MobileInferencePreparationAssets resolveQnnInferenceAssets(
            String preferredDirectory) {
        List<String> architectures = QnnAssetBundleInstaller.parseArchitectures(
                BuildConfig.QNN_HTP_ARCHITECTURES);
        File directory = preferredDirectory == null
                ? null : new File(preferredDirectory);
        String installerDetail = "reused_verified_process_install=true";
        if (directory == null || !directory.isDirectory()) {
            QnnAssetBundleInstaller.Result installed =
                    QnnAssetBundleInstaller.ensureInstalled(
                            getFilesDir(),
                            name -> getAssets().open("qnn/" + name),
                            BuildConfig.QNN_HTP_ARCHITECTURES);
            if (!installed.ready || installed.directory == null) {
                return MobileInferencePreparationAssets.failure(
                        installed.detail, installed.failure);
            }
            directory = installed.directory;
            architectures = installed.architectures;
            installerDetail = installed.detail;
        }
        QnnHtpCompatibilityPolicy.Result compatibility =
                QnnHtpCompatibilityPolicy.evaluate(
                        AndroidDeviceProfileCollector.collectSocModel(),
                        architectures);
        events.write(
                "mobile_qnn_htp_compatibility",
                compatibility.detail()
                        + " packaged_architectures="
                        + String.join(",", architectures));
        if (!compatibility.canAttemptBackend) {
            return MobileInferencePreparationAssets.failure(
                    compatibility.failureCode + " " + compatibility.detail(),
                    null,
                    MobilePipelineCoordinator.FailureDisposition
                            .TERMINAL_INCOMPATIBLE,
                    compatibility.failureCode);
        }
        return MobileInferencePreparationAssets.qnn(
                directory.getAbsolutePath(),
                installerDetail + " " + compatibility.detail());
    }

    private MobileInferencePreparationAssets resolvePortableInferenceAssets(
            MobileModelCatalog.Profile model) {
        PortableModelAssetInstaller.Result installed =
                PortableModelAssetInstaller.ensureInstalled(
                        getFilesDir(),
                        model,
                        name -> getAssets().open("portable_models/" + name));
        events.write(
                installed.ready
                        ? "mobile_portable_model_ready"
                        : "mobile_portable_model_failed",
                installed.detail);
        if (!installed.ready || installed.modelFile == null) {
            return MobileInferencePreparationAssets.failure(
                    installed.detail, installed.failure);
        }
        return MobileInferencePreparationAssets.portable(
                installed.modelFile.getAbsolutePath(), installed.detail);
    }

    private MobilePipelineCoordinator.PrepareResult assetResolutionFailure(
            MobileInferenceBackend backend,
            MobileInferencePreparationAssets assets) {
        String stack = assets.failure == null
                ? "none" : MobileThrowableDiagnostics.format(assets.failure);
        return MobilePipelineCoordinator.PrepareResult.failure(
                "inference_asset_resolution_failed backend=" + backend.token
                        + " detail={" + assets.detail + "} stack={" + stack + "}",
                assets.disposition,
                "inference_asset_resolution_failed".equals(assets.failureCode)
                        ? (backend.requiresQnnRuntime
                        ? "qnn_asset_bundle_invalid"
                        : "portable_model_unavailable")
                        : assets.failureCode);
    }

    private void logInferenceBackendAttempt(
            MobileInferenceBackend backend,
            String result,
            String detail) {
        events.write(
                "mobile_inference_backend_attempt",
                "backend=" + backend.token
                        + " result=" + result
                        + " detail={" + detail + "}");
    }

    private static MobileSocCompatibilityPolicy.Result
            currentSocCompatibility() {
        return MobileSocCompatibilityPolicy.evaluate(
                AndroidDeviceProfileCollector.collectSocModel(),
                AndroidDeviceProfileCollector.collectSocManufacturer(),
                AndroidDeviceProfileCollector.collectSocHardwareEvidence());
    }
}
