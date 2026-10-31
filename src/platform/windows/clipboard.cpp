/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Windows clipboard sync for a Sunshine session.
 */
// standard includes
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <Windows.h>

// local includes
#include "src/logging.h"
#include "src/platform/clipboard_sync.h"
#include "src/platform/common.h"
#include "utf_utils.h"

using namespace std::literals;

namespace platf {
  namespace {
    constexpr UINT WM_APPLY_PENDING = WM_APP + 1;  ///< Asks the watcher thread to write the pending text.
    constexpr int OPEN_ATTEMPTS = 10;  ///< Another application can hold the clipboard open for a short time.
    constexpr DWORD OPEN_RETRY_MS = 10;  ///< Delay between attempts to open the clipboard.
    constexpr DWORD WAKE_MS = 200;  ///< Longest wait before the watcher thread checks its stop token.

    /**
     * @brief Process-wide clipboard watcher state.
     */
    struct state_t {
      std::mutex mutex;  ///< Guards the fields below for the input and watcher threads.
      std::condition_variable ready_cv;  ///< Wakes subscribers waiting for the watcher window.
      bool ready = false;  ///< Whether the watcher can accept clipboard operations.
      bool failed = false;  ///< Whether creating the watcher window failed.
      bool started = false;  ///< Whether the watcher thread has been launched.
      HWND window = nullptr;  ///< Message-only window owned by the watcher thread.
      std::optional<std::string> pending;  ///< Client clipboard text waiting to be written.
      clipboard_subscribers_t subscribers;  ///< One entry for each streaming client that opted in.
      std::jthread thread;  ///< Watcher thread.
    };

    /**
     * @brief Return the process-wide clipboard watcher state.
     *
     * @return Watcher state shared by subscribe, unsubscribe, and the watcher thread.
     */
    state_t &state() {
      static state_t value;
      return value;
    }

    /**
     * @brief Limit text to the protocol maximum without splitting a UTF-8 character.
     *
     * @param text UTF-8 clipboard text.
     */
    void cap_text(std::string &text) {
      if (text.size() <= clipboard_max_bytes) {
        return;
      }

      BOOST_LOG(warning) << "Clipboard text cut from "sv << text.size() << " to "sv << clipboard_max_bytes << " bytes"sv;

      auto length = clipboard_max_bytes;
      while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
      }
      text.resize(length);
    }

    /**
     * @brief Open the clipboard, retrying while another application holds it.
     *
     * @param window Window that will own the clipboard if it gets emptied.
     * @return True when the clipboard is open.
     */
    bool open_clipboard(HWND window) {
      for (int attempt = 0; attempt < OPEN_ATTEMPTS; ++attempt) {
        if (OpenClipboard(window)) {
          return true;
        }
        Sleep(OPEN_RETRY_MS);
      }

      BOOST_LOG(warning) << "Failed to open the clipboard: "sv << GetLastError();
      return false;
    }

    /**
     * @brief Check whether the application that copied the text asked to keep it out of clipboard monitors.
     *
     * @return True when the clipboard must not be sent to clients.
     */
    bool is_excluded_from_monitors() {
      static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
      static const UINT ignore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");

      return IsClipboardFormatAvailable(exclude) || IsClipboardFormatAvailable(ignore);
    }

    /**
     * @brief Read the clipboard text.
     *
     * @param window Window used to open the clipboard.
     * @return UTF-16 text, or an empty string when there is none.
     */
    std::wstring read_clipboard(HWND window) {
      std::wstring text;
      if (!open_clipboard(window)) {
        return text;
      }

      if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        if (const auto *chars = static_cast<const wchar_t *>(GlobalLock(data)); chars != nullptr) {
          text.assign(chars, wcsnlen(chars, GlobalSize(data) / sizeof(wchar_t)));
          GlobalUnlock(data);
        }
      }

