package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Dependency-free byte-framing contracts for the MAKCU runtime response stream. */
final class MakcuResponseStreamParserSelfTest {
    static void run() {
        verifiesSplitCommandResponse();
        verifiesAsciiCommandEchoIsNotAButtonEvent();
        verifiesInterleavedControlCharacterButtonMasks();
        verifiesMultipleResponsesInOneTransfer();
        verifiesTerminatedButtonEventsCannotPoisonTheNextEcho();
        rejectsOtherEchoesAndErrors();
    }

    private static void verifiesSplitCommandResponse() {
        Recorder recorder = new Recorder();
        MakcuResponseStreamParser parser = new MakcuResponseStreamParser(recorder);
        String command = "km.move(12,-7)\r\n";
        byte[] response = (command + ">>> ").getBytes(StandardCharsets.US_ASCII);
        for (byte value : response) parser.accept(new byte[]{value}, 1);

        require(recorder.responses.size() == 1);
        require(MakcuResponseStreamParser.isExecutedAcknowledgement(
                recorder.responses.get(0), command));
        require(recorder.buttonMasks.isEmpty() && recorder.overflows == 0);
    }

    private static void verifiesAsciiCommandEchoIsNotAButtonEvent() {
        Recorder recorder = new Recorder();
        MakcuResponseStreamParser parser = new MakcuResponseStreamParser(recorder);
        String command = "km.left(0)\r\n";
        parser.accept((command + ">>> ").getBytes(StandardCharsets.US_ASCII),
                command.length() + 4);

        require(recorder.responses.size() == 1);
        require(recorder.buttonMasks.isEmpty());
        require(MakcuResponseStreamParser.isExecutedAcknowledgement(
                recorder.responses.get(0), command));
    }

    private static void verifiesInterleavedControlCharacterButtonMasks() {
        Recorder recorder = new Recorder();
        MakcuResponseStreamParser parser = new MakcuResponseStreamParser(recorder);
        String command = "km.move(1,2)\r\n";
        byte[] prefix = command.getBytes(StandardCharsets.US_ASCII);
        parser.accept(prefix, prefix.length);
        parser.accept(new byte[]{'k', 'm', '.', 0x0a, 'k', 'm', '.', 0x0d}, 8);
        parser.accept(">>> ".getBytes(StandardCharsets.US_ASCII), 4);

        require(recorder.buttonMasks.equals(List.of(0x0a, 0x0d)));
        require(recorder.responses.size() == 1);
        require(MakcuResponseStreamParser.isExecutedAcknowledgement(
                recorder.responses.get(0), command));
    }

    private static void verifiesMultipleResponsesInOneTransfer() {
        Recorder recorder = new Recorder();
        MakcuResponseStreamParser parser = new MakcuResponseStreamParser(recorder);
        String first = "km.move(1,0)\r\n";
        String second = "km.move(0,1)\r\n";
        byte[] both = (first + ">>> " + second + ">>> ")
                .getBytes(StandardCharsets.US_ASCII);
        parser.accept(both, both.length);

        require(recorder.responses.size() == 2);
        require(MakcuResponseStreamParser.isExecutedAcknowledgement(
                recorder.responses.get(0), first));
        require(MakcuResponseStreamParser.isExecutedAcknowledgement(
                recorder.responses.get(1), second));
    }

    private static void verifiesTerminatedButtonEventsCannotPoisonTheNextEcho() {
        String command = "km.move(12,-7)\r\n";
        for (int mask : new int[]{0, 1, 10, 13, 31}) {
            for (int insertion : new int[]{0, 5, command.length()}) {
                String event = "km." + (char)mask + "\r\n";
                byte[] bytes = (command.substring(0,insertion) + event
                        + command.substring(insertion) + ">>> ").getBytes(StandardCharsets.US_ASCII);
                for (int chunk=1; chunk<=bytes.length; ++chunk) {
                    Recorder recorder = new Recorder();
                    MakcuResponseStreamParser parser = new MakcuResponseStreamParser(recorder);
                    for (int at=0; at<bytes.length; at+=chunk) {
                        byte[] part=java.util.Arrays.copyOfRange(bytes,at,Math.min(bytes.length,at+chunk));
                        parser.accept(part,part.length);
                    }
                    require(recorder.buttonMasks.equals(List.of(mask)));
                    require(recorder.responses.size()==1);
                    require(MakcuResponseStreamParser.isExecutedAcknowledgement(recorder.responses.get(0),command));
                }
            }
        }
    }

    private static void rejectsOtherEchoesAndErrors() {
        String command="km.move(1,2)\r\n";
        require(MakcuResponseStreamParser.isExecutedAcknowledgement("\r\nkm.move(1,2)\r\n>>> ",command));
        require(!MakcuResponseStreamParser.isExecutedAcknowledgement("km.move(2,1)\r\n>>> ",command));
        require(!MakcuResponseStreamParser.isExecutedAcknowledgement("km.move(1,2)\r\nERR\r\n>>> ",command));
        require(!MakcuResponseStreamParser.isExecutedAcknowledgement(">>> ",command));
        require(!MakcuResponseStreamParser.isExecutedAcknowledgement("km.move(1,2)\r\n",command));
    }

    private static final class Recorder implements MakcuResponseStreamParser.Listener {
        final List<Integer> buttonMasks = new ArrayList<>();
        final List<String> responses = new ArrayList<>();
        int overflows;

        @Override
        public void onButtonMask(int mask) {
            buttonMasks.add(mask);
        }

        @Override
        public void onResponse(String response) {
            responses.add(response);
        }

        @Override
        public void onResponseOverflow() {
            overflows++;
        }
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("MAKCU response-stream contract failed");
    }
}
