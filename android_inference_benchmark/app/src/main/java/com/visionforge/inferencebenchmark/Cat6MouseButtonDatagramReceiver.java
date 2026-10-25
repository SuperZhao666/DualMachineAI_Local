package com.visionforge.inferencebenchmark;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;

/** The real fixed-length UDP admission boundary, including source/session ordering. */
final class Cat6MouseButtonDatagramReceiver {
    static final class Accepted {
        final Cat6MouseButtonProtocol.Packet packet;
        final boolean newSession;
        Accepted(Cat6MouseButtonProtocol.Packet packet,boolean newSession) { this.packet=packet;this.newSession=newSession; }
    }
    private final DatagramPacket datagram=new DatagramPacket(new byte[Cat6MouseButtonProtocol.PACKET_BYTES+1],
            Cat6MouseButtonProtocol.PACKET_BYTES+1);
    private final String host;
    private final int port;
    private int session,lastSequence;
    private boolean sequenceKnown;
    Cat6MouseButtonDatagramReceiver(String host,int port) { this.host=host;this.port=port; }
    Accepted receive(DatagramSocket socket) throws IOException {
        datagram.setLength(datagram.getData().length);
        socket.receive(datagram);
        if(datagram.getLength()!=Cat6MouseButtonProtocol.PACKET_BYTES || datagram.getAddress()==null
                || !host.equals(datagram.getAddress().getHostAddress()) || datagram.getPort()!=port)return null;
        Cat6MouseButtonProtocol.Packet packet=Cat6MouseButtonProtocol.decode(datagram.getData(),datagram.getLength());
        if(packet==null)return null;
        boolean newSession=packet.sessionId!=session;
        if(!newSession && sequenceKnown && !Cat6MouseButtonProtocol.isNewerSequence(packet.sequence,lastSequence))return null;
        session=packet.sessionId;lastSequence=packet.sequence;sequenceKnown=true;
        return new Accepted(packet,newSession);
    }
}
