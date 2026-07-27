# license:BSD-3-Clause
"""NVDA driver for the emulated Telesensory Systems Prose 2000."""

import builtins
from collections import OrderedDict
from ctypes import c_short
import os
import queue
import re
import struct
import subprocess
import threading
import time
import unicodedata

import config
import nvwave
from logHandler import log
from speech.commands import IndexCommand
from synthDriverHandler import SynthDriver, VoiceInfo, synthDoneSpeaking, synthIndexReached

_ = getattr(builtins, "_", lambda text: text)

_HERE = os.path.dirname(__file__)
_ENGINE_DIR = os.path.join(_HERE, "_prose2000")
_HOST_PATH = os.path.join(_ENGINE_DIR, "ProseHost.exe")
_ROM_DIR = os.path.join(_ENGINE_DIR, "roms")
_ROM_NAMES = (
	"v3.4.1__2000__2.u22",
	"v3.4.1__2000__3.u45",
	"v3.4.1__2000__0.u21",
	"v3.4.1__2000__1.u44",
	"v3.12__8-9-88__dsp_prog.u29",
	"v3.12__8-9-88__dsp_data.u29",
)

_MAGIC = 0x4B325250
_SPEAK = 1
_CANCEL = 2
_QUIT = 3
_READY = 101
_AUDIO = 102
_DONE = 103
_ERROR = 104
_CANCELLED = 105
_HEADER = struct.Struct("<IIII")
_MAX_PAYLOAD = 1 << 20
_CREATE_NO_WINDOW = 0x08000000


class _HostError(RuntimeError):
	pass


class _ProseHost:
	"""Own the isolated emulator process and its binary protocol."""

	def __init__(self):
		self._process = None
		self._readerThread = None
		self._messages = queue.Queue(maxsize=32)
		self._writeLock = threading.Lock()

	@staticmethod
	def _readExact(stream, size):
		parts = []
		remaining = size
		while remaining:
			part = stream.read(remaining)
			if not part:
				raise EOFError
			parts.append(part)
			remaining -= len(part)
		return b"".join(parts)

	def _putMessage(self, process, messages, message):
		while process is self._process:
			try:
				messages.put(message, timeout=0.1)
				return
			except queue.Full:
				continue

	def _reader(self, process, messages):
		try:
			while process is self._process:
				header = self._readExact(process.stdout, _HEADER.size)
				magic, messageType, generation, size = _HEADER.unpack(header)
				if magic != _MAGIC or size > _MAX_PAYLOAD:
					raise _HostError("The Prose host returned an invalid message.")
				payload = self._readExact(process.stdout, size) if size else b""
				self._putMessage(process, messages, (messageType, generation, payload))
		except EOFError:
			if process is self._process:
				self._putMessage(
					process,
					messages,
					(None, 0, b"The Prose host stopped unexpectedly."),
				)
		except Exception as error:
			if process is self._process:
				self._putMessage(
					process,
					messages,
					(None, 0, str(error).encode("utf-8", "replace")),
				)

	def start(self):
		self.stop()
		self._messages = queue.Queue(maxsize=32)
		try:
			self._process = subprocess.Popen(
				[_HOST_PATH, _ROM_DIR],
				stdin=subprocess.PIPE,
				stdout=subprocess.PIPE,
				stderr=subprocess.DEVNULL,
				bufsize=0,
				creationflags=_CREATE_NO_WINDOW,
			)
		except OSError as error:
			raise _HostError("The Prose host could not be started.") from error
		process = self._process
		self._readerThread = threading.Thread(
			target=self._reader,
			args=(process, self._messages),
			name="Prose 2000 host reader",
			daemon=True,
		)
		self._readerThread.start()
		try:
			messageType, _, payload = self._messages.get(timeout=10)
		except queue.Empty as error:
			self.stop()
			raise _HostError("The Prose host did not become ready.") from error
		if messageType != _READY:
			self.stop()
			detail = payload.decode("utf-8", "replace")
			raise _HostError(detail or "The Prose host failed during startup.")

	def isRunning(self):
		return self._process is not None and self._process.poll() is None

	def send(self, messageType, generation=0, payload=b""):
		process = self._process
		if process is None or process.poll() is not None:
			raise _HostError("The Prose host is not running.")
		packet = _HEADER.pack(_MAGIC, messageType, generation, len(payload)) + payload
		try:
			with self._writeLock:
				process.stdin.write(packet)
				process.stdin.flush()
		except (OSError, ValueError) as error:
			raise _HostError("The Prose host connection was lost.") from error

	def getMessage(self, timeout):
		return self._messages.get(timeout=timeout)

	def cancel(self, generation):
		if generation is None or not self.isRunning():
			return
		try:
			self.send(_CANCEL, generation)
		except _HostError:
			pass

	def stop(self):
		process = self._process
		self._process = None
		if process is None:
			return
		if process.poll() is None:
			try:
				with self._writeLock:
					process.stdin.write(_HEADER.pack(_MAGIC, _QUIT, 0, 0))
					process.stdin.flush()
			except (OSError, ValueError):
				pass
			try:
				process.wait(timeout=1)
			except subprocess.TimeoutExpired:
				process.terminate()
				try:
					process.wait(timeout=1)
				except subprocess.TimeoutExpired:
					process.kill()
		try:
			process.stdin.close()
		except Exception:
			pass
		try:
			process.stdout.close()
		except Exception:
			pass


