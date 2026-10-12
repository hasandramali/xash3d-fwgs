package su.xash.svencoop.model;

import java.io.ByteArrayOutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class SteamP2PWire {
    private SteamP2PWire() {}

    public static final class Candidate {
        public final String name, protocol, address, type, network, username, password;
        public final int port, generation, extension;
        public final float preference;

        public Candidate(String name, String protocol, String address, int port, float preference,
                         String type, String network, String username, String password, int generation, int extension) {
            this.name = name; this.protocol = protocol; this.address = address; this.port = port;
            this.preference = preference; this.type = type; this.network = network;
            this.username = username; this.password = password; this.generation = generation; this.extension = extension;
        }
    }

    private static void string(ByteArrayOutputStream output, String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length >= 1024 || value.indexOf('\0') >= 0) throw new IllegalArgumentException("candidate string");
        output.write(bytes, 0, bytes.length); output.write(0);
    }

    private static String string(ByteBuffer input) {
        int start = input.position();
        while (input.hasRemaining() && input.position() - start < 1024) {
            if (input.get() == 0) return new String(input.array(), start, input.position() - start - 1, StandardCharsets.UTF_8);
        }
        throw new IllegalArgumentException("candidate terminator");
    }

    public static byte[] encodeCandidate(Candidate candidate) {
        if (candidate.port < 0 || candidate.port > 65535 || candidate.extension < 0 || candidate.extension > 9)
            throw new IllegalArgumentException("candidate bounds");
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        string(output, candidate.name); string(output, candidate.protocol); string(output, candidate.address);
        byte[] numeric = ByteBuffer.allocate(6).putShort((short)candidate.port).putFloat(candidate.preference).array();
        output.write(numeric, 0, numeric.length);
        string(output, candidate.type); string(output, candidate.network); string(output, candidate.username); string(output, candidate.password);
        numeric = ByteBuffer.allocate(4).putInt(candidate.generation).array();
        output.write(numeric, 0, numeric.length); output.write(candidate.extension);
        return output.toByteArray();
    }

    public static Candidate decodeCandidate(byte[] data) {
        if (data.length > 7179) throw new IllegalArgumentException("candidate size");
        ByteBuffer input = ByteBuffer.wrap(data);
        String name = string(input), protocol = string(input), address = string(input);
        int port = input.getShort() & 65535; float preference = input.getFloat();
        String type = string(input), network = string(input), username = string(input), password = string(input);
        int generation = input.getInt();
        if (input.remaining() > 1) throw new IllegalArgumentException("candidate trailing data");
        int extension = input.hasRemaining() ? Math.min(input.get() & 255, 9) : 0;
        return new Candidate(name, protocol, address, port, preference, type, network, username, password, generation, extension);
    }

    public static InetSocketAddress endpoint(Candidate candidate) throws Exception {
        String[] parts = candidate.address.split("\\.", -1);
        if (parts.length != 4 || candidate.port == 0) throw new IllegalArgumentException("IPv4 candidate required");
        byte[] address = new byte[4];
        for (int index = 0; index < 4; index++) {
            if (!parts[index].matches("[0-9]{1,3}")) throw new IllegalArgumentException("candidate address");
            int value = Integer.parseInt(parts[index]);
            if (value > 255) throw new IllegalArgumentException("candidate address");
            address[index] = (byte)value;
        }
        int first = address[0] & 255;
        if (first == 0 || first == 127 || first >= 224) throw new IllegalArgumentException("unsafe candidate address");
        return new InetSocketAddress(InetAddress.getByAddress(address), candidate.port);
    }

    private static void varint(ByteArrayOutputStream output, long value) {
        while ((value & ~127L) != 0) { output.write((int)value & 127 | 128); value >>>= 7; }
        output.write((int)value);
    }

    private static void fixed64(ByteArrayOutputStream output, int field, long value) {
        varint(output, field * 8L + 1);
        byte[] data = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array();
        output.write(data, 0, data.length);
    }

    public static byte[] candidateSignal(long local, long peer, int app, Candidate candidate) {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        fixed64(output, 1, peer); fixed64(output, 2, local);
        varint(output, 24); varint(output, app);
        byte[] data = encodeCandidate(candidate);
        varint(output, 34); varint(output, data.length); output.write(data, 0, data.length);
        varint(output, 56); varint(output, app);
        return output.toByteArray();
    }

    public static final class Signal {
        public long source, destination;
        public boolean hasDestination;
        public int app, secondaryApp, error;
        public byte[] candidate;
    }

    private static long varint(ByteBuffer input) {
        long value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            int next = input.get() & 255;
            if (shift == 63 && next > 1) throw new IllegalArgumentException("varint overflow");
            value |= (long)(next & 127) << shift;
            if ((next & 128) == 0) return value;
        }
        throw new IllegalArgumentException("varint overflow");
    }

    public static Signal decodeSignal(byte[] data, boolean failure) {
        if (data.length > 16384) throw new IllegalArgumentException("signal size");
        ByteBuffer input = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        Signal result = new Signal();
        int seen = 0;
        while (input.hasRemaining()) {
            long tag = varint(input); int field = (int)(tag >>> 3), wire = (int)(tag & 7);
            if (field <= 0 || tag >>> 3 > 0x1fffffff) throw new IllegalArgumentException("signal tag");
            if (field <= 7) {
                int mask = 1 << field;
                if ((seen & mask) != 0) throw new IllegalArgumentException("duplicate signal field");
                seen |= mask;
            }
            if (field == 1 || field == 2) {
                if (wire != 1) throw new IllegalArgumentException("signal identity");
                long value = input.getLong();
                if (field == 1) {
                    result.destination = value;
                    result.hasDestination = true;
                } else result.source = value;
            } else if (field == 3 || (!failure && field == 7) || (failure && field == 4)) {
                if (wire != 0) throw new IllegalArgumentException("signal integer");
                long value = varint(input);
                if (value < 0 || value > 0xffffffffL) throw new IllegalArgumentException("signal integer bounds");
                if (field == 3) result.app = (int)value;
                else if (field == 7) result.secondaryApp = (int)value;
                else result.error = (int)value;
            } else if (!failure && field == 4) {
                if (wire != 2) throw new IllegalArgumentException("signal candidate");
                long length = varint(input);
                if (length <= 0 || length > 7179 || length > input.remaining()) throw new IllegalArgumentException("signal candidate bounds");
                result.candidate = new byte[(int)length]; input.get(result.candidate);
            } else {
                int count;
                if (wire == 0) { varint(input); continue; }
                else if (wire == 1) count = 8;
                else if (wire == 5) count = 4;
                else if (wire == 2) {
                    long length = varint(input);
                    if (length < 0 || length > input.remaining()) throw new IllegalArgumentException("signal length");
                    count = (int)length;
                } else throw new IllegalArgumentException("signal wire type");
                if (count > input.remaining()) throw new IllegalArgumentException("signal truncated");
                input.position(input.position() + count);
            }
        }
        return result;
    }

    public static byte[] datagram(byte[] payload) {
        if (payload.length == 0 || payload.length > 1200) throw new IllegalArgumentException("game packet size");
        byte[] result = new byte[payload.length + 2]; System.arraycopy(payload, 0, result, 2, payload.length); return result;
    }

    public static byte[] gamePayload(byte[] data) {
        if (data.length <= 6 || data[0] != 0 || data[1] != 1) return null;
        if (ByteBuffer.wrap(data, 2, 4).order(ByteOrder.LITTLE_ENDIAN).getInt() != 1) return null;
        if (data.length > 4096) return null;
        return Arrays.copyOfRange(data, 6, data.length);
    }

    public static final class Stun {
        public int type;
        public byte[] transaction;
        public String username;
    }

    public static Stun stun(byte[] data) {
        if (data.length < 20) return null;
        ByteBuffer input = ByteBuffer.wrap(data);
        int type = input.getShort() & 65535, size = input.getShort() & 65535;
        if ((type != 1 && type != 0x101) || size != data.length - 20) return null;
        Stun result = new Stun(); result.type = type; result.transaction = new byte[16]; input.get(result.transaction);
        if (ByteBuffer.wrap(result.transaction).getInt() == 0x2112a442) return null;
        while (input.hasRemaining()) {
            if (input.remaining() < 4) return null;
            int attribute = input.getShort() & 65535, length = input.getShort() & 65535;
            int padded = (length + 3) & ~3;
            if (padded > input.remaining()) return null;
            if (attribute == 6) {
                if (result.username != null || length != 32) return null;
                result.username = new String(data, input.position(), length, StandardCharsets.US_ASCII);
            }
            input.position(input.position() + padded);
        }
        return result;
    }

    private static void attribute(ByteArrayOutputStream output, int type, byte[] value) {
        output.write(type >> 8); output.write(type); output.write(value.length >> 8); output.write(value.length);
        output.write(value, 0, value.length);
        for (int count = value.length; (count & 3) != 0; count++) output.write(0);
    }

    public static byte[] binding(int type, byte[] transaction, String username, InetSocketAddress mapped) {
        if (transaction.length != 16 || username.length() != 32) throw new IllegalArgumentException("STUN identity");
        ByteArrayOutputStream attributes = new ByteArrayOutputStream();
        attribute(attributes, 6, username.getBytes(StandardCharsets.US_ASCII));
        if (mapped != null) {
            byte[] address = mapped.getAddress().getAddress();
            if (address.length != 4) throw new IllegalArgumentException("STUN address family");
            byte[] value = ByteBuffer.allocate(8).putShort((short)1).putShort((short)mapped.getPort()).put(address).array();
            attribute(attributes, 1, value);
        }
        byte[] body = attributes.toByteArray();
        return ByteBuffer.allocate(20 + body.length).putShort((short)type).putShort((short)body.length).put(transaction).put(body).array();
    }
}
