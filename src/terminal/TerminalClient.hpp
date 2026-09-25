#ifndef __ET_TERMINAL_CLIENT__
#define __ET_TERMINAL_CLIENT__

#include <atomic>
#include <chrono>
#include <functional>

#include "ClientConnection.hpp"
#include "Console.hpp"
#include "CryptoHandler.hpp"
#include "ETerminal.pb.h"
#include "ForwardSourceHandler.hpp"
#include "Headers.hpp"
#include "LogHandler.hpp"
#include "PortForwardHandler.hpp"
#include "RawSocketUtils.hpp"
#include "ServerConnection.hpp"
#include "SshSetupHandler.hpp"
#include "TcpSocketHandler.hpp"
#include "TitleParser.hpp"

namespace et {
/**
 * @brief Coordinates the lifecycle of a client connection, console, and
 * tunnels.
 */
class TerminalClient {
 public:
  static const string INVALID_SESSION_CONNECT_ERROR;

  /**
   * @brief Configures the client with the required sockets, console, and
   * tunnels.
   */
  TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                 std::shared_ptr<SocketHandler> _pipeSocketHandler,
                 const SocketEndpoint& _socketEndpoint, const string& id,
                 const string& passkey, shared_ptr<Console> _console,
                 bool jumphost, const string& tunnels,
                 const string& reverseTunnels, bool forwardSshAgent,
                 const string& identityAgent, int _keepaliveDuration,
                 const vector<pair<string, string>>& envVars,
                 bool noPty = false, const string& command = "",
                 int _maxConnectAttempts = 3, bool _resumeSavedSession = false,
                 std::function<bool()> _sessionHeartbeat = {},
                 std::function<bool(const string&)> _sessionTitleUpdate = {});
  /** @brief Tears down the client, closing sockets and stopping background
   * threads. */
  virtual ~TerminalClient();
  /** @brief Runs the interactive session for `command`, optionally staying
   * alive. */
  void run(const string& command, const bool noexit);
  static void configureCloseOnHangup(bool enabled) { closeOnHangup = enabled; }
  static void requestHangupClose(int = 0) { hangupCloseRequested = true; }
  static bool waitForHangupClose(int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!hangupCloseCompleted &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return hangupCloseCompleted;
  }
  static void resetHangupClose() {
    closeOnHangup = false;
    hangupCloseRequested = false;
    hangupCloseCompleted = false;
  }
#ifdef WIN32
  static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType);
#endif
  bool killSession(int timeoutSeconds);
  bool sessionEndedByServer() {
    return connection &&
           connection->lastStatus() == et::ConnectStatus::INVALID_KEY;
  }

  // True when this client adopted a session that was already running rather
  // than creating one. The shell is mid-life, so connect-time setup has already
  // happened and re-running it would type into whatever is in the foreground.
  bool attachedToExisting() { return connection && connection->wasRecovered(); }

  /**
   * @brief Why `run()` returned, in a form fit to show a user.
   *
   * `run()` returning is what ends a control session, and every caller so far
   * has had to guess which of several very different things happened. Ask here
   * instead of inferring it.
   */
  string exitReason() {
    if (sessionEndedByServer()) {
      return "the remote session ended (server no longer has it)";
    }
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        return "shutdown was requested";
      }
    }
    return "the connection closed";
  }
  /**
   * @brief Flags the client loop to exit gracefully on the next iteration.
   */
  void shutdown() {
    lock_guard<recursive_mutex> guard(shutdownMutex);
    shuttingDown = true;
  }

  // True when the client currently holds a live connection to etserver.  ET
  // flips this to false during a drop and back to true once it reconnects, so
  // it distinguishes "link down" from "the daemon is gone" (the latter shows as
  // an unreachable control socket).
  bool isConnected() { return connection && !connection->isDisconnected(); }

 protected:
  /** @brief Console wrapper used for local terminal input/output. */
  shared_ptr<Console> console;
  /** @brief Client connection that talks to the ET server. */
  shared_ptr<ClientConnection> connection;
  /** @brief Handles local/remote port forwarding tunnels. */
  shared_ptr<PortForwardHandler> portForwardHandler;
  /** @brief Guarded flag that ends `run()` when set. */
  bool shuttingDown;
  /** @brief Synchronizes writes to `shuttingDown`. */
  recursive_mutex shutdownMutex;
  /** @brief Keepalive interval (seconds) sent to the server. */
  int keepaliveDuration;
  /** @brief True when this session uses the raw pipe command channel. */
  bool noPty;
  static std::atomic<bool> closeOnHangup;
  static std::atomic<bool> hangupCloseRequested;
  static std::atomic<bool> hangupCloseCompleted;
  std::function<bool()> sessionHeartbeat;
  std::function<bool(const string&)> sessionTitleUpdate;
  TitleParser titleParser;
};

}  // namespace et
#endif  // __ET_TERMINAL_CLIENT__
