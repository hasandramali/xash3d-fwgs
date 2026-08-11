# Steam Auth Implementation Plan (Sven Co-op / GoldSrc ticket broker)

Status: MODERN TOKEN AUTH IMPLEMENTED in `SteamAuthManager.kt` (unified-message flow mirroring
Pluvia/JavaSteam). Needs device testing/CI build. Classic password logon (5514 with field 51) is
kept only as a fallback; the UI now drives the token flow.

## Goal
Embed real Steam account login in the Android app, produce a real app ownership ticket + game
connect token + auth session ticket for Sven Co-op (appid 225840), and serve it to the engine's
existing `cl_steam.c` broker protocol (`cl_ticket_generator "steam"`, broker at `127.0.0.1:27420`).

## Key Decisions
- **Modern token auth is PRIMARY**: WebAPI `IAuthenticationService` over the CM connection via
  unified messages (`ServiceMethodCallFromClientNonAuthed` EMsg 9804), exactly as Pluvia/JavaSteam
  do on Android. Rationale: correct credentials on a guard-protected account return eresult=5 for
  the classic password logon, and Steam's supported path is token auth.
- Flow: `GetPasswordRSAPublicKey` -> RSA(password) -> `BeginAuthSessionViaCredentials` ->
  (email/2FA code via `UpdateAuthSessionWithSteamGuardCode`, or device-confirmation poll) ->
  `PollAuthSessionStatus` -> `refresh_token` -> `ClientLogon` (field 108 `access_token` = refresh token).
- Persistent login via stored `refresh_token` (prefs "steam_auth"). Classic login_key kept as backup.
- Goldberg/fake tickets abandoned. Real Steam ticket chain is unchanged:
  GetAppOwnershipTicket(857) + GameConnectTokens(779) + ClientAuthList(5432) -> session ticket.
- Android appearance: `client_os_type = -500` (AndroidUnknown), `platform_type = SteamClient(1)`.
- `-insecure` toggle in Steam settings, default ON (VAC off, Steam auth still works).
- Engine code untouched. Broker server runs on `127.0.0.1:27420` inside the app process.
- Transport base: `GameDataDownloader.kt` `SteamCMClient` (proven, works with SteamCM).

## Unified message (ServiceMethod) wire details — VERIFIED against JavaSteam
- Request: EMsg **9804** (`ServiceMethodCallFromClientNonAuthed`, chosen because steamID is null
  before logon), protobuf header with `client_steamid=0`, `client_sessionid=0`,
  `jobid_source` (field 10, fixed64), `target_job_name` (field 12, string `Service.Method#1`).
  Body = request proto.
- Response: EMsg **147** (`ServiceMethodResponse`), header carries `jobid_target` (field 11) =
  our jobid_source and `eresult` (field 13, int32, default 2). Body = response proto (raw, no wrapper).
- Job correlation reuses the existing `pendingJobs` map keyed on header `jobid_target`.
- RPC names (ProtoParser.kt: `"${service.name}.${method.methodName}#1"`):
  `Authentication.GetPasswordRSAPublicKey#1`, `Authentication.BeginAuthSessionViaCredentials#1`,
  `Authentication.PollAuthSessionStatus#1`, `Authentication.UpdateAuthSessionWithSteamGuardCode#1`.
- `CAuthentication_*` proto fields (steammessages_auth.steamclient.proto):
  - GetPasswordRSAPublicKey resp: publickey_mod(1), publickey_exp(2), timestamp(3 uint64).
  - BeginAuthSessionViaCredentials req: account_name(2), encrypted_password(3), encryption_timestamp(4),
    persistence(7 ESessionPersistence: Ephemeral=0, Persistent=1), website_id(8, "Client"),
    device_details(9 msg: device_friendly_name(1), platform_type(2=SteamClient), os_type(3=-500)),
    guard_data(10). Resp: client_id(1 uint64), request_id(2 bytes), interval(3 float/fixed32),
    allowed_confirmations(4 repeated msg: confirmation_type(1), associated_message(2)), steamid(5).
  - PollAuthSessionStatus req: client_id(1), request_id(2 bytes). Resp: new_client_id(1),
    refresh_token(3), access_token(4), account_name(6), new_guard_data(7).
  - UpdateAuthSessionWithSteamGuardCode req: client_id(1), steamid(2 fixed64), code(3), code_type(4).
  - EAuthSessionGuardType: None=1, EmailCode=2, DeviceCode=3, DeviceConfirmation=4.
- RSA password: `RSA/ECB/PKCS1Padding` (JavaSteam uses RSA/None/PKCS1Padding), base64, drop trailing "=".
- Token logon body: CMsgClientLogon field **108** = refresh token; JavaSteam also sends
  `client_package_version=1771` (field 5). Header steamid = UNKNOWN_INDIVIDUAL
  `0x0110000100000000` (accountID 0, instance 1) — same as JavaSteam token logon.
- Poll loop: `interval` (float, seconds) -> ms, floor 500 ms; overall timeout 120 s.

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

## Implementation status

