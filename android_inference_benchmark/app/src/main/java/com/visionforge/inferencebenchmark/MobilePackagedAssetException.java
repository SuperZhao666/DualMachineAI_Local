package com.visionforge.inferencebenchmark;

/** Immutable APK asset is present but fails its declared format/integrity. */
final class MobilePackagedAssetException extends java.io.IOException {
    MobilePackagedAssetException(String message) { super(message); }
    MobilePackagedAssetException(String message,Throwable cause) { super(message,cause); }

    interface Source { java.io.InputStream open() throws java.io.IOException; }
    static java.io.InputStream open(String name,Source source) throws java.io.IOException {
        try { return source.open(); }
        catch (java.io.FileNotFoundException missing) {
            throw new MobilePackagedAssetException("packaged_asset_missing asset="+name,missing);
        }
    }
}
