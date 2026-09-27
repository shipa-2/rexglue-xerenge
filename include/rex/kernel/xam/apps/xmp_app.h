#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::audio {
class AudioDriver;
}

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

enum class XmpClient : uint32_t {
  kDash = 0,
  kHud = 1,
  kGame = 2,
  kRemote = 3,
  kMusicPlayer = 4,
  kMsal = 5,
  kMce = 6,
};

enum class PlaybackController : uint32_t {
  kGame = 0,
  kUser = 1,
  kDash = 2,
  kMce = 3,
  kRestore = 4,
};

// Only source of docs for a lot of these functions:
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Scenes/Media/Music/ScnMusic.cpp

class XmpApp : public system::xam::App {
 public:
  enum class State : uint32_t {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  enum class PlaybackMode : uint32_t {
    kInOrder = 0,
    kShuffle = 1,
  };
  enum class RepeatMode : uint32_t {
    kPlaylist = 0,
    kNoRepeat = 1,
  };
  struct Song {
    enum class Format : uint32_t {
      kWma = 0,
      kMp3 = 1,
    };

    uint32_t handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    uint32_t track_number;
    uint32_t duration_ms;
    Format format;
  };
  struct Playlist {
    uint32_t handle;
    std::u16string name;
    uint32_t flags;
    std::vector<std::unique_ptr<Song>> songs;
  };

  explicit XmpApp(system::KernelState* kernel_state);

  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  X_HRESULT XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr, const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle);
  X_HRESULT XMPContinue();
  X_HRESULT XMPStop(uint32_t unk);
  X_HRESULT XMPPause();
  X_HRESULT XMPNext();
  X_HRESULT XMPPrevious();

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  static const uint32_t kMsgStateChanged = 0x0A000001;
  static const uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

  void OnStateChanged();

  // Decodes and plays one song on the worker thread below, blocking until it
  // ends, fails, or is superseded by a different (playlist, song index) pair -
  // a track change or a stop, both visible as active_playlist_/
  // active_song_index_ no longer matching what was passed in. Returns false
  // only on an actual decode/IO failure, not on being superseded.
  bool PlayFile(const std::string& utf8_path, Playlist* playlist, int song_index);
  // Waits for something to play, plays the active playlist starting at
  // active_song_index_, and on a song ending on its own (PlayFile returned
  // true and nothing else changed underneath it) advances per repeat_mode_.
  void WorkerThreadMain();
  bool IsLastSongInPlaylist(const Playlist* playlist, int song_index) const;
  void AdvanceAfterSongFinished(Playlist* playlist, int finished_index);
  // Starts the worker thread on first real use rather than at construction -
  // see the constructor for why.
  void EnsureWorkerStarted();
  bool IsTitleInPlaybackControl() const;
  void OnPlaybackControlChanged();
  void DiscardActiveDriverFrames();
  void SetActiveDriver(rex::audio::AudioDriver* driver);

  State state_;
  // The state last announced to the title (kMsgStateChanged); only a change
  // from it is broadcast.
  std::atomic<uint32_t> last_announced_state_{0xFFFFFFFFu};
  XmpClient xmp_client_;
  PlaybackController playback_controller_;
  bool xmp_override_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  uint32_t unknown_flags_;
  float volume_;
  Playlist* active_playlist_;
  int active_song_index_;
  uint32_t active_song_handle_ = 0;
  // What PlayFile is decoding right now. Compared on XMPPlayTitlePlaylist so a
  // title that recreates its playlist handle every frame while previewing a
  // track does not restart decode when the underlying file did not change.
  std::string active_file_path_;

  rex::thread::global_critical_region global_critical_region_;
  std::unordered_map<uint32_t, Playlist*> playlists_;
  // The title's handle is the address of the storage it gave XMPCreateTitlePlaylist;
  // our handle is only written into that storage, and the title's memory there
  // does not stay intact (Burnout's race start left 0x164 in it). Look up by address.
  std::unordered_map<uint32_t, uint32_t> storage_handles_;
  uint32_t PlaylistHandleFromStorage(uint32_t storage_ptr);
  uint32_t next_playlist_handle_;
  uint32_t next_song_handle_;

  // The decode thread and its own idle/pause wait.
  std::atomic<bool> worker_running_ = {false};
  std::unique_ptr<rex::thread::Thread> worker_thread_;
  rex::thread::Fence resume_fence_;

  // The AudioDriver PlayFile currently owns, if any - set only so
  // DiscardActiveDriverFrames (called from whichever thread dispatches
  // XMPSetPlaybackController) can drop queued frames when the title loses
  // playback control, without reaching across threads into PlayFile's own
  // locals.
  std::mutex active_driver_mutex_;
  rex::audio::AudioDriver* active_driver_ = nullptr;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
