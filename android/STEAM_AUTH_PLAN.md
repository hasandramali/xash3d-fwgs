# Steam Auth Implementation Plan (Sven Co-op / GoldSrc ticket broker)

Status: protocol research VERIFIED (JavaSteam + SteamKit2 cross-checked). Implementation NOT started.

## Goal
Embeed real Steam account login (username/password + 2FA + persistent session) in the Android app,
produce a real app ownership ticket + game connect token + auth session ticket for Sven Co-op
(appid 225840), and serve it to the engine's existing `cl_steam.c` broker protocol
(`cl_ticket_generator "steam"`, broker at `127.0.0.1:27420`).

## Key Decisions
- Goldberg/fake tickets abandoned. Real Steam ticket via JavaSteam/SteamKit2 flow.
- No `CMsgClientGamesPlayed` (5410) — NOT required for the ticket. Ticket chain is
  GetAppOwnershipTicket(857) + GameConnectTokens(779) + ClientAuthList(5432) → session ticket.
- Linux appearance: `CMsgClientLogon.client_os_type = -500` (LinuxUnknown). Downloader already uses -500.
- `-insecure` toggle in Steam settings, default ON (VAC off, Steam auth still works).
- Engine code untouched. Broker server runs on `127.0.0.1:27420` inside the app process.
- Transport base: `GameDataDownloader.kt` `SteamCMClient` (proven, works with SteamCM).

## VERIFIED Wire/Proto Details

### EMsg values (from emsg.steamd)
```
ClientLogon=5514, ClientLogOnResponse=751, ClientLoggedOff=757, ClientLogOff=706,
ClientSessionToken=850, ClientGetAppOwnershipTicket=857, ClientGetAppOwnershipTicketResponse=858,
ClientGameConnectTokens=779, ClientAuthList=5432, ClientAuthListAck=5575,
ClientTicketAuthComplete=5429, ClientNewLoginKey=5463, ClientNewLoginKeyAccepted=5464,
ClientHeartBeat=1009, ClientHello=9805, ClientAccountInfo=768
```
ChannelEncryptRequest=1301, ChannelEncryptResult=1303, ChannelEncryptResponse=1304 (msg id 1304).
PROTO_MASK = 0x80000000.

### EResult codes (from eresult.steamd)
```
OK=1, InvalidPassword=5, AccountDisabled=43, AccountLogonDenied=63 (email code),
AccountLogonDeniedNoMail=66, AccountLogonDeniedVerifiedEmailRequired=74,
RateLimitExceeded=84, AccountLoginDeniedNeedTwoFactor=85,
TwoFactorCodeMismatch=88, TwoFactorActivationCodeMismatch=89
```

### CMsgClientLogon (steammessages_clientserver_login.proto:33) key fields
```
1 protocol_version (65581), 2 deprecated_obfustucated_private_ip, 3 cell_id,
6 client_language, 7 client_os_type (-500 = LinuxUnknown), 8 should_remember_password (bool),
11 obfuscated_private_ip (CMsgIPAddress), 22 client_supplied_steam_id (fixed64),
30 machine_id (bytes), 50 account_name, 51 password, 60 login_key,
83 sha_sentryfile, 84 auth_code, 85 otp_type, 86 otp_value, 87 otp_identifier,
96 machine_name, 100 client_instance_id, 101 two_factor_code, 102 supports_rate_limit_response,
108 access_token
```
JavaSteam SteamUser.kt logon (lines 64-149): accountName=50, password=51,
shouldRememberPassword (bool 8), protocolVersion=65581 (CurrentProtocol), clientOsType code, clientLanguage,
cellID, steam2TicketRequest, clientPackageVersion=1771, supportsRateLimitResponse=true, machineName,
machineId (bytes 30), authCode (email), twoFactorCode (101), accessToken. Sends steamid in protoHeader.steamid (field 1 fixed64), clientSessionid=0 in header.

### CMsgClientLogonResponse (prog line 94)
```
1 eresult (default 2), 2 legacy_out_of_game_heartbeat_seconds, 3 heartbeat_seconds,
7 cell_id, 20 client_supplied_steamid (fixed64)
```

### CMsgClientNewLoginKey / Accepted (login.proto:141/146)
```
NewLoginKey: 1 unique_id (uint32), 2 login_key (string)
NewLoginKeyAccepted: 1 unique_id
```
SteamKit2 SteamUser.cs:276 confirms `logon.Body.should_remember_password = details.ShouldRememberPassword`.

### CMsgClientGetAppOwnershipTicket / Response (clientserver.proto:65/69)
```
Request: 1 app_id (uint32)
Response: 1 eresult, 2 app_id, 3 ticket (bytes)
```
JavaSteam SteamApps.kt:60 sends sourceJobID (header field 10 jobid_source), body.appId.

### CMsgClientGameConnectTokens (clientserver.proto:79)
```
1 max_tokens_to_keep (default 10), 2 tokens (repeated bytes)
```
Arrive unsolicited after login. Queue them, keep only max_tokens_to_keep (drop oldest).

