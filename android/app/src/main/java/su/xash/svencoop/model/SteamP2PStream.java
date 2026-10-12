package su.xash.svencoop.model;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

public final class SteamP2PStream {
    public interface Sink { void send(byte[] data) throws Exception; }
    public interface Receiver { void receive(byte[] data) throws Exception; }
    private final Sink output;
    private final Receiver input;
    private final ArrayDeque<byte[]> pending = new ArrayDeque<>();
    private final Map<Integer, byte[]> reordered = new HashMap<>();
    private final ByteArrayOutputStream frames = new ByteArrayOutputStream();
    private int receiveNext, sendNext = 1, echoTimestamp, flightSequence;
    private byte[] flight;
    private boolean peerConnected, acknowledged;
    private long lastSend;
    private int reorderedBytes;

    public SteamP2PStream(Sink output, Receiver input) {
        this.output = output; this.input = input;
    }

    public boolean isOpen() { return peerConnected && acknowledged; }
    public boolean hasPeerHandshake() { return peerConnected; }

    public void close() throws Exception {
        try { packet(sendNext, 4, new byte[0]); }
        finally { pending.clear(); reordered.clear(); frames.reset(); flight = null; }
    }

    private void packet(int sequence, int flags, byte[] payload) throws Exception {
        byte[] data = ByteBuffer.allocate(25 + payload.length).put((byte)1).putInt(0)
                .putInt(sequence).putInt(receiveNext).put((byte)0).put((byte)flags)
                .putShort((short)61440).putInt((int)(System.nanoTime() / 1_000_000L))
                .putInt(echoTimestamp).put(payload).array();
        output.send(data);
    }

    public void tick(long now) throws Exception {
        if (!isOpen()) {
            if (now - lastSend >= 1_000_000_000L) {
                packet(0, 2, new byte[]{0}); lastSend = now;
            }
            return;
        }
        if (flight == null && !pending.isEmpty()) {
            flight = pending.remove(); flightSequence = sendNext; sendNext += flight.length; lastSend = 0;
        }
        if (flight != null && now - lastSend >= 1_000_000_000L) {
            packet(flightSequence, 0, flight); lastSend = now;
        }
    }

    public void send(byte[] application) {
        if (application.length == 0 || application.length > 1205) throw new IllegalArgumentException("P2P stream frame size");
        byte[] frame = ByteBuffer.allocate(application.length + 4).putInt(application.length).put(application).array();
        if (pending.size() + (frame.length + 1023) / 1024 > 32) return;
        for (int offset = 0; offset < frame.length; offset += 1024)
            pending.add(Arrays.copyOfRange(frame, offset, Math.min(offset + 1024, frame.length)));
    }

    public void receive(byte[] data) throws Exception {
        if (data.length < 25 || data.length > 8192 || data[0] != 1) return;
        ByteBuffer header = ByteBuffer.wrap(data); header.get();
        if (header.getInt() != 0) return;
        int sequence = header.getInt(), acknowledgment = header.getInt();
        if (header.get() != 0) return;
        int flags = header.get() & 255;
        if ((flags & ~6) != 0) return;
        header.getShort();
        int timestamp = header.getInt(); header.getInt();
        byte[] payload = Arrays.copyOfRange(data, 25, data.length);
        if ((flags & 4) != 0) throw new IllegalStateException("Steam P2P stream reset");
        if ((flags & 2) != 0) {
            if (payload.length == 0 || payload.length > 128 || payload[0] != 0 || sequence != 0) return;
            if (!peerConnected) { receiveNext = payload.length; peerConnected = true; }
            else if (receiveNext < payload.length) return;
        }
        if (acknowledgment >= 1 && acknowledgment <= sendNext && peerConnected) acknowledged = true;
        if ((flags & 2) == 0 && payload.length != 0) {
            if (!isOpen()) return;
            int distance = sequence - receiveNext;
            if (distance == 0) {
                append(payload); receiveNext += payload.length;
                byte[] next;
                while ((next = reordered.remove(receiveNext)) != null) {
                    reorderedBytes -= next.length; append(next); receiveNext += next.length;
                }
            } else if (distance > 0 && distance < 61440 && reordered.size() < 32 &&
                    reorderedBytes + payload.length <= 61440 && !reordered.containsKey(sequence)) {
                reordered.put(sequence, payload); reorderedBytes += payload.length;
            }
        }
        if (flight != null && acknowledgment == sendNext) flight = null;
        echoTimestamp = timestamp;
        if (payload.length != 0) packet(sendNext, 0, new byte[0]);
    }

    private void append(byte[] bytes) throws Exception {
        if (frames.size() + bytes.length > 65536) throw new IllegalStateException("P2P stream buffer limit");
        frames.write(bytes);
        byte[] data = frames.toByteArray();
        int offset = 0;
        while (data.length - offset >= 4) {
            int length = ByteBuffer.wrap(data, offset, 4).getInt();
            if (length <= 0 || length > 4095) throw new IllegalStateException("P2P stream frame length");
            if (data.length - offset - 4 < length) break;
            input.receive(Arrays.copyOfRange(data, offset + 4, offset + 4 + length));
            offset += 4 + length;
        }
        frames.reset(); frames.write(data, offset, data.length - offset);
    }
}
