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
