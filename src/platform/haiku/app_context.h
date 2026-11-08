/**
 * @file src/platform/haiku/app_context.h
 * @brief Owns the BApplication that Haiku's app_server and Media Kit APIs require.
 */
#pragma once

namespace platf::haiku {

  /**
   * @brief Start Sunshine's BApplication on a dedicated thread.
   *
   * BScreen, BBitmap and BMediaRoster all talk to Haiku's servers through the team's
   * BApplication. Sunshine's own main loop is not a BLooper, so the application runs its message
   * loop on a helper thread. When Haiku asks the application to quit (for example during system
   * shutdown), Sunshine's regular shutdown path is triggered.
   *
   * Safe to call more than once; later calls are no-ops.
   *
   * @return True when the application is running.
   */
  bool start_application();

  /**
   * @brief Quit the BApplication started by start_application() and join its thread.
   */
  void stop_application();

}  // namespace platf::haiku
