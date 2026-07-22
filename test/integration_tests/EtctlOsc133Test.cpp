// End-to-end OSC-133 test: drive the real etctl binary against a real bash
// running interactively behind a pty, with a minimal but faithful FinalTerm/
// iTerm2 shell integration (DEBUG trap emits C on the first command of a
// prompt, PROMPT_COMMAND emits D;$? at the next prompt). Unlike EtctlRunTest --
// which wires bash through plain pipes and therefore exercises the echo-marker
// fallback -- a pty lets bash's prompt hooks fire, so this verifies the OSC-133
// framing path against a genuine integration: clean output, correct exit codes,
// and no injected marker leakage. Skipped if etctl or bash is unavailable.
#include <atomic>
#include <thread>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>

#include "ControlConsole.hpp"
#include "ControlListener.hpp"
#include "ControlPaths.hpp"
#include "RawSocketUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;

namespace {

string etctlBin() {
  if (const char* env = getenv("ETCTL_BIN")) return string(env);
  return "./etctl";
}

string bashPath() {
  if (::access("/bin/bash", X_OK) == 0) return "/bin/bash";
  if (::access("/usr/bin/bash", X_OK) == 0) return "/usr/bin/bash";
  if (::access("/opt/homebrew/bin/bash", X_OK) == 0)
    return "/opt/homebrew/bin/bash";
  return "";
}

struct RunResult {
  string out;
  int code;
};

RunResult runEtctl(const string& args) {
  const string cmd = etctlBin() + " " + args + " 2>&1";
  RunResult r;
  FILE* p = popen(cmd.c_str(), "r");
  REQUIRE(p != nullptr);
  std::array<char, 4096> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), p)) > 0)
    r.out.append(buf.data(), n);
  int status = pclose(p);
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

// A minimal FinalTerm/iTerm2-style bash integration: C once per command, then
// D;<exit> at the following prompt. Written to a temp rcfile bash reads under
// -i.
const char* kBashOsc133Rc =
    "PS1='$ '\n"
    "set +m\n"
    "__et_pre=\n"
    "__et_pc() { local e=$?; printf '\\033]133;D;%s\\007' \"$e\"; __et_pre=; "
    "}\n"
    "__et_dbg() {\n"
    "  [ -n \"$__et_pre\" ] && return 0\n"
    "  [ \"$BASH_COMMAND\" = \"__et_pc\" ] && return 0\n"
    "  __et_pre=1\n"
    "  printf '\\033]133;C\\007'\n"
    "}\n"
    "PROMPT_COMMAND=__et_pc\n"
    "trap '__et_dbg' DEBUG\n";

}  // namespace

TEST_CASE("EtctlRunOsc133AgainstRealShell", "[EtctlOsc133]") {
  if (::access(etctlBin().c_str(), X_OK) != 0 || bashPath().empty()) {
    WARN("etctl or bash unavailable; skipping OSC-133 pty test");
    SUCCEED();
    return;
  }

  char rcPath[] = "/tmp/etctl_osc133_rc_XXXXXX";
  int rcFd = ::mkstemp(rcPath);
  REQUIRE(rcFd >= 0);
  RawSocketUtils::writeAll(rcFd, kBashOsc133Rc, strlen(kBashOsc133Rc));
  ::close(rcFd);

  int masterFd = -1;
  pid_t pid = forkpty(&masterFd, nullptr, nullptr, nullptr);
  REQUIRE(pid >= 0);
  if (pid == 0) {
    setenv("TERM", "xterm", 1);
    execl(bashPath().c_str(), "bash", "--rcfile", rcPath, "--noprofile", "-i",
          (char*)nullptr);
    _exit(127);
  }
  int flags = fcntl(masterFd, F_GETFL, 0);
  fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);

  const string name = "etctlosc_" + std::to_string(::getpid());
  control_paths::ensureControlDir();
  const string socketPath = control_paths::socketPathForName(name);

  auto console = std::make_shared<ControlConsole>();
  std::atomic<bool> done{false};
  ControlListener listener(console, socketPath, [&]() { done = true; });
  listener.start();

  // Relay A: injected input (etctl write) -> pty master (bash stdin).
  std::thread inRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(console->getFd(), buf, sizeof(buf));
        if (n > 0) {
          RawSocketUtils::writeAll(masterFd, buf, n);
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    } catch (const std::exception&) {
    }
  });
  // Relay B: pty master (bash output) -> scrollback (what etctl read sees).
  std::thread outRelay([&]() {
    try {
      while (!done) {
        char buf[4096];
        ssize_t n = ::read(masterFd, buf, sizeof(buf));
        if (n > 0) {
          console->write(string(buf, n));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    } catch (const std::exception&) {
    }
  });

  // Let bash finish sourcing the rcfile and print its first prompt.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  {
    // Auto-detect: no flag. The prompt speaks OSC 133, so run must frame via it
    // -- clean output, no injected ETCTL_ markers in the scrollback.
    RunResult r = runEtctl("run " + name + " 'echo hello_osc' --timeout 10");
    INFO("auto -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
    CHECK(r.out.find("hello_osc") != string::npos);
    CHECK(r.out.find("ETCTL_") == string::npos);
  }
  {
    // Forced OSC-133 framing, exit code straight from D.
    RunResult r = runEtctl("run --osc133 " + name + " '(exit 7)' --timeout 10");
    INFO("exit7 -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 7);
    CHECK(r.out.find("ETCTL_") == string::npos);
  }
  {
    // No trailing newline: body is exactly the output, prompt-prep trimmed.
    RunResult r =
        runEtctl("run --osc133 " + name + " 'printf foo' --timeout 10");
    INFO("printf -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 0);
    CHECK(r.out == "foo");
  }
  {
    // Multi-digit exit code parsed whole from D.
    RunResult r =
        runEtctl("run --osc133 " + name + " '(exit 137)' --timeout 10");
    INFO("exit137 -> code=" << r.code << " out=[" << r.out << "]");
    CHECK(r.code == 137);
  }

  done = true;
  inRelay.join();
  outRelay.join();
  listener.shutdown();
  ::close(masterFd);
  ::kill(pid, SIGTERM);
  int st;
  ::waitpid(pid, &st, 0);
  ::unlink(rcPath);
}
