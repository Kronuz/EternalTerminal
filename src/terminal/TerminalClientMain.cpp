#include <ctime>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>

#include "BinaryStdioConsole.hpp"
#include "ClientArgParsing.hpp"
#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlPaths.hpp"
#include "DaemonCreator.hpp"
#include "Headers.hpp"
#include "HostParsing.hpp"
#include "ParseConfigFile.hpp"
#include "PipeSocketHandler.hpp"
#include "PseudoTerminalConsole.hpp"
#include "SessionStore.hpp"
#include "SessionTombstone.hpp"
#include "SshSetupHandler.hpp"
#include "SubprocessUtils.hpp"
#include "TelemetryService.hpp"
#include "TerminalClient.hpp"
#include "TunnelUtils.hpp"
#include "WinsockContext.hpp"

using namespace et;

// A short random suffix for an unnamed --ctl session's socket name.
static string genRandomHandle() {
  static const char* kHex = "0123456789abcdef";
  std::random_device rd;
  string handle;
  for (int i = 0; i < 6; i++) {
    handle.push_back(kHex[rd() % 16]);
  }
  return handle;
}

bool ping(SocketEndpoint socketEndpoint,
          shared_ptr<SocketHandler> clientSocketHandler) {
  VLOG(1) << "Connecting";
  int socketFd = clientSocketHandler->connect(socketEndpoint);
  if (socketFd == -1) {
    VLOG(1) << "Could not connect to host";
    return false;
  }
  clientSocketHandler->close(socketFd);
  return true;
}

void handleParseException(std::exception& e, cxxopts::Options& options) {
  CLOG(INFO, "stdout") << "Exception: " << e.what() << "\n" << endl;
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(1);
}

template <class T, class DefaultT>
T extractSingleOptionWithDefault(const cxxopts::ParseResult& result,
                                 const cxxopts::Options& options,
                                 const string& name, DefaultT defaultValue) {
  auto count = result.count(name);
  if (count == 0) {
    return defaultValue;
  }
  if (count == 1) {
    return result[name].as<T>();
  }
  CLOG(INFO, "stdout") << "Value for " << name
                       << " must be specified only once\n";
  CLOG(INFO, "stdout") << options.help({}) << endl;
  exit(0);
}

enum class AttachResult { ATTACHED, INVALID_SESSION, FAILED };
enum class KillResult { KILLED, INVALID_SESSION, FAILED };

bool deleteSavedSession(const string& name) {
  try {
    deleteSession(name);
    return true;
  } catch (const std::exception& e) {
    CLOG(INFO, "stdout") << "Warning: Could not delete saved session '" << name
                         << "': " << e.what() << endl;
    return false;
  }
}

string lowercaseAscii(string value) {
  transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return value;
}

string displayTitle(const string& title) {
  if (title.empty()) {
    return "-";
  }
  if (title.size() <= 32) {
    return title;
  }
  // Leave room for the 3-byte ellipsis without splitting a UTF-8 sequence.
  size_t keep = 29;
  while (keep > 0 && (static_cast<unsigned char>(title[keep]) & 0xc0) == 0x80) {
    --keep;
  }
  return title.substr(0, keep) + "…";
}

void printSessionCandidate(const SessionInfo& session) {
  CLOG(INFO, "stdout") << "  " << session.name << " ["
                       << displayTitle(session.title) << "] (" << session.host
                       << ":" << session.port << ")" << endl;
}

bool sessionNameIsOccupied(const string& name) {
  try {
    const fs::path path = sessionDirPath() + "/" + name;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec) {
      return ec.value() != ENOENT;
    }
    return status.type() != fs::file_type::not_found;
  } catch (...) {
    return true;
  }
}

string makeDefaultSessionName() {
  char date[16];
  const time_t now = time(NULL);
  struct tm localTm;
#ifdef WIN32
  localtime_s(&localTm, &now);
#else
  localtime_r(&now, &localTm);
#endif
  strftime(date, sizeof(date), "%Y%m%d", &localTm);

  const string base = string(date) + "-";
  string lastCandidate;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const string candidate = base + genRandomAlphaNum(4);
    lastCandidate = candidate;
    if (!sessionNameIsOccupied(candidate)) {
      return candidate;
    }
  }

  // The no-clobber save will fail on an occupied name.
  return lastCandidate;
}

optional<SessionInfo> resolveSavedSession(const string& query) {
  const vector<SessionInfo> savedSessions = listSessions();
  for (const auto& candidate : savedSessions) {
    if (candidate.name == query) {
      return candidate;
    }
  }

  if (!query.empty()) {
    const string lowercaseQuery = lowercaseAscii(query);
    vector<SessionInfo> matches;
    for (const auto& candidate : savedSessions) {
      if (lowercaseAscii(candidate.name).find(lowercaseQuery) != string::npos ||
          lowercaseAscii(candidate.title).find(lowercaseQuery) !=
              string::npos) {
        matches.push_back(candidate);
      }
    }
    if (matches.size() == 1) {
      return matches.front();
    }
    if (matches.size() > 1) {
      CLOG(INFO, "stdout") << "Multiple saved sessions match '" << query
                           << "':" << endl;
      for (const auto& candidate : matches) {
        printSessionCandidate(candidate);
      }
      return nullopt;
    }
  }

  CLOG(INFO, "stdout") << "No saved session named '" << query << "'" << endl;
  for (const auto& candidate : savedSessions) {
    printSessionCandidate(candidate);
  }
  return nullopt;
}

