#include "audio_service_win_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <memory>
#include <sstream>
#include <iostream>
#include <fstream>  // std::ifstream
#include <vector>   // std::vector
#include <iterator> // std::istreambuf_iterator
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <optional>
#include <functional>
#include <variant>

static winrt::Windows::Media::Playback::MediaPlayer mediaPlayer{nullptr};
static winrt::Windows::Media::SystemMediaTransportControls smtc{nullptr};
static winrt::Windows::Media::SystemMediaTransportControlsDisplayUpdater updater{nullptr};
static std::unique_ptr<flutter::MethodChannel<>> channel;
static std::string appId;

// ---- Thread-marshalling -------------------------------------------------
// SMTC events arrive on WinRT thread-pool threads, but Flutter's
// MethodChannel must only be used from the platform (UI) thread. Events are
// queued and a custom window message wakes the platform thread to deliver them.
static constexpr UINT kSmtcEventMessage = WM_APP + 0x4A53;
static HWND flutterWindow = nullptr;
static std::mutex eventMutex;
static std::deque<std::pair<std::string, flutter::EncodableValue>> pendingEvents;
// Bumped on every metadata change/stop so late async thumbnail loads from a
// previous track are discarded instead of overwriting the current one.
static std::atomic<uint64_t> metadataGeneration{0};
static std::mutex updaterMutex;

static void PostEvent(const std::string &name, flutter::EncodableValue value)
{
  {
    std::lock_guard<std::mutex> lock(eventMutex);
    pendingEvents.emplace_back(name, std::move(value));
  }
  if (flutterWindow != nullptr)
  {
    ::PostMessage(flutterWindow, kSmtcEventMessage, 0, 0);
  }
}

static void FlushEvents()
{
  std::deque<std::pair<std::string, flutter::EncodableValue>> local;
  {
    std::lock_guard<std::mutex> lock(eventMutex);
    local.swap(pendingEvents);
  }
  if (!channel) return;
  for (auto &e : local)
  {
    channel->InvokeMethod(e.first, std::make_unique<flutter::EncodableValue>(std::move(e.second)));
  }
}

static int64_t GetInt(const flutter::EncodableMap *m, const char *key, int64_t def)
{
  if (!m) return def;
  auto it = m->find(flutter::EncodableValue(key));
  if (it == m->end()) return def;
  if (auto v = std::get_if<int32_t>(&it->second)) return *v;
  if (auto v = std::get_if<int64_t>(&it->second)) return *v;
  return def;
}

static bool GetBool(const flutter::EncodableMap *m, const char *key, bool def)
{
  if (!m) return def;
  auto it = m->find(flutter::EncodableValue(key));
  if (it == m->end()) return def;
  if (auto v = std::get_if<bool>(&it->second)) return *v;
  return def;
}

static double GetDouble(const flutter::EncodableMap *m, const char *key, double def)
{
  if (!m) return def;
  auto it = m->find(flutter::EncodableValue(key));
  if (it == m->end()) return def;
  if (auto v = std::get_if<double>(&it->second)) return *v;
  if (auto v = std::get_if<int32_t>(&it->second)) return *v;
  return def;
}

static winrt::Windows::Foundation::TimeSpan MsToTimeSpan(int64_t ms)
{
  return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::milliseconds(ms < 0 ? 0 : ms));
}

// Publishes duration/position to GSMTC so apps like lyric tools can sync.
static void UpdateTimeline(int64_t durationMs, int64_t positionMs)
{
  if (!smtc) return;
  try
  {
    winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties props;
    if (durationMs < 0) durationMs = 0;
    positionMs = std::clamp<int64_t>(positionMs, 0, durationMs > 0 ? durationMs : std::max<int64_t>(positionMs, 0));
    props.StartTime(MsToTimeSpan(0));
    props.MinSeekTime(MsToTimeSpan(0));
    props.EndTime(MsToTimeSpan(durationMs));
    props.MaxSeekTime(MsToTimeSpan(durationMs));
    props.Position(MsToTimeSpan(positionMs));
    smtc.UpdateTimelineProperties(props);
  }
  catch (const winrt::hresult_error &e)
  {
    std::cerr << "Failed to update SMTC timeline: " << winrt::to_string(e.message()) << std::endl;
  }
}

namespace audio_service_win
{

