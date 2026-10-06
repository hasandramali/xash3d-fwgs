package su.xash.svencoop.ui.steam

import android.os.Bundle
import android.os.SystemClock
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.switchmaterial.SwitchMaterial
import com.google.android.material.textfield.TextInputLayout
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import su.xash.svencoop.R
import su.xash.svencoop.model.SteamAuthManager

class SteamAuthFragment : Fragment() {

    private lateinit var auth: SteamAuthManager

    private lateinit var statusText: TextView
    private lateinit var elapsedText: TextView
    private lateinit var progressBar: LinearProgressIndicator
    private lateinit var usernameInput: EditText
    private lateinit var passwordInput: EditText
    private lateinit var codeInput: EditText
    private lateinit var usernameLayout: TextInputLayout
    private lateinit var passwordLayout: TextInputLayout
    private lateinit var codeLayout: TextInputLayout
    private lateinit var loginButton: MaterialButton
    private lateinit var logoutButton: MaterialButton
    private lateinit var brokerButton: MaterialButton
    private lateinit var autoConnectSwitch: SwitchMaterial

    private var pendingEmailCode = false
    private var pendingTwoFactor = false
    private var deviceWaitJob: Job? = null
    private var tickerJob: Job? = null
    private var brokerBusy = false

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        auth = SteamAuthManager.get(requireContext())
        return inflater.inflate(R.layout.fragment_steam_auth, container, false)
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        statusText = view.findViewById(R.id.authStatus)
        elapsedText = view.findViewById(R.id.authElapsed)
        progressBar = view.findViewById(R.id.authProgress)
        usernameInput = view.findViewById(R.id.usernameInput)
        passwordInput = view.findViewById(R.id.passwordInput)
        codeInput = view.findViewById(R.id.codeInput)
        usernameLayout = view.findViewById(R.id.usernameLayout)
        passwordLayout = view.findViewById(R.id.passwordLayout)
        codeLayout = view.findViewById(R.id.codeLayout)
        loginButton = view.findViewById(R.id.loginButton)
        logoutButton = view.findViewById(R.id.logoutButton)
        brokerButton = view.findViewById(R.id.brokerButton)
        autoConnectSwitch = view.findViewById(R.id.autoConnectSwitch)

        usernameInput.setText(auth.currentUsername)

        loginButton.setOnClickListener {
            if (deviceWaitJob != null) cancelDeviceWait() else doLogin()
        }
        logoutButton.setOnClickListener {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.steam_logout_confirm_title)
                .setPositiveButton(R.string.steam_logout_button) { _, _ ->
                    lifecycleScope.launch {
                        auth.logout()
                        pendingEmailCode = false
                        pendingTwoFactor = false
                        codeLayout.visibility = View.GONE
                        updateUi()
                    }
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
        brokerButton.setOnClickListener { toggleBroker() }

        val prefs = PreferenceManager.getDefaultSharedPreferences(requireContext())
        autoConnectSwitch.isChecked = prefs.getBoolean(SteamAuthManager.PREF_AUTO_CONNECT, true)
        autoConnectSwitch.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(SteamAuthManager.PREF_AUTO_CONNECT, checked).apply()
        }

        updateUi()