/*
 * Serve a control session: detach, answer etctl on the control socket for as
 * long as the session lives, and leave a note behind saying why it stopped.
 *
 * Both ways of arriving at a live session -- bootstrapping a new one over SSH
 * and reattaching to one already running -- end up here, so a control session
 * behaves the same either way.
 */
#ifndef WIN32
void runControlSession(TerminalClient& client,
                       shared_ptr<ControlConsole> controlConsole,
                       const string& ctlName, const string& socketPath,
                       const string& command, const string& hostLabel) {
  // Double-fork into the background; the parent exits here.
  DaemonCreator::create(true, "");

  ControlListener listener(
      controlConsole, socketPath, [&client]() { client.shutdown(); },
      [&client]() { return client.isConnected(); }, hostLabel);
  listener.start();
  // A control session is always persistent, so honor -c/--command as a
  // one-shot startup command run on connect (e.g. to set up a clean-room
  // shell) instead of silently dropping it.  noexit is implied, so run()
  // injects "<command>\n" and does not append "; exit".  An adopted session
  // already ran it when it was created, and its shell may have something in
  // the foreground now, so it is not replayed.
  const bool adopted = client.attachedToExisting();
  client.run(adopted ? "" : command, /*noexit=*/true);
  // run() returning is what ends a control session, and unlinking the socket
  // is all the next `etctl` command would otherwise see. Leave the reason
  // behind first, so that command can say what happened instead of reporting
  // a missing file.
  const string endedBecause = client.exitReason();
  LOG(INFO) << "Control session '" << ctlName << "' ending: " << endedBecause;
  session_tombstone::write(ctlName, endedBecause);
  listener.shutdown();
}
#endif

AttachResult attachSavedSession(
    const string& name, const SessionInfo& session, const string& command,
    bool noexit, bool noTerminal, int keepaliveDuration,
    shared_ptr<Console> consoleOverride = nullptr,
    const std::function<void(TerminalClient&)>& drive = nullptr) {
  SocketEndpoint endpoint;
  endpoint.set_name(session.host);
  endpoint.set_port(session.port);
  shared_ptr<SocketHandler> socket(new TcpSocketHandler());
  shared_ptr<SocketHandler> pipeSocket(new PipeSocketHandler());

  if (!ping(endpoint, socket)) {
    CLOG(INFO, "stdout") << "Could not reach the ET server: " << endpoint.name()
                         << ":" << endpoint.port() << endl;
    return AttachResult::FAILED;
  }

  shared_ptr<Console> console = consoleOverride;
  if (!console && !noTerminal) {
    console.reset(new PseudoTerminalConsole());
  }
  bool sessionEnded = false;
  try {
    TerminalClient client(
        socket, pipeSocket, endpoint, session.id, session.passkey, console,
        /*jumphost=*/false, /*tunnels=*/"", /*reverseTunnels=*/"",
        /*forwardSshAgent=*/false, /*identityAgent=*/"", keepaliveDuration,
        /*envVars=*/{}, /*noPty=*/false, /*command=*/"",
        /*maxConnectAttempts=*/15,
        /*resumeSavedSession=*/true, [name]() { return touchSession(name); },
        [name](const string& title) {
          return updateSessionTitle(name, title);
        });
    if (drive) {
      drive(client);
    } else {
      client.run(command, noexit);
    }
    sessionEnded = client.sessionEndedByServer();
  } catch (const runtime_error& err) {
    if (string(err.what()) == TerminalClient::INVALID_SESSION_CONNECT_ERROR) {
      return AttachResult::INVALID_SESSION;
    }
    CLOG(INFO, "stdout") << "Could not attach to session '" << name
                         << "': " << err.what() << endl;
    return AttachResult::FAILED;
  }

  if (sessionEnded) {
    deleteSavedSession(name);
  }
  return AttachResult::ATTACHED;
}

KillResult killSavedSession(const SessionInfo& session) {
  SocketEndpoint endpoint;
  endpoint.set_name(session.host);
  endpoint.set_port(session.port);
  shared_ptr<SocketHandler> socket(new TcpSocketHandler());
  shared_ptr<SocketHandler> pipeSocket(new PipeSocketHandler());

  if (!ping(endpoint, socket)) {
    CLOG(INFO, "stdout") << "Could not reach the ET server: " << endpoint.name()
                         << ":" << endpoint.port() << endl;
    return KillResult::FAILED;
  }

  try {
    TerminalClient client(
        socket, pipeSocket, endpoint, session.id, session.passkey,
        /*console=*/nullptr, /*jumphost=*/false, /*tunnels=*/"",
        /*reverseTunnels=*/"", /*forwardSshAgent=*/false,
        /*identityAgent=*/"", MAX_CLIENT_KEEP_ALIVE_DURATION,
        /*envVars=*/{}, /*noPty=*/false, /*command=*/"",
        /*maxConnectAttempts=*/3,
        /*resumeSavedSession=*/true);
    if (!client.killSession(15)) {
      CLOG(INFO, "stdout")
          << "The server did not confirm termination of session '"
          << session.name << "'" << endl;
      return KillResult::FAILED;
    }
  } catch (const runtime_error& err) {
    if (string(err.what()) == TerminalClient::INVALID_SESSION_CONNECT_ERROR) {
      return KillResult::INVALID_SESSION;
    }
    CLOG(INFO, "stdout") << "Could not kill session '" << session.name
                         << "': " << err.what() << endl;
    return KillResult::FAILED;
  }
  return KillResult::KILLED;
}

