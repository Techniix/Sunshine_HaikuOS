/**
 * @file src/platform/haiku/audio_tap.cpp
 * @brief System audio capture on Haiku through a Media Kit pass-through node.
 *
 * The node follows the structure of Haiku's own filter nodes (for example the Equalizer media
 * add-on, src/add-ons/media/media-add-ons/equalizer, MIT licensed): buffers from the producer are
 * handed on to the consumer from BufferReceived(), the upstream buffer group is reused, and
 * latency changes are propagated in both directions.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

// platform includes
#include <Buffer.h>
#include <BufferConsumer.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>
#include <MediaRoster.h>
#include <TimeSource.h>

// local includes
#include "audio_tap.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::haiku {
  namespace {
    constexpr int32 TAP_INPUT_ID = 1;
    constexpr int32 TAP_OUTPUT_ID = 2;

    /** Ring buffer length in seconds; plenty to absorb scheduling jitter. */
    constexpr double RING_SECONDS = 1.0;

    /** Canonical Sunshine speaker slots. */
    enum speaker_e : int {
      FL,
      FR,
      FC,
      LFE,
      BL,
      BR,
      SL,
      SR,
      SPEAKER_COUNT
    };

    /**
     * @brief Map Haiku channel-mask bits to Sunshine speaker slots (-1: mix into front pair).
     */
    int speaker_for_mask_bit(uint32 bit) {
      switch (bit) {
        case B_CHANNEL_LEFT:
          return FL;
        case B_CHANNEL_RIGHT:
          return FR;
        case B_CHANNEL_CENTER:
          return FC;
        case B_CHANNEL_SUB:
          return LFE;
        case B_CHANNEL_REARLEFT:
          return BL;
        case B_CHANNEL_REARRIGHT:
          return BR;
        case B_CHANNEL_SIDE_LEFT:
          return SL;
        case B_CHANNEL_SIDE_RIGHT:
          return SR;
        default:
          return -1;
      }
    }

    /**
     * @brief Describe where each source channel goes in Sunshine's speaker layout.
     */
    std::vector<int> source_layout(uint32 channels, uint32 mask) {
      std::vector<int> layout(channels, -1);

      if (mask != 0) {
        uint32 channel = 0;
        for (uint32 bit = 1; bit != 0 && channel < channels; bit <<= 1) {
          if ((mask & bit) != 0) {
            layout[channel++] = speaker_for_mask_bit(bit);
          }
        }
        if (channel == channels) {
          return layout;
        }
        // Mask and channel count disagree; fall through to the conventional order.
      }

      // Conventional orders used by Haiku's mixer and drivers when no mask is given.
      static constexpr std::array<int, 8> conventional {FL, FR, FC, LFE, BL, BR, SL, SR};
      if (channels == 1) {
        layout[0] = FC;  // mono goes to both front speakers below
        return layout;
      }
      for (uint32 i = 0; i < channels && i < conventional.size(); i++) {
        layout[i] = conventional[i];
      }
      return layout;
    }

    /**
     * @brief Convert one raw sample to float.
     */
    inline float sample_to_float(const uint8 *data, uint32 format) {
      switch (format) {
        case media_raw_audio_format::B_AUDIO_FLOAT:
          {
            float value;
            std::memcpy(&value, data, sizeof(value));
            return value;
          }
        case media_raw_audio_format::B_AUDIO_DOUBLE:
          {
            double value;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<float>(value);
          }
        case media_raw_audio_format::B_AUDIO_INT:
          {
            int32 value;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<float>(value / 2147483648.0);
          }
        case media_raw_audio_format::B_AUDIO_SHORT:
          {
            int16 value;
            std::memcpy(&value, data, sizeof(value));
            return value / 32768.0F;
          }
        case media_raw_audio_format::B_AUDIO_UCHAR:
          return (static_cast<int>(*data) - 128) / 128.0F;
        case media_raw_audio_format::B_AUDIO_CHAR:
          return static_cast<int8>(*data) / 128.0F;
        default:
          return 0.0F;
      }
    }
  }  // namespace

  /**
   * @brief State shared between the media node (producer side) and the stream (consumer side).
   *
   * Audio is stored as interleaved float in the mixer's own rate and channel layout; conversion
   * to the stream format happens on read.
   */
  struct tap_shared_t {
    std::mutex mutex;
    std::condition_variable cv;

    std::vector<float> ring;  ///< Interleaved samples.
    std::size_t write_frame = 0;  ///< Total frames written (monotonic).
    std::size_t read_frame = 0;  ///< Total frames consumed (monotonic).
    std::size_t max_chunk_frames = 0;  ///< Largest buffer the mixer delivered, in frames.
    std::chrono::steady_clock::time_point last_push {};  ///< When the last buffer arrived.

    float frame_rate = 0.0F;
    uint32 channels = 0;
    uint32 sample_format = 0;
    uint32 channel_mask = 0;
    std::vector<int> layout;  ///< Source channel -> speaker slot.

    std::atomic_bool forward {true};  ///< Pass audio on to the host's speakers.
    std::atomic_bool connected {false};  ///< Producer currently connected.

    /**
     * @brief Configure the ring for the negotiated mixer format.
     */
    void configure(const media_raw_audio_format &format, uint32 mask) {
      std::lock_guard lock {mutex};
      frame_rate = format.frame_rate;
      channels = format.channel_count;
      sample_format = format.format;
      channel_mask = mask;
      layout = source_layout(channels, mask);
      const auto frames = static_cast<std::size_t>(std::max(1.0, frame_rate * RING_SECONDS));
      ring.assign(frames * std::max<uint32>(channels, 1), 0.0F);
      write_frame = 0;
      read_frame = 0;
      max_chunk_frames = 0;
      last_push = std::chrono::steady_clock::now();
    }

    /**
     * @brief Append raw audio from a media buffer.
     */
    void push(const void *data, std::size_t size) {
      std::lock_guard lock {mutex};
      if (channels == 0 || ring.empty()) {
        return;
      }

      const uint32 sample_size = sample_format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
      if (sample_size == 0) {
        return;
      }
      const std::size_t frame_size = static_cast<std::size_t>(sample_size) * channels;
      const std::size_t frames = size / frame_size;
      const std::size_t capacity = ring.size() / channels;

      const auto *bytes = static_cast<const uint8 *>(data);
      for (std::size_t f = 0; f < frames; f++) {
        float *slot = &ring[((write_frame + f) % capacity) * channels];
        for (uint32 c = 0; c < channels; c++) {
          slot[c] = sample_to_float(bytes + f * frame_size + c * sample_size, sample_format);
        }
      }
      write_frame += frames;
      max_chunk_frames = std::max(max_chunk_frames, frames);
      last_push = std::chrono::steady_clock::now();

      // If the reader fell more than the ring behind, drop the oldest audio.
      if (write_frame - read_frame > capacity) {
        read_frame = write_frame - capacity;
      }

      cv.notify_all();
    }
  };

  /**
   * @brief Filter node placed between the system mixer and its downstream consumer.
   */
  class tap_node_t: public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
  public:
    explicit tap_node_t(std::shared_ptr<tap_shared_t> shared):
        BMediaNode("Sunshine audio tap"),
        BBufferConsumer(B_MEDIA_RAW_AUDIO),
        BBufferProducer(B_MEDIA_RAW_AUDIO),
        BMediaEventLooper(),
        shared_ {std::move(shared)} {
    }

    ~tap_node_t() override {
      Quit();
    }

    // BMediaNode
    BMediaAddOn *AddOn(int32 *id) const override {
      return nullptr;  // application-owned node
    }

    status_t HandleMessage(int32 message, const void *data, size_t size) override {
      if (BBufferConsumer::HandleMessage(message, data, size) == B_OK || BBufferProducer::HandleMessage(message, data, size) == B_OK || BMediaNode::HandleMessage(message, data, size) == B_OK) {
        return B_OK;
      }
      HandleBadMessage(message, data, size);
      return B_ERROR;
    }

  protected:
    void NodeRegistered() override {
      format_.type = B_MEDIA_RAW_AUDIO;
      format_.u.raw_audio = media_raw_audio_format::wildcard;

      input_.destination.port = ControlPort();
      input_.destination.id = TAP_INPUT_ID;
      input_.node = Node();
      input_.source = media_source::null;
      input_.format = format_;
      strncpy(input_.name, "Audio Input", B_MEDIA_NAME_LENGTH);

      output_.source.port = ControlPort();
      output_.source.id = TAP_OUTPUT_ID;
      output_.node = Node();
      output_.destination = media_destination::null;
      output_.format = format_;
      strncpy(output_.name, "Audio Output", B_MEDIA_NAME_LENGTH);

      SetPriority(B_REAL_TIME_PRIORITY);
      Run();
    }

    // BBufferConsumer
    void BufferReceived(BBuffer *buffer) override {
      if (buffer->Header()->destination != input_.destination.id) {
        buffer->Recycle();
        return;
      }

      shared_->push(buffer->Data(), buffer->Header()->size_used);

      if (output_.destination == media_destination::null || !output_enabled_) {
        buffer->Recycle();
        return;
      }

      if (!shared_->forward) {
        // Host audio is disabled for this stream: keep the device fed with silence so its
        // timing (and the system time source) is undisturbed.
        std::memset(buffer->Data(), format_.u.raw_audio.format == media_raw_audio_format::B_AUDIO_UCHAR ? 0x80 : 0, buffer->Header()->size_used);
      }

      if (SendBuffer(buffer, output_.source, output_.destination) != B_OK) {
        buffer->Recycle();
      }
    }

    status_t AcceptFormat(const media_destination &destination, media_format *format) override {
      if (destination != input_.destination) {
        return B_MEDIA_BAD_DESTINATION;
      }
      if (format->type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
      }
      // Pass-through: whatever the mixer produced for the sound card is fine.
      return B_OK;
    }

    status_t GetNextInput(int32 *cookie, media_input *input) override {
      if (*cookie != 0) {
        return B_BAD_INDEX;
      }
      ++*cookie;
      *input = input_;
      return B_OK;
    }

    void DisposeInputCookie(int32 cookie) override {
    }

    status_t FormatChanged(const media_source &source, const media_destination &destination, int32 changeTag, const media_format &format) override {
      return B_MEDIA_BAD_FORMAT;
    }

    void ProducerDataStatus(const media_destination &destination, int32 status, bigtime_t when) override {
      if (output_.destination != media_destination::null) {
        SendDataStatus(status, output_.destination, when);
      }
    }

    status_t GetLatencyFor(const media_destination &destination, bigtime_t *latency, media_node_id *timeSource) override {
      if (destination != input_.destination) {
        return B_MEDIA_BAD_DESTINATION;
      }
      *latency = downstream_latency_;
      *timeSource = TimeSource()->ID();
      return B_OK;
    }

    status_t Connected(const media_source &source, const media_destination &destination, const media_format &format, media_input *input) override {
      if (destination != input_.destination) {
        return B_MEDIA_BAD_DESTINATION;
      }
      if (input_.source != media_source::null) {
        return B_MEDIA_ALREADY_CONNECTED;
      }

      input_.source = source;
      input_.format = format;
      format_ = format;
      *input = input_;

      shared_->configure(format.u.raw_audio, format.u.raw_audio.channel_mask);
      shared_->connected = true;
      return B_OK;
    }

    void Disconnected(const media_source &source, const media_destination &destination) override {
      if (input_.source != source || destination != input_.destination) {
        return;
      }

      input_.source = media_source::null;
      shared_->connected = false;
      shared_->cv.notify_all();
      if (output_.destination == media_destination::null) {
        format_.u.raw_audio = media_raw_audio_format::wildcard;
      }
      input_.format = format_;
    }

    // BBufferProducer
    status_t FormatSuggestionRequested(media_type type, int32 quality, media_format *format) override {
      if (type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
      }
      *format = format_;
      return B_OK;
    }

    status_t FormatProposal(const media_source &source, media_format *format) override {
      if (source != output_.source) {
        return B_MEDIA_BAD_SOURCE;
      }
      if (format->type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
      }
      // The output always carries exactly what the input receives.
      if (input_.source != media_source::null) {
        *format = input_.format;
      }
      return B_OK;
    }

    status_t FormatChangeRequested(const media_source &source, const media_destination &destination, media_format *format, int32 *_deprecated_) override {
      return B_MEDIA_BAD_FORMAT;
    }

    void LateNoticeReceived(const media_source &source, bigtime_t late, bigtime_t when) override {
      if (source != output_.source || input_.source == media_source::null) {
        return;
      }
      NotifyLateProducer(input_.source, late, when);
    }

    status_t GetNextOutput(int32 *cookie, media_output *output) override {
      if (*cookie != 0) {
        return B_BAD_INDEX;
      }
      ++*cookie;
      *output = output_;
      return B_OK;
    }

    status_t DisposeOutputCookie(int32 cookie) override {
      return B_OK;
    }

    status_t SetBufferGroup(const media_source &source, BBufferGroup *group) override {
      if (source != output_.source) {
        return B_MEDIA_BAD_SOURCE;
      }
      if (input_.source == media_source::null) {
        return B_ERROR;
      }
      // Buffers flow through unchanged, so the downstream group is really the mixer's business.
      int32 changeTag;
      return SetOutputBuffersFor(input_.source, input_.destination, group, nullptr, &changeTag);
    }

    status_t PrepareToConnect(const media_source &source, const media_destination &destination, media_format *format, media_source *outSource, char *outName) override {
      if (source != output_.source) {
        return B_MEDIA_BAD_SOURCE;
      }
      if (format->type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
      }
      if (output_.destination != media_destination::null) {
        return B_MEDIA_ALREADY_CONNECTED;
      }
      if (input_.source == media_source::null) {
        // The tap only forwards what it receives; connect the input first.
        return B_MEDIA_NOT_CONNECTED;
      }

      *format = input_.format;  // fully specialized
      output_.destination = destination;
      output_.format = *format;
      *outSource = output_.source;
      strncpy(outName, output_.name, B_MEDIA_NAME_LENGTH);
      return B_OK;
    }

    void Connect(status_t status, const media_source &source, const media_destination &destination, const media_format &format, char *name) override {
      if (status < B_OK) {
        output_.destination = media_destination::null;
        return;
      }

      strncpy(name, output_.name, B_MEDIA_NAME_LENGTH);
      output_.destination = destination;
      output_.format = format;

      media_node_id timeSource;
      FindLatencyFor(output_.destination, &downstream_latency_, &timeSource);
      SetEventLatency(downstream_latency_);

      if (input_.source != media_source::null) {
        SendLatencyChange(input_.source, input_.destination, EventLatency() + SchedulingLatency());
      }

      const auto &raw = format.u.raw_audio;
      const uint32 frame_size = (raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK) * raw.channel_count;
      if (raw.buffer_size > 0 && raw.frame_rate > 0 && frame_size > 0) {
        SetBufferDuration(static_cast<bigtime_t>((raw.buffer_size / frame_size) / raw.frame_rate * 1000000.0));
      }
    }

    void Disconnect(const media_source &source, const media_destination &destination) override {
      if (source != output_.source || destination != output_.destination) {
        return;
      }
      output_.destination = media_destination::null;
      if (input_.source == media_source::null) {
        format_.u.raw_audio = media_raw_audio_format::wildcard;
      }
      output_.format = format_;
    }

    void EnableOutput(const media_source &source, bool enabled, int32 *_deprecated_) override {
      if (source == output_.source) {
        output_enabled_ = enabled;
      }
    }

    status_t GetLatency(bigtime_t *latency) override {
      *latency = EventLatency() + SchedulingLatency();
      return B_OK;
    }

    void LatencyChanged(const media_source &source, const media_destination &destination, bigtime_t latency, uint32 flags) override {
      if (source != output_.source || destination != output_.destination) {
        return;
      }
      downstream_latency_ = latency;
      SetEventLatency(downstream_latency_);
      if (input_.source != media_source::null) {
        SendLatencyChange(input_.source, input_.destination, EventLatency() + SchedulingLatency());
      }
    }

    // BMediaEventLooper
    void HandleEvent(const media_timed_event *event, bigtime_t lateness, bool realTimeEvent) override {
      // Buffers are forwarded directly from BufferReceived(); there are no timed events.
    }

    bigtime_t OfflineTime() override {
      return 0;
    }

  private:
    std::shared_ptr<tap_shared_t> shared_;
    media_format format_;
    media_input input_;
    media_output output_;
    bigtime_t downstream_latency_ = 0;
    bool output_enabled_ = true;
  };

  /**
   * @brief The connection the tap was inserted into.
   */
  struct audio_tap_t::route_t {
    media_node mixer;  ///< System mixer (producer).
    media_node downstream;  ///< Node that consumed the mixer output (normally the sound card).
    media_destination downstream_input;  ///< Input of `downstream` that the mixer fed.
    media_format format;  ///< Format of the original connection.
  };

  namespace {
    /**
     * @brief Find the node and input that consume `source`.
     */
    bool find_consumer_of(BMediaRoster *roster, const media_source &source, media_node &node, media_input &input) {
      // The default audio output is by far the most common consumer; check it first.
      media_node output;
      if (roster->GetAudioOutput(&output) == B_OK) {
        media_input inputs[16];
        int32 count = 0;
        if (roster->GetConnectedInputsFor(output, inputs, 16, &count) == B_OK) {
          for (int32 i = 0; i < count; i++) {
            if (inputs[i].source == source) {
              node = output;
              input = inputs[i];
              return true;
            }
          }
        }
        roster->ReleaseNode(output);
      }

      // Something else sits behind the mixer (a user-inserted filter, for example).
      live_node_info infos[64];
      int32 count = 64;
      if (roster->GetLiveNodes(infos, &count) != B_OK) {
        return false;
      }
      for (int32 n = 0; n < count; n++) {
        if ((infos[n].node.kind & B_BUFFER_CONSUMER) == 0) {
          continue;
        }
        media_input inputs[16];
        int32 found = 0;
        if (roster->GetConnectedInputsFor(infos[n].node, inputs, 16, &found) != B_OK) {
          continue;
        }
        for (int32 i = 0; i < found; i++) {
          if (inputs[i].source == source) {
            // Take a reference so the caller's ReleaseNode() stays balanced.
            if (roster->GetNodeFor(infos[n].node.node, &node) != B_OK) {
              return false;
            }
            input = inputs[i];
            return true;
          }
        }
      }
      return false;
    }

    /**
     * @brief Connect the mixer's (free) output to `destination` with `format`.
     */
    status_t connect_mixer(BMediaRoster *roster, const media_node &mixer, const media_destination &destination, media_format format) {
      media_output outputs[4];
      int32 count = 0;
      status_t status = roster->GetFreeOutputsFor(mixer, outputs, 4, &count, B_MEDIA_RAW_AUDIO);
      if (status != B_OK) {
        return status;
      }
      if (count < 1) {
        return B_MEDIA_ALREADY_CONNECTED;
      }

      media_output connected_output;
      media_input connected_input;
      return roster->Connect(outputs[0].source, destination, &format, &connected_output, &connected_input);
    }
  }  // namespace

  void repair_audio_routing() {
    status_t status;
    BMediaRoster *roster = BMediaRoster::Roster(&status);
    if (roster == nullptr || status != B_OK) {
      return;
    }

    media_node mixer;
    if (roster->GetAudioMixer(&mixer) != B_OK) {
      return;
    }

    media_output connected[4];
    int32 count = 0;
    if (roster->GetConnectedOutputsFor(mixer, connected, 4, &count) == B_OK && count > 0) {
      roster->ReleaseNode(mixer);
      return;  // routing is intact
    }

    media_node output;
    if (roster->GetAudioOutput(&output) != B_OK) {
      roster->ReleaseNode(mixer);
      return;
    }

    media_input inputs[8];
    int32 free_count = 0;
    if (roster->GetFreeInputsFor(output, inputs, 8, &free_count, B_MEDIA_RAW_AUDIO) == B_OK && free_count > 0) {
      media_format format;
      format.type = B_MEDIA_RAW_AUDIO;
      format.u.raw_audio = media_raw_audio_format::wildcard;
      if (connect_mixer(roster, mixer, inputs[0].destination, format) == B_OK) {
        BOOST_LOG(info) << "Reconnected the system mixer to the audio output"sv;
      }
    }

    roster->ReleaseNode(output);
    roster->ReleaseNode(mixer);
  }

  std::unique_ptr<audio_tap_t> audio_tap_t::insert(bool forward_to_output, std::string &error_message) {
    // (Not named "error": BOOST_LOG(error) refers to Sunshine's log severity.)
    status_t status;
    BMediaRoster *roster = BMediaRoster::Roster(&status);
    if (roster == nullptr || status != B_OK) {
      error_message = "media_server is not available";
      return nullptr;
    }

    auto route = std::make_unique<route_t>();
    if (roster->GetAudioMixer(&route->mixer) != B_OK) {
      error_message = "no system audio mixer";
      return nullptr;
    }

    // 1. Find the mixer's current connection.
    media_output mixer_outputs[4];
    int32 count = 0;
    if (roster->GetConnectedOutputsFor(route->mixer, mixer_outputs, 4, &count) != B_OK || count < 1) {
      repair_audio_routing();
      count = 0;
      if (roster->GetConnectedOutputsFor(route->mixer, mixer_outputs, 4, &count) != B_OK || count < 1) {
        roster->ReleaseNode(route->mixer);
        error_message = "the system mixer is not connected to an audio output";
        return nullptr;
      }
    }
    const media_output mixer_output = mixer_outputs[0];

    media_input downstream_input;
    if (!find_consumer_of(roster, mixer_output.source, route->downstream, downstream_input)) {
      roster->ReleaseNode(route->mixer);
      error_message = "couldn't find the node that plays the mixer output";
      return nullptr;
    }
    route->downstream_input = downstream_input.destination;
    route->format = mixer_output.format;

    // 2. Create and register the tap. Our node uses the system time source, like the sound card.
    auto tap = std::unique_ptr<audio_tap_t>(new audio_tap_t());
    tap->shared = std::make_shared<tap_shared_t>();
    tap->shared->forward = forward_to_output;
    tap->node = new tap_node_t(tap->shared);

    if (roster->RegisterNode(tap->node) != B_OK) {
      tap->node->Release();
      tap->node = nullptr;
      roster->ReleaseNode(route->downstream);
      roster->ReleaseNode(route->mixer);
      error_message = "couldn't register the audio tap node";
      return nullptr;
    }

    media_node time_source;
    if (roster->GetTimeSource(&time_source) == B_OK) {
      roster->SetTimeSourceFor(tap->node->Node().node, time_source.node);
      roster->ReleaseNode(time_source);
    }

    // Restores the original route if any later step fails.
    auto restore = [&]() {
      if (connect_mixer(roster, route->mixer, route->downstream_input, route->format) != B_OK) {
        BOOST_LOG(error) << "Couldn't restore the system audio route; use Media preferences to restart media services"sv;
      }
    };

    // 3. Detach the mixer from its consumer.
    status = roster->Disconnect(mixer_output.node.node, mixer_output.source, route->downstream.node, route->downstream_input);
    if (status != B_OK) {
      tap->node->Release();
      tap->node = nullptr;
      roster->ReleaseNode(route->downstream);
      roster->ReleaseNode(route->mixer);
      error_message = "couldn't disconnect the system mixer: "s + strerror(status);
      return nullptr;
    }

    // 4. mixer -> tap
    media_input tap_inputs[1];
    int32 tap_input_count = 0;
    roster->GetFreeInputsFor(tap->node->Node(), tap_inputs, 1, &tap_input_count, B_MEDIA_RAW_AUDIO);
    status = tap_input_count == 1 ? connect_mixer(roster, route->mixer, tap_inputs[0].destination, route->format) : B_ERROR;
    if (status != B_OK) {
      restore();
      tap->node->Release();
      tap->node = nullptr;
      roster->ReleaseNode(route->downstream);
      roster->ReleaseNode(route->mixer);
      error_message = "couldn't connect the mixer to the audio tap: "s + strerror(status);
      return nullptr;
    }

    // 5. tap -> original consumer, in exactly the mixer's format.
    media_output tap_outputs[1];
    int32 tap_output_count = 0;
    roster->GetFreeOutputsFor(tap->node->Node(), tap_outputs, 1, &tap_output_count, B_MEDIA_RAW_AUDIO);
    media_input mixer_side_input;
    media_output tap_connected_output;
    media_input downstream_connected_input;
    media_format format = route->format;
    if (tap_output_count == 1) {
      // The tap input is connected now; find it again to learn the negotiated format.
      media_input connected_inputs[1];
      int32 connected_count = 0;
      if (roster->GetConnectedInputsFor(tap->node->Node(), connected_inputs, 1, &connected_count) == B_OK && connected_count == 1) {
        format = connected_inputs[0].format;
      }
      status = roster->Connect(tap_outputs[0].source, route->downstream_input, &format, &tap_connected_output, &downstream_connected_input);
    } else {
      status = B_ERROR;
    }
    if (status != B_OK) {
      // Undo mixer -> tap, then put the mixer back.
      media_input tap_connected[1];
      int32 n = 0;
      if (roster->GetConnectedInputsFor(tap->node->Node(), tap_connected, 1, &n) == B_OK && n == 1) {
        roster->Disconnect(route->mixer.node, tap_connected[0].source, tap->node->Node().node, tap_connected[0].destination);
      }
      restore();
      tap->node->Release();
      tap->node = nullptr;
      roster->ReleaseNode(route->downstream);
      roster->ReleaseNode(route->mixer);
      error_message = "couldn't connect the audio tap to the audio output: "s + strerror(status);
      return nullptr;
    }

    // 6. Start the tap now in its time source's timeline.
    BTimeSource *ts = roster->MakeTimeSourceFor(tap->node->Node());
    roster->StartNode(tap->node->Node(), ts != nullptr ? ts->Now() : 0);
    if (ts != nullptr) {
      ts->Release();
    }

    tap->route = std::move(route);
    BOOST_LOG(info) << "Capturing system audio: "sv << tap->shared->frame_rate << " Hz, "sv << tap->shared->channels << " channel(s)"sv;
    return tap;
  }

  audio_tap_t::~audio_tap_t() {
    if (node == nullptr) {
      return;
    }

    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (roster != nullptr && route) {
      roster->StopNode(node->Node(), 0, true);

      // tap -> downstream
      media_output outputs[1];
      int32 count = 0;
      if (roster->GetConnectedOutputsFor(node->Node(), outputs, 1, &count) == B_OK && count == 1) {
        roster->Disconnect(node->Node().node, outputs[0].source, route->downstream.node, outputs[0].destination);
      }

      // mixer -> tap
      media_input inputs[1];
      count = 0;
      if (roster->GetConnectedInputsFor(node->Node(), inputs, 1, &count) == B_OK && count == 1) {
        roster->Disconnect(route->mixer.node, inputs[0].source, node->Node().node, inputs[0].destination);
      }

      // mixer -> downstream, as it was before.
      if (connect_mixer(roster, route->mixer, route->downstream_input, route->format) != B_OK) {
        BOOST_LOG(error) << "Couldn't restore the system audio route; use Media preferences to restart media services"sv;
      } else {
        BOOST_LOG(info) << "Restored the system audio route"sv;
      }

      roster->ReleaseNode(route->downstream);
      roster->ReleaseNode(route->mixer);
    }

    node->Release();
    node = nullptr;
  }

  bool audio_tap_t::connected() const {
    return shared && shared->connected;
  }

  std::chrono::milliseconds audio_tap_t::buffer_duration() const {
    std::lock_guard lock {shared->mutex};
    if (shared->frame_rate <= 0.0F || shared->max_chunk_frames == 0) {
      return std::chrono::milliseconds {0};
    }
    const double seconds = static_cast<double>(shared->max_chunk_frames) / static_cast<double>(shared->frame_rate);
    return std::chrono::milliseconds {static_cast<std::chrono::milliseconds::rep>(std::ceil(seconds * 1000.0))};
  }

  std::chrono::steady_clock::duration audio_tap_t::idle_time() const {
    std::lock_guard lock {shared->mutex};
    return std::chrono::steady_clock::now() - shared->last_push;
  }

  std::uint32_t audio_tap_t::discard(std::uint32_t frames, std::uint32_t sample_rate, std::uint32_t keep) {
    auto &s = *shared;
    std::lock_guard lock {s.mutex};
    if (s.frame_rate <= 0.0F || sample_rate == 0 || frames == 0) {
      return 0;
    }

    const double step = static_cast<double>(s.frame_rate) / static_cast<double>(sample_rate);
    if (read_position < static_cast<double>(s.read_frame)) {
      read_position = static_cast<double>(s.read_frame);
    }

    // Leave `keep` frames plus the two source frames read() interpolates across.
    const double available = static_cast<double>(s.write_frame) - read_position - step * keep - 2.0;
    const double drop = std::min(step * frames, available);
    if (drop <= 0.0) {
      return 0;
    }

    read_position += drop;
    s.read_frame = std::max(s.read_frame, static_cast<std::size_t>(read_position));
    return static_cast<std::uint32_t>(drop / step);
  }

  bool audio_tap_t::read(float *out, std::uint32_t frames, int channels, std::uint32_t sample_rate, std::chrono::milliseconds timeout) {
    auto &s = *shared;
    std::unique_lock lock {s.mutex};

    if (s.channels == 0 || s.frame_rate <= 0.0F || sample_rate == 0) {
      return false;
    }

    const double step = static_cast<double>(s.frame_rate) / static_cast<double>(sample_rate);

    // Keep the fractional position relative to the ring's read cursor.
    if (read_position < static_cast<double>(s.read_frame)) {
      read_position = static_cast<double>(s.read_frame);
    }

    // Bound the latency: if audio piled up (stream started late, or the encoder stalled), skip
    // ahead so the client hears the present rather than the past. The mixer hands over audio in
    // buffers that are usually much longer than one stream packet (often ~2048 frames, 43 ms),
    // so up to one such buffer of backlog is normal and must be kept to bridge the time until the
    // next one arrives.
    const double packet = step * frames;
    const double chunk = static_cast<double>(s.max_chunk_frames);
    const double backlog = static_cast<double>(s.write_frame) - read_position;
    if (backlog > chunk + packet * 4.0) {
      read_position = static_cast<double>(s.write_frame) - (chunk + packet * 2.0);
    }

    // Source frames needed: the last output frame interpolates between two input frames.
    const auto needed_end = static_cast<std::size_t>(std::floor(read_position + step * frames)) + 2;
    if (!s.cv.wait_for(lock, timeout, [&]() {
          return s.write_frame >= needed_end || !s.connected;
        })) {
      return false;
    }
    if (s.write_frame < needed_end) {
      return false;  // disconnected
    }

    // The writer may have overrun us while we waited.
    if (read_position < static_cast<double>(s.read_frame)) {
      read_position = static_cast<double>(s.read_frame);
    }

    const std::size_t capacity = s.ring.size() / s.channels;
    std::array<float, SPEAKER_COUNT> speakers {};

    for (std::uint32_t f = 0; f < frames; f++) {
      const double position = read_position + step * f;
      const auto index = static_cast<std::size_t>(position);
      const float fraction = static_cast<float>(position - static_cast<double>(index));

      const float *a = &s.ring[(index % capacity) * s.channels];
      const float *b = &s.ring[((index + 1) % capacity) * s.channels];

      speakers.fill(0.0F);
      for (uint32 c = 0; c < s.channels; c++) {
        const float value = a[c] + (b[c] - a[c]) * fraction;
        const int slot = s.layout[c];
        if (s.channels == 1) {
          speakers[FL] += value;
          speakers[FR] += value;
        } else if (slot >= 0) {
          speakers[slot] += value;
        } else {
          // Channels without a slot (e.g. back center) are shared by the front pair.
          speakers[FL] += value * 0.7071F;
          speakers[FR] += value * 0.7071F;
        }
      }

      float *dst = out + static_cast<std::size_t>(f) * channels;
      if (channels >= SPEAKER_COUNT || channels == 6) {
        // Surround stream: copy the slots it has.
        for (int c = 0; c < channels; c++) {
          dst[c] = c < SPEAKER_COUNT ? speakers[c] : 0.0F;
        }
        if (channels == 6) {
          // 5.1 has no side pair; fold it into the rear speakers.
          dst[BL] += speakers[SL];
          dst[BR] += speakers[SR];
        }
      } else {
        // Stereo (or mono) stream: standard downmix of everything that is present.
        const float left = speakers[FL] + 0.7071F * (speakers[FC] + speakers[BL] + speakers[SL]);
        const float right = speakers[FR] + 0.7071F * (speakers[FC] + speakers[BR] + speakers[SR]);
        if (channels == 1) {
          dst[0] = 0.5F * (left + right);
        } else {
          dst[0] = left;
          dst[1] = right;
          for (int c = 2; c < channels; c++) {
            dst[c] = 0.0F;
          }
        }
      }

      for (int c = 0; c < channels; c++) {
        dst[c] = std::clamp(dst[c], -1.0F, 1.0F);
      }
    }

    read_position += step * frames;
    s.read_frame = std::max(s.read_frame, static_cast<std::size_t>(read_position));
    return true;
  }

}  // namespace platf::haiku
