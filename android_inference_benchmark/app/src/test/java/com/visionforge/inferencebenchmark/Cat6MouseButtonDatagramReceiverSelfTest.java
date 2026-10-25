package com.visionforge.inferencebenchmark;
import java.net.*;
import java.nio.*;
import java.util.Arrays;

final class Cat6MouseButtonDatagramReceiverSelfTest {
    static void run() throws Exception {
        InetAddress loopback=InetAddress.getByName("127.0.0.1");
        try(DatagramSocket sender=new DatagramSocket(new InetSocketAddress(loopback,0));
            DatagramSocket receiver=new DatagramSocket(new InetSocketAddress(loopback,0));
            DatagramSocket stranger=new DatagramSocket(new InetSocketAddress(loopback,0))) {
            receiver.setSoTimeout(3000);
            byte[] valid=packet(8,42,1),longPacket=Arrays.copyOf(valid,4096);
            send(sender,receiver,longPacket);
            DatagramPacket old=new DatagramPacket(new byte[16],16);receiver.receive(old);
            require(Cat6MouseButtonProtocol.decode(old.getData(),old.getLength())!=null);
            System.out.println("legacy_button_udp valid_prefix_4096_accepted=true received_length="+old.getLength());
            Cat6MouseButtonDatagramReceiver boundary=new Cat6MouseButtonDatagramReceiver("127.0.0.1",sender.getLocalPort());
            for(int size:new int[]{17,4096,15,0,17}) {
                send(sender,receiver,Arrays.copyOf(valid,size));require(boundary.receive(receiver)==null);
            }
            send(sender,receiver,valid);var accepted=boundary.receive(receiver);
            require(accepted!=null && accepted.newSession && accepted.packet.buttonMask==8);
            send(sender,receiver,valid);require(boundary.receive(receiver)==null);
            send(sender,receiver,packet(8,42,0));require(boundary.receive(receiver)==null);
            for(byte[] invalid:new byte[][]{packet(32,42,2),packet(8,0,2),reserved(packet(8,42,2))}) {
                send(sender,receiver,invalid);require(boundary.receive(receiver)==null);
            }
            send(stranger,receiver,packet(8,42,2));require(boundary.receive(receiver)==null);
            send(sender,receiver,packet(0,42,2));accepted=boundary.receive(receiver);
            require(accepted!=null && !accepted.newSession && accepted.packet.buttonMask==0);
            Cat6MouseButtonDatagramReceiver wrongHost=new Cat6MouseButtonDatagramReceiver("192.0.2.1",sender.getLocalPort());
            send(sender,receiver,packet(8,42,3));require(wrongHost.receive(receiver)==null);
        }
    }
    private static byte[] packet(int mask,int session,int sequence) {
        return ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN).putInt(0x56464d42).put((byte)1).put((byte)mask)
            .putShort((short)0).putInt(session).putInt(sequence).array();
    }
    private static byte[] reserved(byte[] packet) {packet[6]=1;return packet;}
    private static void send(DatagramSocket from,DatagramSocket to,byte[] bytes)throws Exception {
        from.send(new DatagramPacket(bytes,bytes.length,to.getLocalAddress(),to.getLocalPort()));
    }
    private static void require(boolean condition){if(!condition)throw new AssertionError("button UDP admission");}
}
