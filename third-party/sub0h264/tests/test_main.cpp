/** Sub0h264 test entry point.
 *
 *  Registers a crash handler (Windows: SetUnhandledExceptionFilter;
 *  POSIX: sigaction for SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL) that prints
 *  the currently-running test case name and a human-readable exception
 *  description before the process dies. This makes STATUS_STACK_BUFFER_OVERRUN
 *  (0xC0000409) and friends diagnosable from CTest output alone.
 */
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdio>
#include <cstring>

// ── Platform crash handler ────────────────────────────────────────────────
// Stores the name of the currently-executing test case so the handler can
// report it.  Written by the listener below, read by the crash handler.

static const char* g_currentTestName = "(no test running)";

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const char* exceptionName(DWORD code) noexcept
{
    switch (code)
    {
    case 0xC0000005UL: return "ACCESS_VIOLATION";
    case 0xC0000094UL: return "INTEGER_DIVIDE_BY_ZERO";
    case 0xC0000095UL: return "INTEGER_OVERFLOW";
    case 0xC00000FDUL: return "STACK_OVERFLOW";
    case 0xC0000409UL: return "STACK_BUFFER_OVERRUN";
    case 0xC000001DUL: return "ILLEGAL_INSTRUCTION";
    case 0xC0000025UL: return "NONCONTINUABLE_EXCEPTION";
    case 0x80000001UL: return "GUARD_PAGE";
    default:           return "UNKNOWN";
    }
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) noexcept
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    std::fprintf(stderr,
        "\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
        "CRASH while running test case:\n"
        "  \"%s\"\n"
        "Exception code: 0x%08X  (%s)\n"
        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
        g_currentTestName,
        static_cast<unsigned int>(code),
        exceptionName(code));
    std::fflush(stderr);
    // Return CONTINUE_SEARCH so Windows reports the crash normally (exit code
    // encodes the exception code, which CTest captures).
    return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <unistd.h>

static const char* signalName(int sig) noexcept
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE:  return "SIGFPE (floating point exception)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    default:      return "unknown signal";
    }
}

static void posixCrashHandler(int sig) noexcept
{
    // Avoid stdio in a signal handler where possible; write() is async-safe.
    static const char header[] = "\n!!!!!!!!!!! CRASH !!!!!!!!!!!\nTest: ";
    write(STDERR_FILENO, header, sizeof(header) - 1U);
    write(STDERR_FILENO, g_currentTestName, std::strlen(g_currentTestName));
    const char* desc = signalName(sig);
    write(STDERR_FILENO, "\nSignal: ", 9U);
    write(STDERR_FILENO, desc, std::strlen(desc));
    write(STDERR_FILENO, "\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", 32U);
    // Re-raise with default handler so the exit code reflects the signal.
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}
#endif // platform

// ── Doctest listener: tracks current test name ───────────────────────────

class CrashReporterListener : public doctest::IReporter
{
public:
    explicit CrashReporterListener(const doctest::ContextOptions& /*opts*/) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& data) override
    {
        g_currentTestName = data.m_name;
    }
    void test_case_reenter(const doctest::TestCaseData& data) override
    {
        g_currentTestName = data.m_name;
    }
    void test_case_end(const doctest::CurrentTestCaseStats&) override
    {
        g_currentTestName = "(between tests)";
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

DOCTEST_REGISTER_REPORTER("crash_listener", 0, CrashReporterListener);

// ── main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    // Install crash handler before running any tests.
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#elif defined(__unix__) || defined(__APPLE__)
    struct sigaction sa{};
    sa.sa_handler = posixCrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
#endif

    doctest::Context context;
    context.applyCommandLine(argc, argv);

    // Always activate the crash listener regardless of --reporters arg.
    // It's a silent listener (produces no output on success).
    context.setOption("reporters", "crash_listener,console");

    return context.run();
}
