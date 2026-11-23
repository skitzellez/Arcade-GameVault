// 32-bit test program: threads (CreateThread through the C runtime's std::thread), each with its own thread-local data
#include <windows.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstdlib>
thread_local int t_mine = 0;
std::atomic<int> g_sum{0};
int main(int argc, char** argv) {
    int count = argc > 1 ? atoi(argv[1]) : 4;
    std::vector<std::thread> th;
    for (int i = 1; i <= count; i++)
        th.emplace_back([i] {
            t_mine = i * 100;
            long long s = 0;
            for (int k = 0; k < 2000000; k++) s += k % 7;
            Sleep(10);
            g_sum += t_mine + (int)(s % 5);
        });
    for (auto& t : th) t.join();
    char buf[128];
    sprintf_s(buf, "thread32: %d threads done, sum %d, main thread own value %d", count, g_sum.load(), t_mine);
    OutputDebugStringA(buf);
    return g_sum.load();
}
