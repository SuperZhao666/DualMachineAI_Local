package com.visionforge.inferencebenchmark;
final class DecoderCallbackTokenSelfTest {
    static void run() {
        long[] context={3,10};DecoderCallbackToken token=DecoderCallbackToken.create(context,2,10);
        context[0]=99;context[1]=11;
        if(token.binding!=3 || token.stream!=10 || token.codec!=2)throw new AssertionError("mutable codec identity");
        for(long[] invalid:new long[][]{null,new long[]{1},new long[]{0,10},new long[]{1,0},new long[]{1,11}}) {
            boolean rejected=false;
            try {DecoderCallbackToken.create(invalid,2,10);}catch(IllegalArgumentException expected){rejected=true;}
            if(!rejected)throw new AssertionError("stale/invalid native context accepted");
        }
    }
}
