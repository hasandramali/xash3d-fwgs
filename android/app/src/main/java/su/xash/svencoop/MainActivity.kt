package su.xash.svencoop

import android.os.Bundle
import android.Manifest
import android.content.pm.PackageManager
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.Lifecycle
import androidx.navigation.NavController
import androidx.navigation.fragment.NavHostFragment
import androidx.navigation.ui.AppBarConfiguration
import androidx.navigation.ui.navigateUp
import androidx.navigation.ui.setupActionBarWithNavController
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import su.xash.svencoop.databinding.ActivityMainBinding
import su.xash.svencoop.model.SteamAuthManager
import su.xash.svencoop.model.DownloadService
import su.xash.svencoop.model.DownloadState
import su.xash.svencoop.util.CrashReports
import su.xash.svencoop.util.monospaceTextView
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var appBarConfiguration: AppBarConfiguration
    private lateinit var navController: NavController
    private var pendingMicrophoneLaunch: (() -> Unit)? = null
    private var steamConnectDialog: AlertDialog? = null
    private var steamErrorDialog: AlertDialog? = null
    private var pendingUpdateCommit: String? = null
    private var updateDialog: AlertDialog? = null
    private val microphonePermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val launch = pendingMicrophoneLaunch
        pendingMicrophoneLaunch = null
        if (!granted) Toast.makeText(this, R.string.microphone_permission_denied, Toast.LENGTH_LONG).show()
        if (launch != null && !isFinishing && !isDestroyed && canLaunchGame()) launch()
    }

    fun launchWithMicrophonePermission(launch: () -> Unit) {
        if (!canLaunchGame()) return
        if (pendingMicrophoneLaunch != null) return
        val enabled = PreferenceManager.getDefaultSharedPreferences(this).getBoolean("microphone", true)
        if (!enabled || ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            launch()
        } else {
            pendingMicrophoneLaunch = launch
            microphonePermission.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private fun canLaunchGame(): Boolean {
        if (DownloadService.state.value !is DownloadState.Downloading) return true
        Toast.makeText(this, R.string.download_wait_before_play, Toast.LENGTH_LONG).show()
        return false
    }
    private var steamLibraryDialog: AlertDialog? = null
    private var steamLibraryDialogSession = -1L
    private val steamLibraryListener: () -> Unit = { runOnUiThread { refreshSteamLibraryPrompt() } }

    override fun onStart() {
        super.onStart()
        SteamAuthManager.get(this).addAuthStateListener(steamLibraryListener)
    }

    override fun onStop() {
        SteamAuthManager.get(this).removeAuthStateListener(steamLibraryListener)
        super.onStop()
    }

    override fun onPostResume() {
        super.onPostResume()
        refreshSteamLibraryPrompt()
        showUpdatePromptIfReady()
    }

    override fun onDestroy() {
        steamLibraryDialog?.dismiss()
        super.onDestroy()
    }

    private fun refreshSteamLibraryPrompt() {
        val auth = SteamAuthManager.get(this)
        val session = auth.svenLibrarySession
        if (!auth.isLoggedIn || steamLibraryDialogSession != session) {
            steamLibraryDialog?.dismiss()
            steamLibraryDialog = null
        }
        if (!lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) || isFinishing || isDestroyed ||
            !auth.isLoggedIn || steamLibraryDialog?.isShowing == true) return
        steamLibraryDialogSession = session
        if (auth.svenLibraryPromptPending) {
            steamLibraryDialog = MaterialAlertDialogBuilder(this)
                .setTitle(R.string.steam_library_title)
                .setMessage(R.string.steam_library_notice)
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok) { _, _ -> auth.acknowledgeSvenLibraryNotice(session) }
                .setOnDismissListener {
                    steamLibraryDialog = null
                    showUpdatePromptIfReady()
                }
                .show()

        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val preferences = PreferenceManager.getDefaultSharedPreferences(this)
        if (!preferences.contains("use_volume_buttons")) {
            val gamePreferences = getSharedPreferences("svencoop", MODE_PRIVATE)
            preferences.edit().putBoolean("use_volume_buttons",
                gamePreferences.getBoolean("use_volume_buttons", false)).apply()
        }

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setSupportActionBar(binding.toolbar)

        val navHostFragment =
                supportFragmentManager.findFragmentById(R.id.fragmentContainerView) as NavHostFragment
        navController = navHostFragment.navController
        navController.addOnDestinationChangedListener { _, destination, _ ->
            if (destination.id != R.id.steamAuthFragment) showUpdatePromptIfReady()
        }
        appBarConfiguration = AppBarConfiguration(navController.graph)
        setupActionBarWithNavController(navController, appBarConfiguration)

        CrashReports.prune(this)
        showPendingCrashReport()
        maybeAutoConnectSteam()
        checkForAppUpdate()
    }

    /**
     * Auto-connect: if a Steam session is stored and the switch is on,
     * reconnect right away behind a cancellable "Connecting steam..." popup.
     * Success closes the popup and starts the broker (the toolbar icon turns
     * green via the auth-state listener); failure shows the reason.
     */
    private fun maybeAutoConnectSteam() {
        val auth = SteamAuthManager.get(this)
        val prefs = PreferenceManager.getDefaultSharedPreferences(this)
        if (!prefs.getBoolean(SteamAuthManager.PREF_AUTO_CONNECT, true)) return
        if (!auth.hasStoredKey || auth.isLoggedIn) return

        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.steam_connecting)
            .setCancelable(false)
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        steamConnectDialog = dialog

        var job: Job? = null
        job = lifecycleScope.launch {
            try {
                val state = if (auth.hasStoredRefreshToken) {
                    auth.loginWithStoredRefreshToken()
                } else {
                    auth.loginWithStoredKey()
                }
                if (!dialog.isShowing) return@launch
                dialog.dismiss()
                steamConnectDialog = null
                when (state) {
                    is SteamAuthManager.LoginState.Success -> auth.startBroker()
                    is SteamAuthManager.LoginState.Failed ->
                        showSteamConnectFailed(state.message)
                    is SteamAuthManager.LoginState.Error ->
                        showSteamConnectFailed(state.message)
                    else -> { /* guard states can't happen on stored login */ }
                }
                showUpdatePromptIfReady()
            } catch (e: Exception) {
                if (dialog.isShowing) dialog.dismiss()
                steamConnectDialog = null
                if (e is kotlinx.coroutines.CancellationException) return@launch
                showSteamConnectFailed(e.message ?: "unknown error")
            }
        }
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener {
            job?.cancel()
            if (dialog.isShowing) dialog.dismiss()
            steamConnectDialog = null
            showUpdatePromptIfReady()
        }
    }

    private fun showSteamConnectFailed(message: String) {
        steamErrorDialog = MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.steam_connect_failed, message))
            .setPositiveButton(R.string.steam_retry) { _, _ -> maybeAutoConnectSteam() }
            .setNegativeButton(android.R.string.ok, null)
            .create().also { dialog ->
                dialog.setOnDismissListener {
                    steamErrorDialog = null
                    showUpdatePromptIfReady()
                }
                dialog.show()
            }
    }

    private fun checkForAppUpdate() {
        if (!PreferenceManager.getDefaultSharedPreferences(this)
                .getBoolean("update_checker_enabled", true)) return
        lifecycleScope.launch {
            val releaseCommit = withContext(Dispatchers.IO) {
                var connection: HttpURLConnection? = null
                try {
                    connection = URL("https://api.github.com/repos/hasandramali/xash3d-fwgs/releases/tags/v1sc")
                        .openConnection() as HttpURLConnection
                    connection.connectTimeout = 5_000
                    connection.readTimeout = 5_000
                    connection.setRequestProperty("Accept", "application/vnd.github+json")
                    connection.setRequestProperty("User-Agent", "SvenClient-Android")
                    val release = org.json.JSONObject(connection.inputStream.bufferedReader().use { it.readText() })
                    val body = release.optString("body")
                    Regex("(?i)\\b[0-9a-f]{7,40}\\b")
                        .find(body)?.value?.lowercase(Locale.ROOT)
                } catch (_: Exception) {
                    null
                } finally {
                    connection?.disconnect()
                }
            }
            val installedCommit = BuildConfig.GIT_HASH.lowercase(Locale.ROOT)
            if (releaseCommit != null && !installedCommit.startsWith(releaseCommit) &&
                !releaseCommit.startsWith(installedCommit)) {
                pendingUpdateCommit = releaseCommit
                showUpdatePromptIfReady()
            }
        }
    }

    private fun showUpdatePromptIfReady() {
        if (!PreferenceManager.getDefaultSharedPreferences(this)
                .getBoolean("update_checker_enabled", true)) {
            pendingUpdateCommit = null
            return
        }
        if (pendingUpdateCommit == null) return
        if (!lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) || isFinishing || isDestroyed ||
            steamConnectDialog?.isShowing == true || steamErrorDialog?.isShowing == true ||
            steamLibraryDialog?.isShowing == true || updateDialog?.isShowing == true ||
            navController.currentDestination?.id == R.id.steamAuthFragment) return

        pendingUpdateCommit = null
        updateDialog = MaterialAlertDialogBuilder(this)
            .setMessage(R.string.app_update_available_message)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.app_update_action) { _, _ ->
                startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW,
                    android.net.Uri.parse("https://github.com/hasandramali/xash3d-fwgs/releases/download/v1sc/svenclient-android.apk")))
            }
            .create()
            .also { dialog ->
                dialog.setOnDismissListener {
                    updateDialog = null
                    showUpdatePromptIfReady()
                }
                dialog.show()
            }
    }

    fun getStoragePath(): String {
        val prefs = PreferenceManager.getDefaultSharedPreferences(this)
        val useInternal = prefs.getBoolean("storage_toggle", true)
        return if (useInternal) {
            getExternalFilesDir(null)?.absolutePath ?: "/storage/emulated/0/Android/data/su.xash.svencoop/files"
        } else {
            prefs.getString("game_path", null) ?: "/storage/emulated/0/xash"
        }
    }

    fun getStorageSummary(): String {
        val prefs = PreferenceManager.getDefaultSharedPreferences(this)
        val useInternal = prefs.getBoolean("storage_toggle", true)
        return if (useInternal) "Internal Storage (Android/data)" else "External Storage (/storage/emulated/0/xash)"
    }

    override fun onSupportNavigateUp(): Boolean {
        return navController.navigateUp(appBarConfiguration) || super.onSupportNavigateUp()
    }

    private fun showPendingCrashReport() {
        val pending = CrashReports.pendingStacktrace(this)
        if (!pending.exists() || pending.length() == 0L)
            return

        val historyDir = CrashReports.historyDir(this).apply { mkdirs() }
        val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val entryDir = File(historyDir, "crash-$ts").apply { mkdirs() }

        moveOrCopy(pending, File(entryDir, CrashReports.STACKTRACE_NAME))
        moveOrCopy(CrashReports.pendingSysinfo(this), File(entryDir, CrashReports.SYSINFO_NAME))
        moveOrCopy(CrashReports.pendingIntent(this), File(entryDir, CrashReports.INTENT_NAME))
        moveOrCopy(CrashReports.pendingEngineLog(this), File(entryDir, CrashReports.ENGINELOG_NAME))

        val entry = CrashReports.Entry(entryDir)
        AlertDialog.Builder(this)
            .setTitle(R.string.crash_dialog_title)
            .setView(monospaceTextView(this, entry.summary()))
            .setPositiveButton(R.string.crash_send_to_developers) { _, _ -> CrashReports.sendByEmail(this, entry) }
            .setNeutralButton(R.string.crash_share) { _, _ -> CrashReports.share(this, entry) }
            .setNegativeButton(R.string.crash_dismiss, null)
            .show()
    }

    private fun moveOrCopy(src: File, dst: File) {
        if (!src.exists())
            return

        if (src.renameTo(dst))
            return

        src.copyTo(dst, overwrite = true)
        src.delete()
    }
}
