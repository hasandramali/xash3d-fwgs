package su.xash.svencoop.model;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;

public final class SteamP2PSession implements AutoCloseable {
    public interface SignalSink { void send(byte[] signal) throws Exception; }
    public interface PacketSink { void receive(byte[] payload) throws Exception; }
    public interface FailureSink { void fail(String reason); }
    public interface ProgressSink { void update(String state) throws Exception; }
    private static final String ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    private final long local, peer;
    private final int app;
    private final SignalSink signaling;
    private final PacketSink packets;
    private final FailureSink failures;
    private final ProgressSink progress;
    private final SecureRandom random = new SecureRandom();
    private final String username = token(), password = token();
    private final DatagramSocket socket;
    private final List<Path> paths = new ArrayList<>();
    private final ArrayDeque<byte[]> queued = new ArrayDeque<>();
    private volatile boolean closed;
    private long lastConnectivity = System.nanoTime();
    private Path selected;
    private int signalCount, rejectedSignals;

    private static final class Path {
        InetSocketAddress address;
        String username;
        byte[] transaction;
        long sent, proven;
    }

    public SteamP2PSession(long local, long peer, int app, SignalSink signaling, PacketSink packets, FailureSink failures) throws Exception {
        this(local, peer, app, signaling, packets, failures, state -> {});
    }

    public SteamP2PSession(long local, long peer, int app, SignalSink signaling, PacketSink packets, FailureSink failures, ProgressSink progress) throws Exception {
        if (local <= 0 || peer <= 0 || peer == local) throw new IllegalArgumentException("P2P identity");
        this.local = local; this.peer = peer; this.app = app;
        this.signaling = signaling; this.packets = packets; this.failures = failures;
        this.progress = progress;
        socket = new DatagramSocket(new InetSocketAddress("0.0.0.0", 0));
        socket.setSoTimeout(100);
    }

    private String token() {
        StringBuilder value = new StringBuilder();
        for (int index = 0; index < 16; index++) value.append(ALPHABET.charAt(random.nextInt(ALPHABET.length())));
        return value.toString();
    }

    public void start() {
        Thread worker = new Thread(this::run, "steam-p2p-ice"); worker.setDaemon(true); worker.start();
    }

    public synchronized void receiveSignal(int message, byte[] data) {
        if (closed) return;
        signalCount++;
        try {
            SteamP2PWire.Signal signal = SteamP2PWire.decodeSignal(data, message == 5435);
            if (signal.source != peer || signal.destination != local ||
                    signal.app != app || (signal.secondaryApp != 0 && signal.secondaryApp != app)) {
                rejectedSignals++;
                if (signal.source != peer) progress.update("signal_rejected peer_identity");
                else if (signal.destination != local) progress.update("signal_rejected local_identity");
                else progress.update("signal_rejected app=" + signal.app + " secondary=" + signal.secondaryApp);
                return;
            }
            if (message == 5435) { fail("Steam peer rejected the session (" + signal.error + ")"); return; }
            if (signal.candidate == null) {
                progress.update("signal_without_candidate");
                return;
            }
            SteamP2PWire.Candidate candidate = SteamP2PWire.decodeCandidate(signal.candidate);
            if (!"udp".equals(candidate.protocol) || candidate.username.length() != 16 || paths.size() >= 32) {
                rejectedSignals++;
                progress.update("candidate_rejected protocol_credentials_or_limit");
                return;
            }
            InetSocketAddress address = SteamP2PWire.endpoint(candidate);
            for (Path path : paths) if (path.address.equals(address) && path.username.equals(candidate.username)) return;
            Path path = new Path(); path.address = address; path.username = candidate.username; paths.add(path);
            progress.update("checking remote_candidates=" + paths.size());
        } catch (Exception ignored) {
            rejectedSignals++;
            try { progress.update("signal_rejected invalid_encoding_or_endpoint"); } catch (Exception unavailable) {}
        }
    }

    public synchronized void send(byte[] payload) throws Exception {
        if (closed) throw new IllegalStateException("P2P session closed");
        byte[] data = SteamP2PWire.datagram(payload);
        if (selected != null && System.nanoTime() - selected.proven < 10_000_000_000L) transmit(data, selected.address);
        else if (queued.size() < 32) queued.add(data);
    }

