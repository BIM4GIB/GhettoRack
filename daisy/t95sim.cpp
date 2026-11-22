/** The Daisy's control side on a desktop, for testing the T95 bridge end to end.
 *
 *  Runs the real box.cpp, engine, HostCmd parser and T95Ports routing (no
 *  audio device, no SD card) behind a pseudo-terminal, and puts a symlink to
 *  it at the path given. t95/bridge/test_bridge.py (EndToEndTest) starts it
 *  and points the bridge at that path. Not the USB stack, not the hardware.
 *
 *  Build: see build_host.sh, or
 *    g++ -O1 -std=c++17 -DUSE_DAISYSP_LGPL $INC engine.cpp t95sim.cpp $SRCS -o t95sim
 *  Run:   ./t95sim /tmp/somewhere/daisy
 */
#include "box.cpp"
#include "ports.h"
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

using namespace ab;

static int g_fd = -1;

static bool WriteLine(const char* l)
{
    std::string s = std::string(l) + "\n";
    size_t      o = 0;
    while(o < s.size())
    {
        ssize_t n = write(g_fd, s.data() + o, s.size() - o);
        if(n < 0)
        {
            if(errno == EAGAIN)
            {
                usleep(1000);
                continue;
            }
            return false;
        }
        o += (size_t)n;
    }
    return true;
}

class SimPlat : public Platform
{
  public:
    void     Out(const char* l) override { WriteLine(l); }
    bool     FileOpen(const char*) override { return false; }
    int32_t  FileRead(void*, uint32_t) override { return -1; }
    bool     FileSkip(uint32_t) override { return false; }
    uint32_t FileTell() override { return 0; }
    void     FileClose() override {}
    uint8_t* Scratch(uint32_t* s) override
    {
        *s = sizeof(scratch);
        return scratch;
    }
    bool    DirOpen(const char*) override { return false; }
    bool    DirNext(char*, size_t, bool*) override { return false; }
    void    DirClose() override {}
    bool    WriteFile(const char*, const char*, const void*, uint32_t) override { return false; }
    int32_t ReadFile(const char*, void*, uint32_t) override { return -1; }
    float    CpuAvg() override { return 0.1f; }
    float    CpuMax() override { return 0.2f; }
    bool     ControllerUp() override;
    bool     ControllerPresent() override;
    uint32_t Dropped() override { return 0; }
    bool     AudioRunning() override { return true; }
    uint8_t  scratch[4096];
};

static SimPlat  sim_plat;
static Engine   eng_;
static T95Ports t95;
static HostCmd  cmd;
bool SimPlat::ControllerUp() { return t95.ApcUp(); }
bool SimPlat::ControllerPresent() { return t95.AnyAttached(); }

static uint32_t NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "usage: t95sim <symlink path>\n");
        return 2;
    }
    int master, slave;
    if(openpty(&master, &slave, nullptr, nullptr, nullptr) < 0)
        return 1;
    termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    std::string tmp = std::string(argv[1]) + ".new";
    unlink(tmp.c_str());
    if(symlink(ttyname(slave), tmp.c_str()) < 0 || rename(tmp.c_str(), argv[1]) < 0)
        return 1;
    close(slave);
    g_fd = master;
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    static std::vector<int16_t> slots(1536 * 1024 * (size_t)kTracks);
    static std::vector<float>   dl(4800), dr(4800), cl(4800), cr(4800);
    static daisysp::ReverbSc    rv;
    Memory                      mem;
    for(int k = 0; k < kTracks; k++)
        mem.slot_mem[k] = slots.data() + (size_t)k * 1536 * 1024;
    mem.slot_capacity = 1536 * 1024;
    mem.delay_l = dl.data(), mem.delay_r = dr.data(), mem.delay_len = 4800;
    mem.capture_l = cl.data(), mem.capture_r = cr.data(), mem.capture_len = 4800;
    mem.reverb = &rv;
    eng_.Init(48000.f, mem);
    BoxInit(&sim_plat, &eng_);
    BoxStorageReady(false);
    t95.Init(WriteLine);
    WriteLine("L t95sim start");

    float    outl[48], outr[48], in[48] = {};
    uint32_t last = NowMs();
    for(;;)
    {
        /* Like main.cpp: `now` is read at the top of the pass, frames are
           stamped when parsed, a little later */
        uint32_t now = NowMs();
        usleep(1500);
        uint8_t buf[512];
        ssize_t n = read(master, buf, sizeof(buf));
        for(ssize_t i = 0; i < n; i++)
        {
            HostCmd::Cmd c = cmd.Feed(buf[i]);
            if(c == HostCmd::kResync)
            {
                BoxResync();
                t95.ForceReport();
            }
            else if(c > HostCmd::kBoot)
                t95.OnCmd(cmd, c, NowMs());
        }
        while(last != now) // 1 ms of audio per ms, like the Daisy
        {
            eng_.SetInput(in, in);
            eng_.Process(outl, outr, 48);
            last++;
        }
        BoxPoll(now);
        t95.Poll(now, false);
    }
}