def _makePlayer():
	try:
		return nvwave.WavePlayer(
			channels=1,
			samplesPerSec=10000,
			bitsPerSample=16,
			outputDevice=config.conf["audio"]["outputDevice"],
		)
	except Exception:
		return nvwave.WavePlayer(1, 10000, 16)


def _cleanText(text):
	text = unicodedata.normalize("NFKD", text)
	text = text.encode("ascii", "replace").decode("ascii")
	return re.sub(r"[^\x20-\x7e]", " ", text)


class _AudioProcessor:
	"""Apply one continuous rate and volume transform to an utterance."""

	def __init__(self, rate, rateBoost, volume):
		self._volume = volume
		self._sonic = None
		exponent = (rate - 50) / 50.0
		if rateBoost:
			exponent *= 1.5
		speed = 2.0 ** exponent
		if abs(speed - 1.0) <= 0.001:
			return
		try:
			from synthDrivers import _sonic

			_sonic.initialize()
			self._sonic = _sonic.SonicStream(10000, 1)
			self._sonic.speed = speed
		except Exception:
			log.error("Prose 2000 Sonic rate processing failed", exc_info=True)

	def _applyVolume(self, data):
		if self._volume <= 0:
			return b"\0" * len(data)
		if self._volume >= 100 or not data:
			return data
		sampleCount = len(data) // 2
		samples = (c_short * sampleCount).from_buffer_copy(data)
		factor = self._volume / 100.0
		for index in range(sampleCount):
			samples[index] = int(samples[index] * factor)
		return bytes(samples)

	def process(self, data):
		if self._sonic is not None and data:
			sampleCount = len(data) // 2
			samples = (c_short * sampleCount).from_buffer_copy(data)
			self._sonic.writeShort(samples, sampleCount)
			data = bytes(self._sonic.readShort())
		return self._applyVolume(data)

	def finish(self):
		if self._sonic is None:
			return b""
		self._sonic.flush()
		return self._applyVolume(bytes(self._sonic.readShort()))


