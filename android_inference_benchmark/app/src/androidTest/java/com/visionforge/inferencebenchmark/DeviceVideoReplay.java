package com.visionforge.inferencebenchmark;

import android.os.SystemClock;
import android.os.Bundle;
import java.io.File;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Test-only paced source. Same VFRG wire format as UdpVideoPublisher, local routing. */
final class DeviceVideoReplay implements AutoCloseable {
    private volatile boolean stopped;
    volatile Throwable failure;
    volatile int sent;
    private final Thread worker;
    private final DatagramSocket socket;

    static int captureDimension(Bundle options,String key) {
        String value=options==null?null:options.getString(key);
        if(value==null || !value.matches("[0-9]+"))
            throw new IllegalArgumentException("Explicit physical recording "+key+" is required; no capture geometry is inferred");
        try {
            int parsed=Integer.parseInt(value);
            if(parsed>=64 && parsed<=4096)return parsed;
        } catch(NumberFormatException invalid) { /* Report the same explicit bound below. */ }
        throw new IllegalArgumentException("Physical recording "+key+" must be64..4096 pixels");
    }
    DeviceVideoReplay(File file, String local, int frames, int repeats,int fps,int captureWidth,int captureHeight) throws Exception {
        if(fps<1 || fps>240)throw new IllegalArgumentException("replay FPS out of range");
        if(captureWidth<64 || captureWidth>4096 || captureHeight<64 || captureHeight>4096)
            throw new IllegalArgumentException("Explicit physical recording dimensions must be64..4096 pixels");
        byte[] bytes=Files.readAllBytes(file.toPath());
        List<Integer> starts=new ArrayList<>();
        for (int i=0;i+4<bytes.length;i++) {
            int prefix=bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==1?3:
                    bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==0 && bytes[i+3]==1?4:0;
            if (prefix>0) {
                if ((bytes[i+prefix]&31)==9) starts.add(i);
                i+=prefix;
            }
        }
        if (starts.size()!=frames || starts.get(0)!=0)
            throw new AssertionError("Fixture must contain exactly "+frames+" AUD-delimited access units, got "+starts.size());
        starts.add(bytes.length);
        InetAddress address=InetAddress.getByName(local);
        socket=new DatagramSocket(new InetSocketAddress(address,5101));
        socket.setSendBufferSize(1024*1024);
        worker=new Thread(() -> {
            try {
                byte[] packet=new byte[1420]; ByteBuffer header=ByteBuffer.wrap(packet);
                DatagramPacket datagram=new DatagramPacket(packet,packet.length,address,5000);
                long origin=SystemClock.elapsedRealtimeNanos();
                for (int id=1;id<=frames*repeats && !stopped;id++) {
                    int index=(id-1)%frames, begin=starts.get(index), end=starts.get(index+1);
                    int count=(end-begin+1399)/1400;
                    for (int part=0,offset=begin;offset<end;part++,offset+=1400) {
                        int size=Math.min(1400,end-offset);
                        header.clear(); header.putInt(0x56465347).putInt(id).putShort((short)part).putShort((short)count)
                                .putInt(0).putShort((short)captureWidth).putShort((short)captureHeight);
                        System.arraycopy(bytes,offset,packet,20,size);
                        datagram.setLength(20+size);socket.send(datagram);
                    }
                    sent=id;
                    long remaining=origin+id*1000000000L/fps-SystemClock.elapsedRealtimeNanos();
                    if (remaining>0) Thread.sleep(remaining/1000000,(int)(remaining%1000000));
                }
            } catch (Throwable error) { if (!stopped) failure=error; }
        },"vf-test-local-replay");
        worker.start();
    }
    @Override public void close() {
        stopped=true; socket.close(); worker.interrupt();
        try {worker.join(2000);} catch (InterruptedException error) {Thread.currentThread().interrupt();}
    }
}
