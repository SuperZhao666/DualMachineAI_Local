package com.visionforge.inferencebenchmark;

/** Immutable APK asset is present but fails its declared format/integrity. */
final class MobilePackagedAssetException extends java.io.IOException {
    MobilePackagedAssetException(String message) { super(message); }
}
