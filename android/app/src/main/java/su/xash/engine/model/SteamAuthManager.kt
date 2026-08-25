package su.xash.engine.model

import android.content.Context
import android.os.Build
import android.util.Base64
import android.util.Log
import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream
import java.math.BigInteger
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.KeyFactory
import java.security.SecureRandom
import java.security.spec.RSAPublicKeySpec
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import java.util.zip.CRC32
import java.util.zip.GZIPInputStream
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * Self-contained Steam CM client that performs a real user logon (password / email code / 2FA /
 * persistent login key), buffers game connect tokens, and generates the combined Steam auth
 * session ticket expected by the engine's SteamBroker (cl_steam.c, cl_ticket_generator "steam").
 *
 * It serves a local SBRK broker on 127.0.0.1:27420 that responds to "sb_connect" frames with
 * the combined auth+ownership session ticket. The appid is taken from the engine's sb_connect
 * frame (per gamedir, see cl_steam.c SteamBroker_GetGoldSrcAppId) and defaults to Sven Co-op 225840.
 *
 * Protocol facts verified against steam-refs/ (JavaSteam + SteamKit2):
 *  - EMsg: ClientLogon=5514, ClientLogOnResponse=751, ClientLoggedOff=757, ClientGameConnectTokens=779,
 *    ClientGetAppOwnershipTicket=857/858, ClientAuthList=5432, ClientAuthListAck=5575,
 *    ClientTicketAuthComplete=5429, ClientNewLoginKey=5463/5464, ClientLogOff=706, ClientHeartBeat=1009.
 *  - EResult: OK=1, InvalidPassword=5, AccountLogonDenied=63, AccountLogonDeniedNoMail=66,
 *    LogonDeniedVerifiedEmailRequired=74, RateLimitExceeded=84, AccountLoginDeniedNeedTwoFactor=85,
 *    TwoFactorCodeMismatch=88, TwoFactorActivationCodeMismatch=89.
 */
class SteamAuthManager(private val ctx: Context) {

    companion object {
        private const val TAG = "SteamAuth"
        const val APPID = 225840
        const val BROKER_PORT = 27420

        private val CM_SERVERS = listOf(
            "162.254.197.40" to 27017,
            "162.254.199.170" to 27017,
            "155.133.248.34" to 27017,
            "155.133.248.35" to 27017,
            "155.133.246.43" to 27017,
        )

        private const val PROTO_MASK = 0x80000000.toInt()
        private const val TCP_MAGIC = 0x31305456

        // EResult codes
        private const val ER_OK = 1
        private const val ER_INVALID_PASSWORD = 5
        private const val ER_ACCOUNT_DISABLED = 43
        private const val ER_ACCOUNT_LOGON_DENIED = 63
        private const val ER_ACCOUNT_LOGON_DENIED_NO_MAIL = 66
        private const val ER_LOGON_DENIED_VERIFIED_EMAIL_REQUIRED = 74
        private const val ER_RATE_LIMIT_EXCEEDED = 84
        private const val ER_LOGIN_DENIED_NEED_TWO_FACTOR = 85
        private const val ER_TWO_FACTOR_CODE_MISMATCH = 88
        private const val ER_TWO_FACTOR_ACTIVATION_CODE_MISMATCH = 89
        private const val ER_DUPLICATE_REQUEST = 23
        private const val ER_INVALID_LOGIN_AUTH_CODE = 56

        private const val PROTOCOL_VERSION = 65581
        private const val CLIENT_OS_TYPE_ANDROID_UNKNOWN = -500

        // Unified message (ServiceMethod) EMsg numbers, see JavaSteam emsg.steamd.
        private const val EMsg_SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED = 9804
        private const val EMsg_SERVICE_METHOD_RESPONSE = 147

        // CAuthentication_AllowedConfirmation.confirmation_type (EAuthSessionGuardType)
        private const val GUARD_TYPE_NONE = 1
        private const val GUARD_TYPE_EMAIL_CODE = 2
        private const val GUARD_TYPE_DEVICE_CODE = 3
        private const val GUARD_TYPE_DEVICE_CONFIRMATION = 4

        // EAuthTokenPlatformType
        private const val PLATFORM_STEAM_CLIENT = 1

        // SteamAuthTicket.TicketType (SteamKit2): AuthSession=2, WebApiTicket=5
        private const val TICKET_TYPE_AUTH_SESSION = 2

        // ESessionPersistence (enums.proto)
        private const val PERSISTENCE_EPHEMERAL = 0
        private const val PERSISTENCE_PERSISTENT = 1

        private const val AUTH_POLL_TIMEOUT_MS = 120_000L

        // SteamID(accountID=0, instance=1, universe=1, Individual) used as the header steamid
        // for a fresh username/password logon (mirrors JavaSteam SteamID.UNKNOWN_INDIVIDUAL).
        private const val UNKNOWN_INDIVIDUAL_STEAM_ID = 0x0110000100000000L

        @Volatile
        private var instance: SteamAuthManager? = null

        /** Process-wide singleton so the CM connection + broker outlive the fragment. */
        @JvmStatic
        fun get(ctx: Context): SteamAuthManager =
            instance ?: synchronized(this) {
                instance ?: SteamAuthManager(ctx.applicationContext).also { instance = it }
            }

        // Steam Public Universe RSA Public Key (from SteamKit2 KeyDictionary), same as downloader.
        private val STEAM_PUBLIC_KEY = byteArrayOf(
            0x30.toByte(), 0x81.toByte(), 0x9D.toByte(), 0x30.toByte(), 0x0D.toByte(), 0x06.toByte(), 0x09.toByte(), 0x2A.toByte(),
            0x86.toByte(), 0x48.toByte(), 0x86.toByte(), 0xF7.toByte(), 0x0D.toByte(), 0x01.toByte(), 0x01.toByte(), 0x01.toByte(),
            0x05.toByte(), 0x00.toByte(), 0x03.toByte(), 0x81.toByte(), 0x8B.toByte(), 0x00.toByte(), 0x30.toByte(), 0x81.toByte(),
            0x87.toByte(), 0x02.toByte(), 0x81.toByte(), 0x81.toByte(), 0x00.toByte(), 0xDF.toByte(), 0xEC.toByte(), 0x1A.toByte(),
            0xD6.toByte(), 0x2C.toByte(), 0x10.toByte(), 0x66.toByte(), 0x2C.toByte(), 0x17.toByte(), 0x35.toByte(), 0x3A.toByte(),
            0x14.toByte(), 0xB0.toByte(), 0x7C.toByte(), 0x59.toByte(), 0x11.toByte(), 0x7F.toByte(), 0x9D.toByte(), 0xD3.toByte(),
            0xD8.toByte(), 0x2B.toByte(), 0x7A.toByte(), 0xE3.toByte(), 0xE0.toByte(), 0x15.toByte(), 0xCD.toByte(), 0x19.toByte(),
            0x1E.toByte(), 0x46.toByte(), 0xE8.toByte(), 0x7B.toByte(), 0x87.toByte(), 0x74.toByte(), 0xA2.toByte(), 0x18.toByte(),
            0x46.toByte(), 0x31.toByte(), 0xA9.toByte(), 0x03.toByte(), 0x14.toByte(), 0x79.toByte(), 0x82.toByte(), 0x8E.toByte(),
            0xE9.toByte(), 0x45.toByte(), 0xA2.toByte(), 0x49.toByte(), 0x12.toByte(), 0xA9.toByte(), 0x23.toByte(), 0x68.toByte(),
            0x73.toByte(), 0x89.toByte(), 0xCF.toByte(), 0x69.toByte(), 0xA1.toByte(), 0xB1.toByte(), 0x61.toByte(), 0x46.toByte(),
            0xBD.toByte(), 0xC1.toByte(), 0xBE.toByte(), 0xBF.toByte(), 0xD6.toByte(), 0x01.toByte(), 0x1B.toByte(), 0xD8.toByte(),
            0x81.toByte(), 0xD4.toByte(), 0xDC.toByte(), 0x90.toByte(), 0xFB.toByte(), 0xFE.toByte(), 0x4F.toByte(), 0x52.toByte(),
            0x73.toByte(), 0x66.toByte(), 0xCB.toByte(), 0x95.toByte(), 0x70.toByte(), 0xD7.toByte(), 0xC5.toByte(), 0x8E.toByte(),
            0xBA.toByte(), 0x1C.toByte(), 0x7A.toByte(), 0x33.toByte(), 0x75.toByte(), 0xA1.toByte(), 0x62.toByte(), 0x34.toByte(),
            0x46.toByte(), 0xBB.toByte(), 0x60.toByte(), 0xB7.toByte(), 0x80.toByte(), 0x68.toByte(), 0xFA.toByte(), 0x13.toByte(),
            0xA7.toByte(), 0x7A.toByte(), 0x8A.toByte(), 0x37.toByte(), 0x4B.toByte(), 0x9E.toByte(), 0xC6.toByte(), 0xF4.toByte(),
            0x5D.toByte(), 0x5F.toByte(), 0x3A.toByte(), 0x99.toByte(), 0xF9.toByte(), 0x9E.toByte(), 0xC4.toByte(), 0x3A.toByte(),
            0xE9.toByte(), 0x63.toByte(), 0xA2.toByte(), 0xBB.toByte(), 0x88.toByte(), 0x19.toByte(), 0x28.toByte(), 0xE0.toByte(),
            0xE7.toByte(), 0x14.toByte(), 0xC0.toByte(), 0x42.toByte(), 0x89.toByte(), 0x02.toByte(), 0x01.toByte(), 0x11.toByte()
        )
    }

    sealed class LoginState {
        object Connecting : LoginState()
        object Success : LoginState()
        object NeedEmailCode : LoginState()
        object NeedTwoFactor : LoginState()
        data class Failed(val eresult: Int, val message: String) : LoginState()
        data class Error(val message: String) : LoginState()
    }

    private class SteamMsg(val emsg: Int, val body: ByteArray, val header: ByteArray = ByteArray(0))

    // A decoded packet with the (already stripped) proto header kept for job correlation.
    private class Packet(val emsg: Int, val body: ByteArray, val isProto: Boolean, val header: ByteArray)

