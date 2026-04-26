package su.xash.engine.ui.settings

import android.app.AlertDialog
import android.os.Bundle
import android.util.DisplayMetrics
import android.util.Log
import android.widget.EditText
import androidx.preference.EditTextPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreferenceCompat
import su.xash.engine.MainActivity
import su.xash.engine.R
import android.content.SharedPreferences
import android.preference.PreferenceManager

class AppSettingsPreferenceFragment() : PreferenceFragmentCompat(),
    SharedPreferences.OnSharedPreferenceChangeListener {

    private lateinit var preferences: SharedPreferences
    private lateinit var gamePathPreference: Preference
    private lateinit var globalArgsPreference: Preference

    private var mEngineWidth: Int = 0
    private var mEngineHeight: Int = 0

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.sharedPreferencesName = "app_preferences"
        setPreferencesFromResource(R.xml.app_preferences, rootKey)

        preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        preferences.registerOnSharedPreferenceChangeListener(this)

        gamePathPreference = findPreference("game_path") ?: return
        globalArgsPreference = findPreference("global_arguments") ?: return

        globalArgsPreference.setOnPreferenceClickListener {
            showGlobalArgumentsDialog()
            true
        }

        updateGamePathSummary()
        updateGlobalArgsSummary()

        // Resolution settings
        initResolutionSettings()
    }

    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences, key: String?) {
        when (key) {
            "storage_toggle" -> {
                updateGamePathSummary()
            }
            "global_arguments" -> {
                updateGlobalArgsSummary()
            }
        }
    }

    private fun initResolutionSettings() {
        // Get display metrics for calculating default resolution
        val metrics = DisplayMetrics()
        requireActivity().windowManager.defaultDisplay.getMetrics(metrics)

        // Swap resolution for landscape mode
        if (metrics.widthPixels > metrics.heightPixels) {
            mEngineWidth = metrics.widthPixels
            mEngineHeight = metrics.heightPixels
        } else {
            mEngineWidth = metrics.heightPixels
            mEngineHeight = metrics.widthPixels
        }

        Log.d("AppSettings", "Engine dimensions: ${mEngineWidth}x${mEngineHeight}")

        val resolutionFixed = findPreference<SwitchPreferenceCompat>("resolution_fixed") ?: return
        val resolutionCustom = findPreference<SwitchPreferenceCompat>("resolution_custom") ?: return
        val resolutionScale = findPreference<EditTextPreference>("resolution_scale") ?: return
        val resolutionWidth = findPreference<EditTextPreference>("resolution_width") ?: return
        val resolutionHeight = findPreference<EditTextPreference>("resolution_height") ?: return
        val resolutionResult = findPreference<Preference>("resolution_result") ?: return

        // Set default values if not set
        val prefs = preferenceManager.sharedPreferences!!
        if (!prefs.contains("resolution_width")) {
            prefs.edit().putInt("resolution_width", mEngineWidth).apply()
        }
        if (!prefs.contains("resolution_height")) {
            prefs.edit().putInt("resolution_height", mEngineHeight).apply()
        }

        // Initial update of result
        updateResolutionResult()

        // Set up change listeners
        resolutionFixed.setOnPreferenceChangeListener { _, newValue ->
            val enabled = newValue as Boolean
            resolutionCustom.isVisible = enabled
            resolutionScale.isVisible = enabled && !resolutionCustom.isChecked
            resolutionWidth.isVisible = enabled && resolutionCustom.isChecked
            resolutionHeight.isVisible = enabled && resolutionCustom.isChecked
            resolutionResult.isVisible = enabled
            if (enabled) {
                updateResolutionResult()
            }
            true
        }

        resolutionCustom.setOnPreferenceChangeListener { _, newValue ->
            val custom = newValue as Boolean
            resolutionScale.isVisible = !custom
            resolutionWidth.isVisible = custom
            resolutionHeight.isVisible = custom
            updateResolutionResult()
            true
        }

        // Text change listeners for scale and dimensions
        resolutionScale.setOnPreferenceChangeListener { _, _ ->
            updateResolutionResult()
            true
        }

        resolutionWidth.setOnPreferenceChangeListener { _, newValue ->
            // Auto-calculate height to maintain aspect ratio
            try {
                val width = (newValue as String).toInt()
                val height = ((mEngineHeight.toFloat() / mEngineWidth.toFloat()) * width).toInt()
                prefs.edit().putInt("resolution_height", height).apply()
                resolutionHeight.text = height.toString()
            } catch (e: NumberFormatException) {
                Log.e("AppSettings", "Invalid width value: $newValue")
            }
            updateResolutionResult()
            true
        }

        resolutionHeight.setOnPreferenceChangeListener { _, _ ->
            updateResolutionResult()
            true
        }

        // Initial visibility update
        resolutionCustom.isVisible = resolutionFixed.isChecked
        resolutionScale.isVisible = resolutionFixed.isChecked && !resolutionCustom.isChecked
        resolutionWidth.isVisible = resolutionFixed.isChecked && resolutionCustom.isChecked
        resolutionHeight.isVisible = resolutionFixed.isChecked && resolutionCustom.isChecked
        resolutionResult.isVisible = resolutionFixed.isChecked
    }

    private fun updateResolutionResult() {
        val resolutionResult = findPreference<Preference>("resolution_result") ?: return
        val resolutionCustom = findPreference<SwitchPreferenceCompat>("resolution_custom") ?: return
        val resolutionFixed = findPreference<SwitchPreferenceCompat>("resolution_fixed") ?: return
        val prefs = preferenceManager.sharedPreferences!!

        if (!resolutionFixed.isChecked) {
            resolutionResult.summary = "Using device default resolution"
            return
        }

        val width: Int
        val height: Int

        if (resolutionCustom.isChecked) {
            // Custom resolution mode
            width = prefs.getInt("resolution_width", mEngineWidth)
            height = prefs.getInt("resolution_height", mEngineHeight)
        } else {
            // Scale mode
            val scaleStr = prefs.getString("resolution_scale", "2.0") ?: "2.0"
            val scale = try {
                scaleStr.toFloat()
            } catch (e: NumberFormatException) {
                2.0f
            }
            val safeScale = if (scale < 0.5f) 0.5f else scale
            width = (mEngineWidth / safeScale).toInt()
            height = (mEngineHeight / safeScale).toInt()
        }

        // Enforce minimum resolution
        val finalWidth = if (width < 320) 320 else width
        val finalHeight = if (height < 240) 240 else height

        resolutionResult.summary = "${finalWidth}x${finalHeight}"
        Log.d("AppSettings", "Calculated resolution: ${finalWidth}x${finalHeight}")
    }

    private fun updateGamePathSummary() {
        (activity as? MainActivity)?.let { mainActivity ->
            gamePathPreference.summary = mainActivity.getStorageSummary()
        } ?: run {
            val useInternalStorage = preferences.getBoolean("storage_toggle", false)
            gamePathPreference.summary = if (useInternalStorage) {
                "Internal Storage (Android/data)"
            } else {
                "External Storage (/storage/emulated/0/xash)"
            }
        }
    }

    private fun updateGlobalArgsSummary() {
        val globalArgs = preferences.getString("global_arguments", "")
        if (globalArgs.isNullOrEmpty()) {
            globalArgsPreference.summary = "No global arguments set"
        } else {
            globalArgsPreference.summary = globalArgs
        }
    }

    private fun showGlobalArgumentsDialog() {
        val currentArgs = preferences.getString("global_arguments", "") ?: ""
        
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

    override fun onResume() {
        super.onResume()
        preferences.registerOnSharedPreferenceChangeListener(this)
        updateGamePathSummary()
        updateGlobalArgsSummary()
    }

    override fun onPause() {
        super.onPause()
        preferences.unregisterOnSharedPreferenceChangeListener(this)
    }
}
