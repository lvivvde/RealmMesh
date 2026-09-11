// 把自身放进新的会话后再 exec 目标命令,供缺少 setsid(1) 的平台(macOS)
// 替代它:因为 exec 会替换本进程,调用方记录的 PID 就是目标进程本身,
// 服务组的进程归属与会话隔离语义与 Linux 上一致。
//
// 刻意不 fork:util-linux 的 setsid(1) 在调用方已是进程组组长时会先 fork,
// 但那样 $! 会指向立即退出的中间进程,PID 归属就被破坏了——而"$! 即服务
// 进程"是 scripts/dev-*.sh 的既有契约。这里是脚本以 nohup ... & 拉起的
// 非交互场景(无作业控制,子进程不是组长),setsid 失败时直接报错退出,
// 绝不静默继续。
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
