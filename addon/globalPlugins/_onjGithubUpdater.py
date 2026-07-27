# Shared GitHub release updater for Andre Louis's NVDA add-ons.

import json
import os
import threading
import time
import urllib.request

import addonHandler
import config
import globalVars
import gui
import wx
from core import callLater
from logHandler import log

try:
	from gui.addonGui import promptUserForRestart
except ImportError:
	promptUserForRestart = None

try:
	from systemUtils import ExecAndPump
except ImportError:
	from gui import ExecAndPump


CHECK_INTERVAL_MS = 86400 * 1000
RETRY_INTERVAL_MS = 600 * 1000
DOWNLOAD_BLOCK_SIZE = 8192


def _parseVersion(version):
	try:
		return tuple(int(part) for part in str(version).strip().lstrip("vV").split("."))
	except Exception:
		return (0,)


def _downloadFile(url, destination, update=None):
	request = urllib.request.Request(url, headers={"User-Agent": "Prose 2000 NVDA add-on updater"})
	with urllib.request.urlopen(request, timeout=120) as remote, open(destination, "wb") as local:
		size = int(remote.headers.get("content-length") or 0)
		read = 0
		while True:
			block = remote.read(DOWNLOAD_BLOCK_SIZE)
			if not block:
				break
			local.write(block)
			read += len(block)
			if update and size and update(int(read / size * 100)):
				return False
	return True


class GitHubReleaseUpdater:
	def __init__(self, addonName, addonLabel, owner, repo):
		self.addonName = addonName
		self.addonLabel = addonLabel
		self.apiUrl = "https://api.github.com/repos/%s/%s/releases/latest" % (owner, repo)
		self.updatesDir = os.path.join(globalVars.appArgs.configPath, "updates")
		self.stateFile = os.path.join(globalVars.appArgs.configPath, "%sGithubUpdate.json" % addonName)
		self.timer = None
		self.busy = False
		self.stopped = False
		self.hadError = False
		self.state = self._loadState()

	def _loadState(self):
		try:
			with open(self.stateFile, "r", encoding="utf-8") as source:
				return json.load(source)
		except Exception:
			return {"lastCheck": 0, "pendingFile": ""}

	def _saveState(self):
		try:
			with open(self.stateFile, "w", encoding="utf-8") as destination:
				json.dump(self.state, destination)
		except Exception:
			log.debugWarning("Could not save updater state for %s", self.addonName, exc_info=True)

	def start(self):
		if getattr(config, "isAppX", False):
			return
		self.stopped = False
		self._scheduleNext()

	def stop(self):
		self.stopped = True
		try:
			if self.timer and self.timer.IsRunning():
				self.timer.Stop()
		except Exception:
			pass
		self.timer = None

	def checkNow(self, fromGui=True):
		if self.busy:
			if fromGui:
				gui.messageBox(_("An update check is already in progress."), _("Prose 2000 updates"), wx.OK, gui.mainFrame)
			return
		self.busy = True
		threading.Thread(
			target=self._checkWorker,
			args=(fromGui,),
			name="Prose 2000 update check",
			daemon=True,
		).start()

	def _scheduleNext(self):
		if self.stopped:
			return
		try:
			if self.timer and self.timer.IsRunning():
				self.timer.Stop()
		except Exception:
			pass
		interval = RETRY_INTERVAL_MS if self.hadError else CHECK_INTERVAL_MS
		delay = int(interval - (time.time() * 1000 - self.state.get("lastCheck", 0)))
		self.timer = callLater(max(10000, delay), self.checkNow, False)

	def _currentAddon(self):
		for addon in addonHandler.getAvailableAddons():
			if addon.name == self.addonName:
				return addon
		return None

	def _getUpdateInfo(self):
		request = urllib.request.Request(self.apiUrl, headers={"User-Agent": "Prose 2000 NVDA add-on updater"})
		with urllib.request.urlopen(request, timeout=20) as response:
			data = json.loads(response.read().decode("utf-8"))
		asset = next(
			(item for item in data.get("assets", []) if str(item.get("name", "")).lower().endswith(".nvda-addon")),
			None,
		)
		if not asset:
			raise RuntimeError("The latest release has no NVDA add-on package.")
		return {
			"version": str(data.get("tag_name", "")).lstrip("vV"),
			"name": asset["name"],
			"url": asset["browser_download_url"],
			"notes": data.get("body") or _("No release notes are available."),
		}

	def _checkWorker(self, fromGui):
		try:
			info = self._getUpdateInfo()
			error = None
		except Exception as caught:
			info = None
			error = caught
		wx.CallAfter(self._handleCheck, fromGui, info, error)

	def _handleCheck(self, fromGui, info, error):
		self.busy = False
		if self.stopped:
			return
		current = self._currentAddon()
		if error or current is None:
			self.hadError = True
			log.debugWarning("Could not check GitHub updates for %s: %s", self.addonName, error or "add-on not found")
			if fromGui:
				gui.messageBox(_("Unable to check for updates right now."), _("Update check failed"), wx.OK | wx.ICON_ERROR, gui.mainFrame)
			self._scheduleNext()
			return
		self.hadError = False
		self.state["lastCheck"] = time.time() * 1000
		self._saveState()
		if _parseVersion(info["version"]) <= _parseVersion(current.version):
			if fromGui:
				gui.messageBox(_("There are no updates available for %s.") % self.addonLabel, _("No updates available"), wx.OK | wx.ICON_INFORMATION, gui.mainFrame)
			self._scheduleNext()
			return
		message = _(
			"A new version of {addon} is available: {version}.\n\n"
			"Do you want to download and install it now?\n\n{notes}"
		).format(addon=self.addonLabel, version=info["version"], notes=info["notes"])
		if gui.messageBox(message, _("Update available"), wx.YES | wx.NO | wx.ICON_INFORMATION, gui.mainFrame) == wx.YES:
			self._downloadAndInstall(info)
		self._scheduleNext()

	def _downloadAndInstall(self, info):
		os.makedirs(self.updatesDir, exist_ok=True)
		destination = os.path.join(self.updatesDir, info["name"])
		gui.mainFrame.prePopup()
		dialog = wx.ProgressDialog(
			_("Downloading Prose 2000 update"),
			_("Downloading update"),
			style=wx.PD_CAN_ABORT | wx.PD_ELAPSED_TIME | wx.PD_REMAINING_TIME | wx.PD_AUTO_HIDE,
			parent=gui.mainFrame,
		)
		try:
			def update(value):
				return not dialog.Update(value)[0]
			result = ExecAndPump(_downloadFile, info["url"], destination, update)
			if result.funcRes is False:
				return
			bundle = addonHandler.AddonBundle(destination)
			previous = self._currentAddon()
			installResult = ExecAndPump(addonHandler.installAddonBundle, bundle)
			installed = installResult.funcRes
			if getattr(bundle, "_installExceptions", None):
				raise RuntimeError("NVDA rejected the downloaded add-on.")
			if previous:
				previous.requestRemove()
			if installed:
				installed._cleanupAddonImports()
			if promptUserForRestart:
				promptUserForRestart()
		except Exception:
			log.error("Could not install the Prose 2000 update", exc_info=True)
			gui.messageBox(_("Failed to install the update."), _("Update failed"), wx.OK | wx.ICON_ERROR, gui.mainFrame)
		finally:
			dialog.Destroy()
			gui.mainFrame.postPopup()
			try:
				os.remove(destination)
			except OSError:
				pass