  // static
  void AudioServiceWinPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar)
  {

    static bool plugin_already_registered = false;

    if (plugin_already_registered) {
      // Skip registration in subwindow
      return;
    }

    plugin_already_registered = true;

    channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "audio_service_win",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<AudioServiceWinPlugin>();

    channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto &call, auto result)
        {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    if (auto *view = registrar->GetView())
    {
      flutterWindow = ::GetAncestor(view->GetNativeWindow(), GA_ROOT);
    }
    registrar->RegisterTopLevelWindowProcDelegate(
        [](HWND, UINT message, WPARAM, LPARAM) -> std::optional<LRESULT>
        {
          if (message == kSmtcEventMessage)
          {
            FlushEvents();
            return 0;
          }
          return std::nullopt;
        });

    registrar->AddPlugin(std::move(plugin));
  }

  AudioServiceWinPlugin::AudioServiceWinPlugin() {}

  AudioServiceWinPlugin::~AudioServiceWinPlugin() {}

  // Function to setup System Media Transport Controls (SMTC)
  static void SetupSMTC()
  {
    if (mediaPlayer == nullptr)
    {
      mediaPlayer = winrt::Windows::Media::Playback::MediaPlayer();
      smtc = mediaPlayer.SystemMediaTransportControls();
      updater = smtc.DisplayUpdater();
      updater.AppMediaId(winrt::to_hstring(appId));

      smtc.IsPlayEnabled(true);
      smtc.IsPauseEnabled(true);
      smtc.IsNextEnabled(true);
      smtc.IsPreviousEnabled(true);
      smtc.IsEnabled(false); // Keep disabled until setMediaItem

      std::cout << "SMTC initialized with appid: " << appId << std::endl;

      smtc.ButtonPressed([](auto const &, winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const &args)
                         {
                           using B = winrt::Windows::Media::SystemMediaTransportControlsButton;
                           std::string method;
                           switch (args.Button())
                           {
                           case B::Play: method = "play"; break;
                           case B::Pause: method = "pause"; break;
                           case B::Next: method = "next"; break;
                           case B::Previous: method = "previous"; break;
                           case B::Stop: method = "stop"; break;
                           case B::FastForward: method = "fastForward"; break;
                           case B::Rewind: method = "rewind"; break;
                           default: return;
                           }
                           PostEvent("onSMTCButtonPressed", flutter::EncodableValue(method)); });

      // Seek requests from the system overlay / other apps.
      smtc.PlaybackPositionChangeRequested([](auto const &, winrt::Windows::Media::PlaybackPositionChangeRequestedEventArgs const &args)
                                           {
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition()).count();
        PostEvent("onSMTCSeek", flutter::EncodableValue(ms)); });
    }
  }

} // namespace audio_service_win

namespace audio_service_win
{

  std::string UrlDecode(const std::string &encoded)
  {
    std::ostringstream result;
    for (size_t i = 0; i < encoded.size(); ++i)
    {
      if (encoded[i] == '%' && i + 2 < encoded.size())
      {
        int value;
        std::istringstream is(encoded.substr(i + 1, 2));
        if (is >> std::hex >> value)
        {
          result << static_cast<char>(value);
          i += 2;
        }
        else
        {
          result << encoded[i];
        }
      }
      else if (encoded[i] == '+')
      {
        result << ' ';
      }
      else
      {
        result << encoded[i];
      }
    }
    return result.str();
  }

