package su.xash.svencoop.ui.library

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.view.LayoutInflater
import android.view.Menu
import android.view.MenuInflater
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.graphics.drawable.DrawableCompat
import androidx.core.view.MenuProvider
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.launch
import su.xash.svencoop.BuildConfig
import su.xash.svencoop.R
import su.xash.svencoop.adapters.GameAdapter
import su.xash.svencoop.databinding.FragmentLibraryBinding
import su.xash.svencoop.model.DownloadService
import su.xash.svencoop.model.DownloadState
import su.xash.svencoop.model.GameDataDownloader
import su.xash.svencoop.model.SteamAuthManager
import java.io.File


class LibraryFragment : Fragment(), MenuProvider {
	private var _binding: FragmentLibraryBinding? = null
	private val binding get() = _binding!!

	private val libraryViewModel: LibraryViewModel by activityViewModels()
	private var isPermissionRequested = false
	private var hasShownFirstRunDialog = false
	private var steamMenuItem: MenuItem? = null
	private var downloadMenuItem: MenuItem? = null
	private val authStateListener: () -> Unit = { refreshSteamIcon() }
	private var progressContainer: View? = null
	private var progressBar: LinearProgressIndicator? = null
	private var statusText: TextView? = null
	private var lastTerminalState: DownloadState? = null

