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

static winrt::Windows::Media::Playback::MediaPlayer mediaPlayer{nullptr};
static winrt::Windows::Media::SystemMediaTransportControls smtc{nullptr};
static winrt::Windows::Media::SystemMediaTransportControlsDisplayUpdater updater{nullptr};
static std::unique_ptr<flutter::MethodChannel<>> channel;

namespace audio_service_win
{

  // static
  void AudioServiceWinPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar)
  {
    channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "audio_service_win",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<AudioServiceWinPlugin>();

    channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto &call, auto result)
        {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
  }

  AudioServiceWinPlugin::AudioServiceWinPlugin() {}

  AudioServiceWinPlugin::~AudioServiceWinPlugin() {}

  void AudioServiceWinPlugin::HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {

    // Initialize System Media Transport Controls (SMTC)
    if (method_call.method_name().compare("initializeSMTC") == 0)
    {
      std::cout << "Starting SMTC..." << std::endl;

      // Extract the appid argument from the method call
      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (arguments && arguments->find(flutter::EncodableValue("appid")) != arguments->end())
      {
        auto appid = std::get<std::string>(arguments->at(flutter::EncodableValue("appid")));

        if (mediaPlayer == nullptr)
        {
          mediaPlayer = winrt::Windows::Media::Playback::MediaPlayer();
          smtc = mediaPlayer.SystemMediaTransportControls();
          updater = smtc.DisplayUpdater();
          updater.AppMediaId(winrt::to_hstring(appid));
          updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);

          smtc.IsEnabled(false);
          smtc.IsPlayEnabled(true);
          smtc.IsPauseEnabled(true);
          smtc.IsNextEnabled(true);
          smtc.IsPreviousEnabled(true);
          updater.Update();
          std::cout << "SMTC initialized with appid: " << appid << std::endl;

          smtc.ButtonPressed([](auto const &, winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const &args)
                               {
                        std::string* method = new std::string;

                        switch (args.Button()) {
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Play:
                            *method = "play";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Pause:
                            *method = "pause";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Next:
                            *method = "next";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Previous:
                            *method = "previous";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Stop:
                            *method = "stop";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::FastForward:
                            *method = "fastForward";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Rewind:
                            *method = "rewind";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::Record:
                            *method = "record";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::ChannelUp:
                            *method = "channelUp";
                            break;
                        case winrt::Windows::Media::SystemMediaTransportControlsButton::ChannelDown:
                            *method = "channelDown";
                            break;
                        default:
                            *method = "other";
                            break;
                        }

                      
                          channel->InvokeMethod(
                            "onSMTCButtonPressed",
                            std::make_unique<flutter::EncodableValue>(*method)
                          );
                          delete method; // Clean up the dynamically allocated string
                        });
        }
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
        auto title = std::get<std::string>(arguments->at(flutter::EncodableValue("title")));
        auto artist = std::get<std::string>(arguments->at(flutter::EncodableValue("artist")));
        auto album = std::get<std::string>(arguments->at(flutter::EncodableValue("album")));

        std::string artUri;
        if (arguments->find(flutter::EncodableValue("artUri")) != arguments->end())
        {
          const auto& artUriValue = arguments->at(flutter::EncodableValue("artUri"));
          if (!artUriValue.IsNull() && std::holds_alternative<std::string>(artUriValue))
          {
            artUri = std::get<std::string>(artUriValue);
          }
        }

        if (updater)
        {
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
              winrt::Windows::Foundation::Uri uri(winrt::to_hstring(artUri));
              auto thumbRef = winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(uri);
              updater.Thumbnail(thumbRef);
            }
            catch (...)
            {
              // If thumbnail fails, continue without it
              std::cerr << "Failed to set thumbnail in Notification: " << artUri << std::endl;
            }
          }
          updater.Update();
        }
        result->Success();
      }
      else
      {
        result->Error("InvalidArguments", "title, artist, and album are required");
      }
    }


    // Update State
    else if (method_call.method_name().compare("updateState") == 0)
    {
      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (arguments && arguments->find(flutter::EncodableValue("state")) != arguments->end())
      {
        int32_t state = std::get<int32_t>(arguments->at(flutter::EncodableValue("state")));
        if (smtc)
        {
          smtc.IsEnabled(true);
          switch (state)
          {
          case 0: // Playing
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);
            break;
          case 1: // Paused
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Paused);
            break;
          case 2: // Stopped
            smtc.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Stopped);
            smtc.IsEnabled(false);
            updater.ClearAll();
            updater.Update();
            break;
          default:
            result->Error("InvalidArguments", "Invalid playback state");
            return;
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
