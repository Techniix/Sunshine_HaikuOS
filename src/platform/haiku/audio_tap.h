/**
 * @file src/platform/haiku/audio_tap.h
 * @brief System audio capture on Haiku through a Media Kit pass-through node.
 *
 * Haiku has no loopback device. The system mixer has exactly one output, which media_server
 * connects to the sound card. To hear what the computer plays, Sunshine inserts a small filter
 * node into that connection while a stream is active:
 *
 *     system mixer  ->  Sunshine audio tap  ->  sound card (or whatever was connected before)
 *
 * The tap forwards every buffer unchanged (or silenced when host audio is disabled) and copies the
 * samples into a ring buffer that the stream reads from. When the stream ends, or Sunshine quits,
 * the original connection is restored. The node is created inside Sunshine's team, so if Sunshine
 * ever crashes media_server drops it, and the next start reconnects mixer and output.
 *
 * Only public Media Kit API is used (BMediaRoster, BBufferConsumer, BBufferProducer,
 * BMediaEventLooper), the same building blocks as Haiku's own filter add-ons.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace platf::haiku {

  struct tap_shared_t;
  class tap_node_t;

  /**
   * @brief Active audio tap; destroying it restores the original audio routing.
   */
  class audio_tap_t {
  public:
    /**
     * @brief Insert the tap after the system mixer.
     *
     * @param forward_to_output Whether the host keeps playing the audio.
     * @param error_message Receives a description when insertion fails.
     * @return Running tap, or null when audio cannot be captured.
     */
    static std::unique_ptr<audio_tap_t> insert(bool forward_to_output, std::string &error_message);

    ~audio_tap_t();

    audio_tap_t(const audio_tap_t &) = delete;
    audio_tap_t &operator=(const audio_tap_t &) = delete;

    /**
     * @brief Read audio converted to the stream format.
     *
     * Blocks until enough audio arrived or `timeout` passed.
     *
     * @param out Interleaved destination, `frames * channels` samples, canonical Sunshine speaker
     *        order (FL, FR, FC, LFE, BL, BR, SL, SR).
     * @param frames Number of frames to produce.
     * @param channels Number of output channels (1, 2, 6 or 8).
     * @param sample_rate Output sample rate in Hz.
     * @param timeout Maximum time to wait for audio.
     * @return `true` when `out` was filled with captured audio, `false` on timeout.
     */
    bool read(float *out, std::uint32_t frames, int channels, std::uint32_t sample_rate, std::chrono::milliseconds timeout);

    /**
     * @brief Drop captured audio that has not been read yet.
     *
     * Used after the stream had to be filled with silence, so the time that silence added does
     * not stay in the stream as permanent latency.
     *
     * @param frames Number of stream frames to drop at most.
     * @param sample_rate Stream sample rate in Hz.
     * @param keep Number of stream frames to leave readable.
     * @return Number of stream frames dropped.
     */
    std::uint32_t discard(std::uint32_t frames, std::uint32_t sample_rate, std::uint32_t keep);

    /**
     * @brief Duration of the longest buffer the mixer delivered so far.
     *
     * @return Zero until the first buffer arrived.
     */
    std::chrono::milliseconds buffer_duration() const;

    /**
     * @brief Time since the mixer last delivered audio (or since the tap was connected).
     */
    std::chrono::steady_clock::duration idle_time() const;

    /**
     * @brief Check whether the tap is still connected to the mixer.
     *
     * @return `false` once media_server tore the connection down (e.g. media services restarted).
     */
    bool connected() const;

  private:
    audio_tap_t() = default;

    std::shared_ptr<tap_shared_t> shared;
    tap_node_t *node = nullptr;  ///< Owned through the Media Kit reference count.

    struct route_t;
    std::unique_ptr<route_t> route;  ///< Saved original connection for restoring it.

    double read_position = 0.0;  ///< Fractional read position in source frames, for resampling.
  };

  /**
   * @brief Reconnect the system mixer to the audio output if a previous run left them apart.
   *
   * Called at startup. Does nothing when the mixer output is connected.
   */
  void repair_audio_routing();

}  // namespace platf::haiku