### DONE (all in `android/app/src/main/java/su/xash/engine/model/SteamAuthManager.kt` + UI files)
- Self-contained CM client: TCP handshake (1301/1304/1303) + ClientHello 9805 + AES transport,
  reader thread that survives a single malformed packet, restartable on death.
- **Modern token auth (primary)**: `loginModern()` + `submitAuthCode()` +
  `loginWithStoredRefreshToken()`. Unified-message pipeline over EMsg 9804/147 with job
  correlation via header jobid_target; RSA password encryption; persistence=Persistent;
  guard-code submission; poll loop (interval ms, 120 s timeout); token logon via field 108.
- Classic fallback: `login()` (password/email/2FA, fields 51/84/101) and `loginWithStoredKey()`
  (field 60), both retained. Fragment now drives the modern flow.
- Ticket chain unchanged and working: GetAppOwnershipTicket(857→858) + GameConnectTokens(779) +
  ClientAuthList(5432→5575) → combined ticket via the local broker.
- UI: SteamAuthFragment (username/password + guard-code input), nav, drawable, strings,
  `-insecure` preference. XashActivity argv injection + `hasStoredSteamSession()`.
- Cold-start UI fix: `loginWithStoredRefreshToken()`/`loginWithStoredKey()` now set the in-memory
  `loggedIn` flag on success, so the fragment shows "Logged as" + logout button after a restore
  (previously it stayed on the login screen despite being logged on).
- Idle timeout: CM socket `soTimeout` relaxed to 120 s; reader survives read timeouts (kept the
  loop, no socket teardown), so long waits (e.g. guard code entry) no longer kill the connection.
- Re-entrancy guard: `loginModern()` returns Success immediately if already logged in, preventing
  a second auth flow that caused `logged off (eresult=26)` + connection close in testing.
- **Detailed diagnostics logging** (`-dev 9 -log` / logcat):
  - `engine/client/cl_steam.c`: logs the `sb_connect` request (server, server_steamid, secure,
    challenge) and hex-dumps the full broker ticket on receipt (`SteamBroker_DumpHex`).
  - `engine/client/cl_main.c`: logs the GoldSrc challenge auth info (steam_auth, server_steamid,
    vac2_secure), the full outgoing connect packet (protinfo/challenge/server/ticket hex)
    (`CL_DumpHex`), and the exact server rejection text in `CL_Reject`.
  - `SteamAuthManager.kt` broker: logs full ticket hex (`Ticket hex:`) alongside size/steamId.
- **Research (server-side validation)**:
  - ReHLDS/HLDS does *not* parse the ticket: it passes the raw blob to
    `steamclient.so::SendUserConnectAndAuthenticate`, which performs all checks. `CrossAuth`
    (metamod-r for goldsrc) documents the failure mode: *"the server checks the AppId stored in
    the player's ticket"* — a mismatched AppID yields `GameMismatch` / "STEAM validation rejected".
  - Our blob format was verified **byte-for-byte against 5 real Steam-generated CS:GO tickets**:
    `[20 GC magic][GCToken 8][SteamID 8][genDate 4][24][1][2][8 random][time][count]`
    `[appTicketSize][ticketDataLen][version][steamid][AppID][...][signature]`. JavaSteam's
    20-byte GameConnectToken layout matches (CM token = GCToken+SteamID+genDate).
  - AppID is embedded at **blob offset 0x48** (4-byte LE). For Sven Co-op (225840 = 0x370d0) the
    bytes at 0x48 should read `d0 70 03 00`. Confirm this in the new hex logs; if present, the
    mismatch is server-side (server's `steam_appid.txt`/launch AppID ≠ 225840).

### TODO
- CI build + device test: sign in, watch logcat `SteamAuth` for `begin auth` /
  `guard needed` / `logon response eresult=1`, then Sven Co-op connect
  (`148.251.68.215:27017`, server_steamid `90290412949884943`) with a real ticket.
- Verify refresh-token restore path (`loginWithStoredRefreshToken`) on a cold start
  now that it correctly flips `loggedIn` (UI should show "Logged as" after restore).
- DONE (verified against JavaSteam): Steam's `interval` is float seconds (typically 5.0); we use
  `interval * 1000` ms floor 500. JavaSteam's `delay(pollingInterval.toLong())` truncates the float
  to a Long of milliseconds (5.0 -> 5 ms) which is a bug in the reference; our interpretation is correct.

## Risks / Notes
- CM login can be rate-limited (eresult 84) — backoff.
- Guard-protected accounts: BeginAuthSession returns allowed_confirmations; we handle
  None / EmailCode / DeviceCode / DeviceConfirmation. DeviceConfirmation polls until the
  mobile-app approve (120 s timeout).
- Refresh token + guard_data stored in SharedPreferences ("steam_auth") as plaintext.
- GameConnectTokens may arrive slightly after logon; if the queue is empty at ticket time,
  wait/retry.
- Job IDs: jobid_source in header; response matched on jobid_target. Unified responses (147)
  match the same map.
- The broker must keep the CM connection + heartbeat alive for the whole game session.