// Resolved SSH config information for a host
struct ResolvedSshConfig {
  string hostname;  // Resolved HostName (or original if not an alias)
  string username;  // Username from SSH config (empty if not specified)
};

// Parse the selected SSH policy. An empty path preserves ET's legacy ambient
// behavior, "none" disables parsing, and every other value names the sole
// configuration file to read.
void parseSelectedSshConfig(const string& host, Options* options,
                            const string& sshConfigPath) {
  if (sshConfigPath == "none") {
    return;
  }
  if (!sshConfigPath.empty()) {
    parse_ssh_config_file(host.c_str(), options, sshConfigPath);
    return;
  }

  char* homeDir = ssh_get_user_home_dir();
  if (homeDir != NULL) {
    parse_ssh_config_file(host.c_str(), options,
                          string(homeDir) + USER_SSH_CONFIG_PATH);
    free(homeDir);
  }
  parse_ssh_config_file(host.c_str(), options, SYSTEM_SSH_CONFIG_PATH);
}

// Resolve a host alias via SSH config lookup
ResolvedSshConfig resolveSshConfigHost(const string& hostAlias,
                                       const string& sshConfigPath) {
  ResolvedSshConfig result;
  result.hostname = hostAlias;  // Default to original if not resolved

  Options opts = {NULL, NULL, NULL, NULL, NULL, NULL, 0,    0, 0,
                  0,    0,    NULL, NULL, 0,    0,    NULL, {}};

  ssh_options_set(&opts, SSH_OPTIONS_HOST, hostAlias.c_str());
  parseSelectedSshConfig(hostAlias, &opts, sshConfigPath);

  if (opts.host) {
    result.hostname = string(opts.host);
  }
  if (opts.username) {
    result.username = string(opts.username);
  }

  freeOptionsFields(&opts);
  return result;
}