    // A service method (unified message) call that failed with a non-OK eresult.
    private class AuthException(val eresult: Int, message: String) : Exception(message)

    // Result of CAuthentication_PollAuthSessionStatus.
    private class PollResult(
        val accountName: String,
        val accessToken: String,
        val refreshToken: String,
        val guardData: String
    )

    // In-flight credentials auth session (mirrors JavaSteam CredentialsAuthSession).
    private class AuthSessionState(
        var clientId: Long,
        val requestId: ByteArray,
        var pollingIntervalMs: Long,
        val allowedConfirmations: MutableList<Pair<Int, String>>,
        var steamId: Long,
        var pendingCodeType: Int = GUARD_TYPE_DEVICE_CODE
    )

    private val prefs = ctx.getSharedPreferences("steam_auth", Context.MODE_PRIVATE)

    // connection state
    private val connectLock = Mutex()
    private var socket: Socket? = null
    private var output: OutputStream? = null
    private var input: InputStream? = null
    private var sessionKey = ByteArray(32)
    @Volatile private var encrypted = false
    @Volatile private var currentSessionId = 0
    @Volatile private var currentSteamId = 0L
    private var heartbeatJob: Thread? = null
    private var heartbeatSeconds = 9
    private var readerThread: Thread? = null
    @Volatile private var connected = false
    @Volatile private var loggedIn = false

    // job correlation
    private val nextJobId = AtomicLong(1L)
    private val pendingJobs = ConcurrentHashMap<Long, CompletableFuture<SteamMsg>>()

    // logon response future (single in-flight logon at a time)
    private var logonFuture: CompletableFuture<Int>? = null
    @Volatile private var expectDisconnect = false

    // ticket state
    @Volatile private var currentGameDir: String? = null
    private val ticketSequence = AtomicLong(0L)
    private val gameConnectTokens = LinkedBlockingQueue<ByteArray>()
    private val ticketsByGame = ConcurrentHashMap<Int, MutableList<CMsgAuthTicket>>()
    private val ticketChangeLock = Any()

    // broker
    private var brokerServer: ServerSocket? = null
    private var brokerThread: Thread? = null

    // stored session
    val accountName: String? get() = prefs.getString("account_name", null)
    val storedSteamId: Long get() = prefs.getLong("steam_id", 0L)
    val hasStoredKey: Boolean
        get() = !prefs.getString("login_key", "").isNullOrEmpty() ||
            !prefs.getString("refresh_token", "").isNullOrEmpty()
    val hasStoredRefreshToken: Boolean get() = !prefs.getString("refresh_token", "").isNullOrEmpty()
    val isLoggedIn: Boolean get() = loggedIn
    val currentUsername: String get() = prefs.getString("account_name", null) ?: ""

    // auth pipeline serialization + in-flight session (kept across user code submission)
    private val authMutex = Mutex()
    private var authSession: AuthSessionState? = null

    private fun machineName(): String = Build.MODEL.ifEmpty { "Android" }
    private fun machineId(): ByteArray = (Build.MODEL + "-Xash3D").toByteArray(Charsets.UTF_8)

    /**
     * Connects to CM, performs the encryption handshake and starts the reader thread.
     * Throws on failure.
     */
    suspend fun connect() = withContext(Dispatchers.IO) {
        connectLock.withLock {
            if (connected) return@withContext
            connectSocket()
            doHandshake()
            startReader()
            connected = true
            Log.i(TAG, "connected to Steam CM")
        }
    }

    /**
     * Logs in with username/password, optionally supplying an email auth code or 2FA code.
     * On success persists the login key returned by Steam.
     */
    suspend fun login(
        username: String,
        password: String,
        authCode: String? = null,
        twoFactorCode: String? = null
    ): LoginState = withContext(Dispatchers.IO) {
        try {
            val result = connectLock.withLock {
                if (!connected) {
                    connectSocket()
                    doHandshake()
                    startReader()
                    connected = true
                }
                prefs.edit().putString("account_name", username).apply()

                sendLogon(
                    username = username,
                    password = password,
                    authCode = authCode,
                    twoFactorCode = twoFactorCode,
                    loginKey = null,
                    steamId = null
                )
            }
            handleLogonResult(result)
        } catch (e: Exception) {
            Log.e(TAG, "login error: ${e.message}")
            LoginState.Error(e.message ?: "login error")
        }
    }

    /**
     * Re-login using a stored login key (persistent session).
     */
    suspend fun loginWithStoredKey(): LoginState = withContext(Dispatchers.IO) {
        val loginKey = prefs.getString("login_key", null)
        val steamId = storedSteamId
        if (loginKey.isNullOrEmpty() || steamId == 0L) return@withContext LoginState.Failed(0, "No stored login key")
        try {
            authMutex.withLock {
                if (loggedIn) return@withContext LoginState.Success
                val result = connectLock.withLock {
                    if (!connected) {
                        connectSocket()
                        doHandshake()
                        startReader()
                        connected = true
                    }
                    sendLogon(
                        username = prefs.getString("account_name", "") ?: "",
                        password = null,
                        authCode = null,
                        twoFactorCode = null,
                        loginKey = loginKey,
                        steamId = steamId
                    )
                }
                if (result == ER_OK) {
                    loggedIn = true
                    prefs.edit().putBoolean("logged_in", true).apply()
                    startHeartbeat()
                    return@withContext LoginState.Success
                }
                prefs.edit().putBoolean("logged_in", false).apply()
                LoginState.Failed(result, "Login key rejected ($result)")
            }
        } catch (e: Exception) {
            Log.e(TAG, "stored key login error: ${e.message}")
            LoginState.Error(e.message ?: "stored key login error")
        }
    }

    // ------------------------------------------------------------------
    // Modern token auth (SteamWeb API via unified messages, mirrors Pluvia/JavaSteam)
    // ------------------------------------------------------------------

    /**
     * Logs in with username/password through the modern token-auth flow:
     * GetPasswordRSAPublicKey -> BeginAuthSessionViaCredentials -> (guard) -> PollAuthSessionStatus
     * -> refresh_token -> ClientLogon with access_token (field 108).
     * Returns NeedEmailCode/NeedTwoFactor when a guard code is required; call [submitAuthCode]
     * to continue. Persistent refresh token is stored for [loginWithStoredRefreshToken].
     */
    suspend fun loginModern(
        username: String,
        password: String,
        persistent: Boolean = true
    ): LoginState = withContext(Dispatchers.IO) {
        authMutex.withLock {
            if (loggedIn) return@withLock LoginState.Success
            try {
                connectLock.withLock {
                    if (!connected) {
                        connectSocket()
                        doHandshake()
                        startReader()
                        connected = true
                    }
                }
                prefs.edit().putString("account_name", username).apply()

                val session = beginAuthSession(username, password, persistent)
                authSession = session

                val preferred = sortConfirmations(session.allowedConfirmations).firstOrNull()
                    ?: return@withLock LoginState.Failed(2, "No allowed confirmations")
                when (preferred.first) {
                    GUARD_TYPE_NONE -> finishAuthSession(session)
                    GUARD_TYPE_DEVICE_CONFIRMATION -> finishAuthSession(session)
                    GUARD_TYPE_EMAIL_CODE -> {
                        session.pendingCodeType = GUARD_TYPE_EMAIL_CODE
                        LoginState.NeedEmailCode
                    }
                    GUARD_TYPE_DEVICE_CODE -> {
                        session.pendingCodeType = GUARD_TYPE_DEVICE_CODE
                        LoginState.NeedTwoFactor
                    }
                    else -> LoginState.Failed(2, "Unsupported confirmation type ${preferred.first}")
                }
            } catch (e: AuthException) {
                Log.w(TAG, "loginModern auth failure: eresult=${e.eresult} ${e.message}")
                authSession = null
                LoginState.Failed(e.eresult, e.message ?: "authentication failed")
            } catch (e: Exception) {
                Log.e(TAG, "loginModern error: ${e.message}")
                authSession = null
                LoginState.Error(e.message ?: "login error")
            }
        }
    }

