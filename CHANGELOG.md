## 0.0.6
* Fix Windows build error C2589 in 0.0.5 (`std::min` clashed with the `min` macro from `<windows.h>`).

## 0.0.5
* Performance: `setState` no longer blocks the UI thread when called often. SMTC setters (status, buttons, rate) are only called when their value changes, and the timeline is only republished on a status/rate/duration change or a position jump (seek) of more than 1s. Previously every call made ~10 synchronous calls into the Windows shell, which could freeze apps that update playback state frequently.
* Re-sending the same media item (e.g. once its duration is known) now only updates the duration instead of clearing metadata, reloading artwork and resetting the position to 0.
* SMTC (WinRT) errors are reported and logged instead of crashing the app or breaking playback.

## 0.0.4
* sends duration, position and speed to GSMTC.
* plugin listens for PlaybackPositionChangeRequested and calls seek.
* Next, previous, stop, fast-forward and rewind are enabled only if the app's controls include them.
* If you skip tracks quickly, a slow artwork load from the previous track is now discarded instead of overwriting the current one. A mutex now guards the metadata updates.
## 0.0.3
* Local cover art support by @GerryDush
* Crash fixed by @AfalpHy - Null data handling

## 0.0.2

* Bug resolved for blank notification at start.
* Log cleanup.

## 0.0.1

* Initial release 🎉
* Windows platform implementation for `audio_service`.
* System Media Transport Controls (SMTC) integration.
* Media metadata (title, artist, album, artwork) display.
* Media button events (play, pause, next, previous, etc.).
* Compatible with `audio_service_platform_interface`.
