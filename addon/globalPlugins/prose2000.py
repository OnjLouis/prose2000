import addonHandler
import globalPluginHandler
import gui
import wx

from ._onjGithubUpdater import GitHubReleaseUpdater

addonHandler.initTranslation()


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super().__init__()
		self._updater = GitHubReleaseUpdater("prose2000", "Prose 2000", "OnjLouis", "prose2000")
		self._updateMenuItem = gui.mainFrame.sysTrayIcon.toolsMenu.Append(
			wx.ID_ANY,
			_("Check for Prose 2000 &updates..."),
		)
		gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self._onCheckForUpdates, self._updateMenuItem)
		self._updater.start()

	def _onCheckForUpdates(self, event):
		self._updater.checkNow(True)

	def terminate(self):
		self._updater.stop()
		try:
			gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self._updateMenuItem)
		except Exception:
			pass
		super().terminate()