    /**
     * Continues an in-progress credentials auth session by submitting the Steam Guard
     * code (email or 2FA) the account asked for, then polls to completion.
     */
    suspend fun submitAuthCode(code: String): LoginState = withContext(Dispatchers.IO) {
        authMutex.withLock {
            val session = authSession
                ?: return@withLock LoginState.Failed(0, "No auth session in progress")
            try {
                sendSteamGuardCode(session, code.trim(), session.pendingCodeType)
                finishAuthSession(session)
            } catch (e: AuthException) {
                when (e.eresult) {
                    ER_INVALID_LOGIN_AUTH_CODE, ER_TWO_FACTOR_CODE_MISMATCH -> {
                        // Session stays alive; user can retype and resubmit.
                        LoginState.Failed(e.eresult, "Incorrect code, try again")
                    }
                    else -> {
                        authSession = null
                        LoginState.Failed(e.eresult, e.message ?: "code rejected")
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "submitAuthCode error: ${e.message}")
                authSession = null
                LoginState.Error(e.message ?: "guard code error")
            }
        }
    }

    /**
     * Re-login using a stored refresh token (persistent session from [loginModern]).
     */
    suspend fun loginWithStoredRefreshToken(): LoginState = withContext(Dispatchers.IO) {
        val refreshToken = prefs.getString("refresh_token", null)
        val accountName = prefs.getString("account_name", null)
        if (refreshToken.isNullOrEmpty() || accountName.isNullOrEmpty()) {
            return@withContext LoginState.Failed(0, "No stored refresh token")
        }
        try {
            authMutex.withLock {
                if (loggedIn) return@withContext LoginState.Success
                val result = connectLock.withLock {
                    if (!connected) {
                        connectSocket()
                        doHandshake()
                        startReader()
                        connected = true
                    }
                    sendLogon(
                        username = accountName,
                        password = null,
                        authCode = null,
                        twoFactorCode = null,
                        loginKey = null,
                        steamId = null,
                        accessToken = refreshToken
                    )
                }
                if (result == ER_OK) {
                    loggedIn = true
                    prefs.edit().putBoolean("logged_in", true).apply()
                    startHeartbeat()
                    return@withContext LoginState.Success
                }
                prefs.edit().putBoolean("logged_in", false).apply()
                LoginState.Failed(result, "Refresh token rejected ($result)")
            }
        } catch (e: Exception) {
            Log.e(TAG, "stored refresh token login error: ${e.message}")
            LoginState.Error(e.message ?: "stored refresh token login error")
        }
    }

    fun logout() {
        try {
            if (connected) {
                sendRawMsg(ByteArray(0), 706) // ClientLogOff
            }
        } catch (_: Exception) { }
        prefs.edit()
            .putBoolean("logged_in", false)
            .putString("refresh_token", null)
            .apply()
        loggedIn = false
        stopBroker()
        disconnect()
    }

    /**
     * Non-suspending helper for XashActivity: restores the persisted session in the background
     * and starts the broker, so a game launched directly (without visiting the auth screen)
     * still gets tickets.
     */
    fun ensureSessionAsync() {
        Log.i(TAG, "ensureSessionAsync: isLoggedIn=$isLoggedIn hasStoredKey=$hasStoredKey")
        if (isLoggedIn) {
            Log.i(TAG, "ensureSessionAsync: already logged in, starting broker")
            startBroker()
            return
        }
        if (!hasStoredKey) return
        Thread {
            kotlinx.coroutines.runBlocking {
                val state = if (hasStoredRefreshToken) loginWithStoredRefreshToken() else loginWithStoredKey()
                Log.i(TAG, "ensureSessionAsync: restore result=$state")
                if (state is LoginState.Success) {
                    Log.i(TAG, "ensureSessionAsync: restore OK, starting broker")
                    startBroker()
                } else {
                    Log.e(TAG, "ensureSessionAsync: restore FAILED, broker NOT started")
                }
            }
        }.apply { isDaemon = true; start() }
    }

    /**
     * Generates the combined Steam auth session ticket for [appid]:
     * 1. GetAppOwnershipTicket (857 -> 858) - the real Valve-issued encrypted app ticket.
     *    For GoldSrc legacy this IS the trailing blob of the InitiateGameConnection ticket.
     * 2. consume a game connect token (779, buffered by reader)
     * 3. build the raw auth session ticket (legacy for GoldSrc, modern for others)
     * 4. send ClientAuthList (5432) and await the ack (5575)  [SKIPPED for GoldSrc legacy]
     * 5. for modern: return authTicket + int32LE(appTicket.size) + appTicket.
     *    For GoldSrc legacy: the app ticket is already embedded inside buildAuthTicket, so
     *    buildAuthTicket's output is returned directly.
     *
     * [serverSteamId] is the game server's SteamID from the engine's sb_connect frame;
     * when non-zero it is bound into the ticket via a SteamNetworkingIdentity (Option B).
     */
    suspend fun getSessionTicket(appid: Int = APPID, serverSteamId: Long = 0L): ByteArray = withContext(Dispatchers.IO) {
        if (!connected) throw Exception("Not connected to Steam")
        Log.i(TAG, "getSessionTicket: START appid=$appid serverSteamId=$serverSteamId connected=$connected loggedIn=$loggedIn tokensQueued=${gameConnectTokens.size}")

        // GoldSrc games (Half-Life, CS 1.6, Sven Coop, etc.) use legacy InitiateGameConnection format.
        // The legacy auth ticket already includes app ownership; no separate app ticket or ClientAuthList needed.
        val isGoldSrc = isGoldSrcAppId(appid)

        val appTicket = requestAppOwnershipTicket(appid)
            .also { Log.i(TAG, "getSessionTicket: app ownership ticket OK size=${it.size} (isGoldSrc=$isGoldSrc)") }

        val token = gameConnectTokens.poll()
            ?: throw Exception("No game connect tokens left (not yet delivered?)")
        Log.i(TAG, "getSessionTicket: got game connect token size=${token.size}")

        val authTicket = buildAuthTicket(token, serverSteamId, appTicket)
        Log.i(TAG, "getSessionTicket: built auth ticket size=${authTicket.size}")

        if (isGoldSrc) {
            // Legacy format: authTicket IS the complete ticket (218 bytes for Sven Coop).
            // BUT the Steam backend still requires the ticket to be registered via ClientAuthList
            // (EMsg 5432) before a server's SteamGameServer_BeginAuthSession can validate it.
            // A real PC Steam client always registers the ticket; without it the server's async
            // Steam auth callback never returns OK and it just NOPs the client forever (connect
            // accepted, signon never starts). Mirror the modern path: register + await ack.
            val crc = crc32(authTicket)
            synchronized(ticketChangeLock) {
                ticketsByGame.getOrPut(appid) { ArrayList() }.add(
                    CMsgAuthTicket(gameid = appid.toLong(), ticket = authTicket, ticketCrc = crc)
                )
            }
            val ackBody = sendAuthList(appid)
            val ackCrcs = readRepeatedVarints(ackBody, 1)
            Log.i(TAG, "getSessionTicket(GoldSrc): auth list sent, ack crcs=${ackCrcs.joinToString()} our=$crc")
            if (ackCrcs.none { it == crc }) {
                Log.w(TAG, "getSessionTicket(GoldSrc): AuthList ack did not contain our crc $crc (got ${ackCrcs.joinToString()})")
            }
            Log.i(TAG, "getSessionTicket: LEGACY mode - returning authTicket (${authTicket.size} bytes) after ClientAuthList")
            return@withContext authTicket
        }

        val crc = crc32(authTicket)
        Log.i(TAG, "getSessionTicket: auth crc=$crc")
        synchronized(ticketChangeLock) {
            ticketsByGame.getOrPut(appid) { ArrayList() }.add(
                CMsgAuthTicket(
                    gameid = appid.toLong(),
                    ticket = authTicket,
                    ticketCrc = crc
                )
            )
        }

        val ackBody = sendAuthList(appid)
        Log.i(TAG, "getSessionTicket: auth list sent, ack body size=${ackBody.size}")
        val ackCrcs = readRepeatedVarints(ackBody, 1)
        Log.i(TAG, "getSessionTicket: ack crcs=${ackCrcs.joinToString()} our=$crc")
        if (ackCrcs.none { it == crc }) {
            Log.w(TAG, "AuthList ack did not contain our crc $crc (got ${ackCrcs.joinToString()})")
        }

        val combined = combineTickets(authTicket, appTicket)
        Log.i(TAG, "getSessionTicket: DONE combined ticket size=${combined.size}")
        combined
    }

    private fun isGoldSrcAppId(appid: Int): Boolean {
        return when (appid) {
            10, 20, 30, 40, 50, 60, 70, 80, 100, 130, 150, 225840 -> true
            else -> false
        }
    }

    // ------------------------------------------------------------------
    // Broker server (SBRK protocol for cl_steam.c)
    // ------------------------------------------------------------------

    fun startBroker() {
        if (brokerServer != null) return
        Log.i(TAG, "startBroker: creating ServerSocket on 127.0.0.1:$BROKER_PORT")
        brokerThread = Thread {
            try {
                val server = ServerSocket(BROKER_PORT, 4, InetAddress.getByName("127.0.0.1"))
                brokerServer = server
                Log.i(TAG, "SteamBroker listening on 127.0.0.1:$BROKER_PORT (thread=${Thread.currentThread().name})")
                while (!server.isClosed) {
                    try {
                        Log.d(TAG, "broker: waiting for accept()")
                        val client = server.accept()
                        Log.i(TAG, "broker: ACCEPTED client from ${client.inetAddress.hostAddress}:${client.port}")
                        client.soTimeout = 30000
                        Thread { handleBrokerClient(client) }.apply { isDaemon = true; start() }
                    } catch (e: Exception) {
                        if (server.isClosed) break
                        Log.w(TAG, "broker accept error: ${e.message}", e)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "broker thread FAILED: ${e.message}", e)
            }
            Log.i(TAG, "broker: accept loop exited (isClosed=${brokerServer?.isClosed})")
        }.apply { isDaemon = true; name = "sbrk-broker"; start() }
    }

    fun stopBroker() {
        Log.i(TAG, "stopBroker: closing broker")
        try { brokerServer?.close() } catch (e: Exception) { Log.w(TAG, "stopBroker close: ${e.message}", e) }
        brokerServer = null
        brokerThread?.interrupt()
        brokerThread = null
    }

    private fun handleBrokerClient(client: Socket) {
        val tag = "sbrk-client-${client.port}"
        try {
            client.use { c ->
                val ins = c.getInputStream()
                val outs = c.getOutputStream()
                Log.i(TAG, "$tag: handler started")
                while (true) {
                    Log.d(TAG, "$tag: waiting for SBRK frame")
                    val payload = readSbrkFrame(ins, tag) ?: run {
                        Log.w(TAG, "$tag: readSbrkFrame returned null (EOF/bad header), closing client")
                        break
                    }
                    val command = String(payload, Charsets.UTF_8)
                    Log.i(TAG, "$tag: frame payload(${payload.size}): \"$command\" hex=${payload.joinToString(" ") { "%02x".format(it) }}")
                    if (command.startsWith("sb_connect")) {
                        val parts = command.split(" ")
                        val serverAddr = if (parts.size > 1) parts[1] else ""
                        val serverSteamId = if (parts.size > 2) parts[2].toLongOrNull() ?: 0L else 0L
                        val secure = if (parts.size > 3) parts[3] != "0" else false
                        val challenge = if (parts.size > 4) parts[4].toIntOrNull() ?: 0 else 0
                        val reqAppid = if (parts.size > 5) parts[5].toIntOrNull() ?: 0 else 0
                        val effectiveAppid = if (reqAppid > 0) reqAppid else (appidFromGameDir(currentGameDir) ?: APPID)
                        Log.i(TAG, "sb_connect server=$serverAddr serverSteamId=$serverSteamId secure=$secure challenge=$challenge reqAppid=$reqAppid effectiveAppid=$effectiveAppid gamedir=$currentGameDir parts=${parts.toList()}")
                        Log.d(TAG, "$tag: connected=${connected} loggedIn=$loggedIn tokens=${gameConnectTokens.size} steamId=$currentSteamId")
                        val ticket = try {
                            kotlinx.coroutines.runBlocking(Dispatchers.IO) { getSessionTicket(effectiveAppid, serverSteamId) }
                        } catch (e: Exception) {
                            Log.e(TAG, "$tag: getSessionTicket FAILED: ${e.message}", e)
                            throw e
                        }
                        val steamId = currentSteamId
                        Log.i(TAG, "Sending ticket size=${ticket.size} steamId=$steamId appid=$effectiveAppid")
                        Log.i(TAG, "Ticket hex: ${ticket.joinToString(" ") { "%02x".format(it) }}")
                        writeSbrkResponse(outs, challenge, steamId, ticket)
                        Log.i(TAG, "$tag: response written")
                    } else if (command.startsWith("sb_gamedir")) {
                        val parts = command.split(" ")
                        currentGameDir = if (parts.size > 1) parts[1] else null
                        Log.i(TAG, "sb_gamedir=$currentGameDir")
                    } else if (command.startsWith("sb_disconnect")) {
                        Log.i(TAG, "sb_disconnect ignored")
                        break
                    } else {
                        Log.w(TAG, "unknown broker command: $command")
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "$tag: broker client error: ${e.message}", e)
        }
        Log.i(TAG, "$tag: handler exiting")
    }

    private fun appidFromGameDir(gameDir: String?): Int? = when (gameDir?.lowercase()) {
        "svencoop" -> 225840
        "gearbox" -> 50
        "bshift" -> 130
        "tfc" -> 20
        "dod" -> 30
        "dmc" -> 40
        "ricochet" -> 60
        else -> 10 // valve, cstrike, hldm and other GoldSrc mods
    }

    private fun readSbrkFrame(ins: InputStream, tag: String = "sbrk"): ByteArray? {
        val header = ByteArray(4)
        var off = 0
        while (off < 4) {
            val r = ins.read(header, off, 4 - off)
            if (r == -1) return null
            off += r
        }
        Log.d(TAG, "$tag: frame header=${String(header, Charsets.US_ASCII)} bytes=${header.joinToString(" ") { "%02x".format(it) }}")
        if (String(header, Charsets.US_ASCII) != "SBRK") {
            Log.w(TAG, "bad SBRK header: ${String(header, Charsets.US_ASCII)}")
            return null
        }
        val lenBytes = ByteArray(2)
        off = 0
        while (off < 2) {
            val r = ins.read(lenBytes, off, 2 - off)
            if (r == -1) return null
            off += r
        }
        val size = (lenBytes[0].toInt() and 0xFF) or ((lenBytes[1].toInt() and 0xFF) shl 8)
        Log.d(TAG, "$tag: frame size=$size")
        if (size <= 0 || size > 8192) return null
        val payload = ByteArray(size)
        off = 0
        while (off < size) {
            val r = ins.read(payload, off, size - off)
            if (r == -1) return null
            off += r
        }
        return payload
    }

    private fun writeSbrkResponse(outs: OutputStream, challenge: Int, steamId: Long, ticket: ByteArray) {
        val responseHeader = "sb_connect\n".toByteArray(Charsets.UTF_8)
        val payload = ByteArrayOutputStream()
        payload.write(responseHeader)
        payload.write(Proto.packInt32(challenge))
        payload.write(Proto.packInt64(steamId))
        payload.write(Proto.packInt32(ticket.size))
        payload.write(ticket)
        val body = payload.toByteArray()
        val frame = ByteArrayOutputStream()
        frame.write("SBRK".toByteArray(Charsets.US_ASCII))
        frame.write(byteArrayOf((body.size and 0xFF).toByte(), ((body.size shr 8) and 0xFF).toByte()))
        frame.write(body)
        Log.i(TAG, "writeSbrkResponse: header=${String(responseHeader, Charsets.UTF_8).trim()} challenge=$challenge steamId=$steamId ticketSize=${ticket.size} bodySize=${body.size} frameSize=${body.size + 6}")
        Log.i(TAG, "writeSbrkResponse frame hex: ${frame.toByteArray().joinToString(" ") { "%02x".format(it) }}")
        outs.write(frame.toByteArray())
        outs.flush()
    }

    // ------------------------------------------------------------------
    // CM connection + handshake
    // ------------------------------------------------------------------

    private suspend fun connectSocket() = suspendCancellableCoroutine<Unit> { cont ->
        try {
            for ((host, port) in CM_SERVERS) {
                try {
                    val sock = Socket()
                    sock.connect(InetSocketAddress(host, port), 10000)
                    sock.soTimeout = 120000
                    socket = sock
                    input = sock.getInputStream()
                    output = sock.getOutputStream()
                    Log.i(TAG, "connected to $host:$port")
                    cont.resume(Unit)
                    return@suspendCancellableCoroutine
                } catch (e: Exception) {
                    Log.w(TAG, "connect failed $host:$port: ${e.message}")
                }
            }
            cont.resumeWithException(Exception("No Steam CM server reachable"))
        } catch (e: Exception) {
            cont.resumeWithException(e)
        }
    }

    private fun doHandshake() {
        SecureRandom().nextBytes(sessionKey)
        encrypted = false

        // 1. ChannelEncryptRequest (1301) — read and parse RSA key + challenge
        val encReq = readPacket()
        val pubKeyData: ByteArray
        val challenge: ByteArray
        if (encReq.isProto) {
            val pr = ProtoReader(encReq.body)
            pr.skipFieldAtTag() // protocol_version
            pr.skipFieldAtTag() // universe
            val tag3 = pr.readVarint()
            if (tag3 != (3 shl 3 or 2)) throw Exception("unexpected encrypt request field")
            val keyLen = pr.readVarint()
            pubKeyData = pr.readBytes(keyLen)
            challenge = if (pr.remaining > 0) pr.readBytes(pr.remaining) else ByteArray(0)
        } else {
            // binary handshake: header[8] then RSA key; use hardcoded key
            pubKeyData = STEAM_PUBLIC_KEY
            challenge = if (encReq.body.size > 8) encReq.body.copyOfRange(8, encReq.body.size) else ByteArray(0)
        }

        val pubKey = if (pubKeyData.contentEquals(STEAM_PUBLIC_KEY)) {
            KeyFactory.getInstance("RSA").generatePublic(
                java.security.spec.X509EncodedKeySpec(pubKeyData)
            )
        } else {
            val expLen = pubKeyData[0].toInt() and 0xFF
            val exp = pubKeyData.copyOfRange(1, 1 + expLen)
            val mod = pubKeyData.copyOfRange(1 + expLen, pubKeyData.size)
            KeyFactory.getInstance("RSA").generatePublic(
                RSAPublicKeySpec(BigInteger(1, mod), BigInteger(1, exp))
            )
        }

        val rsa = Cipher.getInstance("RSA/ECB/OAEPWithSHA-1AndMGF1Padding")
        rsa.init(Cipher.ENCRYPT_MODE, pubKey)
        val encryptedBlob = rsa.doFinal(sessionKey + challenge)
        val crc = CRC32().apply { update(encryptedBlob) }.value.toInt()

        // 2. ChannelEncryptResponse (1304)
        sendRaw(buildEncryptResponse(encryptedBlob, crc))

        // 3. ChannelEncryptResult (1303) — expect eresult 1
        val result = readPacket()
        val eresult = Proto.readInt32LE(result.body, 0)
        if (eresult != 1) throw Exception("Encryption handshake failed: eresult=$eresult")
        encrypted = true
        Log.i(TAG, "handshake complete, encryption on")

        // 4. ClientHello (9805)
        sendProtobufMsg(9805, buildClientHello(), buildProtoHeader(0L, 0))
    }

    private fun startReader() {
        if (readerThread?.isAlive == true) return
        readerThread = Thread {
            Log.i(TAG, "reader: thread started (name=${Thread.currentThread().name})")
            try {
                while (!Thread.interrupted()) {
                    try {
                        handlePacket(readPacket())
                    } catch (e: java.net.SocketTimeoutException) {
                        // soTimeout guard: keep the loop (and the CM session) alive while idle.
                        Log.d(TAG, "reader read timeout, continuing")
                    }
                }
            } catch (e: Exception) {
                if (Thread.interrupted()) return@Thread
                Log.w(TAG, "reader stopped: ${e.message}", e)
            } finally {
                readerThread = null
                if (!expectDisconnect) {
                    Log.e(TAG, "reader: UNEXPECTED STOP — marking disconnected (was connected=$connected loggedIn=$loggedIn)")
                    connected = false
                    loggedIn = false
                    prefs.edit().putBoolean("logged_in", false).apply()
                    try { heartbeatJob?.interrupt() } catch (_: Exception) { }
                    try { socket?.close() } catch (_: Exception) { }
                    logonFuture?.completeExceptionally(Exception("Connection lost"))
                } else {
                    Log.i(TAG, "reader: stopped after expected disconnect")
                }
            }
        }.apply { isDaemon = true; name = "sbrk-reader"; start() }
    }

    private fun handlePacket(packet: Packet) {
        val emsg = packet.emsg
        try {
            when (emsg) {
                1 -> handleMulti(packet.body)
                779 -> handleGameConnectTokens(packet.body)
                5463 -> handleNewLoginKey(packet.body)
                751 -> handleLogonResponse(packet)
                757 -> {
                    Log.i(TAG, "logged off (eresult=${readEresult(packet.body)})")
                    connected = false
                    loggedIn = false
                    logonFuture?.completeExceptionally(Exception("Logged off"))
                }
                5429 -> { /* TicketAuthComplete — engine already validated */ }
                else -> {
                    if (packet.isProto) {
                        val jobId = readHeaderJobIdTarget(packet.header)
                        if (jobId != 0L) {
                            pendingJobs.remove(jobId)?.complete(SteamMsg(emsg, packet.body, packet.header))
                            return
                        }
                    }
                    Log.d(TAG, "ignoring emsg=$emsg size=${packet.body.size}")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "handlePacket emsg=$emsg FAILED: ${e.message} body=${packet.body.take(128).joinToString("") { "%02X".format(it) }} header=${packet.header.take(64).joinToString("") { "%02X".format(it) }}")
            // The packet is already fully consumed by readPacket(); do not tear down
            // the reader thread (and with it the CM session + logon future) on a
            // single malformed body/header.
        }
    }

    private fun handleLogonResponse(packet: Packet) {
        // extract steamid (field 1 fixed64) + sessionid (field 2 varint) from proto header
        try {
            if (packet.isProto) {
                val hr = ProtoReader(packet.header)
                while (hr.remaining > 0) {
                    val tag = hr.readVarint()
                    when (tag shr 3) {
                        1 -> if ((tag and 7) == 1) currentSteamId = hr.readLongLE() else hr.skipField(tag)
                        2 -> if ((tag and 7) == 0) currentSessionId = hr.readVarint() else hr.skipField(tag)
                        else -> hr.skipField(tag)
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "logon header parse failed: ${e.message} header=${packet.header.joinToString("") { "%02X".format(it) }}")
        }
        val rdr = ProtoReader(packet.body)
        var eresult = 2
        var eresultExtended = 0
        var legacyHeartbeat = 0
        try {
            while (rdr.remaining > 0) {
                val tag = rdr.readVarint()
                when (tag shr 3) {
                    1 -> eresult = rdr.readVarint()
                    2 -> legacyHeartbeat = rdr.readVarint()
                    3 -> heartbeatSeconds = rdr.readVarint()
                    10 -> eresultExtended = rdr.readVarint()
                    else -> rdr.skipField(tag)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "logon body parse failed: ${e.message} body=${packet.body.joinToString("") { "%02X".format(it) }}")
            logonFuture?.completeExceptionally(e)
            return
        }
        if (heartbeatSeconds <= 0) heartbeatSeconds = legacyHeartbeat
        Log.i(TAG, "logon response eresult=$eresult extended=$eresultExtended steamId=$currentSteamId sessionId=$currentSessionId heartbeat=$heartbeatSeconds")
        logonFuture?.complete(eresult)
    }

    private fun handleMulti(body: ByteArray) {
        val rdr = ProtoReader(body)
        var sizeUnzipped = 0u
        var msgBody: ByteArray? = null
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> sizeUnzipped = rdr.readVarint().toUInt()
                2 -> { val len = rdr.readVarint(); msgBody = rdr.readBytes(len) }
                else -> rdr.skipField(tag)
            }
        }
        if (msgBody == null) return
        var subData = msgBody
        if (sizeUnzipped > 0u) {
            val gis = GZIPInputStream(ByteArrayInputStream(msgBody))
            subData = gis.readBytes()
            gis.close()
            Log.d(TAG, "multi decompressed ${msgBody.size} -> ${subData.size} bytes")
        }
        var off = 0
        while (off < subData.size) {
            val subSize = Proto.readInt32LE(subData, off)
            off += 4
            if (subSize <= 0 || off + subSize > subData.size) break
            val subMsg = subData.copyOfRange(off, off + subSize)
            off += subSize
            val rawEmsg = Proto.readInt32LE(subMsg, 0)
            val subIsProto = (rawEmsg and PROTO_MASK) != 0
            val subEmsg = rawEmsg and 0x7FFFFFFF
            Log.d(TAG, "multi sub-msg emsg=$subEmsg proto=$subIsProto size=${subMsg.size}")
            if (subIsProto) {
                val headerLen = Proto.readInt32LE(subMsg, 4)
                val subHeader = if (headerLen > 0) subMsg.copyOfRange(8, 8 + headerLen) else ByteArray(0)
                val subBody = subMsg.copyOfRange(8 + headerLen, subMsg.size)
                handlePacket(Packet(subEmsg, subBody, true, subHeader))
            } else {
                handlePacket(Packet(subEmsg, subMsg.copyOfRange(20, subMsg.size), false, ByteArray(0)))
            }
        }
    }

    private fun handleGameConnectTokens(body: ByteArray) {
        val rdr = ProtoReader(body)
        var maxTokens = 10
        val tokens = mutableListOf<ByteArray>()
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> maxTokens = rdr.readVarint()
                2 -> { val len = rdr.readVarint(); tokens.add(rdr.readBytes(len)) }
                else -> rdr.skipField(tag)
            }
        }
        tokens.forEach { gameConnectTokens.offer(it) }
        while (gameConnectTokens.size > maxTokens) gameConnectTokens.poll()
        Log.i(TAG, "buffered ${tokens.size} game connect tokens (total ${gameConnectTokens.size})")
    }

    private fun handleNewLoginKey(body: ByteArray) {
        val rdr = ProtoReader(body)
        var uniqueId = 0
        var key = ""
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> uniqueId = rdr.readVarint()
                2 -> { val len = rdr.readVarint(); key = String(rdr.readBytes(len), Charsets.UTF_8) }
                else -> rdr.skipField(tag)
            }
        }
        Log.i(TAG, "received new login key (unique_id=$uniqueId)")
        prefs.edit()
            .putString("login_key", key)
            .putLong("steam_id", currentSteamId)
            .putBoolean("logged_in", true)
            .apply()
        loggedIn = true
        // ack ClientNewLoginKeyAccepted (5464)
        try {
            val ackBody = ByteArrayOutputStream()
            ackBody.write(Proto.packVarint(1 shl 3 or 0))
            ackBody.write(Proto.packVarint(uniqueId))
            sendProtobufMsg(5464, ackBody.toByteArray(), buildProtoHeader(currentSteamId, currentSessionId))
        } catch (e: Exception) {
            Log.w(TAG, "new login key ack failed: ${e.message}")
        }
    }

    // ------------------------------------------------------------------
    // Logon
    // ------------------------------------------------------------------

    private fun sendLogon(
        username: String,
        password: String?,
        authCode: String?,
        twoFactorCode: String?,
        loginKey: String?,
        steamId: Long?,
        accessToken: String? = null
    ): Int {
        val body = buildLogonBody(
            username = username,
            password = password,
            authCode = authCode,
            twoFactorCode = twoFactorCode,
            loginKey = loginKey,
            steamId = steamId,
            accessToken = accessToken
        )
        val headerSteamId = steamId ?: UNKNOWN_INDIVIDUAL_STEAM_ID
        val header = buildProtoHeader(headerSteamId, 0)
        val future = CompletableFuture<Int>()
        logonFuture = future
        sendProtobufMsg(5514, body, header)

        try {
            return future.get(60, TimeUnit.SECONDS)
        } catch (e: Exception) {
            throw Exception("Timed out waiting for logon response: ${e.message}")
        } finally {
            logonFuture = null
        }
    }

    private fun handleLogonResult(result: Int): LoginState {
        return when (result) {
            ER_OK -> {
                prefs.edit().putBoolean("logged_in", true).apply()
                loggedIn = true
                startHeartbeat()
                LoginState.Success
            }
            ER_INVALID_PASSWORD -> LoginState.Failed(result, "Invalid password")
            ER_ACCOUNT_DISABLED -> LoginState.Failed(result, "Account disabled")
            ER_ACCOUNT_LOGON_DENIED,
            ER_ACCOUNT_LOGON_DENIED_NO_MAIL,
            ER_LOGON_DENIED_VERIFIED_EMAIL_REQUIRED -> LoginState.NeedEmailCode
            ER_LOGIN_DENIED_NEED_TWO_FACTOR -> LoginState.NeedTwoFactor
            ER_TWO_FACTOR_CODE_MISMATCH,
            ER_TWO_FACTOR_ACTIVATION_CODE_MISMATCH -> LoginState.Failed(result, "Two-factor code mismatch")
            ER_RATE_LIMIT_EXCEEDED -> LoginState.Failed(result, "Rate limited, try again later")
            else -> LoginState.Failed(result, "Login failed (eresult=$result)")
        }
    }

    // ------------------------------------------------------------------
    // Token auth pipeline internals (mirror JavaSteam SteamAuthentication/AuthSession)
    // ------------------------------------------------------------------

    /** Sends a unified message (ServiceMethodCallFromClientNonAuthed) and awaits the response. */
    private suspend fun unifiedRequest(
        serviceMethod: String,
        body: ByteArray,
        allowedEresults: Set<Int> = setOf(ER_OK)
    ): ByteArray {
        val jobId = nextJobId.getAndIncrement()
        val future = CompletableFuture<SteamMsg>()
        pendingJobs[jobId] = future
        sendProtobufMsg(
            EMsg_SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED,
            body,
            buildUnifiedHeader(serviceMethod, jobId)
        )
        val resp = awaitJob(future, serviceMethod)
        val eresult = readHeaderEresult(resp.header)
        if (eresult !in allowedEresults) {
            throw AuthException(eresult, "$serviceMethod failed: eresult=$eresult")
        }
        return resp.body
    }

    private suspend fun beginAuthSession(
        username: String,
        password: String,
        persistent: Boolean
    ): AuthSessionState {
        // 1. GetPasswordRSAPublicKey
        val rsaReq = ByteArrayOutputStream()
        writeStringField(rsaReq, 1, username)
        val rsaResp = unifiedRequest("Authentication.GetPasswordRSAPublicKey#1", rsaReq.toByteArray())
        val rdr = ProtoReader(rsaResp)
        var modHex = ""
        var expHex = ""
        var timestamp = 0L
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> { val len = rdr.readVarint(); modHex = String(rdr.readBytes(len), Charsets.UTF_8) }
                2 -> { val len = rdr.readVarint(); expHex = String(rdr.readBytes(len), Charsets.UTF_8) }
                3 -> timestamp = rdr.readLongVarint()
                else -> rdr.skipField(tag)
            }
        }
        if (modHex.isEmpty() || expHex.isEmpty()) throw Exception("No password RSA key returned")
        val encryptedPassword = encryptPasswordRsa(password, modHex, expHex)

        // 2. BeginAuthSessionViaCredentials
        val req = ByteArrayOutputStream()
        writeStringField(req, 2, username)
        writeStringField(req, 3, encryptedPassword)
        req.write(Proto.packVarint(4 shl 3 or 0)); req.write(Proto.packLongVarint(timestamp))
        req.write(Proto.packVarint(7 shl 3 or 0)); req.write(Proto.packVarint(if (persistent) PERSISTENCE_PERSISTENT else PERSISTENCE_EPHEMERAL))
        writeStringField(req, 8, "Client")
        val device = ByteArrayOutputStream()
        writeStringField(device, 1, machineName())
        device.write(Proto.packVarint(2 shl 3 or 0)); device.write(Proto.packVarint(PLATFORM_STEAM_CLIENT))
        device.write(Proto.packVarint(3 shl 3 or 0)); device.write(Proto.packVarint(CLIENT_OS_TYPE_ANDROID_UNKNOWN))
        val devBytes = device.toByteArray()
        req.write(Proto.packVarint(9 shl 3 or 2)); req.write(Proto.packVarint(devBytes.size)); req.write(devBytes)
        prefs.getString("guard_data", null)?.takeIf { it.isNotEmpty() }?.let { writeStringField(req, 10, it) }

        val resp = unifiedRequest("Authentication.BeginAuthSessionViaCredentials#1", req.toByteArray())
        val respRdr = ProtoReader(resp)
        var clientId = 0L
        var requestId = ByteArray(0)
        var interval = 5f
        var steamId = 0L
        val confirmations = mutableListOf<Pair<Int, String>>()
        while (respRdr.remaining > 0) {
            val tag = respRdr.readVarint()
            when (tag shr 3) {
                1 -> clientId = respRdr.readLongVarint()
                2 -> { val len = respRdr.readVarint(); requestId = respRdr.readBytes(len) }
                3 -> interval = Float.fromBits(respRdr.readFixed32())
                4 -> { val len = respRdr.readVarint(); confirmations.add(parseAllowedConfirmation(respRdr.readBytes(len))) }
                5 -> steamId = respRdr.readLongVarint()
                else -> respRdr.skipField(tag)
            }
        }
        if (clientId == 0L) throw Exception("BeginAuthSessionViaCredentials returned no client_id")
        return AuthSessionState(
            clientId = clientId,
            requestId = requestId,
            pollingIntervalMs = (interval.toLong() * 1000L).coerceAtLeast(500L),
            allowedConfirmations = confirmations,
            steamId = steamId
        )
    }

