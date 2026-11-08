/**
 * @file src/platform/haiku/audio.cpp
 * @brief Haiku audio capture for streaming.
 *
 * System audio is captured with the Media Kit tap in audio_tap.cpp, which is inserted after the
 * system mixer only while a client streams. If the tap cannot be inserted (no media_server, an
 * unusual audio setup), the stream still runs and sends silence, and the log explains why.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// local includes
#include "audio_tap.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr auto HOST_SINK_NAME = "Haiku system mixer";

    /**
     * @brief Stream microphone fed by the system-audio tap.
     */
    class haiku_mic_t: public mic_t {
    public:
      haiku_mic_t(std::unique_ptr<haiku::audio_tap_t> tap, int channels, std::uint32_t sample_rate, std::uint32_t frame_size):
          tap {std::move(tap)},
          channels {channels},
          sample_rate {sample_rate},
          frame_size {frame_size},
          frame_duration {std::chrono::nanoseconds {std::chrono::seconds {1}} * frame_size / sample_rate},
          next_frame {std::chrono::steady_clock::now()},
          created {next_frame} {
      }

      capture_e sample(std::vector<float> &frame_buffer) override {
        const auto needed = static_cast<std::size_t>(frame_size) * channels;
        if (frame_buffer.size() < needed) {
          frame_buffer.resize(needed);
        }

        const auto now = std::chrono::steady_clock::now();

        if (!tap) {
          // The tap could not be inserted (for example while media services restart). Sunshine
          // only retries when asked, so ask again after a while; meanwhile stream silence.
          if (now - created >= TAP_RETRY_INTERVAL) {
            return capture_e::reinit;
          }
          return silence(frame_buffer, needed);
        }

        // While audio flows, wait long enough to bridge the gap between two mixer buffers,
        // which are usually longer than one stream packet. While silent (also at the start),
        // only check for audio without waiting, so the silence keeps the stream's pace.
        std::chrono::milliseconds timeout {0};
        if (!silent) {
          const auto buffer = tap->buffer_duration();
          timeout = std::max({20ms, std::chrono::duration_cast<std::chrono::milliseconds>(frame_duration * 3), buffer + buffer / 2});
        } else if (silence_frames > 0) {
          // Audio is back after a gap: the silence already sent took up stream time, so drop as
          // much of the waiting audio, keeping one packet to send now.
          const auto dropped = tap->discard(silence_frames, sample_rate, frame_size);
          if (dropped > 0) {
            silence_frames = 0;
          }
        }

        if (tap->read(frame_buffer.data(), frame_size, channels, sample_rate, timeout)) {
          silent = false;
          silence_frames = 0;
          next_frame = now + frame_duration;
          return capture_e::ok;
        }

        if (!tap->connected()) {
          // media_server tore the route down (e.g. media services were restarted); Sunshine
          // will ask for a new microphone, which inserts a fresh tap.
          BOOST_LOG(warning) << "System audio tap was disconnected"sv;
          return capture_e::reinit;
        }

        if (!silent) {
          silent = true;
          next_frame = std::chrono::steady_clock::now();
        }

        // Connected but no buffers for a long time: the route may be wedged. Insert the tap
        // again, but not over and over if the mixer is simply not producing.
        if (tap->idle_time() > STALL_REINIT_AFTER && now - last_stall_reinit() > STALL_REINIT_INTERVAL) {
          last_stall_reinit() = now;
          BOOST_LOG(warning) << "System mixer stopped delivering audio; reconnecting the audio tap"sv;
          return capture_e::reinit;
        }

        silence_frames = std::min<std::uint32_t>(silence_frames + frame_size, sample_rate);
        return silence(frame_buffer, needed);
      }

    private:
      /**
       * @brief Fill the packet with silence at the stream's pace.
       */
      capture_e silence(std::vector<float> &frame_buffer, std::size_t needed) {
        std::fill_n(frame_buffer.begin(), needed, 0.0F);
        std::this_thread::sleep_until(next_frame);
        next_frame = std::max(next_frame + frame_duration, std::chrono::steady_clock::now() - frame_duration);
        return capture_e::ok;
      }

      /** How long a microphone without a tap streams silence before asking to be recreated. */
      static constexpr auto TAP_RETRY_INTERVAL = 5s;

      /** Reconnect a tap that stayed connected but received nothing for this long. */
      static constexpr auto STALL_REINIT_AFTER = 3s;

      /** Minimum time between two such reconnects (they are audible on the host). */
      static constexpr auto STALL_REINIT_INTERVAL = 30s;

      /** Shared across microphones, since a reconnect creates a new one. */
      static std::chrono::steady_clock::time_point &last_stall_reinit() {
        static std::chrono::steady_clock::time_point last {};
        return last;
      }

      std::unique_ptr<haiku::audio_tap_t> tap;
      int channels;
      std::uint32_t sample_rate;
      std::uint32_t frame_size;
      std::chrono::nanoseconds frame_duration;
      std::chrono::steady_clock::time_point next_frame;
      std::chrono::steady_clock::time_point created;
      /** No audio arrived recently; produce paced silence. Streams start silent until the
          mixer's first buffer, so the stream keeps its pace from the first packet. */
      bool silent = true;
      std::uint32_t silence_frames = 0;  ///< Silence sent since audio last flowed, in stream frames.
    };

    /**
     * @brief Haiku audio control: one host sink (the system mixer), captured through the tap.
     */
    class haiku_audio_control_t: public audio_control_t {
    public:
      haiku_audio_control_t() {
        // A crashed earlier run may have left the mixer disconnected from the output.
        haiku::repair_audio_routing();
      }

      int set_sink(const std::string &sink) override {
        // Haiku has a single system mixer and no virtual sinks to switch to.
        return 0;
      }

      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, bool host_audio_enabled) override {
        // `mapping` is the Opus multistream mapping, applied by the encoder itself; capture
        // delivers channel i as canonical speaker i, like the other platforms.
        std::string reason;
        auto tap = haiku::audio_tap_t::insert(host_audio_enabled, reason);
        if (!tap) {
          BOOST_LOG(error) << "Couldn't capture system audio ("sv << reason << "); the stream will be silent"sv;
        }
        return std::make_unique<haiku_mic_t>(std::move(tap), channels, sample_rate, frame_size);
      }

      bool is_sink_available(const std::string &sink) override {
        return sink.empty() || sink == HOST_SINK_NAME;
      }

      std::optional<sink_t> sink_info() override {
        sink_t sink;
        sink.host = HOST_SINK_NAME;
        return sink;
      }
    };
  }  // namespace

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<haiku_audio_control_t>();
  }
}  // namespace platf