int main(int argc, char** argv) {
  WinsockContext context;
  string tmpDir = GetTempDirectory();

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);
  LogHandler::setupStdoutLogger();

  et::HandleTerminate();

  // Empty when the session is not saved.
  string sessionName = "";
  bool sessionEndedByServer = false;

  // Override easylogging handler for sigint
  ::signal(SIGINT, et::InterruptSignalHandler);

  Options sshConfigOptions = {
      NULL,  // username
      NULL,  // host
      NULL,  // sshdir
      NULL,  // knownhosts
      NULL,  // ProxyCommand
      NULL,  // ProxyJump
      0,     // timeout
      0,     // port
      0,     // StrictHostKeyChecking
      0,     // ssh2
      0,     // ssh1
      NULL,  // gss_server_identity
      NULL,  // gss_client_identity
      0,     // gss_delegate_creds
      0,     // forward_agent
      NULL,  // identity_agent
      {}     // local_forwards (empty vector)
  };

  // Parse command line arguments
  cxxopts::Options options("et", "Remote shell for the busy and impatient");
  try {
    options.allow_unrecognised_options();
    options.positional_help("");
    options.custom_help(
        "[OPTION...] [user@]host[:port] [command...]\n\n"
        "  Note that 'host' can be a hostname or ipv4 address with or without "
        "a port\n  or an ipv6 address. If the ipv6 address is abbreviated with "
        ":: then it must\n  be specified without a port (use -p,--port).\n"
        "  A positional command after the host is equivalent to -c/--command "
        "(ssh-style).");

    options.add_options()             //
        ("h,help", "Print help")      //
        ("version", "Print version")  //
        ("u,username", "Username",
         cxxopts::value<std::string>())  //
        ("host", "Remote host name",
         cxxopts::value<std::string>())  //
        ("p,port", "Remote machine etserver port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("c,command", "Run command on connect and exit after command is run",
         cxxopts::value<std::string>())  //
        ("e,noexit",
         "Used together with -c to not exit after command is run")  //
        ("terminal-path",
         "Path to etterminal on server side. "
         "Use if etterminal is not on the system path.",
         cxxopts::value<std::string>())  //
        ("t,tunnel",
         "Tunnel: Array of source:destination ports or "
         "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
         "10080:80,10443:443, 10090-10092:8000-8002), ssh-style -L/-R "
         "argument, or Unix socket paths (e.g. "
         "/tmp/local.sock:/tmp/remote.sock, 8080:/tmp/remote.sock, "
         "/tmp/local.sock:8080). Defaults to localhost for bind address "
         "unless ssh-style tunnel argument is used.",
         cxxopts::value<std::string>())  //
        ("r,reversetunnel",
         "Reverse Tunnel: Same syntax as -t/--tunnel but reversed.",
         cxxopts::value<std::string>())  //
        ("j,jumphost", "jumphost between localhost and destination",
         cxxopts::value<std::string>())  //
        ("jport", "Jumphost machine port",
         cxxopts::value<int>()->default_value("2022"))  //
        ("jserverfifo",
         "If set, communicate to jumphost on the matching fifo name",
         cxxopts::value<string>()->default_value(""))  //
        ("x,kill-other-sessions",
         "kill all old sessions belonging to the user")  //
        ("close-on-hangup",
         "terminate the remote session when this terminal receives SIGHUP or "
         "closes")  //
        ("macserver",
         "Set when connecting to an macOS server.  Sets "
         "--terminal-path=/usr/local/bin/etterminal")  //
        ("v,verbose", "Enable verbose logging",
         cxxopts::value<int>()->default_value("0"))  //
        ("k,keepalive", "Client keepalive duration in seconds",
         cxxopts::value<int>())  //
        ("l,logdir", "Base directory for log files.",
         cxxopts::value<std::string>()->default_value(tmpDir))  //
        ("logtostdout", "Write log to stdout")                  //
        ("silent", "Disable logging")                           //
        ("N,no-terminal", "Do not create a terminal")           //
        ("T,no-pty",
         "Run -c command on pipes instead of a pty (binary stdio, "
         "separate stderr, no shell injection)")  //
        ("ctl",
         "Run as a background control session driven by etctl (no local "
         "terminal); name it with --name, relocate its socket with "
         "--ctl-socket. With -c/--command, that command runs once on "
         "connect and the session stays alive.")  //
        ("ctl-socket",
         "Path for the --ctl socket (default ~/.et/control/<name>.sock)",
         cxxopts::value<std::string>())                      //
        ("f,forward-ssh-agent", "Forward ssh-agent socket")  //
        ("ssh-socket", "The ssh-agent socket to forward",
         cxxopts::value<std::string>())  //
        ("ssh-config",
         "Read only this absolute SSH configuration file (or 'none')",
         cxxopts::value<std::string>())  //
        ("no-ssh-config",
         "Do not read user or system SSH configuration for the destination "
         "or jumphost")  //
        ("telemetry",
         "Allow et to anonymously send errors to guide future improvements",
         cxxopts::value<bool>()->default_value("true"))  //
        ("name", "Name this session so it can be reattached later",
         cxxopts::value<std::string>())                             //
        ("no-persist", "Do not save credentials for this session")  //
        ("attach", "Reattach by session name or unique title substring",
         cxxopts::value<std::string>())  //
        ("kill", "End a saved session by name or unique title substring",
         cxxopts::value<std::string>())           //
        ("list", "List saved sessions and exit")  //
        ("serverfifo",
         "If set, communicate to etserver on the matching fifo name",
         cxxopts::value<std::string>()->default_value(""))  //
        ("ssh-option", "Options to pass down to `ssh -o`",
         cxxopts::value<std::vector<std::string>>());

    options.parse_positional({"host"});
    vector<string> rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      rawArgs.emplace_back(argv[i]);
    }
    EtArgvSplit argvSplit = splitEtArgvAtHost(rawArgs);
    vector<char*> clientArgv;
    clientArgv.reserve(argvSplit.clientArgs.size());
    for (auto& arg : argvSplit.clientArgs) {
      clientArgv.push_back(&arg[0]);
    }
    auto result =
        options.parse(static_cast<int>(clientArgv.size()), clientArgv.data());
    TerminalClient::configureCloseOnHangup(result.count("close-on-hangup"));
    if (result.count("close-on-hangup")) {
#ifdef WIN32
      SetConsoleCtrlHandler(TerminalClient::consoleCtrlHandler, TRUE);
#else
      ::signal(SIGHUP, TerminalClient::requestHangupClose);
#endif
    }

    if (result.count("help")) {
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (result.count("version")) {
      CLOG(INFO, "stdout") << "et version " << ET_VERSION << endl;
      exit(0);
    }

    if (result.count("kill") &&
        (result.count("name") || result.count("attach") ||
         result.count("list") || result.count("host"))) {
      CLOG(INFO, "stdout")
          << "--kill takes a saved session name; it cannot be combined with "
             "--name, --attach, --list, or a host"
          << endl;
      exit(1);
    }

    if (result.count("no-persist") &&
        (result.count("name") || result.count("attach") ||
         result.count("kill"))) {
      CLOG(INFO, "stdout")
          << "--no-persist cannot be combined with --name, --attach, or "
             "--kill"
          << endl;
      exit(1);
    }

    if (result.count("list")) {
      CLOG(INFO, "stdout") << left << setw(24) << "NAME" << ' ' << setw(34)
                           << "TITLE" << ' ' << setw(24) << "HOST" << ' '
                           << setw(8) << "PORT" << ' ' << "LAST SEEN" << endl;
      const int64_t now = static_cast<int64_t>(time(NULL));
      for (const auto& session : listSessions()) {
        CLOG(INFO, "stdout")
            << left << setw(24) << session.name << ' ' << setw(34)
            << displayTitle(session.title) << ' ' << setw(24) << session.host
            << ' ' << setw(8) << session.port << ' '
            << formatLastSeen(session.lastSeenAt, now) << endl;
      }
      exit(0);
    }

    if (result.count("attach") &&
        (result.count("name") || result.count("host"))) {
      CLOG(INFO, "stdout") << "--attach takes a session name; it cannot be "
                              "combined with --name or a host"
                           << endl;
      exit(1);
    }
    if (result.count("attach") &&
        (result.count("tunnel") || result.count("reversetunnel") ||
         result.count("forward-ssh-agent") || result.count("jumphost"))) {
      CLOG(INFO, "stdout")
          << "--attach cannot be combined with -t, -r, -f, or -j; reconnect "
             "without --attach to establish forwarding or a jumphost"
          << endl;
      exit(1);
    }

    if (result.count("T") && (result.count("name") || result.count("attach"))) {
      CLOG(INFO, "stdout")
          << "-T/--no-pty sessions are not saved and cannot be named or "
             "reattached; drop --name/--attach"
          << endl;
      exit(1);
    }

    el::Loggers::setVerboseLevel(result["verbose"].as<int>());

    // silent Flag, since etclient doesn't read /etc/et.cfg file
    if (result.count("silent")) {
      defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
    }

    LogHandler::setupLogFiles(&defaultConf, result["logdir"].as<string>(),
                              "etclient", result.count("logtostdout"),
                              !result.count("logtostdout"));

    el::Loggers::reconfigureLogger("default", defaultConf);
    // set thread name
    el::Helpers::setThreadName("client-main");

    // Install log rotation callback
    el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    srand(1);

    TelemetryService::create(result["telemetry"].as<bool>(),
                             tmpDir + "/.sentry-native-et", "Client");

    if (result.count("kill")) {
      const optional<SessionInfo> session =
          resolveSavedSession(result["kill"].as<string>());
      if (!session) {
        exit(1);
      }
      const KillResult killResult = killSavedSession(*session);
      if (killResult == KillResult::INVALID_SESSION) {
        if (!deleteSavedSession(session->name)) {
          exit(1);
        }
        CLOG(INFO, "stdout")
            << "Session '" << session->name
            << "' was already gone; removed stale record" << endl;
        exit(0);
      }
      if (killResult == KillResult::FAILED) {
        CLOG(INFO, "stdout") << "Session '" << session->name
                             << "' was not removed; retry --kill or delete "
                                "~/.et/sessions/"
                             << session->name << " manually" << endl;
        exit(1);
      }
      if (!deleteSavedSession(session->name)) {
        exit(1);
      }
      CLOG(INFO, "stdout") << "Killed session '" << session->name << "'"
                           << endl;
      exit(0);
    }

    if (result.count("attach")) {
      const optional<SessionInfo> session =
          resolveSavedSession(result["attach"].as<string>());
      if (!session) {
        exit(1);
      }
      const string attachName = session->name;

      int attachKeepalive = extractSingleOptionWithDefault<int>(
          result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
      const AttachResult attachResult = attachSavedSession(
          attachName, *session,
          result.count("command") ? result["command"].as<string>() : "",
          result.count("noexit"), result.count("N"), attachKeepalive);
      if (attachResult == AttachResult::INVALID_SESSION) {
        deleteSavedSession(attachName);
        CLOG(INFO, "stdout")
            << "Session '" << attachName << "' is no longer running on "
            << session->host << endl;
        exit(1);
      }
      if (attachResult == AttachResult::FAILED) {
        exit(1);
      }
      exit(0);
    }
    string username = "";
    if (result.count("username")) {
      username = result["username"].as<string>();
    }
    int destinationPort = result["port"].as<int>();
    string destinationHost;

    // Parse command-line argument
    if (!result.count("host")) {
      CLOG(INFO, "stdout") << "Missing host to connect to" << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }
    string host_arg = result["host"].as<std::string>();
    ParsedEtDestination parsedDestination;
    try {
      parsedDestination = parseEtDestinationHost(host_arg);
    } catch (const std::invalid_argument&) {
      CLOG(INFO, "stdout") << "Invalid host positional arg: " << host_arg
                           << endl;
      exit(1);
    }
    if (!parsedDestination.username.empty()) {
      username = parsedDestination.username;
    }
    if (parsedDestination.hasExplicitPort) {
      destinationPort = parsedDestination.port;
    }
    destinationHost = parsedDestination.host;
    // host_alias is used for the initiating ssh call, if sshd runs on a port
    // other than 22, either configure your .ssh/config with an alias with an
    // overridden port or pass --ssh-option Port=<sshd_port>
    string host_alias = destinationHost;

    const bool jumphostSpecified = result.count("jumphost") > 0;
    string jumphost =
        extractSingleOptionWithDefault<string>(result, options, "jumphost", "");
    if (strcasecmp(jumphost.c_str(), "none") == 0) {
      jumphost.clear();
    }
    if (result.count("ssh-config") && result.count("no-ssh-config")) {
      CLOG(INFO, "stdout")
          << "--ssh-config and --no-ssh-config are mutually exclusive" << endl;
      exit(1);
    }
    string sshConfigPath;
    if (result.count("ssh-config")) {
      sshConfigPath = result["ssh-config"].as<string>();
      if (sshConfigPath != "none" &&
          !std::filesystem::path(sshConfigPath).is_absolute()) {
        CLOG(INFO, "stdout")
            << "--ssh-config must be an absolute path or 'none': "
            << sshConfigPath << endl;
        exit(1);
      }
      if (sshConfigPath != "none" &&
          !SshSetupHandler::IsSshConfigPathSafeForProxyJump(sshConfigPath)) {
        CLOG(INFO, "stdout")
            << "--ssh-config must contain only ASCII letters, digits, '/', "
#ifdef WIN32
               "'\\\\', ':', "
#endif
               "'.', '_', and '-'; OpenSSH does not quote this path when "
               "propagating it through ProxyJump"
            << endl;
        exit(1);
      }
      if (sshConfigPath != "none") {
        std::error_code configError;
        std::filesystem::file_status configStatus =
            std::filesystem::symlink_status(sshConfigPath, configError);
        bool regularFile = std::filesystem::is_regular_file(configStatus);
        bool readableFile = false;
        if (!configError && regularFile) {
          std::ifstream configFile(sshConfigPath);
          readableFile = configFile.good();
        }
        if (!readableFile) {
          CLOG(INFO, "stdout")
              << "--ssh-config must name a readable, non-symlink regular file"
              << endl;
          exit(1);
        }
      }
    } else if (result.count("no-ssh-config")) {
      sshConfigPath = "none";
    }
    bool noSshConfig = sshConfigPath == "none";
    int keepaliveDuration = extractSingleOptionWithDefault<int>(
        result, options, "keepalive", MAX_CLIENT_KEEP_ALIVE_DURATION);
    if (keepaliveDuration < 1 ||
        keepaliveDuration > MAX_CLIENT_KEEP_ALIVE_DURATION) {
      CLOG(INFO, "stdout") << "Keep-alive duration must between 1 and "
                           << MAX_CLIENT_KEEP_ALIVE_DURATION << " seconds"
                           << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(0);
    }

    if (!noSshConfig) {
      ssh_options_set(&sshConfigOptions, SSH_OPTIONS_HOST,
                      destinationHost.c_str());
      parseSelectedSshConfig(destinationHost, &sshConfigOptions, sshConfigPath);
      if (sshConfigOptions.host) {
        LOG(INFO) << "Parsed ssh config file, connecting to "
                  << sshConfigOptions.host;
        destinationHost = string(sshConfigOptions.host);
      }
    }

    // --name attaches if the session exists and creates it otherwise.
    optional<SessionInfo> namedSession;
    if (result.count("name")) {
      sessionName = result["name"].as<string>();
      if (!isValidSessionName(sessionName)) {
        CLOG(INFO, "stdout") << "Invalid session name: " << sessionName << endl;
        exit(1);
      }
#ifdef WIN32
      CLOG(INFO, "stdout")
          << "Warning: Session persistence is unavailable on Windows until "
             "owner-only credential storage is configured"
          << endl;
      sessionName.clear();
#else
      try {
        namedSession = loadSession(sessionName);
      } catch (const std::exception& e) {
        CLOG(INFO, "stdout")
            << "Warning: Named session storage is unavailable: " << e.what()
            << ". Continuing without saving this session." << endl;
        sessionName.clear();
      }
#endif
    }

    // Parse username: cmdline > sshconfig > localuser
    if (username.empty()) {
      if (sshConfigOptions.username) {
        username = string(sshConfigOptions.username);
      } else {
        char* usernamePtr = ssh_get_local_username();
        username = string(usernamePtr);
        SAFE_FREE(usernamePtr);
      }
    }

    // Parse jumphost: cmd > sshconfig
    if (!jumphostSpecified && sshConfigOptions.ProxyJump &&
        strcasecmp(sshConfigOptions.ProxyJump, "none") != 0 &&
        jumphost.length() == 0) {
      string proxyjump = string(sshConfigOptions.ProxyJump);
      // Keep full ProxyJump value including SSH port for ssh -J command
      jumphost = proxyjump;
      LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
    }

    bool is_jumphost = false;
    SocketEndpoint socketEndpoint;
    if (!jumphost.empty()) {
      is_jumphost = true;
      LOG(INFO) << "Setting port to jumphost port";

      // Parse [user@]host[:sshport] format
      ParsedHostString parsed = parseHostString(jumphost);

      // Resolve jumphost aliases only when SSH configuration is enabled.
      // In --no-ssh-config mode, keep the command-line host and user exact.
      ResolvedSshConfig resolved =
          resolveSshConfigHost(parsed.host, sshConfigPath);
      if (resolved.hostname != parsed.host) {
        LOG(INFO) << "Resolved jumphost alias '" << parsed.host
                  << "' to hostname: " << resolved.hostname;
      }

      // Determine username: command-line > SSH config > local user
      string jumphostUser = parsed.user;
      if (jumphostUser.empty() && !resolved.username.empty()) {
        jumphostUser = resolved.username;
        LOG(INFO) << "Using jumphost username from SSH config: "
                  << jumphostUser;
      }
      if (jumphostUser.empty() && !noSshConfig) {
        char* localUsernamePtr = ssh_get_local_username();
        jumphostUser = string(localUsernamePtr);
        SAFE_FREE(localUsernamePtr);
      }

      // Reconstruct jumphost with resolved hostname for SSH -J flag
      jumphost = (jumphostUser.empty() ? "" : jumphostUser + "@") +
                 resolved.hostname + parsed.portSuffix;

      socketEndpoint.set_name(resolved.hostname);
      socketEndpoint.set_port(result["jport"].as<int>());
    } else {
      socketEndpoint.set_name(destinationHost);
      socketEndpoint.set_port(destinationPort);
    }

    bool forwardingRequested =
        result.count("tunnel") || result.count("reversetunnel");
#ifndef WIN32
    forwardingRequested = forwardingRequested || result.count("f") ||
                          sshConfigOptions.forward_agent ||
                          !sshConfigOptions.local_forwards.empty();
#else
    forwardingRequested = forwardingRequested || result.count("f");
#endif

    if (is_jumphost) {
      if (namedSession) {
        CLOG(INFO, "stdout")
            << "Session '" << sessionName
            << "' cannot be reattached through a jumphost because the saved "
               "record does not contain jumphost metadata; use --attach "
               "without a jumphost"
            << endl;
        exit(1);
      }
      if (!result.count("no-persist")) {
        CLOG(INFO, "stdout")
            << "Warning: Sessions using a jumphost are not saved because the "
               "saved record does not contain jumphost metadata"
            << endl;
        sessionName.clear();
      }
    } else if (!result.count("name") && !result.count("no-persist") &&
               !result.count("T")) {
      // Neither --attach nor a restarted etserver can resume a -T stream.
#ifdef WIN32
      CLOG(INFO, "stdout")
          << "Warning: Session persistence is unavailable on Windows until "
             "owner-only credential storage is configured"
          << endl;
#else
      sessionName = makeDefaultSessionName();
#endif
    }

    if (forwardingRequested && !sessionName.empty()) {
      CLOG(INFO, "stdout")
          << "Warning: Saved-session reattach restores the shell but does "
             "not recreate port or SSH agent forwarding"
          << endl;
    }

    shared_ptr<Console> console;
    const bool noPty = result.count("T") > 0;
    string command = resolveRemoteCommand(
        argvSplit.commandOperands, result.count("command") > 0,
        result.count("command") ? result["command"].as<string>() : "");
    if (noPty && command.empty()) {
      CLOG(INFO, "stdout") << "-T/--no-pty requires -c/--command" << endl;
      CLOG(INFO, "stdout") << options.help({}) << endl;
      exit(1);
    }
    shared_ptr<ControlConsole> controlConsole;
    if (result.count("ctl")) {
      // Programmatic control mode: drive the session through a local socket
      // instead of a TTY.  ControlConsole is a Console, so TerminalClient is
      // unchanged.
      controlConsole.reset(new ControlConsole());
      console = controlConsole;
    } else if (!result.count("N")) {
      if (noPty) {
        console.reset(new BinaryStdioConsole());
      } else {
        console.reset(new PseudoTerminalConsole());
      }
    }

    /*
     * Resolve the control socket before either path connects, so a bad path
     * fails before a session exists, and announce it on the real stdout while
     * one still exists to announce on.  A persisted session is already named,
     * so the control socket takes that name and the two cannot drift apart;
     * only --no-persist needs a locally generated one.
     */
    string ctlName;
    string ctlSocketPath;
    std::function<void(TerminalClient&)> driveCtl;
    if (controlConsole) {
#ifdef WIN32
      CLOG(INFO, "stdout") << "--ctl is not supported on Windows" << endl;
      exit(1);
#else
      ctlName = sessionName.empty()
                    ? (destinationHost + "-" + genRandomHandle())
                    : sessionName;
      try {
        if (result.count("ctl-socket")) {
          // Explicit location: honor it verbatim, creating parent dirs as
          // needed. Such a session won't appear in `etctl sessions` (it lives
          // outside the control dir); address it by path.
          ctlSocketPath = result["ctl-socket"].as<string>();
          size_t slash = ctlSocketPath.find_last_of('/');
          if (slash != string::npos && slash > 0) {
            control_paths::mkdirp0700(ctlSocketPath.substr(0, slash));
          }
        } else {
          control_paths::ensureControlDir();
          ctlSocketPath = control_paths::socketPathForName(ctlName);
        }
      } catch (const std::exception& e) {
        CLOG(INFO, "stdout")
            << "Could not prepare control socket: " << e.what() << endl;
        exit(1);
      }
      // This name is starting, so whatever ended it last time no longer
      // applies; clear the note before anyone can read a stale one.
      session_tombstone::clear(ctlName);
      CLOG(INFO, "stdout") << "et control session: " << ctlName << endl;
      CLOG(INFO, "stdout") << "control socket: " << ctlSocketPath << endl;
      const string hostLabel = username.empty()
                                   ? destinationHost
                                   : (username + "@" + destinationHost);
      driveCtl = [&, hostLabel](TerminalClient& client) {
        runControlSession(client, controlConsole, ctlName, ctlSocketPath,
                          command, hostLabel);
      };
#endif
    }

    if (namedSession) {
      if (namedSession->host != socketEndpoint.name() ||
          namedSession->port != socketEndpoint.port()) {
        CLOG(INFO, "stdout") << "session " << sessionName << " is saved for "
                             << namedSession->host << ":" << namedSession->port
                             << "; use --attach " << sessionName
                             << " or a different --name" << endl;
        exit(1);
      }

      const AttachResult attachResult = attachSavedSession(
          sessionName, *namedSession, command, result.count("noexit"),
          result.count("N"), keepaliveDuration, console, driveCtl);
      if (attachResult == AttachResult::ATTACHED) {
        exit(0);
      }
      if (attachResult == AttachResult::FAILED) {
        exit(1);
      }

      deleteSavedSession(sessionName);
      CLOG(INFO, "stdout") << "Session '" << sessionName
                           << "' is no longer running; creating a fresh session"
                           << endl;
    }

    shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
    shared_ptr<SocketHandler> clientPipeSocket(new PipeSocketHandler());

    if (!ping(socketEndpoint, clientSocket)) {
      CLOG(INFO, "stdout") << "Could not reach the ET server: "
                           << socketEndpoint.name() << ":"
                           << socketEndpoint.port() << endl;
      exit(1);
    }

    string jServerFifo = "";
    if (result["jserverfifo"].as<string>() != "") {
      jServerFifo = result["jserverfifo"].as<string>();
    }

    string serverFifo = "";
    if (result["serverfifo"].as<string>() != "") {
      serverFifo = result["serverfifo"].as<string>();
    }
    std::vector<string> ssh_options;
    if (result.count("ssh-option")) {
      ssh_options = result["ssh-option"].as<std::vector<string>>();
    }
    string etterminal_path = "";
    if (result.count("macserver") > 0) {
      etterminal_path = "/usr/local/bin/etterminal";
    }
    if (result.count("terminal-path")) {
      etterminal_path = result["terminal-path"].as<string>();
    }

    bool forwardAgent = result.count("f") > 0;
    string sshSocket = "";
#ifndef WIN32
    if (sshConfigOptions.identity_agent) {
      sshSocket = string(sshConfigOptions.identity_agent);
    }
    forwardAgent |= sshConfigOptions.forward_agent;
#endif
    if (result.count("ssh-socket")) {
      sshSocket = result["ssh-socket"].as<string>();
    }
    TelemetryService::get()->logToDatadog("Session Started", el::Level::Info,
                                          __FILE__, __LINE__);
    string tunnel_arg =
        extractSingleOptionWithDefault<string>(result, options, "tunnel", "");
    string r_tunnel_arg = extractSingleOptionWithDefault<string>(
        result, options, "reversetunnel", "");

    for (const auto& localForward : sshConfigOptions.local_forwards) {
      string tunnelEntry =
          to_string(localForward.first) + ":" + to_string(localForward.second);
      LOG(INFO) << "Adding tunnel from SSH config LocalForward: "
                << tunnelEntry;
      if (tunnel_arg.empty()) {
        tunnel_arg = tunnelEntry;
      } else {
        tunnel_arg += "," + tunnelEntry;
      }
    }

    auto subprocessUtils = make_shared<SubprocessUtils>();
    SshSetupHandler sshSetupHandler(subprocessUtils, sshConfigPath);
    sshSetupHandler.setDisplayLoginOutput(console != nullptr);
    pair<string, string> idpasskeypair;
    try {
      idpasskeypair = sshSetupHandler.SetupSsh(
          username, destinationHost, host_alias, destinationPort, jumphost,
          jServerFifo, result.count("x") > 0, result["verbose"].as<int>(),
          etterminal_path, serverFifo, ssh_options);
    } catch (const runtime_error&) {
      // SetupSsh already printed a message without the ssh output.
      exit(1);
    }

    // Save before connecting so a local failure leaves the session
    // recoverable.
    if (!sessionName.empty()) {
      try {
        SessionInfo sessionInfo;
        sessionInfo.name = sessionName;
        sessionInfo.host = socketEndpoint.name();
        sessionInfo.port = socketEndpoint.port();
        sessionInfo.id = idpasskeypair.first;
        sessionInfo.passkey = idpasskeypair.second;
        sessionInfo.savedAt = (int64_t)time(NULL);
        saveSession(sessionInfo, /*replaceExisting=*/false);
      } catch (const std::exception& se) {
        LOG(WARNING) << "Could not save session '" << sessionName
                     << "': " << se.what();
        CLOG(INFO, "stdout")
            << "Warning: Could not save session '" << sessionName
            << "': this connection will not be recoverable "
               "after the client exits"
            << endl;
        sessionName = "";
      }
    }

    TerminalClient terminalClient(
        clientSocket, clientPipeSocket, socketEndpoint, idpasskeypair.first,
        idpasskeypair.second, console, is_jumphost, tunnel_arg, r_tunnel_arg,
        forwardAgent, sshSocket, keepaliveDuration, sshConfigOptions.env_vars,
        noPty, command, /*maxConnectAttempts=*/3,
        /*resumeSavedSession=*/false,
        [&sessionName]() {
          return sessionName.empty() || touchSession(sessionName);
        },
        [&sessionName](const string& title) {
          return sessionName.empty() || updateSessionTitle(sessionName, title);
        });

    if (driveCtl) {
      driveCtl(terminalClient);
      sessionEndedByServer = terminalClient.sessionEndedByServer();
    } else {
      terminalClient.run(command, result.count("noexit"));
      sessionEndedByServer = terminalClient.sessionEndedByServer();
    }
  } catch (TunnelParseException& tpe) {
    handleParseException(tpe, options);
  } catch (cxxopts::exceptions::exception& oe) {
    handleParseException(oe, options);
  }

  // Clean up ssh config options
  freeOptionsFields(&sshConfigOptions);

#ifdef WIN32
  WSACleanup();
#endif

  TelemetryService::get()->shutdown();
  TelemetryService::destroy();

  // Any other exit leaves the remote shell running and reattachable.
  if (!sessionName.empty() && sessionEndedByServer) {
    deleteSavedSession(sessionName);
  }

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();

  return 0;
}
