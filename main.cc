/**
 * @file    main.cc
 * @authors Stavros Avramidis
 */


/* C++ libs */
#include <cctype>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <stack>
/* C libs */
#include <cstdio>
/* Qt */
#include <QApplication>
#include <QPushButton>
#include <QAction>
#include <QMenu>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QCoreApplication>
/* local libs*/
#include "discord_rpc.h"
#include "discord_register.h"
#include "httplib.hh"
#include "json.hh"

#ifdef WIN32
#include <Windows.h>
#pragma comment(lib, "discord-rpc.lib")
#include "windows_api_hook.hh"

#elif defined(__APPLE__) or defined(__MACH__)
#include <Carbon/Carbon.h>
#include "osx_api_hook.hh"

#elif defined(__linux__)
#else
#   error "unkown target os"
#endif

#define CURRENT_TIME std::time(nullptr)
#define HIFI_ASSET "hifi"

static std::string lastfmUsername;
static const char* APPLICATION_ID = "584458858731405315";
volatile bool isPresenceActive = true;

struct Song {
  enum AudioQualityEnum { master, hifi, normal };

  std::string title;
  std::string artist;
  std::string album;
  std::string url;
  int64_t starttime;
  int64_t runtime;
  uint64_t pausedtime;
  uint_fast8_t trackNumber;
  uint_fast8_t volumeNumber;
  bool isPaused = false;
  AudioQualityEnum quality;

  void setQuality(const std::string& q) {
    if (q=="HI_RES") {
      quality = master;
    } else {
      quality = hifi;
    }
  }

  inline bool isHighRes() const noexcept {
    return quality==master;
  }

  friend std::ostream& operator<<(std::ostream& out, const Song& song) {
    out << song.title << " of " << song.album << " from " << song.artist << "(" << song.runtime << ")";
    return out;
  }
};

std::string urlEncode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (std::string::value_type c: value) {
    if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~')
      escaped << c;
    else {
      escaped << std::uppercase;
      escaped << '%' << std::setw(2) << int((unsigned char) c);
      escaped << std::nouppercase;
    }
  }
  return escaped.str();
}

static void updateDiscordPresence(const Song& song) {
  if (isPresenceActive) {
    DiscordRichPresence discordPresence;
    memset(&discordPresence, 0, sizeof(discordPresence));
    discordPresence.state = song.title.c_str();
    discordPresence.details = song.artist.c_str();
    if (song.isPaused) {
      discordPresence.smallImageKey = "pause";
      discordPresence.smallImageText = "Paused";
    } else {
      if (song.runtime)discordPresence.endTimestamp = song.starttime + song.runtime + song.pausedtime;
      else discordPresence.startTimestamp = song.starttime;
    }
    discordPresence.largeImageKey = song.isHighRes() ? "test" : HIFI_ASSET;
    discordPresence.largeImageText = song.isHighRes() ? "Playing High-Res Audio" : "";
    discordPresence.instance = 0;

    Discord_UpdatePresence(&discordPresence);
  } else {
    Discord_ClearPresence();
  }
}

static void handleDiscordReady(const DiscordUser* connectedUser) {
  printf("\nDiscord: connected to user %s#%s - %s\n",
         connectedUser->username,
         connectedUser->discriminator,
         connectedUser->userId);
}

static void handleDiscordDisconnected(int errcode, const char* message) {
  printf("\nDiscord: disconnected (%d: %s)\n", errcode, message);
}

static void handleDiscordError(int errcode, const char* message) {
  printf("\nDiscord: error (%d: %s)\n", errcode, message);
}

static void discordInit() {
  DiscordEventHandlers handlers;
  memset(&handlers, 0, sizeof(handlers));
  handlers.ready = handleDiscordReady;
  handlers.disconnected = handleDiscordDisconnected;
  handlers.errored = handleDiscordError;
  Discord_Initialize(APPLICATION_ID, &handlers, 1, NULL);
}

