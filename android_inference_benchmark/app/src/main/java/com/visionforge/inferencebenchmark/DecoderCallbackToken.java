package com.visionforge.inferencebenchmark;

/** Immutable identity captured before codec creation, never read from a later codec. */
final class DecoderCallbackToken {
    final long binding,stream,codec;
    private DecoderCallbackToken(long binding,long stream,long codec) {this.binding=binding;this.stream=stream;this.codec=codec;}
    static DecoderCallbackToken create(long[] context,long codec) {
        return create(context,codec,0);
    }
    static DecoderCallbackToken create(long[] context,long codec,long expectedStream) {
        if(context==null || context.length!=2 || context[0]<=0 || context[1]<=0 || codec<=0)
            throw new IllegalArgumentException("native_decoder_callback_context_unavailable");
        if(expectedStream>0 && context[1]!=expectedStream)throw new IllegalArgumentException("stale_native_stream_restart");
        return new DecoderCallbackToken(context[0],context[1],codec);
    }
}
