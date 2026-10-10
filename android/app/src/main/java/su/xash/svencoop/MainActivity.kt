package su.xash.svencoop

import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
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
import su.xash.svencoop.util.CrashReports
import su.xash.svencoop.util.monospaceTextView
import androidx.preference.PreferenceManager
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var appBarConfiguration: AppBarConfiguration
    private lateinit var navController: NavController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setSupportActionBar(binding.toolbar)

        val navHostFragment =
                supportFragmentManager.findFragmentById(R.id.fragmentContainerView) as NavHostFragment
        navController = navHostFragment.navController
        appBarConfiguration = AppBarConfiguration(navController.graph)
        setupActionBarWithNavController(navController, appBarConfiguration)

        CrashReports.prune(this)
        showPendingCrashReport()
        maybeAutoConnectSteam()
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
                when (state) {
                    is SteamAuthManager.LoginState.Success -> auth.startBroker()
                    is SteamAuthManager.LoginState.Failed ->
                        showSteamConnectFailed(state.message)
                    is SteamAuthManager.LoginState.Error ->
                        showSteamConnectFailed(state.message)
                    else -> { /* guard states can't happen on stored login */ }
                }
            } catch (e: Exception) {
                if (dialog.isShowing) dialog.dismiss()
                if (e is kotlinx.coroutines.CancellationException) return@launch
                showSteamConnectFailed(e.message ?: "unknown error")
            }
        }
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener {
            job?.cancel()
            if (dialog.isShowing) dialog.dismiss()
        }
    }

    private fun showSteamConnectFailed(message: String) {
        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.steam_connect_failed, message))
            .setPositiveButton(R.string.steam_retry) { _, _ -> maybeAutoConnectSteam() }
            .setNegativeButton(android.R.string.ok, null)
            .show()
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