inline void rpcLoop() {

  using json = nlohmann::json;
  using string = std::string;
  httplib::Client cli("api.tidal.com");

  char getSongInfoBuf[1024];
  json j;

  Song curSong;

  for (;;) {
    std::wstring tmpTrack, tmpArtist;

    auto localStatus = tidalInfo(tmpTrack, tmpArtist);

    // If song is playing
    if (localStatus==playing) {
      // if new song is playing
      if (safeWstringToString(tmpTrack)!=curSong.title || safeWstringToString(tmpArtist)!=curSong.artist) {
        // assign new info to current track
        curSong.title = safeWstringToString(tmpTrack);
        curSong.artist = safeWstringToString(tmpArtist);

        curSong.runtime = 0;
        curSong.pausedtime = 0;
        curSong.setQuality("");

        // get info form TIDAL api
        auto search_param =
            std::string(curSong.title + " - " + curSong.artist.substr(0, curSong.artist.find('&')));
        sprintf(getSongInfoBuf,
                "/v1/search?query=%s&limit=50&offset=0&types=TRACKS&countryCode=GR?token=wdgaB1CilGA-S_s2",
                urlEncode(search_param).c_str());

        auto res = cli.Get(getSongInfoBuf);

        if (res && res->status==200) {
          try {
            j = json::parse(res->body);
            for (auto i = 0u; i < j["tracks"]["totalNumberOfItems"].get<unsigned>(); i++) {
              // convert title from windows and from tidal api to strings, json lib doesn't support wide string
              // so wstrings are pared as strings and have the same convention errors
              auto fetched_str = j["tracks"]["items"][i]["title"].get<std::string>();
              auto c_str = rawWstringToString(tmpTrack);

              if (fetched_str==c_str) {
                curSong.setQuality(j["tracks"]["items"][i]["audioQuality"].get<std::string>());
                curSong.trackNumber = j["tracks"]["items"][i]["trackNumber"].get<uint_fast8_t>();
                curSong.volumeNumber = j["tracks"]["items"][i]["volumeNumber"].get<uint_fast8_t>();
                curSong.runtime = j["tracks"]["items"][i]["duration"].get<int64_t>();
                break;
              }
            }
          } catch (...) {
            std::cerr << "Error getting info from api: " << curSong << "\n";
          }
        }

        #ifdef DEBUG
        std::cout << curSong.title << "\tFrom: " << curSong.artist << std::endl;
        #endif

        // get time just before passing it to RPC handlers
        curSong.starttime = 2 + CURRENT_TIME;  // add 2 seconds to be more accurate.
        updateDiscordPresence(curSong);
      } else {
        if (curSong.isPaused) {
          curSong.isPaused = false;
          updateDiscordPresence(curSong);
        }
      }

    } else if (localStatus==opened) {
      if ((CURRENT_TIME - (curSong.starttime + curSong.runtime + curSong.pausedtime) > 5)) {
        curSong.pausedtime += 1;
        curSong.isPaused = true;
      }
      updateDiscordPresence(curSong);
    }

    _continue:
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  }
}

int main(int argc, char** argv) {
  using json = nlohmann::json;
  using string = std::string;

  try {
    std::ifstream i("settings.json");
    json j;
    i >> j;
    lastfmUsername = j["last_fm_username"].get<string>();
  } catch (...) {
    std::cerr << "Couldn't read last.fm username\n";
  }

  QApplication app(argc, argv);
  app.setWindowIcon(QIcon("icon.ico"));

  QSystemTrayIcon tray(QIcon("icon.ico"), &app);

  QAction titleAction(QIcon("icon.ico"), "TIDAL - Discord RPC ", nullptr);

  QAction changePresenceStatusAction("Running", nullptr);
  changePresenceStatusAction.setCheckable(true);
  changePresenceStatusAction.setChecked(true);
  QObject::connect(&changePresenceStatusAction,
                   &QAction::triggered,
                   [&changePresenceStatusAction]() {
                     isPresenceActive = !isPresenceActive;
                     changePresenceStatusAction.setText(isPresenceActive ? "Running"
                                                                         : "Disabled (click to re-enable)");
                   }
  );

  QAction quitAction("Exit", nullptr);
  QObject::connect(&quitAction, &QAction::triggered, [&app]() {
    Discord_ClearPresence();
    app.quit();
  });

  QMenu trayMenu("TIDAL - RPC", nullptr);

  trayMenu.addAction(&titleAction);
  trayMenu.addAction(&changePresenceStatusAction);
  trayMenu.addAction(&quitAction);

  tray.setContextMenu(&trayMenu);

  tray.show();

  std::thread t1(rpcLoop);
  t1.detach();

  discordInit();

  return app.exec();

}