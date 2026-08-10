package su.xash.engine.ui.steam

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputLayout
import kotlinx.coroutines.launch
import su.xash.engine.R
import su.xash.engine.model.SteamAuthManager

class SteamAuthFragment : Fragment() {

    private lateinit var auth: SteamAuthManager

    private lateinit var statusText: TextView
    private lateinit var usernameInput: EditText
    private lateinit var passwordInput: EditText
    private lateinit var codeInput: EditText
    private lateinit var usernameLayout: TextInputLayout
    private lateinit var passwordLayout: TextInputLayout
    private lateinit var codeLayout: TextInputLayout
    private lateinit var loginButton: MaterialButton
    private lateinit var logoutButton: MaterialButton

    private var pendingEmailCode = false
    private var pendingTwoFactor = false

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        auth = SteamAuthManager.get(requireContext())
        return inflater.inflate(R.layout.fragment_steam_auth, container, false)
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        statusText = view.findViewById(R.id.authStatus)
        usernameInput = view.findViewById(R.id.usernameInput)
        passwordInput = view.findViewById(R.id.passwordInput)
        codeInput = view.findViewById(R.id.codeInput)
        usernameLayout = view.findViewById(R.id.usernameLayout)
        passwordLayout = view.findViewById(R.id.passwordLayout)
        codeLayout = view.findViewById(R.id.codeLayout)
        loginButton = view.findViewById(R.id.loginButton)
        logoutButton = view.findViewById(R.id.logoutButton)

        usernameInput.setText(auth.currentUsername)

        loginButton.setOnClickListener { doLogin() }
        logoutButton.setOnClickListener {
            auth.logout()
            updateUi()
        }

        updateUi()

        // If we already have a stored login key, try to restore the session.
        if (auth.hasStoredKey && !auth.isLoggedIn) {
            lifecycleScope.launch {
                statusText.text = getString(R.string.steam_connecting)
                val state = auth.loginWithStoredKey()
                handleState(state)
            }
        }
    }

    private fun doLogin() {
        val username = usernameInput.text.toString().trim()
        val password = passwordInput.text.toString()
        val code = codeInput.text.toString().trim()
        if (username.isEmpty() || password.isEmpty()) {
            statusText.text = getString(R.string.steam_username_hint) + " / " + getString(R.string.steam_password_hint)
            return
        }
        lifecycleScope.launch {
            setLoading(true)
            statusText.text = getString(R.string.steam_authenticating)
            val state = auth.login(
                username = username,
                password = password,
                authCode = if (pendingEmailCode) code.ifEmpty { null } else null,
                twoFactorCode = if (pendingTwoFactor) code.ifEmpty { null } else null
            )
            setLoading(false)
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
            is SteamAuthManager.LoginState.Failed -> {
                statusText.text = getString(R.string.steam_error, state.message)
            }
            is SteamAuthManager.LoginState.Error -> {
                statusText.text = getString(R.string.steam_error, state.message)
            }
        }
    }

    private fun setLoading(loading: Boolean) {
        loginButton.isEnabled = !loading
        usernameInput.isEnabled = !loading
        passwordInput.isEnabled = !loading
        codeInput.isEnabled = !loading
    }

    private fun updateUi() {
        val loggedIn = auth.isLoggedIn
        loginButton.visibility = if (loggedIn) View.GONE else View.VISIBLE
        logoutButton.visibility = if (loggedIn) View.VISIBLE else View.GONE
        usernameLayout.visibility = if (loggedIn) View.GONE else View.VISIBLE
        passwordLayout.visibility = if (loggedIn) View.GONE else View.VISIBLE
        if (loggedIn) {
            statusText.text = getString(R.string.steam_logged_in, auth.currentUsername)
        }
    }
}