	private val startActivityForResult =
		registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
			isPermissionRequested = false
			if (checkStoragePermissions()) {
				libraryViewModel.reloadGames(requireContext())
			}
		}

	private val requiredPermissions = arrayOf(
		Manifest.permission.READ_EXTERNAL_STORAGE,
		Manifest.permission.WRITE_EXTERNAL_STORAGE
	)

	private val requestPermissionLauncher = registerForActivityResult(
		ActivityResultContracts.RequestMultiplePermissions()
	) { permissions ->
		val granted = permissions.entries.all { it.value }
		if (granted) {
			libraryViewModel.reloadGames(requireContext())
		}
	}

	private fun checkStoragePermissions(): Boolean {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
			if (!Environment.isExternalStorageManager()) {
				if (isPermissionRequested) return false
				isPermissionRequested = true
				MaterialAlertDialogBuilder(requireContext()).apply {
					setTitle(R.string.file_access_required)
					setMessage(R.string.file_access_message)
					setPositiveButton(R.string.open_settings) { _, _ ->
						startActivityForResult.launch(
							Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).setData(
								Uri.fromParts("package", BuildConfig.APPLICATION_ID, null)
							)
						)
					}
					setNeutralButton(android.R.string.cancel, null)
					setCancelable(false)
					show()

					return false
				}
			} else {
				return true
			}
		} else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
			val permissionsNeeded = requiredPermissions.filter {
				ContextCompat.checkSelfPermission(
					requireContext(),
					it
				) != PackageManager.PERMISSION_GRANTED
			}.toTypedArray()

			if (!permissionsNeeded.isEmpty()) {
				if (isPermissionRequested) return false
				isPermissionRequested = true
				val showRationale = permissionsNeeded.any {
					ActivityCompat.shouldShowRequestPermissionRationale(requireActivity(), it)
				}

				MaterialAlertDialogBuilder(requireContext()).apply {
					setTitle(R.string.external_storage_required)
					setMessage(R.string.external_storage_message)
					setPositiveButton(R.string.open_settings) { _, _ ->
						if (showRationale) {
							requestPermissionLauncher.launch(permissionsNeeded)
						} else {
							val intent =
								Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
									data =
										Uri.fromParts("package", requireContext().packageName, null)
								}
							startActivity(intent)
						}
					}
					setNeutralButton(android.R.string.cancel, null)
					setCancelable(false)
					show()
				}

				return false
			} else {
				return true
			}
		} else {
			return true
		}
	}

	override fun onCreateView(
		inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
	): View {
		_binding = FragmentLibraryBinding.inflate(inflater, container, false)

		val adapter = GameAdapter(libraryViewModel)
		binding.gamesList.adapter = adapter

		requireActivity().addMenuProvider(this, viewLifecycleOwner, Lifecycle.State.RESUMED)

		return binding.root
	}

	override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
		binding.swipeRefresh.setOnRefreshListener { libraryViewModel.reloadGames(requireContext()) }

		progressContainer = view.findViewById(R.id.downloadProgressContainer)
		progressBar = view.findViewById(R.id.downloaderProgress)
		statusText = view.findViewById(R.id.downloaderStatus)
		view.findViewById<View>(R.id.cancelButton)?.setOnClickListener { cancelSvencoopDownload() }

		observeDownloadState()

		libraryViewModel.isReloading.observe(viewLifecycleOwner) {
			binding.swipeRefresh.isRefreshing = it
		}

		libraryViewModel.installedGames.observe(viewLifecycleOwner) {
			(binding.gamesList.adapter as GameAdapter).submitList(it)
			if (it.isEmpty()) {
				checkFirstRunGameData()
			}
		}

		if (checkStoragePermissions()) {
			libraryViewModel.reloadGames(requireContext())
		}
	}

	private fun checkFirstRunGameData() {
		val downloader = GameDataDownloader(requireContext())
		if (downloader.hasCheckedFirstRun()) return
		if (hasShownFirstRunDialog) return
		if (!checkStoragePermissions()) return

		val basedir = libraryViewModel.getBaseDir()
		if (downloader.hasAnyGameData(basedir)) {
			downloader.setFirstRunChecked()
			return
		}

		hasShownFirstRunDialog = true
		MaterialAlertDialogBuilder(requireContext())
			.setTitle(R.string.no_game_data_found)
			.setMessage(R.string.no_game_data_message)
			.setPositiveButton(R.string.download_now) { _, _ ->
				downloader.setFirstRunChecked()
				startSvencoopDownload()
			}
			.setNegativeButton(R.string.no_thanks) { _, _ ->
				downloader.setFirstRunChecked()
			}
			.setCancelable(false)
			.show()
	}

	/**
	 * The toolbar download action downloads Sven Co-op straight away (there
	 * is no multi-game picker anymore): same guards and overwrite warning
	 * the old downloader tab used, then the foreground DownloadService
	 * reports progress/completion via notification.
	 */
	private fun startSvencoopDownload() {
		if (DownloadService.state.value is DownloadState.Downloading) return
		val gamedir = "svencoop"
		val info = GameDataDownloader.GAMES[gamedir] ?: return
		if (!checkStoragePermissions()) return
		val basedir = libraryViewModel.getBaseDir()
		val targetDir = File(basedir, gamedir)

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
			MaterialAlertDialogBuilder(requireContext())
				.setTitle(R.string.file_access_required)
				.setMessage(R.string.file_access_message)
				.setPositiveButton(R.string.open_settings) { _, _ ->
					startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).setData(
						Uri.fromParts("package", requireContext().packageName, null)
					))
				}
				.setNegativeButton(R.string.cancel, null)
				.show()
			return
		}

		if (targetDir.isDirectory && targetDir.listFiles()?.isNotEmpty() == true) {
			MaterialAlertDialogBuilder(requireContext())
				.setTitle(R.string.overwrite_confirm_title)
				.setMessage(getString(R.string.overwrite_confirm_message, info.displayName))
				.setPositiveButton(R.string.overwrite) { _, _ -> beginSvencoopDownload(gamedir) }
				.setNegativeButton(R.string.keep, null)
				.show()
		} else {
			// No game data: short confirm, then start.
			MaterialAlertDialogBuilder(requireContext())
				.setTitle(getString(R.string.install_svencoop_title))
				.setPositiveButton(android.R.string.yes) { _, _ -> beginSvencoopDownload(gamedir) }
				.setNegativeButton(android.R.string.no, null)
				.show()
		}
	}

	private fun beginSvencoopDownload(gamedir: String) {
		val info = GameDataDownloader.GAMES[gamedir] ?: return
		val basedir = libraryViewModel.getBaseDir()
		val prefs = requireContext().getSharedPreferences("depot_settings", 0)
		val enabledDepots = info.depotIds.filter { depotId ->
			prefs.getBoolean("${gamedir}_depot_$depotId", depotId == 1)
		}
		DownloadService.start(requireContext(), gamedir, File(basedir, gamedir), enabledDepots)
	}

	private fun cancelSvencoopDownload() {
		DownloadService.cancel(requireContext())
	}

	private fun observeDownloadState() {
		viewLifecycleOwner.lifecycleScope.launch {
			viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
				DownloadService.state.collect { state ->
					when (state) {
						is DownloadState.Idle -> {
							lastTerminalState = null
							if (progressContainer?.visibility == View.VISIBLE) {
								progressContainer?.visibility = View.GONE
							}
							refreshDownloadAction()
						}
						is DownloadState.Downloading -> {
							lastTerminalState = null
							progressContainer?.visibility = View.VISIBLE
							refreshDownloadAction()
							if (state.total > 0) {
								progressBar?.isIndeterminate = false
								progressBar?.progress = ((state.current * 100) / state.total).toInt()
							} else {
								progressBar?.isIndeterminate = true
							}
							statusText?.text = state.status
						}
						is DownloadState.Success -> {
							if (lastTerminalState != state) {
								lastTerminalState = state
								progressContainer?.visibility = View.GONE
								refreshDownloadAction()
								DownloadService.consumeCompletion()
								libraryViewModel.reloadGames(requireContext())
								MaterialAlertDialogBuilder(requireContext())
									.setTitle(R.string.download_complete)
									.setPositiveButton(android.R.string.ok) { _, _ ->
										DownloadService.resetState()
									}
									.show()
							}
						}
						is DownloadState.Error -> {
							if (lastTerminalState != state) {
								lastTerminalState = state
								progressContainer?.visibility = View.GONE
								refreshDownloadAction()
								val errorMsg = state.message
								MaterialAlertDialogBuilder(requireContext())
									.setTitle(R.string.download_failed)
									.setMessage(errorMsg)
									.setPositiveButton(android.R.string.ok) { _, _ ->
										DownloadService.resetState()
									}
									.setNeutralButton("Copy") { _, _ ->
										val clipboard = requireContext().getSystemService(android.content.Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
										val clip = android.content.ClipData.newPlainText("Error", errorMsg)
										clipboard.setPrimaryClip(clip)
									}
									.show()
							}
						}
						is DownloadState.Cancelled -> {
							if (lastTerminalState != state) {
								lastTerminalState = state
								statusText?.text = getString(R.string.download_cancelled)
								progressContainer?.postDelayed({
									progressContainer?.visibility = View.GONE
									refreshDownloadAction()
									DownloadService.resetState()
								}, 1500)
							}
						}
					}
				}
			}
		}
	}

	override fun onDestroyView() {
		super.onDestroyView()
		steamMenuItem = null
		downloadMenuItem = null
		_binding = null
	}

	override fun onCreateMenu(menu: Menu, menuInflater: MenuInflater) {
		menuInflater.inflate(R.menu.menu_library, menu)
		steamMenuItem = menu.findItem(R.id.action_steam)
		downloadMenuItem = menu.findItem(R.id.action_download)
		refreshSteamIcon()
		refreshDownloadAction()
	}

	override fun onMenuItemSelected(menuItem: MenuItem): Boolean {
		when (menuItem.itemId) {
			R.id.action_settings -> {
				findNavController().navigate(R.id.action_libraryFragment_to_appSettingsFragment)
			}
			R.id.action_download -> {
				startSvencoopDownload()
			}
			R.id.action_steam -> {
				findNavController().navigate(R.id.action_libraryFragment_to_steamAuthFragment)
			}
		}

		return false
	}

	override fun onResume() {
		super.onResume()

		SteamAuthManager.get(requireContext()).addAuthStateListener(authStateListener)
		refreshSteamIcon()
		refreshDownloadAction()

		if (checkStoragePermissions()) {
			libraryViewModel.reloadGames(requireContext())
		}
	}

	override fun onPause() {
		SteamAuthManager.get(requireContext()).removeAuthStateListener(authStateListener)
		super.onPause()
	}

	/** Download action is dimmed + inert while a download runs; back to normal after. */
	private fun refreshDownloadAction() {
		val item = downloadMenuItem ?: return
		if (!isAdded) return
		val downloading = DownloadService.state.value is DownloadState.Downloading
		requireActivity().runOnUiThread {
			item.isEnabled = !downloading
			item.icon?.mutate()?.alpha = if (downloading) 100 else 255
		}
	}
	/** Tints the toolbar Steam icon green while a Steam session is live. */
	private fun refreshSteamIcon() {
		val item = steamMenuItem ?: return
		if (!isAdded) return
		val auth = try {
			SteamAuthManager.get(requireContext())
		} catch (_: Exception) {
			return
		}
		requireActivity().runOnUiThread {
			val icon = item.icon?.mutate() ?: return@runOnUiThread
			if (auth.isLoggedIn) {
				DrawableCompat.setTint(icon, ContextCompat.getColor(requireContext(), R.color.steam_online_green))
			} else {
				DrawableCompat.setTintList(icon, null)
			}
			item.icon = icon
		}
	}
}