class SynthDriver(SynthDriver):
	name = "prose2000"
	description = _("Prose 2000")
	supportedSettings = (
		SynthDriver.RateSetting(minStep=5),
		SynthDriver.RateBoostSetting(),
		SynthDriver.VolumeSetting(minStep=5),
	)
	supportedCommands = {IndexCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	@classmethod
	def check(cls):
		return os.path.isfile(_HOST_PATH) and all(
			os.path.isfile(os.path.join(_ROM_DIR, name)) for name in _ROM_NAMES
		)

	def _get_availableVoices(self):
		return OrderedDict((
			("prose2000", VoiceInfo("prose2000", "Prose 2000", language="en")),
		))

	def _get_voice(self):
		return "prose2000"

	def _set_voice(self, value):
		pass

	def __init__(self):
		super().__init__()
		self._rate = 50
		self._rateBoost = False
		self._volume = 100
		self._player = _makePlayer()
		self._host = _ProseHost()
		self._jobs = queue.Queue()
		self._generation = 1
		self._activeGeneration = None
		self._stateLock = threading.Lock()
		self._stopping = threading.Event()
		self._workerThread = threading.Thread(
			target=self._worker,
			name="Prose 2000 synth",
			daemon=True,
		)
		self._workerThread.start()

	def _get_rate(self):
		return self._rate

	def _set_rate(self, value):
		self._rate = max(0, min(100, int(value)))

	def _get_rateBoost(self):
		return self._rateBoost

	def _set_rateBoost(self, value):
		self._rateBoost = bool(value)

	def _get_volume(self):
		return self._volume

	def _set_volume(self, value):
		self._volume = max(0, min(100, int(value)))

	def _isCurrent(self, generation):
		with self._stateLock:
			return not self._stopping.is_set() and generation == self._generation

	def speak(self, speechSequence):
		events = []
		textParts = []

		def flushText():
			text = _cleanText("".join(textParts)).strip()
			textParts.clear()
			if text:
				events.append(("text", text))

		for item in speechSequence:
			if isinstance(item, str):
				textParts.append(item)
			elif isinstance(item, IndexCommand):
				flushText()
				events.append(("index", item.index))
		flushText()
		if not events:
			return
		with self._stateLock:
			generation = self._generation
		self._jobs.put((generation, events))

	def cancel(self):
		with self._stateLock:
			active = self._activeGeneration
			self._generation += 1
		self._host.cancel(active)
		try:
			while True:
				self._jobs.get_nowait()
				self._jobs.task_done()
		except queue.Empty:
			pass
		try:
			self._player.stop()
		except Exception:
			log.debugWarning("Prose 2000: audio cancellation failed", exc_info=True)

	def pause(self, switch):
		try:
			self._player.pause(switch)
		except Exception:
			log.debugWarning("Prose 2000: audio pause failed", exc_info=True)

	def terminate(self):
		self._stopping.set()
		self.cancel()
		self._jobs.put(None)
		self._workerThread.join(timeout=3)
		self._host.stop()
		try:
			self._player.close()
		except Exception:
			pass
		super().terminate()

	def _worker(self):
		# Boot the emulator away from NVDA's main thread so the first focused
		# object does not pay the firmware startup cost.
		try:
			self._host.start()
		except Exception:
			log.error("Prose 2000 host startup failed", exc_info=True)
		while not self._stopping.is_set():
			job = self._jobs.get()
			if job is None:
				self._jobs.task_done()
				return
			try:
				if self._isCurrent(job[0]):
					self._render(*job)
			except Exception:
				log.error("Prose 2000 synthesis failed", exc_info=True)
			finally:
				self._jobs.task_done()

	def _restartHost(self):
		self._host.stop()
		self._host.start()

	def _notifyIndex(self, generation, index):
		if not self._isCurrent(generation):
			return
		synthIndexReached.notify(synth=self, index=index)

	def _notifyDone(self, generation):
		if not self._isCurrent(generation):
			return
		synthDoneSpeaking.notify(synth=self)

	def _synthesizeText(self, generation, text):
		if not self._host.isRunning():
			self._host.start()
		self._host.send(_SPEAK, generation, text.encode("ascii", "replace"))
		cancelledAt = None
		lastProgressAt = time.monotonic()
		processor = _AudioProcessor(self._rate, self._rateBoost, self._volume)
		while True:
			if not self._isCurrent(generation) and cancelledAt is None:
				cancelledAt = time.monotonic()
			try:
				messageType, messageGeneration, payload = self._host.getMessage(0.25)
			except queue.Empty:
				if cancelledAt is not None and time.monotonic() - cancelledAt > 1:
					self._restartHost()
					return None
				if time.monotonic() - lastProgressAt > 30 and self._isCurrent(generation):
					self._restartHost()
					raise _HostError("The Prose host stopped responding.")
				continue
			lastProgressAt = time.monotonic()
			if messageType is None:
				self._restartHost()
				raise _HostError(payload.decode("utf-8", "replace"))
			if messageGeneration != generation:
				continue
			if messageType == _AUDIO:
				if self._isCurrent(generation):
					audio = processor.process(payload)
					if audio:
						self._player.feed(audio)
			elif messageType == _DONE:
				audio = processor.finish()
				if audio and self._isCurrent(generation):
					self._player.feed(audio)
				return True
			elif messageType == _CANCELLED:
				return None
			elif messageType == _ERROR:
				raise _HostError(payload.decode("utf-8", "replace"))

	def _render(self, generation, events):
		with self._stateLock:
			if generation != self._generation:
				return
			self._activeGeneration = generation
		try:
			for eventType, value in events:
				if not self._isCurrent(generation):
					return
				if eventType == "index":
					self._notifyIndex(generation, value)
					continue
				completed = self._synthesizeText(generation, value)
				if completed is None or not self._isCurrent(generation):
					return
				self._player.idle()
			self._notifyDone(generation)
		finally:
			with self._stateLock:
				if self._activeGeneration == generation:
					self._activeGeneration = None
