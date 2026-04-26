package su.xash.engine.ui.settings

import android.app.AlertDialog
import android.os.Bundle
import android.util.DisplayMetrics
import android.util.Log
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreferenceCompat
import su.xash.engine.MainActivity
import su.xash.engine.R
import android.content.SharedPreferences
import android.preference.PreferenceManager
import android.view.ViewGroup
import android.widget.FrameLayout

class AppSettingsPreferenceFragment() : PreferenceFragmentCompat(),
    SharedPreferences.OnSharedPreferenceChangeListener {

    private lateinit var preferences: SharedPreferences
    private lateinit var gamePathPreference: Preference
    private lateinit var globalArgsPreference: Preference

    private var mEngineWidth: Int = 0
    private var mEngineHeight: Int = 0
    private lateinit var resolutionSettingsPreference: Preference
    private lateinit var resolutionCurrentPreference: Preference

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.sharedPreferencesName = "app_preferences"
        setPreferencesFromResource(R.xml.app_preferences, rootKey)

        preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        preferences.registerOnSharedPreferenceChangeListener(this)

        gamePathPreference = findPreference("game_path") ?: return
        globalArgsPreference = findPreference("global_arguments") ?: return
        resolutionSettingsPreference = findPreference("resolution_settings") ?: return
        resolutionCurrentPreference = findPreference("resolution_current") ?: return

        globalArgsPreference.setOnPreferenceClickListener {
            showGlobalArgumentsDialog()
            true
        }

        resolutionSettingsPreference.setOnPreferenceClickListener {
            showResolutionDialog()
            true
        }

        updateGamePathSummary()
        updateGlobalArgsSummary()
        updateResolutionSummary()
    }

    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences, key: String?) {
        when (key) {
            "storage_toggle" -> {
                updateGamePathSummary()
            }
            "global_arguments" -> {
                updateGlobalArgsSummary()
            }
            "resolution_enabled" -> {
                updateResolutionSummary()
            }
        }
    }

    private fun getEngineDimensions() {
        val metrics = DisplayMetrics()
        requireActivity().windowManager.defaultDisplay.getMetrics(metrics)

        if (metrics.widthPixels > metrics.heightPixels) {
            mEngineWidth = metrics.widthPixels
            mEngineHeight = metrics.heightPixels
        } else {
            mEngineWidth = metrics.heightPixels
            mEngineHeight = metrics.widthPixels
        }
    }

    private fun showResolutionDialog() {
        getEngineDimensions()

        val prefs = preferenceManager.sharedPreferences!!
        val currentWidth = prefs.getInt("resolution_width", mEngineWidth)
        val currentHeight = prefs.getInt("resolution_height", mEngineHeight)

        val layout = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(50, 30, 50, 0)
        }

        val widthLabel = TextView(requireContext()).apply {
            text = "Width:"
        }
        val widthInput = EditText(requireContext()).apply {
            setText(currentWidth.toString())
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }

        val heightLabel = TextView(requireContext()).apply {
            text = "Height:"
        }
        val heightInput = EditText(requireContext()).apply {
            setText(currentHeight.toString())
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }

        val infoText = TextView(requireContext()).apply {
            text = "Leave empty to use device default\nMin: 320x240"
            textSize = 12f
            setPadding(0, 20, 0, 0)
        }

        layout.addView(widthLabel)
        layout.addView(widthInput)
        layout.addView(heightLabel)
        layout.addView(heightInput)
        layout.addView(infoText)

        AlertDialog.Builder(requireContext())
            .setTitle("Custom Resolution")
            .setView(layout)
            .setPositiveButton("OK") { _, _ ->
                try {
                    val width = widthInput.text.toString().toInt()
                    val height = heightInput.text.toString().toInt()

                    val safeWidth = if (width < 320) 320 else width
                    val safeHeight = if (height < 240) 240 else height

                    prefs.edit()
                        .putInt("resolution_width", safeWidth)
                        .putInt("resolution_height", safeHeight)
                        .apply()

                    updateResolutionSummary()
                    Log.d("AppSettings", "Resolution set to ${safeWidth}x${safeHeight}")
                } catch (e: NumberFormatException) {
                    Log.e("AppSettings", "Invalid resolution value")
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun updateResolutionSummary() {
        if (!isAdded) return

        getEngineDimensions()

        val prefs = preferenceManager.sharedPreferences ?: return
        val enabled = prefs.getBoolean("resolution_enabled", false)

        if (!enabled) {
            resolutionCurrentPreference.summary = "Using device default (${mEngineWidth}x${mEngineHeight})"
            return
        }

        val width = prefs.getInt("resolution_width", mEngineWidth)
        val height = prefs.getInt("resolution_height", mEngineHeight)
        resolutionCurrentPreference.summary = "${width}x${height}"
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
        updateResolutionSummary()
    }

    override fun onPause() {
        super.onPause()
        preferences.unregisterOnSharedPreferenceChangeListener(this)
    }
}
