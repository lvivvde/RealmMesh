// 把自身放进新的会话后再 exec 目标命令,语义等同于 util-linux 的 setsid(1)。
//
// 开发脚本用 `nohup <detach> <cmd> ... &` 拉起服务,在缺少 setsid(1) 的平台
// (macOS)上替代它:因为 exec 会替换本进程,调用方记录的 PID 就是目标进程本身,
// 服务组的进程归属与会话隔离语义与 Linux 上一致。
#include <cstdio>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: realm_detach <command> [args...]\n");
        return 2;
    }
    if (::setsid() < 0) {
        std::perror("realm_detach: setsid");
        return 1;
    }
    ::execvp(argv[1], &argv[1]);
    std::perror("realm_detach: execvp");
    return 127;
}
