package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Incremental parser for MAKCU's shared command-response/button-event stream.
 *
 * <p>Command responses end in {@code ">>> "}. A physical button snapshot is
 * encoded as {@code 'k','m','.',mask}; the mask may itself be CR or LF, so
 * line-based parsing is not safe.</p>
 */
final class MakcuResponseStreamParser {
    private static final byte[] BUTTON_PREFIX = {'k', 'm', '.'};
    private static final byte[] RESPONSE_PROMPT = {'>', '>', '>', ' '};
    private static final int MAXIMUM_RESPONSE_BYTES = 4_096;

    interface Listener {
        void onButtonMask(int mask);

        void onResponse(String response);

        void onResponseOverflow();
    }

    private final Listener listener;
    private final byte[] response = new byte[MAXIMUM_RESPONSE_BYTES];
    private int responseLength;
    private int buttonPrefixLength;
    private int buttonDelimiterState;

    MakcuResponseStreamParser(Listener listener) {
        this.listener = listener;
    }

    void accept(byte[] source, int count) {
        if (source == null || count <= 0) return;
        int boundedCount = Math.min(source.length, count);
        for (int index = 0; index < boundedCount; index++) {
            acceptUnsigned(source[index] & 0xff);
        }
    }

    private void acceptUnsigned(int value) {
        // V4 prefixed button events include CRLF; older streams omit it.
        // These delimiters belong to the event, even if it interrupted an
        // echoed command. Never leave them in the command response buffer.
        if (buttonDelimiterState != 0) {
            int state = buttonDelimiterState;
            buttonDelimiterState = 0;
            if (state == 1 && value == '\r') { buttonDelimiterState = 2; return; }
            if (value == '\n') return;
        }
        if (buttonPrefixLength == 0) {
            if (value == (BUTTON_PREFIX[0] & 0xff)) {
                buttonPrefixLength = 1;
            } else {
                appendResponse(value);
            }
            return;
        }
        if (buttonPrefixLength < BUTTON_PREFIX.length) {
            if (value == (BUTTON_PREFIX[buttonPrefixLength] & 0xff)) {
                buttonPrefixLength++;
                return;
            }
            flushButtonPrefix();
            acceptUnsigned(value);
            return;
        }
        if (value <= 0x1f) {
            buttonPrefixLength = 0;
            buttonDelimiterState = 1;
            listener.onButtonMask(value);
            return;
        }
        flushButtonPrefix();
        acceptUnsigned(value);
    }

    private void flushButtonPrefix() {
        int matched = buttonPrefixLength;
        buttonPrefixLength = 0;
        for (int index = 0; index < matched; index++) {
            appendResponse(BUTTON_PREFIX[index] & 0xff);
        }
    }

    private void appendResponse(int value) {
        if (responseLength == 0 && (value == '\r' || value == '\n')) return;
        if (responseLength >= response.length) {
            responseLength = 0;
            listener.onResponseOverflow();
        }
        response[responseLength++] = (byte) value;
        if (!endsWithPrompt()) return;
        String completed = new String(
                Arrays.copyOf(response, responseLength), StandardCharsets.US_ASCII);
        responseLength = 0;
        listener.onResponse(completed);
    }

    private boolean endsWithPrompt() {
        if (responseLength < RESPONSE_PROMPT.length) return false;
        int start = responseLength - RESPONSE_PROMPT.length;
        for (int index = 0; index < RESPONSE_PROMPT.length; index++) {
            if (response[start + index] != RESPONSE_PROMPT[index]) return false;
        }
        return true;
    }

    static boolean isExecutedAcknowledgement(String response, String command) {
        if (response == null || command == null || command.isEmpty()) return false;
        if (!response.endsWith(">>> ")) return false;
        return trimLineEnds(response.substring(0, response.length() - 4))
                .equals(trimLineEnds(command));
    }

    private static String trimLineEnds(String value) {
        int start = 0, end = value.length();
        while (start < end && (value.charAt(start) == '\r' || value.charAt(start) == '\n')) ++start;
        while (end > start && (value.charAt(end - 1) == '\r' || value.charAt(end - 1) == '\n')) --end;
        return value.substring(start, end);
    }
}
