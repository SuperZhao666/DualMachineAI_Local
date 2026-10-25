package com.visionforge.inferencebenchmark;

import android.annotation.SuppressLint;
import android.content.Context;
import android.os.Build;
import android.provider.Settings;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Locale;

/** Collects bounded hardware metadata without privileged identifiers. */
final class AndroidDeviceProfileCollector {
    private static final String FINGERPRINT_DOMAIN =
            "visionforge-android-device-profile-v1";

    private AndroidDeviceProfileCollector() {
    }

    /** Public Android SoC identity; never substitutes for the QNN graph probe. */
    static String collectSocModel() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                ? safe(Build.SOC_MODEL)
                : safe(Build.HARDWARE);
    }

    static String collectSocManufacturer() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                ? safe(Build.SOC_MANUFACTURER)
                : safe(Build.MANUFACTURER);
    }

    static String collectSocHardwareEvidence() {
        String hardware = safe(Build.HARDWARE);
        String board = safe(Build.BOARD);
        return hardware.equals(board) ? hardware : hardware + " " + board;
    }

    private static String safe(String value) { return value == null ? "" : value.trim(); }
}
