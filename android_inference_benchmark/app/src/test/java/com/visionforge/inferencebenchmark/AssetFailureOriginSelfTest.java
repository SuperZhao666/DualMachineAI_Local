package com.visionforge.inferencebenchmark;

import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/** Fault injection at the actual source-open -> local-output-open boundary. */
final class AssetFailureOriginSelfTest {
    static void run() throws Exception {
        qnnLocalOpenFailureRemainsRetryable();
        portableLocalOpenFailureRemainsRetryable();
        missingAndInvalidPackageStayTerminal();
    }
    private static final byte[] CONTENT={1,2,3,4,5,6};
    private static MobilePipelineCoordinator.FailureDisposition disposition(Throwable failure) {
        return MobileInferencePreparationAssets.failure("installer_failure",failure).disposition;
    }
    private static void qnnLocalOpenFailureRemainsRetryable() throws Exception {
        File root=Files.createTempDirectory("vf-qnn-open-fault").toFile();
        String target=QnnAssetBundleInstaller.ASSET_NAMES[0];
        Map<String,Integer> opens=new HashMap<>();
        QnnAssetBundleInstaller.Result result=QnnAssetBundleInstaller.ensureInstalled(root,name->{
            int n=opens.merge(name,1,Integer::sum);
            if(name.equals(target) && n==2)blockLocalTarget(new File(new File(root,QnnAssetBundleInstaller.DIRECTORY_NAME),name+".part"));
            return new ByteArrayInputStream(CONTENT);
        });
        require(!result.ready && result.failure instanceof FileNotFoundException,"actual local QNN output did not fail as FileNotFound");
        System.out.println("qnn_local_open failure="+result.failure.getClass().getSimpleName()+" disposition="+disposition(result.failure));
        require(disposition(result.failure)==MobilePipelineCoordinator.FailureDisposition.RETRYABLE,"local QNN open became terminal");
        MobileRuntimeRetryPolicy retry=new MobileRuntimeRetryPolicy();
        retry.failed(100,disposition(result.failure),"qnn_asset_io_failure");
        require(!retry.mayAttempt(5099) && retry.mayAttempt(5100),"local open did not retain backoff retry");
        unblockLocalTarget(new File(new File(root,QnnAssetBundleInstaller.DIRECTORY_NAME),target+".part"));
        require(QnnAssetBundleInstaller.ensureInstalled(root,name->new ByteArrayInputStream(CONTENT)).ready,"local QNN retry failed");
    }
    private static void portableLocalOpenFailureRemainsRetryable() throws Exception {
        File root=Files.createTempDirectory("vf-portable-open-fault").toFile();
        var descriptor=new PortableModelAssetInstaller.Descriptor("fixture.onnx",CONTENT.length,hex(CONTENT));
        File temporary=new File(new File(root,PortableModelAssetInstaller.DIRECTORY_NAME),"fixture.onnx.part");
        PortableModelAssetInstaller.Result result=PortableModelAssetInstaller.ensureInstalled(root,descriptor,name->{
            blockLocalTarget(temporary);return new ByteArrayInputStream(CONTENT);
        });
        require(!result.ready && result.failure instanceof FileNotFoundException,"actual local portable output did not fail as FileNotFound");
        require(disposition(result.failure)==MobilePipelineCoordinator.FailureDisposition.RETRYABLE,"local portable open became terminal");
        unblockLocalTarget(temporary);
        require(PortableModelAssetInstaller.ensureInstalled(root,descriptor,name->new ByteArrayInputStream(CONTENT)).ready,"portable retry failed");
    }
    private static void missingAndInvalidPackageStayTerminal() throws Exception {
        File root=Files.createTempDirectory("vf-package-origin-fault").toFile();
        var missing=QnnAssetBundleInstaller.ensureInstalled(root,name->{throw new FileNotFoundException("asset="+name);});
        require(!missing.ready && missing.failure instanceof MobilePackagedAssetException,"package missing did not preserve typed origin");
        require(missing.failure.getCause() instanceof FileNotFoundException,"package missing lost original cause");
        require(disposition(missing.failure)==MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE,"missing package is retryable");
        var empty=QnnAssetBundleInstaller.ensureInstalled(root,name->new ByteArrayInputStream(new byte[0]));
        require(!empty.ready && disposition(empty.failure)==MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE,"empty package is retryable");
        var descriptor=new PortableModelAssetInstaller.Descriptor("missing.onnx",CONTENT.length,hex(CONTENT));
        var portable=PortableModelAssetInstaller.ensureInstalled(root,descriptor,name->{throw new FileNotFoundException(name);});
        require(!portable.ready && disposition(portable.failure)==MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE,"portable package missing is retryable");
        var corrupt=PortableModelAssetInstaller.ensureInstalled(root,descriptor,name->new ByteArrayInputStream(new byte[CONTENT.length]));
        require(!corrupt.ready && disposition(corrupt.failure)==MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE,"corrupt package is retryable");
    }
    private static void blockLocalTarget(File path) throws IOException {
        Files.createDirectories(path.toPath());Files.writeString(path.toPath().resolve("blocker"),"fault");
    }
    private static void unblockLocalTarget(File path) throws IOException {
        Files.delete(path.toPath().resolve("blocker"));Files.delete(path.toPath());
    }
    private static String hex(byte[] bytes) throws Exception {
        return HexFormat.of().withUpperCase().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }
    private static void require(boolean condition,String reason){if(!condition)throw new AssertionError(reason);}
}
