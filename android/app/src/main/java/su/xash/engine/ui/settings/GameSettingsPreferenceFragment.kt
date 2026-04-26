package su.xash.engine.ui.settings

import android.os.Bundle
import android.util.DisplayMetrics
import android.util.Log
import androidx.preference.EditTextPreference
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreferenceCompat
import su.xash.engine.R
import su.xash.engine.model.Game

class GameSettingsPreferenceFragment(val game: Game) : PreferenceFragmentCompat() {

	private var mEngineWidth: Int = 0
	private var mEngineHeight: Int = 0

	override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
		preferenceManager.sharedPreferencesName = game.basedir.name;
		setPreferencesFromResource(R.xml.game_preferences, rootKey);

		val packageList = findPreference<ListPreference>("package_name")!!
		packageList.entries = arrayOf(getString(R.string.app_name))
		packageList.entryValues = arrayOf(requireContext().packageName)

		if (packageList.value == null) {
			packageList.setValueIndex(0);
		}

		val separatePackages = findPreference<SwitchPreferenceCompat>("separate_libraries")!!
		val clientPackage = findPreference<ListPreference>("client_package")!!
		val serverPackage = findPreference<ListPreference>("server_package")!!
		separatePackages.setOnPreferenceChangeListener { _, newValue ->
			if (newValue == true) {
				packageList.isVisible = false
				clientPackage.isVisible = true
				serverPackage.isVisible = true
			} else {
				packageList.isVisible = true
				clientPackage.isVisible = false
				serverPackage.isVisible = false
			}

			true
		}

		// Resolution settings
		initResolutionSettings()
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

		Log.d("GameSettings", "Engine dimensions: ${mEngineWidth}x${mEngineHeight}")

		val resolutionFixed = findPreference<SwitchPreferenceCompat>("resolution_fixed")!!
		val resolutionCustom = findPreference<SwitchPreferenceCompat>("resolution_custom")!!
		val resolutionScale = findPreference<EditTextPreference>("resolution_scale")!!
		val resolutionWidth = findPreference<EditTextPreference>("resolution_width")!!
		val resolutionHeight = findPreference<EditTextPreference>("resolution_height")!!
		val resolutionResult = findPreference<Preference>("resolution_result")!!

		// Set default values if not set
		val prefs = preferenceManager.sharedPreferences!!
		if (!prefs.contains("resolution_width")) {
			prefs.edit().putInt("resolution_width", mEngineWidth).apply()
		}
		if (!prefs.contains("resolution_height")) {
			prefs.edit().putInt("resolution_height", mEngineHeight).apply()
		}

		// Update visibility based on settings
		resolutionCustom.isVisible = resolutionFixed.isChecked
		resolutionScale.isVisible = resolutionFixed.isChecked && !resolutionCustom.isChecked
		resolutionWidth.isVisible = resolutionFixed.isChecked && resolutionCustom.isChecked
		resolutionHeight.isVisible = resolutionFixed.isChecked && resolutionCustom.isChecked
		resolutionResult.isVisible = resolutionFixed.isChecked

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
				prefs!!.edit().putInt("resolution_height", height).apply()
				resolutionHeight.text = height.toString()
			} catch (e: NumberFormatException) {
				Log.e("GameSettings", "Invalid width value: $newValue")
			}
			updateResolutionResult()
			true
		}

		resolutionHeight.setOnPreferenceChangeListener { _, _ ->
			updateResolutionResult()
			true
		}

		// Initial update
		if (resolutionFixed.isChecked) {
			updateResolutionResult()
		}
	}

	private fun updateResolutionResult() {
		val resolutionResult = findPreference<Preference>("resolution_result") ?: return
		val resolutionCustom = findPreference<SwitchPreferenceCompat>("resolution_custom") ?: return
		val prefs = preferenceManager.sharedPreferences!!

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
		Log.d("GameSettings", "Calculated resolution: ${finalWidth}x${finalHeight}")
	}
}
