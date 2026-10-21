package su.xash.svencoop.ui.settings

import android.app.AlertDialog
import android.content.ClipData
import android.content.Intent
import android.os.Bundle
import android.widget.EditText
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreferenceCompat
import su.xash.svencoop.MainActivity
import su.xash.svencoop.R
import android.content.SharedPreferences
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileNotFoundException

class AppSettingsPreferenceFragment() : PreferenceFragmentCompat(),
    SharedPreferences.OnSharedPreferenceChangeListener {

    private lateinit var preferences: SharedPreferences
    private lateinit var gamePathPreference: Preference
    private lateinit var globalArgsPreference: Preference
    private lateinit var renderResolutionPreference: Preference
    private var storageTogglePreference: SwitchPreferenceCompat? = null

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.app_preferences, rootKey)

        preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        preferences.registerOnSharedPreferenceChangeListener(this)

        gamePathPreference = findPreference("game_path") ?: return
        globalArgsPreference = findPreference("global_arguments") ?: return
        renderResolutionPreference = findPreference("render_resolution") ?: return

        storageTogglePreference = findPreference<SwitchPreferenceCompat>("storage_toggle")
        storageTogglePreference?.onPreferenceChangeListener =
            Preference.OnPreferenceChangeListener { _, newValue ->
                if (newValue == false) {
                    showExternalStorageWarning()
                    false // don't apply yet; applied manually on OK
                } else {
                    true
                }
            }

        globalArgsPreference.setOnPreferenceClickListener {
            showGlobalArgumentsDialog()
            true
        }

        renderResolutionPreference.setOnPreferenceClickListener {
            showRenderResolutionDialog()
            true
        }

        findPreference<Preference>("share_engine_log")?.setOnPreferenceClickListener {
            shareEngineLog(it)
            true
        }

        updateGamePathSummary()
        updateGlobalArgsSummary()
        updateRenderResolutionSummary()
    }

    private fun shareEngineLog(preference: Preference) {
        val context = requireContext()
        val storagePath = (activity as? MainActivity)?.getStoragePath() ?: return
        preference.isEnabled = false
        lifecycleScope.launch {
            try {
                val uri = withContext(Dispatchers.IO) {
                    val source = File(storagePath, "engine.log")
                    if (!source.isFile) throw FileNotFoundException()
                    val directory = File(context.cacheDir, "shared_logs").apply { mkdirs() }
                    val snapshot = File(directory, "engine.log")
                    source.copyTo(snapshot, overwrite = true)
                    FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", snapshot)
                }
                if (!isAdded) return@launch
                val intent = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_STREAM, uri)
                    clipData = ClipData.newRawUri("engine.log", uri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                startActivity(Intent.createChooser(intent, getString(R.string.share_engine_log)))
            } catch (error: java.util.concurrent.CancellationException) {
                throw error
            } catch (error: Exception) {
                if (isAdded) {
                    AlertDialog.Builder(context)
                        .setTitle(R.string.share_engine_log)
                        .setMessage(if (error is FileNotFoundException) R.string.engine_log_not_found else R.string.engine_log_share_failed)
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
            } finally {
                preference.isEnabled = true
            }
        }
    }

    private fun showExternalStorageWarning() {
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.external_storage_warning_title)
            .setMessage(R.string.external_storage_warning_message)
            .setPositiveButton(R.string.external_storage_warning_ok) { _, _ ->
                preferences.edit().putBoolean("storage_toggle", false).apply()
                storageTogglePreference?.isChecked = false
                updateGamePathSummary()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences, key: String?) {
        when (key) {
            "storage_toggle" -> {
                updateGamePathSummary()
            }
            "global_arguments" -> {
                updateGlobalArgsSummary()
            }
            "render_resolution" -> {
                updateRenderResolutionSummary()
            }
        }
    }

    private fun updateGamePathSummary() {
        (activity as? MainActivity)?.let { mainActivity ->
            gamePathPreference.summary = mainActivity.getStorageSummary()
        } ?: run {
            val useInternalStorage = preferences.getBoolean("storage_toggle", true)
            gamePathPreference.summary = if (useInternalStorage) {
                "Internal Storage (Android/data)"
            } else {
                "External Storage (/storage/emulated/0/xash)"
            }
        }
    }

    private fun updateGlobalArgsSummary() {
        val globalArgs = preferences.getString("global_arguments", getString(R.string.default_global_arguments))
        if (globalArgs.isNullOrEmpty()) {
            globalArgsPreference.summary = "No global arguments set"
        } else {
            globalArgsPreference.summary = globalArgs
        }
    }

    private fun updateRenderResolutionSummary() {
        val resolution = preferences.getString("render_resolution", "")
        if (resolution.isNullOrEmpty()) {
            renderResolutionPreference.summary = getString(R.string.render_resolution_summary)
        } else {
            renderResolutionPreference.summary = resolution
        }
    }

    private fun showGlobalArgumentsDialog() {
        val currentArgs = preferences.getString("global_arguments", getString(R.string.default_global_arguments)) ?: ""
        
        val editText = EditText(requireContext())
        editText.setText(currentArgs)
        editText.hint = "e.g., -dev -log"
        
        AlertDialog.Builder(requireContext())
            .setTitle("Global Command-line Arguments")
            .setMessage("These arguments will be added to all games")
            .setView(editText)
            .setPositiveButton("OK") { dialog, which ->
                val newArgs = editText.text.toString().trim()
                preferences.edit().putString("global_arguments", newArgs).commit()
                updateGlobalArgsSummary()
            }
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Clear") { dialog, which ->
                preferences.edit().putString("global_arguments", "").commit()
                updateGlobalArgsSummary()
            }
            .show()
    }

    private fun showRenderResolutionDialog() {
        val currentResolution = preferences.getString("render_resolution", "") ?: ""

        val editText = EditText(requireContext())
        editText.setText(currentResolution)
        editText.hint = "e.g., 640x480"

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.render_resolution_dialog)
            .setMessage("Leave empty to use native display resolution")
            .setView(editText)
            .setPositiveButton("OK") { dialog, which ->
                val newResolution = editText.text.toString().trim()
                preferences.edit().putString("render_resolution", newResolution).commit()
                updateRenderResolutionSummary()
            }
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Clear") { dialog, which ->
                preferences.edit().putString("render_resolution", "").commit()
                updateRenderResolutionSummary()
            }
            .show()
    }

    override fun onResume() {
        super.onResume()
        preferences.registerOnSharedPreferenceChangeListener(this)
        updateGamePathSummary()
        updateGlobalArgsSummary()
        updateRenderResolutionSummary()
    }

    override fun onPause() {
        super.onPause()
        preferences.unregisterOnSharedPreferenceChangeListener(this)
    }
}
