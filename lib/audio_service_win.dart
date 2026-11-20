import 'dart:developer';

import 'package:audio_service_platform_interface/audio_service_platform_interface.dart';
import 'package:flutter/services.dart';

class AudioServiceWin extends AudioServicePlatform {
  AudioHandlerCallbacks? _handlerCallbacks;
  final methodChannel = const MethodChannel('audio_service_win');
  bool _methodCallHandlerSet = false;

  static void registerWith() {
    final instance = AudioServiceWin();
    AudioServicePlatform.instance = instance;
  }

  @override
  Future<void> configure(ConfigureRequest request) async {
    log('Configure AudioServiceWin.', name: 'audio_service_win');
    assert(request.config.androidNotificationChannelId != null,
        "androidNotificationChannelId is required for registering DBus object. e.g com.ryanheise.myapp.channel.audio");

    // Set up method call handler if not already done
    if (!_methodCallHandlerSet) {
      await _setupMethodCallHandler();
      _methodCallHandlerSet = true;
    }

    await methodChannel.invokeMethod<String>('initializeSMTC',
        {'appid': request.config.androidNotificationChannelId});
  }

  /// Duration of the current item in ms (0 = unknown/live).
  int _durationMs = 0;

  @override
  Future<void> setMediaItem(SetMediaItemRequest request) async {
    final item = request.mediaItem;
    _durationMs = item.duration?.inMilliseconds ?? 0;
    final art = item.artUri;
    await methodChannel.invokeMethod('setMediaItem', {
      'title': item.title,
      'artist': item.artist,
      'album': item.album,
      // Avoid sending the literal string "null" for missing artwork.
      'artUri': art?.toString(),
      'durationMs': _durationMs,
    });
  }

  @override
  Future<void> setState(SetStateRequest request) async {
    /* States: 0 playing, 1 paused, 2 stopped, 3 loading/buffering */
    final s = request.state;
    int state;
    if (s.processingState == AudioProcessingStateMessage.idle && !s.playing) {
      state = 1;
    } else if (s.playing &&
        (s.processingState == AudioProcessingStateMessage.loading ||
            s.processingState == AudioProcessingStateMessage.buffering)) {
      state = 3;
    } else {
      state = s.playing ? 0 : 1;
    }

    // Current position = last reported position + elapsed time * speed.
    var positionMs = s.updatePosition.inMilliseconds;
    if (s.playing && s.processingState == AudioProcessingStateMessage.ready) {
      final elapsed = DateTime.now().difference(s.updateTime).inMilliseconds;
      if (elapsed > 0) positionMs += (elapsed * s.speed).round();
    }
    if (_durationMs > 0 && positionMs > _durationMs) positionMs = _durationMs;
    if (positionMs < 0) positionMs = 0;

    final actions = s.systemActions;
    final controls = s.controls.map((c) => c.action).toSet();
    await methodChannel.invokeMethod('updateState', {
      'state': state,
      'positionMs': positionMs,
      'durationMs': _durationMs,
      'speed': s.speed,
      'canPlay': true,
      'canPause': true,
      'canNext': controls.contains(MediaActionMessage.skipToNext),
      'canPrevious': controls.contains(MediaActionMessage.skipToPrevious),
      'canStop': controls.contains(MediaActionMessage.stop),
      'canFastForward': controls.contains(MediaActionMessage.fastForward) ||
          actions.contains(MediaActionMessage.fastForward),
      'canRewind': controls.contains(MediaActionMessage.rewind) ||
          actions.contains(MediaActionMessage.rewind),
    });
  }

  @override
  Future<void> setQueue(SetQueueRequest request) async {
    log('setQueue() has not been implemented for Windows.',
        name: 'audio_service_win');
  }

  @override
  Future<void> stopService(StopServiceRequest request) async {
    _durationMs = 0;
    await methodChannel.invokeMethod('updateState', {'state': 2});
  }

  Future<void> _setupMethodCallHandler() async {
    methodChannel.setMethodCallHandler((call) async {
      if (call.method == 'onSMTCSeek') {
        final ms = (call.arguments as num).toInt();
        _handlerCallbacks?.seek(SeekRequest(position: Duration(milliseconds: ms)));
      } else if (call.method == 'onSMTCButtonPressed') {
        final button = call.arguments;
        log('SMTC Button Pressed: $button', name: 'audio_service_win');

        // Handle button presses using callbacks if available
        if (_handlerCallbacks != null) {
          switch (button) {
            case 'play':
              _handlerCallbacks!.play(const PlayRequest());
              break;
            case 'pause':
              _handlerCallbacks!.pause(const PauseRequest());
              break;
            case 'stop':
              _handlerCallbacks!.stop(const StopRequest());
              break;
            case 'next':
              _handlerCallbacks!.skipToNext(const SkipToNextRequest());
              break;
            case 'previous':
              _handlerCallbacks!.skipToPrevious(const SkipToPreviousRequest());
              break;
            case 'fastForward':
              _handlerCallbacks!.fastForward(const FastForwardRequest());
              break;
            case 'rewind':
              _handlerCallbacks!.rewind(const RewindRequest());
              break;
            default:
              log('Unhandled button: $button', name: 'audio_service_win');
              break;
          }
        }
      }
    });
  }

  @override
  void setHandlerCallbacks(AudioHandlerCallbacks callbacks) {
    _handlerCallbacks = callbacks;
  }
}
