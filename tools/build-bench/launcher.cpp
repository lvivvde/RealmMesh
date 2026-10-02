// 构建测量 launcher：作为 CMAKE_<LANG>_{COMPILER,LINKER}_LAUNCHER 包住每次真实编译/链接，
// 原样执行 argv 并返回其退出码；设置 REALMMESH_BUILD_BENCH_EVENTS 时，向该目录写一条事件 JSON
// （argv、cwd、墙钟、退出码、user/sys 时间、最大 RSS）。用法见同目录 README.md。
#include <chrono>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static std::string quote(const char* value) {
    std::string out = "\"";
    for (const char c : std::string(value)) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out + '"';
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto pid = fork();
    if (pid == 0) { execvp(argv[1], argv + 1); _exit(127); }
    if (pid < 0) return 127;
    int status = 0;
    struct rusage usage{};
    while (wait4(pid, &status, 0, &usage) < 0) {}
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const int result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (const char* directory = std::getenv("REALMMESH_BUILD_BENCH_EVENTS")) {
#ifdef __APPLE__
        const long maxrss_kib = static_cast<long>(usage.ru_maxrss / 1024);  // macOS 以字节计
#else
        const long maxrss_kib = static_cast<long>(usage.ru_maxrss);  // Linux 以 KiB 计
#endif
        char cwd[PATH_MAX] = "";
        if (getcwd(cwd, sizeof(cwd)) == nullptr) cwd[0] = '\0';
        const auto stamp = start.time_since_epoch().count();
        std::ofstream file(std::string(directory) + "/" + std::to_string(getpid()) + "-" + std::to_string(stamp) + ".json");
        file.precision(12);
        file << "{\"wall_s\":" << seconds << ",\"exit\":" << result
             << ",\"maxrss_kib\":" << maxrss_kib
             << ",\"user_s\":" << static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1e6
             << ",\"sys_s\":" << static_cast<double>(usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_stime.tv_usec) / 1e6
             << ",\"cwd\":" << quote(cwd)
             << ",\"argv\":[";
        for (int i = 1; i < argc; ++i) { if (i > 1) file << ','; file << quote(argv[i]); }
        file << "]}";
    }
    return result;
}