        // If we already have a stored session, try to restore it.
        if (auth.hasStoredKey && !auth.isLoggedIn) {
            lifecycleScope.launch {
                showBusy(true)
                statusText.text = getString(R.string.steam_connecting)
                val state = if (auth.hasStoredRefreshToken) {
                    auth.loginWithStoredRefreshToken()
                } else {
                    auth.loginWithStoredKey()
                }
                showBusy(false)
                handleState(state)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshBrokerButton()
    }

    override fun onDestroyView() {
        deviceWaitJob?.cancel()
        tickerJob?.cancel()
        deviceWaitJob = null
        tickerJob = null
        super.onDestroyView()
    }

    private fun doLogin() {
        val username = usernameInput.text.toString().trim()
        val password = passwordInput.text.toString()
        val code = codeInput.text.toString().trim()
        if (pendingEmailCode || pendingTwoFactor) {
            if (code.isEmpty()) {
                statusText.text = getString(R.string.steam_need_guard_code)
                return
            }
        } else if (username.isEmpty() || password.isEmpty()) {
            statusText.text = getString(R.string.steam_username_hint) + " / " + getString(R.string.steam_password_hint)
            return
        }
        lifecycleScope.launch {
            showBusy(true)
            statusText.text = getString(R.string.steam_authenticating)
            val state = if (pendingEmailCode || pendingTwoFactor) {
                auth.submitAuthCode(code)
            } else {
                auth.loginModern(username = username, password = password)
            }
            showBusy(false)
            handleState(state)
        }
    }

    private fun handleState(state: SteamAuthManager.LoginState) {
        when (state) {
            is SteamAuthManager.LoginState.Connecting -> {
                statusText.text = getString(R.string.steam_connecting)
            }
            is SteamAuthManager.LoginState.Success -> {
                pendingEmailCode = false
                pendingTwoFactor = false
                codeLayout.visibility = View.GONE
                hideCredentialInputs(false)
                statusText.text = getString(R.string.steam_logged_in, auth.currentUsername)
                auth.startBroker()
                updateUi()
            }
            is SteamAuthManager.LoginState.NeedEmailCode -> {
                pendingEmailCode = true
                pendingTwoFactor = false
                codeLayout.hint = getString(R.string.steam_email_code_hint)
                codeLayout.visibility = View.VISIBLE
                statusText.text = getString(R.string.steam_need_email_code)
            }
            is SteamAuthManager.LoginState.NeedTwoFactor -> {
                pendingEmailCode = false
                pendingTwoFactor = true
                codeLayout.hint = getString(R.string.steam_twofactor_hint)
                codeLayout.visibility = View.VISIBLE
                statusText.text = getString(R.string.steam_need_twofactor)
            }
            is SteamAuthManager.LoginState.NeedDeviceConfirmation -> {
                // No code to type: approve in the Steam mobile app. Poll up to
                // 5 minutes while the user switches apps and back.
                pendingEmailCode = false
                pendingTwoFactor = false
                codeLayout.visibility = View.GONE
                hideCredentialInputs(true)
                statusText.text = getString(R.string.steam_need_device_confirm)
                startDeviceWait()
            }
            is SteamAuthManager.LoginState.Failed -> {
                statusText.text = getString(R.string.steam_error, state.message)
            }
            is SteamAuthManager.LoginState.Error -> {
                statusText.text = getString(R.string.steam_error, state.message)
            }
        }
    }

    /** Polls for the in-app approval with elapsed timer + cancel. */
    private fun startDeviceWait() {
        deviceWaitJob?.cancel()
        tickerJob?.cancel()
        loginButton.setText(android.R.string.cancel)
        progressBar.visibility = View.VISIBLE
        elapsedText.visibility = View.VISIBLE
        val t0 = SystemClock.elapsedRealtime()
        tickerJob = viewLifecycleOwner.lifecycleScope.launch {
            while (isActive) {
                val s = ((SystemClock.elapsedRealtime() - t0) / 1000).toInt()
                elapsedText.text = getString(
                    R.string.steam_waiting_elapsed,
                    "%d:%02d".format(s / 60, s % 60)
                )
                delay(1000)
            }
        }
        deviceWaitJob = viewLifecycleOwner.lifecycleScope.launch {
            try {
                val result = auth.awaitDeviceConfirmation()
                handleState(result)
            } finally {
                stopDeviceWaitUi()
            }
        }
    }

    private fun cancelDeviceWait() {
        deviceWaitJob?.cancel()
        deviceWaitJob = null
        auth.cancelPendingAuth()
        stopDeviceWaitUi()
        hideCredentialInputs(false)
        statusText.text = getString(R.string.steam_device_confirm_cancelled)
        updateUi()
    }

    private fun stopDeviceWaitUi() {
        tickerJob?.cancel()
        tickerJob = null
        deviceWaitJob = null
        if (isAdded) {
            progressBar.visibility = View.GONE
            elapsedText.visibility = View.GONE
            loginButton.setText(R.string.steam_login_button)
        }
    }

    private fun hideCredentialInputs(hide: Boolean) {
        // Tucks username/password away during the device-approve wait; the
        // login button stays visible because it doubles as Cancel.
        if (auth.isLoggedIn) return
        val v = if (hide) View.GONE else View.VISIBLE
        usernameLayout.visibility = v
        passwordLayout.visibility = v
    }

    private fun toggleBroker() {
        if (brokerBusy) return
        brokerBusy = true
        val running = auth.isBrokerRunning()
        brokerButton.isEnabled = false
        brokerButton.alpha = 0.5f
        brokerButton.text = getString(
            if (running) R.string.steam_broker_stopping else R.string.steam_broker_starting
        )
        lifecycleScope.launch {
            if (running) {
                withContext(Dispatchers.IO) { auth.stopBroker() }
            } else {
                auth.startBroker()
                // startBroker binds async; give it a beat before reading state back.
                delay(800)
            }
            brokerBusy = false
            refreshBrokerButton()
        }
    }

    private fun refreshBrokerButton() {
        if (!isAdded) return
        if (brokerBusy) return
        val running = try {
            auth.isBrokerRunning()
        } catch (_: Exception) {
            false
        }
        brokerButton.isEnabled = true
        brokerButton.alpha = 1.0f
        brokerButton.text = getString(
            if (running) R.string.steam_broker_stop else R.string.steam_broker_start
        )
    }

    private fun showBusy(busy: Boolean) {
        progressBar.visibility = if (busy) View.VISIBLE else View.GONE
        loginButton.isEnabled = !busy
        usernameInput.isEnabled = !busy
        passwordInput.isEnabled = !busy
        codeInput.isEnabled = !busy
    }

    private fun updateUi() {
        val loggedIn = auth.isLoggedIn
        loginButton.visibility = if (loggedIn) View.GONE else View.VISIBLE
        logoutButton.visibility = if (loggedIn) View.VISIBLE else View.GONE
        usernameLayout.visibility = if (loggedIn) View.GONE else View.VISIBLE
        passwordLayout.visibility = if (loggedIn) View.GONE else View.VISIBLE
        // No account, no broker: the toggle only makes sense once logged in.
        brokerButton.visibility = if (loggedIn) View.VISIBLE else View.GONE
        if (loggedIn) {
            statusText.text = getString(R.string.steam_logged_in, auth.currentUsername)
        }
        refreshBrokerButton()
    }
}
