/**
 * @file src/platform/haiku/app_context.cpp
 * @brief Owns the BApplication that Haiku's app_server and Media Kit APIs require.
 */

// standard includes
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// platform includes
#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>

// local includes
#include "app_context.h"
#include "src/entry_handler.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::haiku {
  namespace {
    constexpr const char *APP_SIGNATURE = "application/x-vnd.LizardByte-Sunshine";

    /**
     * How long a quit request during system shutdown waits for Sunshine's own shutdown. The
     * registrar gives each application 3 seconds before it treats it as hung, and a negative
     * answer would cancel the system shutdown, so the application always agrees in time then.
     */
    constexpr auto SHUTDOWN_GRACE_PERIOD = 2500ms;

    /**
     * @brief State shared by the application thread and start/stop_application().
     *
     * Kept outside the BApplication object: that object is deleted on its own thread as soon as
     * its loop ends, so nothing else may hold a pointer to it.
     */
    struct app_state_t {
      std::mutex mutex;
      std::condition_variable cv;
      bool allow_quit = false;  ///< Set by stop_application(): Sunshine finished cleaning up.
      bool shutdown_requested = false;  ///< A Sunshine shutdown was already triggered.
    };

    /**
     * @brief BApplication whose quit requests turn into a regular Sunshine shutdown.
     */
    class sunshine_app_t: public BApplication {
    public:
      sunshine_app_t(std::shared_ptr<app_state_t> state, status_t *init_status):
          BApplication(APP_SIGNATURE, init_status),
          state_ {std::move(state)} {
      }

      bool QuitRequested() override {
        // The registrar marks the quit requests it sends during a system shutdown.
        bool system_shutdown = false;
        if (const BMessage *message = CurrentMessage(); message != nullptr) {
          system_shutdown = message->GetBool("_shutdown_", false);
        }

        std::unique_lock lock {state_->mutex};
        if (state_->allow_quit) {
          return true;
        }

        // Start Sunshine's regular shutdown so it can restore audio routing and release input.
        if (!state_->shutdown_requested) {
          state_->shutdown_requested = true;
          BOOST_LOG(info) << (system_shutdown ? "Haiku is shutting down; stopping Sunshine"sv : "Haiku requested Sunshine to quit"sv);
          lifetime::exit_sunshine(0, true);
        }

        if (!system_shutdown) {
          // An ordinary quit (Deskbar, ProcessController, `hey`): keep the application, which
          // Sunshine still uses for the Media Kit and app_server, until stop_application()
          // quits it at the end of the shutdown.
          return false;
        }

        // System shutdown: give Sunshine a moment, then agree so the shutdown is not cancelled.
        if (!state_->cv.wait_for(lock, SHUTDOWN_GRACE_PERIOD, [this]() {
              return state_->allow_quit;
            })) {
          BOOST_LOG(warning) << "Sunshine is still shutting down; letting Haiku quit the application"sv;
        }
        return true;
      }

    private:
      std::shared_ptr<app_state_t> state_;
    };

    std::mutex app_mutex;
    std::thread app_thread;
    std::shared_ptr<app_state_t> app_state;
    BMessenger app_messenger;  ///< Safe to use after the application is gone (sends just fail).
  }  // namespace

  bool start_application() {
    std::lock_guard lock {app_mutex};
    if (app_thread.joinable()) {
      return true;
    }
    if (be_app != nullptr) {
      // Something else in this team already created the application; use it as is.
      return true;
    }

    auto state = std::make_shared<app_state_t>();
    std::promise<BMessenger> created;
    auto created_future = created.get_future();

    // The promise and the state are moved into the thread, so nothing it uses lives on this
    // function's stack.
    std::thread thread([state, created = std::move(created)]() mutable {
      rename_thread(find_thread(nullptr), "sunshine app");

      // The BApplication must be constructed on the thread that runs its loop.
      // (Not named "error": BOOST_LOG(error) refers to Sunshine's log severity.)
      status_t init_status = B_OK;
      auto *instance = new sunshine_app_t(state, &init_status);
      if (init_status != B_OK) {
        BOOST_LOG(error) << "Couldn't create the Haiku application object: "sv << strerror(init_status);
        delete instance;
        created.set_value(BMessenger {});
        return;
      }

      created.set_value(BMessenger {instance});
      instance->Run();
      delete instance;
    });

    BMessenger messenger = created_future.get();
    if (!messenger.IsValid()) {
      thread.join();
      return false;
    }

    app_thread = std::move(thread);
    app_state = std::move(state);
    app_messenger = messenger;
    return true;
  }

  void stop_application() {
    std::lock_guard lock {app_mutex};
    if (!app_thread.joinable()) {
      return;
    }

    {
      std::lock_guard state_lock {app_state->mutex};
      app_state->allow_quit = true;
    }
    // Wakes a QuitRequested() that is waiting for Sunshine's shutdown to finish.
    app_state->cv.notify_all();

    // If Haiku already quit the application, the messenger is stale and this fails harmlessly;
    // otherwise the application answers with Quit(), Run() returns, and the thread deletes it.
    app_messenger.SendMessage(B_QUIT_REQUESTED);

    app_thread.join();
    app_messenger = BMessenger {};
    app_state.reset();
  }

}  // namespace platf::haiku
