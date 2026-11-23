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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    /* An optional folder as the Daisy's SD card (second argument): sets can be saved and loaded */
    std::string root;
    std::string Full(const char* p) { return root + "/" + p; }
    FILE*       rf = nullptr;
    FILE*       wf = nullptr;
    DIR*        d  = nullptr;
    std::string dpath;
    bool     FileOpen(const char* p) override { return !root.empty() && (rf = fopen(Full(p).c_str(), "rb")) != nullptr; }
    int32_t  FileRead(void* dst, uint32_t n) override { return rf ? (int32_t)fread(dst, 1, n, rf) : -1; }
    bool     FileSkip(uint32_t n) override { return rf && fseek(rf, n, SEEK_CUR) == 0; }
    uint32_t FileTell() override { return rf ? (uint32_t)ftell(rf) : 0; }
    void     FileClose() override
    {
        if(rf)
            fclose(rf);
        rf = nullptr;
    }
    bool FileCreate(const char* dd, const char* p) override
    {
        if(root.empty())
            return false;
        std::string acc;
        for(const char* q = dd; *q; q++)
        {
            acc += *q;
            if(q[1] == '/' || q[1] == 0)
                mkdir(Full(acc.c_str()).c_str(), 0755);
        }
        wf = fopen(Full(p).c_str(), "wb");
        return wf != nullptr;
    }
    int32_t FileWrite(const void* s, uint32_t n) override { return wf ? (int32_t)fwrite(s, 1, n, wf) : -1; }
    bool    FileWriteClose() override
    {
        bool ok = wf && fclose(wf) == 0;
        wf      = nullptr;
        return ok;
    }
    bool Remove(const char* p) override { return !root.empty() && ::remove(Full(p).c_str()) == 0; }
    bool Rename(const char* a, const char* b) override { return !root.empty() && ::rename(Full(a).c_str(), Full(b).c_str()) == 0; }
    uint8_t* Scratch(uint32_t* s) override
    {
        *s = sizeof(scratch);
        return scratch;
    }
    bool DirOpen(const char* p) override
    {
        if(root.empty())
            return false;
        dpath = Full(p);
        return (d = opendir(dpath.c_str())) != nullptr;
    }
    bool DirNext(char* name, size_t cap, bool* is_dir) override
    {
        dirent* e;
        do
        {
            e = d ? readdir(d) : nullptr;
        } while(e && e->d_name[0] == '.');
        if(!e)
            return false;
        snprintf(name, cap, "%s", e->d_name);
        struct stat sb;
        *is_dir = stat((dpath + "/" + e->d_name).c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
        return true;
    }
    void DirClose() override
    {
        if(d)
            closedir(d);
        d = nullptr;
    }
    bool WriteFile(const char* dd, const char* p, const void* data, uint32_t n) override
    {
        if(root.empty())
            return false;
        mkdir(Full(dd).c_str(), 0755);
        FILE* f = fopen(Full(p).c_str(), "wb");
        if(!f)
            return false;
        bool ok = fwrite(data, 1, n, f) == n;
        fclose(f);
        return ok;
    }
    int32_t ReadFile(const char* p, void* data, uint32_t cap) override
    {
        if(root.empty())
            return -1;
        FILE* f = fopen(Full(p).c_str(), "rb");
        if(!f)
            return -1;
        int32_t r = (int32_t)fread(data, 1, cap, f);
        fclose(f);
        return r;
    }
    float    CpuAvg() override { return 0.1f; }
    float    CpuMax() override { return 0.2f; }
    bool     ControllerUp() override;
    bool     ControllerPresent() override;
    uint32_t Dropped() override { return 0; }
    bool     AudioRunning() override { return true; }
    int16_t* UndoMem(uint32_t* n) override
    {
        static std::vector<int16_t> u(1536 * 1024);
        *n = (uint32_t)u.size();
        return u.data();
    }
    int16_t* Staging(uint32_t* n) override // like the Daisy's (SDRAM there)
    {
        static std::vector<int16_t> stage(4600 * 1024);
        *n = (uint32_t)stage.size();
        return stage.data();
    }
    uint8_t  scratch[16384];
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
        fprintf(stderr, "usage: t95sim <symlink path> [folder to use as the SD card]\n");
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
    if(argc > 2)
        sim_plat.root = argv[2];
    BoxInit(&sim_plat, &eng_);
    BoxStorageReady(false);
    t95.Init(WriteLine);
    WriteLine("L t95sim start");

    float    outl[48], outr[48], in[48] = {};
    uint32_t last = NowMs();
    const bool tone = getenv("T95SIM_INPUT") != nullptr;
    uint32_t   tone_n = 0;
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
            if(tone) // T95SIM_INPUT=1: something on the audio input (a plucked 220 Hz twice a second)
                for(int k = 0; k < 48; k++, tone_n++)
                    in[k] = 0.5f * expf(-(float)(tone_n % 24000) / 6000.f) * sinf(6.2831853f * 220.f * (float)(tone_n % 48000) / 48000.f);
            eng_.SetInput(in, in);
            eng_.Process(outl, outr, 48);
            last++;
        }
        BoxPoll(now);
        t95.Poll(now, false);
    }
}