  void AudioServiceWinPlugin::HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {

    // Initialize System Media Transport Controls (SMTC) but do NOT enable or update notification yet
    if (method_call.method_name().compare("initializeSMTC") == 0)
    {
      std::cout << "Starting SMTC..." << std::endl;

      // Extract the appid argument from the method call
      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (arguments && arguments->find(flutter::EncodableValue("appid")) != arguments->end())
      {
        appId = std::get<std::string>(arguments->at(flutter::EncodableValue("appid")));

        result->Success();
      }
      else
      {
        result->Error("InvalidArguments", "appid is required");
      }
    }

    // Set/Update new Media metadata
    else if (method_call.method_name().compare("setMediaItem") == 0)
    {
      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (arguments && arguments->find(flutter::EncodableValue("title")) != arguments->end() &&
          arguments->find(flutter::EncodableValue("artist")) != arguments->end() &&
          arguments->find(flutter::EncodableValue("album")) != arguments->end())
      {
        std::string title;
        const auto &titleValue =
            arguments->at(flutter::EncodableValue("title"));
        if (!titleValue.IsNull() &&
            std::holds_alternative<std::string>(titleValue)) {
          title = std::get<std::string>(titleValue);
        }

        std::string artist;
        const auto &artistValue =
            arguments->at(flutter::EncodableValue("artist"));
        if (!artistValue.IsNull() &&
            std::holds_alternative<std::string>(artistValue)) {
          artist = std::get<std::string>(artistValue);
        }

        std::string album;
        const auto &albumValue =
            arguments->at(flutter::EncodableValue("album"));
        if (!albumValue.IsNull() &&
            std::holds_alternative<std::string>(albumValue)) {
          album = std::get<std::string>(albumValue);
        }

        // Ensure SMTC is initialized
        if (!smtc)
        {
          SetupSMTC();
        }

        std::string artUri;
        if (arguments->find(flutter::EncodableValue("artUri")) != arguments->end())
        {
          const auto &artUriValue = arguments->at(flutter::EncodableValue("artUri"));
          if (!artUriValue.IsNull() && std::holds_alternative<std::string>(artUriValue))
          {
            artUri = std::get<std::string>(artUriValue);
          }
        }

        if (updater)
        {
          metadataGeneration++;
          std::lock_guard<std::mutex> lock(updaterMutex);
          smtc.IsEnabled(true);
          updater.ClearAll();
          updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
          updater.MusicProperties().Title(winrt::to_hstring(title));
          updater.MusicProperties().Artist(winrt::to_hstring(artist));
          updater.MusicProperties().AlbumTitle(winrt::to_hstring(album));

          if (!artUri.empty())
          {
            try
            {

              const uint64_t gen = metadataGeneration.load();
              std::thread([artUri, gen]()
                          {
              winrt::init_apartment(winrt::apartment_type::multi_threaded);
              try {
                  if (artUri.rfind("http://", 0) == 0 || artUri.rfind("https://", 0) == 0) {
                      winrt::Windows::Foundation::Uri uri(winrt::to_hstring(artUri));
                      auto thumbRef = winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(uri);
                      if (gen != metadataGeneration.load()) return;
                      std::lock_guard<std::mutex> lock(updaterMutex);
                      if (gen == metadataGeneration.load()) updater.Thumbnail(thumbRef);
                  } else {
                      std::string localPath = artUri;
                      if (localPath.rfind("file://", 0) == 0) {
                          localPath = localPath.substr(8);
                          std::replace(localPath.begin(), localPath.end(), '/', '\\');
                      }
                      auto storageFile = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::to_hstring(UrlDecode(localPath))).get();
                      auto thumbRef = winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(storageFile);
                      if (gen != metadataGeneration.load()) return;
                      std::lock_guard<std::mutex> lock(updaterMutex);
                      if (gen == metadataGeneration.load()) updater.Thumbnail(thumbRef);
                  }
                  std::lock_guard<std::mutex> lock(updaterMutex);
                  if (gen == metadataGeneration.load()) updater.Update();
              } catch (const winrt::hresult_error& e) {
                  std::cerr << "Failed to set thumbnail: " << winrt::to_string(e.message()) << std::endl;
              } })
                  .detach();
            }
            catch (...)
            {
              // If thumbnail fails, continue without it
              std::cerr << "Failed to set thumbnail in Notification: " << artUri << std::endl;
            }
          }
          updater.Update();
          // New track: reset the timeline to 0 with the new duration (0 = unknown).
          UpdateTimeline(GetInt(arguments, "durationMs", 0), 0);
        }
        result->Success();
      }
      else
      {
        result->Error("InvalidArguments", "title, artist, and album are required");
      }
    }

    // Update State (+ timeline + enabled controls)
    else if (method_call.method_name().compare("updateState") == 0)
    {
      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (arguments && arguments->find(flutter::EncodableValue("state")) != arguments->end())
      {
        int32_t state = static_cast<int32_t>(GetInt(arguments, "state", 1));
        if (!smtc && state != 2)
        {
          SetupSMTC();
        }
        if (smtc)
        {
          switch (state)
          {
          case 0: // Playing
            smtc.IsEnabled(true);
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);
            break;
          case 1: // Paused
            smtc.IsEnabled(true);
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Paused);
            break;
          case 3: // Loading / buffering
            smtc.IsEnabled(true);
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Changing);
            break;
          case 2: // Stopped
            metadataGeneration++;
            {
              std::lock_guard<std::mutex> lock(updaterMutex);
              smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Stopped);
              UpdateTimeline(0, 0);
              smtc.IsEnabled(false);
              updater.ClearAll();
              updater.Update();
            }
            result->Success();
            return;
          default:
            result->Error("InvalidArguments", "Invalid playback state");
            return;
          }

          // Enabled buttons reflect what the app actually supports.
          smtc.IsPlayEnabled(GetBool(arguments, "canPlay", true));
          smtc.IsPauseEnabled(GetBool(arguments, "canPause", true));
          smtc.IsNextEnabled(GetBool(arguments, "canNext", true));
          smtc.IsPreviousEnabled(GetBool(arguments, "canPrevious", true));
          smtc.IsStopEnabled(GetBool(arguments, "canStop", false));
          smtc.IsFastForwardEnabled(GetBool(arguments, "canFastForward", false));
          smtc.IsRewindEnabled(GetBool(arguments, "canRewind", false));

          double speed = GetDouble(arguments, "speed", 1.0);
          if (speed > 0.0) smtc.PlaybackRate(speed);

          if (arguments->find(flutter::EncodableValue("positionMs")) != arguments->end())
          {
            UpdateTimeline(GetInt(arguments, "durationMs", 0), GetInt(arguments, "positionMs", 0));
          }
        }
        result->Success();
      }
      else
      {
        result->Error("InvalidArguments", "state is required");
      }
    }

    else
    {
      result->NotImplemented();
    }
  }

} // namespace audio_service_win