### CMsgClientAuthList / Ack (clientserver.proto:148/158)
```
AuthList: 1 tokens_left, 2 last_request_seq, 3 last_request_seq_from_server,
         4 tickets (repeated CMsgAuthTicket), 5 app_ids (repeated uint32)
AuthListAck: 1 ticket_crc (repeated uint32), 2 app_ids, 3 message_sequence
```

### CMsgAuthTicket (steammessages_base.proto:162)
```
1 estate, 2 eresult (default 2), 3 steamid (fixed64), 4 gameid (fixed64),
5 h_steam_pipe, 6 ticket_crc, 7 ticket (bytes), 8 server_secret (bytes), 9 ticket_type
```
For AuthSession ticket: gameid=appid(225840), ticket=authToken bytes, ticket_crc=crc32(authToken).
server_secret only when WebApi identity present (we use none for AuthSession).

### Ticket generation flow (JavaSteam SteamAuthTicket.kt:103-148, SteamKit2 SteamAuthTicket.cs:83-117)
```
1. appTicket = GetAppOwnershipTicket(857) await (858). Must be EResult.OK.
2. token = gameConnectTokens.poll() — else throw "no game connect tokens left".
3. authTicket = buildAuthTicket(token, TicketType.AuthSession=2)
4. crc = crc32(authTicket)
5. add CMsgAuthTicket{gameid=appid, ticket=authTicket, ticket_crc=crc} to ticketsByGame[appid]
6. send CMsgClientAuthList (5432) with sourceJobID, tokens_left, app_ids, tickets
7. await AuthListAck (5575): verify crc present in activeTicketsCRC
8. combined = authTicket + int32LE(appTicket.size) + appTicket  (CombineTickets)
```

### BuildAuthTicket byte layout (both impls identical, little-endian ints)
```
int32 gameConnectToken.size
gameConnectToken bytes
int32 sessionSize = 24 (4+4+4+4+4+4)
int32 1            (unknown, always 1)
int32 2            (TicketType.AuthSession)
8 random bytes     (public IP v4 slot + private IP v4 slot)
int32 timestamp    (System.nanoTime().toInt() / Stopwatch)
int32 sequence     (AtomicInteger.incrementAndGet, starts 0)
```
So session ticket = [size][token][24][1][2][8 randoms][ts][seq].

### CMsgClientAuthListAck job matching
JavaSteam uses AsyncJobSingle keyed by sourceJobID in proto header (field 10 jobid_source);
the ack's targetJobID (field 11 jobid_target) matches. Verify ack.ticket_crc contains our crc.

### Proto header (CMsgExtendedClientMsgHdr, base.proto:98-103)
```
1 steamid (fixed64), 2 client_sessionid (int32), 10 jobid_source (fixed64), 11 jobid_target, 12 target_job_name
```
Existing buildProtoHeader() in GameDataDownloader writes steamid(1), sessionid(2), jobidSource(10), targetJobName(12).

## Broker Protocol (engine cl_steam.c — DO NOT change engine)
- Request frame: `"SBRK"` (4 bytes) + uint16 LE payload_size + payload.
  payload = text: `sb_connect <ip:port> <server_steamid> <secure> <challenge>`
  (line 462: `sb_connect %s %"PRIu64" %d %d`, secure = vac2_secure?1:0)
  Also `sb_disconnect <ip:port> <challenge>` (line 480).
- Response frame: `"SBRK"` (4) + uint16 LE payload_size + payload.
  payload = `"sb_connect\n"` (11 bytes) + int32 LE challenge + uint64 LE steam_id + uint32 LE ticket_size + ticket bytes
  (ProcessFrame lines 239-267: header "sb_connect\n", MSG_ReadLong challenge, 8-byte steam_id, MSG_ReadDword ticket_size, ticket bytes).
- Engine checks challenge matches, copies steam_id to cls.steamid, sends GoldSrc connect packet with ticket.
- SBRK_MAX_FRAME_SIZE=4096, ticket max 2048.
- Broker must return ticket that is the COMBINED auth+ownership ticket.

## Machine ID (for real login; SteamKit2 HardwareUtils.cs:424-484)
MachineID is a binary KeyValues MessageObject. SteamKit2 writes via
KeyValue.RecursiveSaveBinaryToStream (KeyValue.cs:705-732):
- root (no value, has children): byte 0x00 (Type.None), name "MessageObject\0", children..., byte 0x08 (End), then final byte 0x08 (End).
- string child: byte 0x01 (Type.String), name\0, value\0.
MachineID children (HardwareUtils.cs:380-412): BB3=hex(SHA1(machineGuid)), FF2=hex(SHA1(macAddress)),
3B3=hex(SHA1(diskId)). GetHexString = SHA1 then lowercase hex.
Fallbacks (DefaultMachineInfoProvider): guid = machinename+"-SteamKit", mac="SteamKit-MacAddress", disk="SteamKit-DiskId".
Current downloader sends raw "JavaSteam-SerialNumber" for anon — works for anon but for a REAL logon use the well-formed MessageObject to be safe.