    private void transmit(byte[] data, InetSocketAddress address) throws Exception {
        socket.send(new DatagramPacket(data, data.length, address));
    }

    private void gather() throws Exception {
        int count = 0;
        Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
        while (!closed && interfaces.hasMoreElements() && count < 16) {
            NetworkInterface network = interfaces.nextElement();
            if (!network.isUp() || network.isLoopback()) continue;
            Enumeration<InetAddress> addresses = network.getInetAddresses();
            while (!closed && addresses.hasMoreElements() && count < 16) {
                InetAddress address = addresses.nextElement();
                if (!(address instanceof Inet4Address) || address.isLoopbackAddress() || address.isAnyLocalAddress()) continue;
                SteamP2PWire.Candidate candidate = new SteamP2PWire.Candidate("valvep2p", "udp", address.getHostAddress(),
                        socket.getLocalPort(), 1.0f, "local", network.getName(), username, password, 0, 0);
                signaling.send(SteamP2PWire.candidateSignal(local, peer, app, candidate)); count++;
            }
        }
        if (!closed && count == 0) throw new IllegalStateException("No IPv4 interface for Steam P2P");
        if (!closed) progress.update("gathered local_candidates=" + count);
    }

    private synchronized void tick() throws Exception {
        long now = System.nanoTime();
        for (Path path : paths) {
            if (now - path.sent < 1_000_000_000L) continue;
            path.transaction = token().getBytes(java.nio.charset.StandardCharsets.US_ASCII);
            path.sent = now;
            transmit(SteamP2PWire.binding(1, path.transaction, path.username + username, null), path.address);
        }
        if (selected != null && now - selected.proven >= 10_000_000_000L) {
            selected = null;
            progress.update("rechecking");
        }
        if (selected == null && now - lastConnectivity > 45_000_000_000L)
            fail("Steam P2P ICE timed out: signals=" + signalCount + " rejected=" + rejectedSignals + " candidates=" + paths.size());
    }

    private synchronized void incoming(byte[] data, InetSocketAddress address) throws Exception {
        SteamP2PWire.Stun stun = SteamP2PWire.stun(data);
        if (stun != null) {
            if (stun.username == null) return;
            Path accepted = null;
            for (Path path : paths) {
                if (stun.type == 1 && stun.username.equals(username + path.username)) { accepted = path; break; }
                if (stun.type == 0x101 && path.address.equals(address) && path.transaction != null &&
                        Arrays.equals(path.transaction, stun.transaction) && stun.username.equals(path.username + username)) {
                    accepted = path; break;
                }
            }
            if (accepted == null) return;
            if (stun.type == 1) {
                transmit(SteamP2PWire.binding(0x101, stun.transaction, stun.username, address), address);
                if (!accepted.address.equals(address)) {
                    if (paths.size() >= 32) return;
                    Path learned = new Path(); learned.address = address; learned.username = accepted.username;
                    paths.add(learned); accepted = learned;
                }
            }
            accepted.proven = System.nanoTime();
            lastConnectivity = accepted.proven;
            boolean newlyConnected = selected == null;
            if (selected == null || selected.address.equals(address)) selected = accepted;
            if (newlyConnected) progress.update("ready");
            while (selected != null && !queued.isEmpty()) transmit(queued.remove(), selected.address);
            return;
        }
        boolean proven = false;
        for (Path path : paths) if (path.address.equals(address) && System.nanoTime() - path.proven < 10_000_000_000L) proven = true;
        if (!proven) return;
        byte[] payload = SteamP2PWire.gamePayload(data);
        if (payload != null) packets.receive(payload);
    }

    private void run() {
        try {
            gather();
            byte[] buffer = new byte[8192];
            while (!closed) {
                tick();
                DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                try { socket.receive(packet); }
                catch (SocketTimeoutException ignored) { continue; }
                if (packet.getLength() == buffer.length) continue;
                incoming(Arrays.copyOf(buffer, packet.getLength()), (InetSocketAddress)packet.getSocketAddress());
            }
        } catch (Exception failure) {
            if (!closed) fail("Steam P2P transport failed: " + failure.getClass().getSimpleName());
        } finally { close(); }
    }

    private synchronized void fail(String reason) {
        if (closed) return;
        close(); failures.fail(reason);
    }

    @Override public synchronized void close() {
        closed = true; socket.close(); queued.clear(); paths.clear(); selected = null;
    }
}
