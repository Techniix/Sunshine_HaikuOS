/**
 * @file src/platform/haiku/display.cpp
 * @brief Haiku screen capture through app_server.
 *
 * Frames are captured with BScreen::ReadBitmap(), which asks app_server to copy its front buffer
 * into a shared BBitmap. app_server converts from whatever depth the screen runs at, so this
 * works with every graphics driver Haiku supports (VESA, framebuffer, native accelerants, and
 * GPU drivers such as NVIDIA's) without any driver cooperation.
 *
 * Each pooled image owns its own B_RGB32 BBitmap, so a frame is read straight into the buffer
 * the encoder consumes. B_RGB32 is stored as B, G, R, X in memory, which is FFmpeg's BGR0 format
 * used by Sunshine's software color conversion. Hardware encoders (Vulkan Video) receive the
 * converted frame through FFmpeg's upload path.
 */

// standard includes
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <Bitmap.h>
#include <Screen.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace platf {
  namespace {
    /** How often the capture loop checks for a resolution change. */
    constexpr auto MODE_CHECK_INTERVAL = 1s;

    /**
     * @brief Captured frame backed by a BBitmap that app_server can write into.
     */
    struct haiku_img_t: public img_t {
      ~haiku_img_t() override {
        // `data` points into the bitmap; it must not be freed separately.
        data = nullptr;
      }

      std::unique_ptr<BBitmap> bitmap;  ///< Server-linked bitmap receiving the screen contents.
    };

    /**
     * @brief Resolve a Sunshine display name ("0", "1", ...) to a Haiku screen ID.
     *
     * @param display_name Display name from the configuration or display list; empty selects the
     *        main screen.
     * @return Screen ID to open.
     */
    screen_id screen_for_name(const std::string &display_name) {
      if (display_name.empty()) {
        return B_MAIN_SCREEN_ID;
      }

      char *end = nullptr;
      const long index = std::strtol(display_name.c_str(), &end, 10);
      if (end == display_name.c_str() || *end != '\0' || index < 0) {
        BOOST_LOG(warning) << "Unknown display ["sv << display_name << "], using the main screen"sv;
        return B_MAIN_SCREEN_ID;
      }

      return screen_id {static_cast<int32>(index)};
    }

    /**
     * @brief Advance the frame pacing timeline (same policy as the Linux capture backends).
     */
    void handle_pacing(std::chrono::steady_clock::time_point &next_frame, std::chrono::nanoseconds delay, logging::time_delta_periodic_logger &logger) {
      auto now = std::chrono::steady_clock::now();

      if (next_frame > now) {
        std::this_thread::sleep_until(next_frame);
        logger.first_point(next_frame);
        logger.second_point_now_and_log();
      }

      next_frame += delay;
      if (next_frame < now) {  // a major slowdown happened; we couldn't keep up
        next_frame = now + delay;
      }
    }

    /**
     * @brief app_server based display capture.
     */
    class haiku_display_t: public display_t {
    public:
      haiku_display_t() = default;

      int init(const std::string &display_name, const ::video::config_t &config) {
        screen = screen_for_name(display_name);

        BScreen bscreen(screen);
        if (!bscreen.IsValid()) {
          BOOST_LOG(error) << "Couldn't open Haiku screen "sv << screen.id;
          return -1;
        }

        const BRect frame = bscreen.Frame();
        width = frame.IntegerWidth() + 1;
        height = frame.IntegerHeight() + 1;
        if (width <= 0 || height <= 0) {
          BOOST_LOG(error) << "Haiku screen "sv << screen.id << " reports an empty frame"sv;
          return -1;
        }

        // Haiku has one desktop coordinate space per screen and no display scaling of pointer
        // coordinates, so the logical and physical sizes are the same.
        offset_x = 0;
        offset_y = 0;
        env_width = width;
        env_height = height;
        logical_width = width;
        logical_height = height;
        env_logical_width = width;
        env_logical_height = height;

        delay = ::video::capture_frame_interval(config);

        BOOST_LOG(info) << "Capturing Haiku screen "sv << screen.id << ": "sv << width << 'x' << height
                        << " ("sv << color_space_name(bscreen.ColorSpace()) << ')';
        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();
        auto next_mode_check = next_frame + MODE_CHECK_INTERVAL;

        sleep_overshoot_logger.reset();

        while (true) {
          handle_pacing(next_frame, delay, sleep_overshoot_logger);

          if (std::chrono::steady_clock::now() >= next_mode_check) {
            next_mode_check = std::chrono::steady_clock::now() + MODE_CHECK_INTERVAL;
            if (mode_changed()) {
              BOOST_LOG(info) << "Haiku screen mode changed, reinitializing capture"sv;
              return capture_e::reinit;
            }
          }

          std::shared_ptr<img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, *cursor);
          switch (status) {
            case capture_e::reinit:
            case capture_e::capture_error:
            case capture_e::interrupted:
              return status;
            case capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return capture_e::ok;
              }
              break;
            case capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
              return status;
          }
        }
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<haiku_img_t>();

        // B_RGB32 bitmaps live in an area shared with app_server, so ReadBitmap() writes into
        // them directly.
        img->bitmap = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), B_RGB32);
        if (img->bitmap->InitCheck() != B_OK || img->bitmap->Bits() == nullptr) {
          BOOST_LOG(error) << "Couldn't allocate a "sv << width << 'x' << height << " capture bitmap"sv;
          img->bitmap.reset();
          return nullptr;
        }

        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = static_cast<std::int32_t>(img->bitmap->BytesPerRow());
        img->data = static_cast<std::uint8_t *>(img->bitmap->Bits());
        return img;
      }

      int dummy_img(img_t *img) override {
        if (!img || !img->data) {
          return -1;
        }
        // Black frame (B, G, R and padding all zero).
        std::memset(img->data, 0, static_cast<std::size_t>(img->row_pitch) * img->height);
        return 0;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
        // Frames are in system memory. Sunshine converts them in software and, for hardware
        // encoders such as Vulkan Video, FFmpeg uploads the converted frame to the GPU.
        return std::make_unique<avcodec_encode_device_t>();
      }

    private:
      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<img_t> &img_out, bool draw_cursor) {
        if (!pull_free_image_cb(img_out)) {
          return capture_e::interrupted;
        }

        auto *img = static_cast<haiku_img_t *>(img_out.get());
        if (!img->bitmap) {
          return capture_e::reinit;
        }

        BScreen bscreen(screen);
        if (!bscreen.IsValid()) {
          return capture_e::reinit;
        }

        BRect bounds(0, 0, width - 1, height - 1);
        const auto frame_timestamp = std::chrono::steady_clock::now();
        const status_t status = bscreen.ReadBitmap(img->bitmap.get(), draw_cursor, &bounds);
        if (status != B_OK) {
          // app_server refuses the read while the mode is being switched; let the caller rebuild.
          BOOST_LOG(warning) << "BScreen::ReadBitmap() failed: "sv << strerror(status);
          return capture_e::reinit;
        }

        img->frame_timestamp = frame_timestamp;
        return capture_e::ok;
      }

      bool mode_changed() {
        BScreen bscreen(screen);
        if (!bscreen.IsValid()) {
          return true;
        }
        const BRect frame = bscreen.Frame();
        return frame.IntegerWidth() + 1 != width || frame.IntegerHeight() + 1 != height;
      }

      static const char *color_space_name(color_space space) {
        switch (space) {
          case B_RGB32:
            return "32-bit";
          case B_RGBA32:
            return "32-bit with alpha";
          case B_RGB24:
            return "24-bit";
          case B_RGB16:
            return "16-bit";
          case B_RGB15:
            return "15-bit";
          case B_CMAP8:
            return "8-bit palette";
          default:
            return "other";
        }
      }

      screen_id screen = B_MAIN_SCREEN_ID;
      std::chrono::nanoseconds delay {};
    };
  }  // namespace

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    // Capture always lands in system memory. Vulkan encoding is supported because FFmpeg uploads
    // the converted frame itself.
    if (hwdevice_type != mem_type_e::system && hwdevice_type != mem_type_e::vulkan) {
      BOOST_LOG(error) << "Haiku capture doesn't support this encoder memory type"sv;
      return nullptr;
    }

    auto disp = std::make_shared<haiku_display_t>();
    if (disp->init(display_name, config)) {
      return nullptr;
    }
    return disp;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    std::vector<std::string> names;
    if (hwdevice_type != mem_type_e::system && hwdevice_type != mem_type_e::vulkan) {
      return names;
    }

    BScreen screen(B_MAIN_SCREEN_ID);
    while (screen.IsValid()) {
      names.emplace_back(std::to_string(screen.ID().id));
      if (screen.SetToNext() != B_OK) {
        break;
      }
    }

    return names;
  }

  bool needs_encoder_reenumeration() {
    // The GPU cannot change while Haiku runs (no hotplug or driver reload), and every probe of
    // the HEVC/AV1 encoders costs memory inside FFmpeg, so the startup probe is kept.
    return false;
  }
}  // namespace platf