## Implementation plan (files)

### 1. `android/app/src/main/java/su/xash/engine/model/SteamAuthManager.kt` (NEW, main file)
Self-contained CM client + auth + ticket + broker. Reuse transport code from GameDataDownloader
(handshake, AES, readMessage, sendRaw, sendProtobufMsg, Proto, ProtoReader, ByteArrayOutputStream,
TCP_MAGIC, STEAM_PUBLIC_KEY, CM_SERVERS). Either:
  (a) duplicate the needed private helpers, or
  (b) extract shared transport into a reusable class (cleaner; consider this first).
Components:
- `class SteamAuthManager(ctx)` with state: logged-in account name, steamid, login_key persisted
  in SharedPreferences (e.g. "steam_auth"). Suspend API:
  - `connectAndLogin(username, password): Result` — handshake, logon; handles email/2FA/rate limits
  - `submitAuthCode(code)`, `submitTwoFactor(code)` — retry logon with code
  - `loginWithStoredKey(): Boolean` — logon using saved login_key (persistent session)
  - `logout()` — send ClientLogOff(706) or just disconnect
  - `getSessionTicket(appid=225840): ByteArray` — full ticket chain (above)
- Heartbeat thread (1009) using heartbeat_seconds from logon response.
- Handle async messages in a reader loop: GameConnectTokens(779) → queue;
  NewLoginKey(5463) → save login_key + reply NewLoginKeyAccepted(5464);
  ClientLoggedOff(757) → mark disconnected; AccountInfo(768) → ignore;
  TicketAuthComplete(5429) → optional log.
- Logon body fields: protocol_version=65581, client_language="english", client_os_type=-500,
  should_remember_password=true, machine_name, machine_id (well-formed MessageObject),
  account_name, password OR login_key, auth_code/two_factor_code when resubmitting,
  client_supplied_steam_id=stored steamid when using login key.
- Parse logon response 751: eresult from body field 1; heartbeat_seconds field 3;
  steamid from protoHeader.steamid (field 1 fixed64) + client_sessionid (field 2) for later headers.
- Distinguish email code (eresult 63 → auth_code field 84) vs 2FA (85 → two_factor_code field 101).
- On login success with should_remember_password, await NewLoginKey(5463), store login_key, ack 5464.

### 2. Broker server (in SteamAuthManager.kt or separate SteamBrokerServer.kt)
- `ServerSocket(27420)` on 127.0.0.1, accept loop on background thread.
- Read: 4 bytes "SBRK", uint16 LE len, payload. Parse `sb_connect`.
- On sb_connect: call `getSessionTicket(225840)` (suspend → run in executor/coroutine),
  respond SBRK frame: "sb_connect\n" + int32 LE challenge + uint64 LE steamid + uint32 LE ticket_size + ticket.
- On sb_disconnect: ignore (log).
- If not logged in → respond with empty ticket or close connection gracefully (log + no response).
- Broker lifecycle: start when logged in (and while game runs). Stop on logout.

### 3. UI
- Drawable `ic_baseline_steam_24.xml` (Steam logo vector).
- `menu_library.xml`: add Steam item (action_steam) with steam icon.
- `LibraryFragment.kt`: handle R.id.action_steam → navigate to SteamAuthFragment.
- `nav_graph.xml`: add SteamAuthFragment destination + action from libraryFragment.
- `SteamAuthFragment.kt` + layout: account status, login form (username/password), 2FA code field,
  email code field, login/logout buttons, status text (connecting/auth-code-needed/2fa-needed/error),
  `-insecure` SwitchPreference (key `steam_insecure`, default true).
- `strings.xml`: steam title, login, logout, account label, 2FA hint, email code hint, statuses, insecure.

### 4. XashActivity argv injection
In `getFinalArgv()` add when logged in:
- `+set cl_ticket_generator "steam"`
- `+set cl_steam_broker_addr 127.0.0.1:27420`
- `-insecure` when `steam_insecure` pref true (default on).
Gate on SteamAuthManager.isLoggedIn (or stored login_key exists). Use hasArgument() to avoid duplicates.

### 5. Test
- Test server `148.251.68.215:27017` (auth=1, server_steamid=90290412949884943, vac=1).
- Flow: login → start broker → launch Sven Co-op → connect → observe `CL_SendGoldSrcConnectPacket`
  with real ticket, no `STEAM validation rejected`.

## Risks / Notes
- CM login can be rate-limited (eresult 84) — backoff.
- Machine ID well-formedness matters for real logins; use SteamKit2 binary KV format.
- GameConnectTokens may need an interval; if queue empty, may need to wait/retry after login.
- Job IDs: set jobid_source in header for 857/5432; match ack jobid_target.
- The broker must keep the CM connection + heartbeat alive for the whole game session.
- Credentials/login_key stored in SharedPreferences (plaintext; note in UI? acceptable).
