#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "android/app/src/main/java/su/xash/svencoop/model/SteamAuthManager.kt").read_text()
start = source.index("    private suspend fun completeAuth(")
end = source.index("    private fun buildLogonBody(", start)
method = source[start:end].replace("private suspend fun", "suspend fun", 1)
compiler = Path(shutil.which("kotlinc")).resolve()
coroutines = Path(os.environ.get("KOTLIN_COROUTINES_JAR",
    str(compiler.parents[1] / "lib/kotlinx-coroutines-core-jvm.jar")))

harness = '''
import java.io.IOException
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

data class PollResult(val accountName: String, val refreshToken: String, val guardData: String?)
data class LoginState(val result: Int)
class Preferences {
    fun getString(key: String, fallback: String) = fallback
    fun edit() = this
    fun putString(key: String, value: String) = this
    fun putLong(key: String, value: Long) = this
    fun apply() {}
}
class Harness {
    val prefs = Preferences()
    var authSession: Any? = Any()
    val connectLock = Mutex()
    var connected = false
    var calls = 0
    var disconnected = 0
    var failures = 0
    var resultCode = 1
    var cancel = false
    fun connectSocket() {}
    fun doHandshake() {}
    fun startReader() { connected = true }
    fun disconnect() { disconnected++; connected = false }
    fun handleLogonResult(result: Int) = LoginState(result)
    suspend fun sendLogon(username: String, password: String?, authCode: String?,
        twoFactorCode: String?, loginKey: String?, steamId: Long?, accessToken: String?): Int {
        calls++
        check(accessToken == "token" && password == null && authCode == null)
        if (cancel) throw CancellationException("cancelled")
        if (calls <= failures) throw IOException("Connection lost")
        return resultCode
    }
'''

cases = '''
}
fun main() = runBlocking {
    val token = PollResult("account", "token", null)
    val recovered = Harness().apply { failures = 1 }
    check(recovered.completeAuth(token, 42).result == 1)
    check(recovered.calls == 2 && recovered.disconnected == 1)
    val failed = Harness().apply { failures = 2 }
    try { failed.completeAuth(token, 42); error("expected failure") } catch (_: IOException) {}
    check(failed.calls == 2 && failed.disconnected == 2)
    val rejected = Harness().apply { resultCode = 5 }
    check(rejected.completeAuth(token, 42).result == 5 && rejected.calls == 1)
    val cancelled = Harness().apply { cancel = true }
    try { cancelled.completeAuth(token, 42); error("expected cancellation") } catch (_: CancellationException) {}
    check(cancelled.calls == 1)
    println("PASS: token reconnect, bounded failures, rejection not retried, cancellation preserved")
}
'''

with tempfile.TemporaryDirectory(prefix="steam-auth-test-") as directory:
    test = Path(directory) / "AuthRetry.kt"
    output = Path(directory) / "test.jar"
    test.write_text(harness + method + cases)
    subprocess.run([str(compiler), "-nowarn", "-cp", str(coroutines), str(test),
        "-include-runtime", "-d", str(output)], check=True)
    subprocess.run(["java", "-cp", f"{output}{os.pathsep}{coroutines}", "AuthRetryKt"], check=True)