    private fun encryptPasswordRsa(password: String, modHex: String, expHex: String): String {
        val pubKey = KeyFactory.getInstance("RSA").generatePublic(
            RSAPublicKeySpec(BigInteger(modHex, 16), BigInteger(expHex, 16))
        )
        val cipher = Cipher.getInstance("RSA/ECB/PKCS1Padding")
        cipher.init(Cipher.ENCRYPT_MODE, pubKey)
        val encrypted = cipher.doFinal(password.toByteArray(Charsets.UTF_8))
        // JavaSteam: base64 then drop the trailing "="
        return Base64.encodeToString(encrypted, Base64.NO_WRAP).dropLast(1)
    }

    private fun parseAllowedConfirmation(data: ByteArray): Pair<Int, String> {
        val rdr = ProtoReader(data)
        var type = 0
        var msg = ""
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> type = rdr.readVarint()
                2 -> { val len = rdr.readVarint(); msg = String(rdr.readBytes(len), Charsets.UTF_8) }
                else -> rdr.skipField(tag)
            }
        }
        return type to msg
    }

    private fun sortConfirmations(list: List<Pair<Int, String>>): List<Pair<Int, String>> {
        val order = mapOf(
            GUARD_TYPE_NONE to 0,
            GUARD_TYPE_DEVICE_CONFIRMATION to 1,
            GUARD_TYPE_DEVICE_CODE to 2,
            GUARD_TYPE_EMAIL_CODE to 3,
            5 to 4, // EmailConfirmation
            6 to 5, // MachineToken
            0 to 6  // Unknown
        )
        return list.sortedBy { order[it.first] ?: Int.MAX_VALUE }
    }

    private suspend fun sendSteamGuardCode(session: AuthSessionState, code: String, codeType: Int) {
        val body = ByteArrayOutputStream()
        body.write(Proto.packVarint(1 shl 3 or 0)); body.write(Proto.packLongVarint(session.clientId))
        body.write(Proto.packVarint(2 shl 3 or 1)); body.write(Proto.packInt64(session.steamId))
        writeStringField(body, 3, code)
        body.write(Proto.packVarint(4 shl 3 or 0)); body.write(Proto.packVarint(codeType))
        // DuplicateRequest happens when the code was already accepted elsewhere; the next poll succeeds.
        unifiedRequest(
            "Authentication.UpdateAuthSessionWithSteamGuardCode#1",
            body.toByteArray(),
            allowedEresults = setOf(ER_OK, ER_DUPLICATE_REQUEST)
        )
    }

    private suspend fun pollAuthSessionStatus(session: AuthSessionState): PollResult? {
        val body = ByteArrayOutputStream()
        body.write(Proto.packVarint(1 shl 3 or 0)); body.write(Proto.packLongVarint(session.clientId))
        body.write(Proto.packVarint(2 shl 3 or 2))
        body.write(Proto.packVarint(session.requestId.size))
        body.write(session.requestId)
        val resp = unifiedRequest("Authentication.PollAuthSessionStatus#1", body.toByteArray())
        val rdr = ProtoReader(resp)
        var refreshToken = ""
        var accessToken = ""
        var accountName = ""
        var guardData = ""
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> session.clientId = rdr.readLongVarint()
                3 -> { val len = rdr.readVarint(); refreshToken = String(rdr.readBytes(len), Charsets.UTF_8) }
                4 -> { val len = rdr.readVarint(); accessToken = String(rdr.readBytes(len), Charsets.UTF_8) }
                6 -> { val len = rdr.readVarint(); accountName = String(rdr.readBytes(len), Charsets.UTF_8) }
                7 -> { val len = rdr.readVarint(); guardData = String(rdr.readBytes(len), Charsets.UTF_8) }
                else -> rdr.skipField(tag)
            }
        }
        if (refreshToken.isEmpty()) return null
        return PollResult(accountName, accessToken, refreshToken, guardData)
    }

    private suspend fun finishAuthSession(session: AuthSessionState): LoginState {
        val deadline = System.currentTimeMillis() + AUTH_POLL_TIMEOUT_MS
        while (System.currentTimeMillis() < deadline) {
            val result = pollAuthSessionStatus(session)
            if (result != null) return completeAuth(result, session.steamId)
            delay(session.pollingIntervalMs)
        }
        authSession = null
        return LoginState.Failed(2, "Auth session timed out")
    }

    private suspend fun completeAuth(result: PollResult, steamId: Long): LoginState {
        val username = result.accountName.ifEmpty { prefs.getString("account_name", "") ?: "" }
        prefs.edit()
            .putString("account_name", username)
            .putString("refresh_token", result.refreshToken)
            .putString("guard_data", result.guardData.orEmpty())
            .putLong("steam_id", steamId)
            .apply()
        authSession = null
        val logonResult = connectLock.withLock {
            if (!connected) {
                connectSocket()
                doHandshake()
                startReader()
                connected = true
            }
            sendLogon(
                username = username,
                password = null,
                authCode = null,
                twoFactorCode = null,
                loginKey = null,
                steamId = null,
                accessToken = result.refreshToken
            )
        }
        return handleLogonResult(logonResult)
    }

    private fun buildLogonBody(
        username: String,
        password: String?,
        authCode: String?,
        twoFactorCode: String?,
        loginKey: String?,
        steamId: Long?,
        accessToken: String? = null
    ): ByteArray {
        val buf = ByteArrayOutputStream()
        // 1 protocol_version
        buf.write(Proto.packVarint(1 shl 3 or 0)); buf.write(Proto.packVarint(PROTOCOL_VERSION))
        // 5 client_package_version (JavaSteam sends 1771 on token logons)
        if (accessToken != null) {
            buf.write(Proto.packVarint(5 shl 3 or 0)); buf.write(Proto.packVarint(1771))
        }
        // 6 client_language
        writeStringField(buf, 6, "english")
        // 7 client_os_type
        buf.write(Proto.packVarint(7 shl 3 or 0)); buf.write(Proto.packVarint(CLIENT_OS_TYPE_ANDROID_UNKNOWN))
        // 8 should_remember_password
        buf.write(Proto.packVarint(8 shl 3 or 0)); buf.write(Proto.packVarint(1))
        // 30 machine_id
        val mid = machineId()
        buf.write(Proto.packVarint(30 shl 3 or 2)); buf.write(Proto.packVarint(mid.size)); buf.write(mid)
        // 50 account_name
        writeStringField(buf, 50, username)
        // 51 password
        password?.let { writeStringField(buf, 51, it) }
        // 60 login_key
        loginKey?.let { writeStringField(buf, 60, it) }
        // 22 client_supplied_steam_id (fixed64)
        steamId?.let { buf.write(Proto.packVarint(22 shl 3 or 1)); buf.write(Proto.packInt64(it)) }
        // 84 auth_code (email)
        authCode?.let { writeStringField(buf, 84, it) }
        // 101 two_factor_code
        twoFactorCode?.let { writeStringField(buf, 101, it) }
        // 96 machine_name
        writeStringField(buf, 96, machineName())
        // 102 supports_rate_limit_response
        buf.write(Proto.packVarint(102 shl 3 or 0)); buf.write(Proto.packVarint(1))
        // 108 access_token (refresh token from the token auth flow)
        accessToken?.let { writeStringField(buf, 108, it) }
        return buf.toByteArray()
    }

    private fun writeStringField(buf: ByteArrayOutputStream, field: Int, value: String) {
        val bytes = value.toByteArray(Charsets.UTF_8)
        buf.write(Proto.packVarint(field shl 3 or 2))
        buf.write(Proto.packVarint(bytes.size))
        buf.write(bytes)
    }

    // ------------------------------------------------------------------
    // Ticket flow
    // ------------------------------------------------------------------

    private suspend fun requestAppOwnershipTicket(appid: Int): ByteArray {
        val jobId = nextJobId.getAndIncrement()
        val future = CompletableFuture<SteamMsg>()
        pendingJobs[jobId] = future

        val body = ByteArrayOutputStream()
        body.write(Proto.packVarint(1 shl 3 or 0)); body.write(Proto.packVarint(appid))
        sendProtobufMsg(857, body.toByteArray(), buildProtoHeader(currentSteamId, currentSessionId, jobId))
        Log.i(TAG, "requestAppOwnershipTicket: sent emsg=857 jobId=$jobId steamId=$currentSteamId sessionId=$currentSessionId")

        val resp = awaitJob(future, "GetAppOwnershipTicket")
        Log.i(TAG, "requestAppOwnershipTicket: response emsg=${resp.emsg} bodySize=${resp.body.size} bodyHex=${resp.body.joinToString(" ") { "%02x".format(it) }}")
        val rdr = ProtoReader(resp.body)
        var eresult = 2
        var ticket: ByteArray? = null
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                1 -> eresult = rdr.readVarint()
                3 -> { val len = rdr.readVarint(); ticket = rdr.readBytes(len) }
                else -> rdr.skipField(tag)
            }
        }
        Log.i(TAG, "requestAppOwnershipTicket: eresult=$eresult ticket=${ticket?.size?.let { "${it} bytes" } ?: "null"}")
        if (eresult != ER_OK || ticket == null) {
            throw Exception("Failed to obtain app ownership ticket: eresult=$eresult")
        }
        return ticket
    }

    private fun buildAuthTicket(gameConnectToken: ByteArray, serverSteamId: Long = 0L, appTicket: ByteArray): ByteArray {
        // Legacy GoldSrc/InitiateGameConnection ticket format:
        //   [0..4)   tokenLen
        //   [4..4+tokenLen) 20-byte game-connect token from CM
        //   [4+tokenLen..+4) sessionSize
        //   session(= appTicket verbatim):
        //     [0..4)   version/type = 0x3E
        //     [4..8)   field_count  = 0x04
        //     [8..16)  clientSteamID (LE)
        //     [16..20) appID (LE)
        //     [20..24) timestamp (LE)
        //     [24..28) clientIP (LE)
        //     [28..32) serverIP (LE) - 0
        //     [32..)   158-byte Valve-signed encrypted app ticket
        //
        // The CM app-ownership ticket (EMsg 858, requestAppOwnershipTicket) ALREADY IS the complete
        // legacy session block (it begins with 3e 00 00 00 04 00 00 00 ... and ends with the 158-byte
        // signature). Verified against a real Steam capture (steam-refs/sven/scop.pcap) and the PC
        // steamclient ticket: byte-identical 158-byte blob, static per account/app. We embed it
        // VERBATIM — do NOT add another header (that double-wraps and fails Steam validation).
        val stream = ByteArrayOutputStream()
        stream.write(Proto.packInt32(gameConnectToken.size)) // tokenLen = 20
        stream.write(gameConnectToken)                        // 20-byte game connect token from CM
        stream.write(Proto.packInt32(appTicket.size))          // sessionSize (190 for Sven Coop)
        stream.write(appTicket)                               // the session block, verbatim

        val bytes = stream.toByteArray()
        Log.i(TAG, "buildAuthTicket(legacy): serverSteamId=$serverSteamId ticket=${bytes.size}B session=${appTicket.size}B (expected 218 / 190)")
        return bytes
    }

    private suspend fun sendAuthList(appid: Int): ByteArray {
        val jobId = nextJobId.getAndIncrement()
        val future = CompletableFuture<SteamMsg>()
        pendingJobs[jobId] = future

        val body = ByteArrayOutputStream()
        // 1 tokens_left
        body.write(Proto.packVarint(1 shl 3 or 0)); body.write(Proto.packVarint(gameConnectTokens.size))
        // 5 app_ids
        body.write(Proto.packVarint(5 shl 3 or 0)); body.write(Proto.packVarint(appid))
        // 4 tickets (CMsgAuthTicket: gameid=4 fixed64, ticket_crc=6 varint, ticket=7 bytes)
        val tickets = synchronized(ticketChangeLock) { ticketsByGame[appid] ?: emptyList() }
        tickets.forEach { t ->
            val sub = ByteArrayOutputStream()
            sub.write(Proto.packVarint(4 shl 3 or 1)); sub.write(Proto.packInt64(t.gameid))
            sub.write(Proto.packVarint(6 shl 3 or 0)); sub.write(Proto.packVarint(t.ticketCrc))
            sub.write(Proto.packVarint(7 shl 3 or 2)); sub.write(Proto.packVarint(t.ticket.size)); sub.write(t.ticket)
            body.write(Proto.packVarint(4 shl 3 or 2)); body.write(Proto.packVarint(sub.size)); body.write(sub.toByteArray())
        }

        sendProtobufMsg(5432, body.toByteArray(), buildProtoHeader(currentSteamId, currentSessionId, jobId))
        Log.i(TAG, "sendAuthList: sent emsg=5432 jobId=$jobId tokensLeft=${gameConnectTokens.size} appid=$appid tickets=${tickets.size}")
        val resp = awaitJob(future, "ClientAuthListAck")
        Log.i(TAG, "sendAuthList: ack emsg=${resp.emsg} bodySize=${resp.body.size} bodyHex=${resp.body.joinToString(" ") { "%02x".format(it) }}")
        return resp.body
    }

    private fun combineTickets(authTicket: ByteArray, appTicket: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(authTicket)
        out.write(Proto.packInt32(appTicket.size))
        out.write(appTicket)
        Log.i(TAG, "combineTickets: authTicket=${authTicket.size}B appTicket=${appTicket.size}B combined=${out.size}B")
        return out.toByteArray()
    }

    private class CMsgAuthTicket(val gameid: Long, val ticket: ByteArray, val ticketCrc: Int)

    private fun crc32(data: ByteArray): Int = CRC32().apply { update(data) }.value.toInt()

    // ------------------------------------------------------------------
    // Low-level proto helpers
    // ------------------------------------------------------------------

    private fun readEresult(body: ByteArray): Int {
        val rdr = ProtoReader(body)
        return try {
            rdr.readVarint() // tag
            rdr.readVarint()
        } catch (e: Exception) { 0 }
    }

    private fun readRepeatedVarints(body: ByteArray, fieldNum: Int): List<Int> {
        val rdr = ProtoReader(body)
        val result = mutableListOf<Int>()
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            if (tag shr 3 == fieldNum && (tag and 7) == 0) result.add(rdr.readVarint())
            else rdr.skipField(tag)
        }
        return result
    }

    private fun readHeaderJobIdTarget(header: ByteArray): Long {
        val rdr = ProtoReader(header)
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                11 -> if ((tag and 7) == 1) return rdr.readLongLE() else rdr.skipField(tag)
                else -> rdr.skipField(tag)
            }
        }
        return 0L
    }

    // CMsgProtoBufHeader.eresult (field 13) — carries the service method result.
    private fun readHeaderEresult(header: ByteArray): Int {
        val rdr = ProtoReader(header)
        var eresult = 2
        while (rdr.remaining > 0) {
            val tag = rdr.readVarint()
            when (tag shr 3) {
                13 -> if ((tag and 7) == 0) eresult = rdr.readVarint() else rdr.skipField(tag)
                else -> rdr.skipField(tag)
            }
        }
        return eresult
    }

    // ------------------------------------------------------------------
    // Packet framing / encryption (mirrors GameDataDownloader transport)
    // ------------------------------------------------------------------

    private fun readPacket(): Packet {
        val lenBytes = readExact(4)
        val packetLen = (lenBytes[0].toInt() and 0xFF) or
            ((lenBytes[1].toInt() and 0xFF) shl 8) or
            ((lenBytes[2].toInt() and 0xFF) shl 16) or
            ((lenBytes[3].toInt() and 0xFF) shl 24)
        if (packetLen < 4 || packetLen > 1000000) throw Exception("suspicious packetLen=$packetLen")

        val magicBytes = readExact(4)
        val magic = (magicBytes[0].toInt() and 0xFF) or
            ((magicBytes[1].toInt() and 0xFF) shl 8) or
            ((magicBytes[2].toInt() and 0xFF) shl 16) or
            ((magicBytes[3].toInt() and 0xFF) shl 24)
        if (magic != TCP_MAGIC) throw Exception("invalid magic 0x%08X".format(magic))

        val encryptedPayload = readExact(packetLen)
        Log.d(TAG, String.format("[read] packetLen=%d magic=0x%08X encHex=%s", packetLen, magic, encryptedPayload.take(16).joinToString("") { "%02X".format(it) } + "..."))
        val payload = if (encrypted) decryptAES(encryptedPayload) else encryptedPayload
        if (payload.size < 8) throw Exception("payload too small: ${payload.size}")

        val rawEmsg = Proto.readInt32LE(payload, 0)
        val isProto = (rawEmsg and PROTO_MASK) != 0
        val emsg = rawEmsg and 0x7FFFFFFF
        val body: ByteArray
        val header: ByteArray
        if (isProto) {
            val headerLen = Proto.readInt32LE(payload, 4)
            if (headerLen > payload.size - 8) throw Exception("headerLen=$headerLen exceeds payload")
            header = if (headerLen > 0) payload.copyOfRange(8, 8 + headerLen) else ByteArray(0)
            body = payload.copyOfRange(8 + headerLen, payload.size)
        } else {
            header = ByteArray(0)
            body = payload.copyOfRange(20, payload.size)
        }
        Log.d(TAG, "[read] emsg=$emsg proto=$isProto size=${body.size} bodyHex=${body.take(24).joinToString("") { "%02X".format(it) }}")
        return Packet(emsg, body, isProto, header)
    }

    private fun readExact(len: Int): ByteArray {
        val data = ByteArray(len)
        var off = 0
        while (off < len) {
            val r = input!!.read(data, off, len - off)
            if (r == -1) throw Exception("connection closed")
            off += r
        }
        return data
    }

    private fun sendRaw(data: ByteArray) {
        Log.d(TAG, "sendRaw: ${data.size} bytes chunks...")
        val final = ByteArray(data.size + 8)
        Proto.packInt32(data.size).copyInto(final, 0)
        Proto.packInt32(TCP_MAGIC).copyInto(final, 4)
        data.copyInto(final, 8)
        output!!.write(final)
        output!!.flush()
    }

    private fun sendRawMsg(body: ByteArray, emsg: Int) {
        val bos = ByteArrayOutputStream()
        bos.write(Proto.packInt32(emsg or PROTO_MASK))
        bos.write(Proto.packInt32(0))
        bos.write(body)
        sendRaw(if (encrypted) encryptAES(bos.toByteArray()) else bos.toByteArray())
    }

    private fun sendProtobufMsg(emsg: Int, body: ByteArray, header: ByteArray?) {
        val bos = ByteArrayOutputStream()
        bos.write(Proto.packInt32(emsg or PROTO_MASK))
        val headerBytes = header ?: ByteArray(0)
        bos.write(Proto.packInt32(headerBytes.size))
        bos.write(headerBytes)
        bos.write(body)
        sendRaw(if (encrypted) encryptAES(bos.toByteArray()) else bos.toByteArray())
    }

    private fun encryptAES(data: ByteArray): ByteArray {
        val random3 = ByteArray(3)
        SecureRandom().nextBytes(random3)
        val hmac = Mac.getInstance("HmacSHA1")
        hmac.init(SecretKeySpec(sessionKey.copyOfRange(0, 16), "HmacSHA1"))
        hmac.update(random3)
        hmac.update(data)
        val hash = hmac.doFinal()
        val iv = hash.copyOfRange(0, 13) + random3
        val ecb = Cipher.getInstance("AES/ECB/NoPadding")
        ecb.init(Cipher.ENCRYPT_MODE, SecretKeySpec(sessionKey, "AES"))
        val ecbIv = ecb.doFinal(iv)
        val cbc = Cipher.getInstance("AES/CBC/PKCS5Padding")
        cbc.init(Cipher.ENCRYPT_MODE, SecretKeySpec(sessionKey, "AES"), IvParameterSpec(iv))
        return ecbIv + cbc.doFinal(data)
    }

    private fun decryptAES(data: ByteArray): ByteArray {
        val ecb = Cipher.getInstance("AES/ECB/NoPadding")
        ecb.init(Cipher.DECRYPT_MODE, SecretKeySpec(sessionKey, "AES"))
        val iv = ecb.doFinal(data.copyOfRange(0, 16))
        val cbc = Cipher.getInstance("AES/CBC/PKCS5Padding")
        cbc.init(Cipher.DECRYPT_MODE, SecretKeySpec(sessionKey, "AES"), IvParameterSpec(iv))
        return cbc.doFinal(data.copyOfRange(16, data.size))
    }

    private fun disconnect() {
        try { heartbeatJob?.interrupt() } catch (_: Exception) { }
        try { readerThread?.interrupt() } catch (_: Exception) { }
        try { socket?.close() } catch (_: Exception) { }
        socket = null
        input = null
        output = null
        readerThread = null
        heartbeatJob = null
        connected = false
        loggedIn = false
        gameConnectTokens.clear()
        synchronized(ticketChangeLock) { ticketsByGame.clear() }
        authSession = null
        pendingJobs.values.forEach { it.completeExceptionally(Exception("Disconnected")) }
        pendingJobs.clear()
    }

    private fun startHeartbeat() {
        heartbeatJob?.interrupt()
        heartbeatJob = Thread {
            try {
                while (!Thread.interrupted()) {
                    Thread.sleep(heartbeatSeconds * 1000L)
                    if (Thread.interrupted()) break
                    val body = ByteArrayOutputStream()
                    body.write(Proto.packVarint(1 shl 3 or 0)); body.write(Proto.packVarint(1))
                    sendProtobufMsg(1009, body.toByteArray(), buildProtoHeader(currentSteamId, currentSessionId))
                }
            } catch (_: InterruptedException) {
            } catch (e: Exception) {
                Log.w(TAG, "heartbeat error: ${e.message}")
            }
        }.apply { isDaemon = true; start() }
    }

    private suspend fun awaitJob(future: CompletableFuture<SteamMsg>, what: String): SteamMsg =
        suspendCancellableCoroutine { cont ->
            future.whenComplete { value, error ->
                if (error != null) cont.resumeWithException(error)
                else cont.resume(value)
            }
        }

    // ------------------------------------------------------------------
    // Proto encode helpers (duplicated from GameDataDownloader, file-private there)
    // ------------------------------------------------------------------

    private class ByteArrayOutputStream {
        private val data = mutableListOf<Byte>()
        fun write(b: ByteArray) { data.addAll(b.toList()) }
        fun write(b: Byte) { data.add(b) }
        val size get() = data.size
        fun toByteArray(): ByteArray = data.toByteArray()
    }

    private class ProtoReader(val data: ByteArray) {
        var pos = 0
        val remaining get() = data.size - pos

        fun readVarint(): Int {
            var result = 0
            var shift = 0
            while (pos < data.size) {
                val b = data[pos++].toInt() and 0xFF
                result = result or ((b and 0x7F) shl shift)
                if (b and 0x80 == 0) return result
                shift += 7
            }
            throw Exception("Truncated varint")
        }

        fun readLongVarint(): Long {
            var result = 0L
            var shift = 0
            while (pos < data.size) {
                val b = data[pos++].toInt() and 0xFF
                result = result or ((b and 0x7F).toLong() shl shift)
                if (b and 0x80 == 0) return result
                shift += 7
            }
            throw Exception("Truncated varint")
        }

        fun readFixed32(): Int {
            val result = (data[pos].toInt() and 0xFF) or
                ((data[pos + 1].toInt() and 0xFF) shl 8) or
                ((data[pos + 2].toInt() and 0xFF) shl 16) or
                ((data[pos + 3].toInt() and 0xFF) shl 24)
            pos += 4
            return result
        }

        fun readLongLE(): Long {
            val result = (data[pos].toLong() and 0xFF) or
                ((data[pos + 1].toLong() and 0xFF) shl 8) or
                ((data[pos + 2].toLong() and 0xFF) shl 16) or
                ((data[pos + 3].toLong() and 0xFF) shl 24) or
                ((data[pos + 4].toLong() and 0xFF) shl 32) or
                ((data[pos + 5].toLong() and 0xFF) shl 40) or
                ((data[pos + 6].toLong() and 0xFF) shl 48) or
                ((data[pos + 7].toLong() and 0xFF) shl 56)
            pos += 8
            return result
        }

        fun readBytes(len: Int): ByteArray {
            val result = data.copyOfRange(pos, pos + len)
            pos += len
            return result
        }

        fun skipField(tag: Int) {
            when (tag and 0x7) {
                0 -> readVarint()
                1 -> pos += 8
                2 -> { val len = readVarint(); pos += len }
                3, 4 -> { }
                5 -> pos += 4
                else -> pos += 1
            }
        }

        fun skipFieldAtTag() {
            if (pos >= data.size) return
            val tag = readVarint()
            skipField(tag)
        }
    }

    private object Proto {
        fun packVarint(value: Int): ByteArray {
            val bos = ByteArrayOutputStream()
            var v = value
            while (v and -0x80 != 0) {
                bos.write(((v and 0x7F) or 0x80).toByte())
                v = v ushr 7
            }
            bos.write((v and 0x7F).toByte())
            return bos.toByteArray()
        }

        fun packInt32(v: Int) = byteArrayOf(
            (v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(), ((v shr 24) and 0xFF).toByte()
        )

        fun packLongVarint(value: Long): ByteArray {
            val bos = ByteArrayOutputStream()
            var v = value
            while (v and -0x80L != 0L) {
                bos.write(((v and 0x7F) or 0x80).toByte())
                v = v ushr 7
            }
            bos.write((v and 0x7F).toByte())
            return bos.toByteArray()
        }

        fun packInt64(v: Long): ByteArray = byteArrayOf(
            (v and 0xFF).toByte(),
            ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(),
            ((v shr 24) and 0xFF).toByte(),
            ((v shr 32) and 0xFF).toByte(),
            ((v shr 40) and 0xFF).toByte(),
            ((v shr 48) and 0xFF).toByte(),
            ((v shr 56) and 0xFF).toByte()
        )

        fun readInt32LE(data: ByteArray, offset: Int): Int {
            return (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8) or
                ((data[offset + 2].toInt() and 0xFF) shl 16) or
                ((data[offset + 3].toInt() and 0xFF) shl 24)
        }
    }

    // ------------------------------------------------------------------
    // Builders (same layout as GameDataDownloader)
    // ------------------------------------------------------------------

    private fun buildEncryptResponse(encryptedKey: ByteArray, keyCrc: Int): ByteArray {
        val msgHdr = ByteArrayOutputStream()
        msgHdr.write(Proto.packInt32(1304))
        msgHdr.write(Proto.packInt64(-1L))
        msgHdr.write(Proto.packInt64(-1L))
        val hdrBytes = msgHdr.toByteArray()

        val body = ByteArrayOutputStream()
        body.write(Proto.packInt32(1))
        body.write(Proto.packInt32(encryptedKey.size))
        val bodyBytes = body.toByteArray()

        val payload = ByteArrayOutputStream()
        payload.write(encryptedKey)
        payload.write(Proto.packInt32(keyCrc))
        payload.write(Proto.packInt32(0))
        val payloadBytes = payload.toByteArray()

        val fos = ByteArrayOutputStream()
        fos.write(hdrBytes)
        fos.write(bodyBytes)
        fos.write(payloadBytes)
        return fos.toByteArray()
    }

    private fun buildClientHello(): ByteArray {
        val buf = ByteArrayOutputStream()
        buf.write(Proto.packVarint(1 shl 3 or 0))
        buf.write(Proto.packVarint(65581))
        return buf.toByteArray()
    }

    private fun buildProtoHeader(steamId: Long, sessionId: Int, jobidSource: Long? = null): ByteArray {
        val buf = ByteArrayOutputStream()
        buf.write(Proto.packVarint(1 shl 3 or 1)); buf.write(Proto.packInt64(steamId))
        buf.write(Proto.packVarint(2 shl 3 or 0)); buf.write(Proto.packVarint(sessionId))
        if (jobidSource != null) {
            buf.write(Proto.packVarint(10 shl 3 or 1)); buf.write(Proto.packInt64(jobidSource))
        }
        return buf.toByteArray()
    }

    // Non-authed ServiceMethod header: steamid/sessionid 0, jobid_source, target_job_name.
    private fun buildUnifiedHeader(serviceMethod: String, jobidSource: Long): ByteArray {
        val buf = ByteArrayOutputStream()
        buf.write(Proto.packVarint(1 shl 3 or 1)); buf.write(Proto.packInt64(0L))
        buf.write(Proto.packVarint(2 shl 3 or 0)); buf.write(Proto.packVarint(0))
        buf.write(Proto.packVarint(10 shl 3 or 1)); buf.write(Proto.packInt64(jobidSource))
        val name = serviceMethod.toByteArray(Charsets.UTF_8)
        buf.write(Proto.packVarint(12 shl 3 or 2)); buf.write(Proto.packVarint(name.size)); buf.write(name)
        return buf.toByteArray()
    }
}