      CloseClipboard();
      return text;
    }

    /**
     * @brief Publish the host clipboard to every subscriber that does not already have it.
     *
     * @param window Watcher window.
     */
    void publish_clipboard(HWND window) {
      auto &clipboard = state();
      {
        std::lock_guard lock {clipboard.mutex};
        if (clipboard.subscribers.empty()) {
          return;
        }
      }

      if (GetClipboardOwner() == window || !IsClipboardFormatAvailable(CF_UNICODETEXT) || is_excluded_from_monitors()) {
        return;
      }

      auto text = utf_utils::to_utf8(read_clipboard(window));
      if (text.empty()) {
        return;
      }
      cap_text(text);

      std::vector<clipboard_queue_t> queues;
      {
        std::lock_guard lock {clipboard.mutex};
        queues = clipboard.subscribers.publish(text);
      }
      for (const auto &queue : queues) {
        queue->raise(clipboard_text_t {text});
      }
    }

    /**
     * @brief Replace the host clipboard with the pending client text.
     *
     * @param window Watcher window, which becomes the clipboard owner.
     */
    void apply_pending(HWND window) {
      std::optional<std::string> text;
      {
        auto &clipboard = state();
        std::lock_guard lock {clipboard.mutex};
        text.swap(clipboard.pending);
      }
      if (!text) {
        return;
      }

      const auto wide = utf_utils::from_utf8(*text);
      if (wide.empty()) {
        return;
      }

      const auto size = (wide.size() + 1) * sizeof(wchar_t);
      HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, size);
      if (data == nullptr) {
        BOOST_LOG(warning) << "Failed to allocate clipboard text: "sv << GetLastError();
        return;
      }

      auto *chars = GlobalLock(data);
      if (chars == nullptr) {
        BOOST_LOG(warning) << "Failed to lock clipboard text: "sv << GetLastError();
        GlobalFree(data);
        return;
      }
      std::memcpy(chars, wide.c_str(), size);
      GlobalUnlock(data);

      if (!open_clipboard(window)) {
        GlobalFree(data);
        return;
      }

      // The clipboard owns the memory after SetClipboardData succeeds.
      EmptyClipboard();
      if (SetClipboardData(CF_UNICODETEXT, data) == nullptr) {
        BOOST_LOG(warning) << "Failed to set the clipboard: "sv << GetLastError();
        GlobalFree(data);
      }
      CloseClipboard();
    }

    /**
     * @brief Handle messages for the watcher window.
     *
     * @param window Watcher window.
     * @param message Message identifier.
     * @param wparam Message parameter.
     * @param lparam Message parameter.
     * @return Message result.
     */
    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      if (message == WM_CLIPBOARDUPDATE) {
        publish_clipboard(window);
        return 0;
      }
      if (message == WM_APPLY_PENDING) {
        apply_pending(window);
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }

    /**
     * @brief Create the watcher window and register it as a clipboard listener.
     *
     * @return The window, or null on failure.
     */
    HWND create_window() {
      const auto instance = GetModuleHandleW(nullptr);

      WNDCLASSEXW window_class {};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = window_proc;
      window_class.hInstance = instance;
      window_class.lpszClassName = L"SunshineClipboard";
      if (RegisterClassExW(&window_class) == 0) {
        BOOST_LOG(warning) << "Failed to register the clipboard window class: "sv << GetLastError();
        return nullptr;
      }

      HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
      if (window == nullptr) {
        BOOST_LOG(warning) << "Failed to create the clipboard window: "sv << GetLastError();
        return nullptr;
      }

      if (!AddClipboardFormatListener(window)) {
        BOOST_LOG(warning) << "Failed to listen for clipboard changes: "sv << GetLastError();
        DestroyWindow(window);
        return nullptr;
      }

      return window;
    }

    /**
     * @brief Watch the clipboard until the process exits.
     *
     * This thread owns a window, so it can't use syncThreadDesktop() and stays on the process desktop.
     *
     * @param stop Stop token signalled when the state is destroyed.
     */
    void thread_main(std::stop_token stop) {
      auto &clipboard = state();

      HWND window = create_window();
      {
        std::lock_guard lock {clipboard.mutex};
        clipboard.window = window;
        clipboard.ready = window != nullptr;
        clipboard.failed = window == nullptr;
        clipboard.ready_cv.notify_all();
      }
      if (window == nullptr) {
        return;
      }

      while (!stop.stop_requested()) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, WAKE_MS, QS_ALLINPUT);
      }

      {
        std::lock_guard lock {clipboard.mutex};
        clipboard.window = nullptr;
        clipboard.ready = false;
      }
      RemoveClipboardFormatListener(window);
      DestroyWindow(window);
    }
  }  // namespace

  void clipboard_set(std::string_view text, const clipboard_queue_t &origin) {
    std::string capped {text};
    cap_text(capped);

    std::vector<clipboard_queue_t> queues;
    {
      auto &clipboard = state();
      std::lock_guard lock {clipboard.mutex};
      if (!clipboard.ready) {
        return;
      }
      clipboard.pending = capped;
      queues = clipboard.subscribers.note_local(origin, capped);
      PostMessageW(clipboard.window, WM_APPLY_PENDING, 0, 0);
    }
    for (const auto &queue : queues) {
      queue->raise(clipboard_text_t {capped});
    }
  }

  void clipboard_subscribe(clipboard_queue_t queue) {
    auto &clipboard = state();
    std::unique_lock lock {clipboard.mutex};
    if (!clipboard.started) {
      clipboard.started = true;
      clipboard.thread = std::jthread(thread_main);
    }
    clipboard.ready_cv.wait(lock, [&clipboard]() {
      return clipboard.ready || clipboard.failed;
    });

    // No initial pull, so a new stream doesn't replace the client's clipboard.
    if (clipboard.ready) {
      clipboard.subscribers.add(std::move(queue));
    }
  }

  void clipboard_unsubscribe(const clipboard_queue_t &queue) {
    auto &clipboard = state();
    std::lock_guard lock {clipboard.mutex};
    clipboard.subscribers.remove(queue);
  }
}  // namespace platf
