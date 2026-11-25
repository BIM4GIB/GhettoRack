/** ApcBox on the Mac: the same engine and control code as the Daisy
 *  firmware, with the APC40 plugged into the Mac and sound out of the
 *  Mac's audio output.
 *
 *   ./build/apcbox [sample folder]      (default ~/ApcBox)
 *
 *  The sample folder is laid out like the SD card: kits/, samples/,
 *  apcbox/project.bin. Monitor page: http://localhost:8040
 */
#include "box.h"
#include "engine.h"
#include "RtMidi.h"
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ab;

static uint32_t NowMs()
{
    using namespace std::chrono;
    static auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

/* ================================================================== */
/* Monitor stream: lines fan out to every open /events connection      */

class LineHub
{
  public:
    void Push(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(m_);
        lines_.push_back(s);
        if(lines_.size() > kKeep)
        {
            lines_.pop_front();
            first_++;
        }
        cv_.notify_all();
    }
    /** Blocks until there is something after `cursor`; returns false on timeout */
    bool Wait(uint64_t& cursor, std::vector<std::string>& out)
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(2), [&] { return first_ + lines_.size() > cursor; });
        if(cursor < first_)
            cursor = first_; // too slow, skip ahead
        out.clear();
        for(uint64_t i = cursor; i < first_ + lines_.size(); i++)
            out.push_back(lines_[i - first_]);
        cursor = first_ + lines_.size();
        return !out.empty();
    }
    uint64_t End()
    {
        std::lock_guard<std::mutex> lk(m_);
        return first_ + lines_.size();
    }

  private:
    static constexpr size_t kKeep = 4000;
    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    uint64_t                first_ = 0;
};

LineHub           hub;
std::atomic<bool> resync_req{false};

/* MIDI injected from the page (/midi?t=&c=&a=&b=) for testing without hardware */
struct Injected
{
    int t, c, a, b;
};
std::mutex            inject_m;
std::vector<Injected> injected;

/* ================================================================== */
/* Platform: plain files under the sample folder                       */

class MacPlatform : public Platform
{
  public:
    std::string root;
    float       cpu_avg = 0.f, cpu_max = 0.f;
    bool        apc_up = false, audio = false;

    void Out(const char* line) override
    {
        hub.Push(line);
        if(line[0] == 'L' && line[1] == ' ')
            printf("%s\n", line + 2);
    }

    bool FileOpen(const char* path) override
    {
        file_ = fopen(Full(path).c_str(), "rb");
        return file_ != nullptr;
    }
    int32_t FileRead(void* dst, uint32_t n) override
    {
        if(!file_)
            return -1;
        return (int32_t)fread(dst, 1, n, file_);
    }
    bool FileSkip(uint32_t n) override
    {
        return file_ && fseek(file_, (long)n, SEEK_CUR) == 0;
    }
    uint32_t FileTell() override { return file_ ? (uint32_t)ftell(file_) : 0; }
    void     FileClose() override
    {
        if(file_)
            fclose(file_);
        file_ = nullptr;
    }
    uint8_t* Scratch(uint32_t* size) override
    {
        *size = sizeof(scratch_);
        return scratch_;
    }

    bool DirOpen(const char* path) override
    {
        dir_path_ = Full(path);
        dir_      = opendir(dir_path_.c_str());
        return dir_ != nullptr;
    }
    bool DirNext(char* name, size_t cap, bool* is_dir) override
    {
        if(!dir_)
            return false;
        struct dirent* e = readdir(dir_);
        if(!e)
            return false;
        if(strlen(e->d_name) < cap)
            strcpy(name, e->d_name);
        else
            name[0] = 0;
        struct stat sb;
        std::string p = dir_path_ + "/" + e->d_name;
        *is_dir       = stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
        return true;
    }
    void DirClose() override
    {
        if(dir_)
            closedir(dir_);
        dir_ = nullptr;
    }

    bool WriteFile(const char* d, const char* path, const void* data, uint32_t n) override
    {
        mkdir(Full(d).c_str(), 0755);
        FILE* f = fopen(Full(path).c_str(), "wb");
        if(!f)
            return false;
        size_t w = fwrite(data, 1, n, f);
        fclose(f);
        return w == n;
    }
    int32_t ReadFile(const char* path, void* data, uint32_t cap) override
    {
        FILE* f = fopen(Full(path).c_str(), "rb");
        if(!f)
            return -1;
        size_t r = fread(data, 1, cap, f);
        fclose(f);
        return (int32_t)r;
    }

    bool FileCreate(const char* d, const char* path) override
    {
        std::string acc;
        for(const char* q = d; *q; q++)
        {
            acc += *q;
            if(q[1] == '/' || q[1] == 0)
                mkdir(Full(acc.c_str()).c_str(), 0755);
        }
        if(wfile_)
            fclose(wfile_);
        wfile_ = fopen(Full(path).c_str(), "wb");
        return wfile_ != nullptr;
    }
    int32_t FileWrite(const void* src, uint32_t n) override
    {
        if(!wfile_)
            return -1;
        return (int32_t)fwrite(src, 1, n, wfile_);
    }
    bool FileWriteClose() override
    {
        if(!wfile_)
            return false;
        bool ok = fclose(wfile_) == 0;
        wfile_  = nullptr;
        return ok;
    }
    bool Remove(const char* path) override
    {
        std::string f = Full(path);
        return ::remove(f.c_str()) == 0;
    }
    bool Rename(const char* from, const char* to) override
    {
        std::string a = Full(from), b = Full(to);
        return ::rename(a.c_str(), b.c_str()) == 0;
    }
    int16_t* UndoMem(uint32_t* n) override
    {
        static std::vector<int16_t> u(768 * 1024);
        *n = (uint32_t)u.size();
        return u.data();
    }
    int16_t* SpareMem(uint32_t* n) override // like the Daisy's: 8 blocks of two slots
    {
        static std::vector<int16_t> sp(8 * 1536 * 1024);
        *n = (uint32_t)sp.size();
        return sp.data();
    }
    int16_t* Staging(uint32_t* samples) override
    {
        static std::vector<int16_t> mem(4600 * 1024);
        *samples = (uint32_t)mem.size();
        return mem.data();
    }

    float    CpuAvg() override { return cpu_avg; }
    float    CpuMax() override { return cpu_max; }
    bool     ControllerUp() override { return apc_up; }
    bool     ControllerPresent() override { return apc_up; }
    uint32_t Dropped() override { return 0; }
    bool     AudioRunning() override { return audio; }

  private:
    std::string Full(const char* p) const { return root + "/" + p; }
    FILE*       file_ = nullptr;
    FILE*       wfile_ = nullptr;
    DIR*        dir_  = nullptr;
    std::string dir_path_;
    uint8_t     scratch_[65536];
};

MacPlatform plat;
Engine      engine;

/* ================================================================== */
/* Audio: miniaudio pulls, the engine renders in 48-sample blocks      */

constexpr int kBlock = 48;
float         blk_l[kBlock], blk_r[kBlock];
int           blk_pos = kBlock;
double        cpu_acc = 0.0, cpu_peak = 0.0;
uint32_t      cpu_frames = 0;

/* Mic / line in (mono), one block behind the output, like the Daisy */
float in_blk[2][kBlock];
int   in_cur = 0;

void Render(float* out, uint32_t frames, const float* in = nullptr)
{
    auto t0 = std::chrono::steady_clock::now();
    for(uint32_t i = 0; i < frames; i++)
    {
        if(blk_pos == kBlock)
        {
            in_cur ^= 1;
            engine.SetInput(in_blk[in_cur ^ 1], in_blk[in_cur ^ 1]);
            engine.Process(blk_l, blk_r, kBlock);
            blk_pos = 0;
        }
        in_blk[in_cur][blk_pos] = in ? in[i] : 0.f;
        out[2 * i]     = blk_l[blk_pos];
        out[2 * i + 1] = blk_r[blk_pos];
        blk_pos++;
    }
    double used  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double avail = frames / 48000.0;
    double load  = used / avail;
    cpu_acc += used;
    cpu_frames += frames;
    if(load > cpu_peak)
        cpu_peak = load;
    if(cpu_frames >= 48000)
    {
        plat.cpu_avg = (float)(cpu_acc / (cpu_frames / 48000.0));
        plat.cpu_max = (float)cpu_peak;
        cpu_acc      = 0.0;
        cpu_peak     = 0.0;
        cpu_frames   = 0;
    }
}

void AudioCallback(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    Render((float*)out, frames, (const float*)in);
}

/** No audio device (or --silent): keep the engine clock running anyway */
void SilentClock()
{
    std::vector<float> buf(2 * 480), tone(480, 0.f);
    const bool         test_tone = getenv("APCBOX_TONE") != nullptr; // fake mic for tests
    double             ph        = 0.0;
    auto               next      = std::chrono::steady_clock::now();
    for(;;)
    {
        if(test_tone)
            for(auto& v : tone)
            {
                v = 0.3f * (float)sin(ph);
                ph += 2.0 * M_PI * 220.0 / 48000.0;
            }
        Render(buf.data(), 480, test_tone ? tone.data() : nullptr);
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
}

/* ================================================================== */
/* APC40 over CoreMIDI                                                  */

RtMidiIn*  midi_in  = nullptr;
RtMidiOut* midi_out = nullptr;
uint32_t   mode_time = 0;

int FindPort(RtMidi& m)
{
    unsigned n = m.getPortCount();
    for(unsigned i = 0; i < n; i++)
    {
        std::string name = m.getPortName(i);
        if(name.find("APC40") != std::string::npos || name.find("APC 40") != std::string::npos)
            return (int)i;
    }
    return -1;
}

void CloseApc()
{
    if(midi_in)
        midi_in->closePort();
    if(midi_out)
        midi_out->closePort();
    if(plat.apc_up)
    {
        plat.apc_up = false;
        BoxControllerGone();
        BoxLog("APC40 disconnected");
    }
}

void CheckApc(uint32_t now)
{
    if(!midi_in || !midi_out)
        return;
    static uint32_t last = 0;
    if(now - last < 1000 && last)
        return;
    last = now;
    try
    {
        int pi = FindPort(*midi_in), po = FindPort(*midi_out);
        if(plat.apc_up && (pi < 0 || po < 0))
            CloseApc();
        if(!plat.apc_up && pi >= 0 && po >= 0)
        {
            midi_in->openPort(pi, "ApcBox in");
            midi_in->ignoreTypes(false, true, true); // keep sysex, skip clock, sensing
            midi_out->openPort(po, "ApcBox out");
            /* Who are you? (reply is logged and answered), then Alternate
               Ableton Live mode (0x42) to broadcast ID 7F and IDs 00, 01 */
            std::vector<unsigned char> inq = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
            midi_out->sendMessage(&inq);
            for(unsigned char id : {0x7F, 0x00, 0x01})
            {
                std::vector<unsigned char> sx = {0xF0, 0x47, id, 0x73, 0x60, 0x00,
                                                 0x04, 0x42, 0x01, 0x01, 0x01, 0xF7};
                midi_out->sendMessage(&sx);
            }
            mode_time   = now;
            plat.apc_up = true;
            BoxInvalidateLeds();
            BoxLog("APC40 connected: %s", midi_in->getPortName(pi).c_str());
        }
    }
    catch(RtMidiError& e)
    {
        BoxLog("MIDI: %s", e.getMessage().c_str());
        CloseApc();
    }
}

void PollMidi(uint32_t now)
{
    if(!plat.apc_up)
        return;
    std::vector<unsigned char> msg;
    for(int guard = 0; guard < 256; guard++)
    {
        midi_in->getMessage(&msg);
        if(msg.empty())
            break;
        if(msg[0] == 0xF0)
        {
            std::string hex;
            char        b[4];
            for(size_t i = 0; i < msg.size() && i < 40; i++)
            {
                snprintf(b, sizeof(b), "%02X ", msg[i]);
                hex += b;
            }
            BoxLog("sysex in (%d bytes): %s", (int)msg.size(), hex.c_str());
            if(msg.size() >= 15 && msg[1] == 0x7E && msg[3] == 0x06 && msg[4] == 0x02
               && msg[5] == 0x47 && msg[6] == 0x73)
            {
                BoxLog("APC40 says: device id %d", msg[13]);
                std::vector<unsigned char> sx = {0xF0, 0x47, msg[13], 0x73, 0x60, 0x00,
                                                 0x04, 0x42, 0x01, 0x01, 0x01, 0xF7};
                midi_out->sendMessage(&sx);
                BoxInvalidateLeds();
            }
            continue;
        }
        if(msg.size() < 3)
            continue;
        int st = msg[0] & 0xF0, ch = msg[0] & 0x0F;
        if(st == 0x90)
            BoxMidi(1, ch, msg[1], msg[2], now);
        else if(st == 0x80)
            BoxMidi(0, ch, msg[1], msg[2], now);
        else if(st == 0xB0)
            BoxMidi(3, ch, msg[1], msg[2], now);
    }
}

void FlushLeds(uint32_t now)
{
    if(!plat.apc_up || now - mode_time < 150)
        return;
    int      n;
    LedSlot* leds = BoxLeds(&n);
    int      sent = 0;
    try
    {
        for(int i = 0; i < n && sent < 64; i++)
        {
            LedSlot& l = leds[i];
            if(l.want == l.sent)
                continue;
            unsigned char m[3] = {l.status, l.d1, l.want};
            midi_out->sendMessage(m, 3);
            l.sent = l.want;
            sent++;
        }
    }
    catch(RtMidiError& e)
    {
        BoxLog("MIDI out: %s", e.getMessage().c_str());
        CloseApc();
    }
}

/* ================================================================== */
/* Tiny HTTP server: the page, the event stream, and test injection    */

std::string page_path;

bool SendAll(int fd, const std::string& s)
{
    size_t off = 0;
    while(off < s.size())
    {
        ssize_t w = send(fd, s.data() + off, s.size() - off, 0);
        if(w <= 0)
            return false;
        off += (size_t)w;
    }
    return true;
}

int QueryInt(const std::string& q, const char* key, int def)
{
    std::string k = std::string(key) + "=";
    size_t      p = q.find(k);
    if(p == std::string::npos)
        return def;
    return atoi(q.c_str() + p + k.size());
}

/* The 3.5" screen, previewed: the main loop draws, /tft.bmp serves it,
   /ui?a=turn&d=1 | push | back stands in for the encoder,
   /ui?a=touch&x=..&y=.. for a tap on the screen */
uint16_t              tft_fb[480 * 320];
std::vector<uint8_t>  tft_bmp;
std::mutex            tft_m;
struct UiEvent { int kind, d, x = 0, y = 0; };
std::vector<UiEvent>  ui_events;

void MakeBmp()
{
    const int W = 480, H = 320, row = W * 3, size = 54 + row * H;
    std::vector<uint8_t> b(size, 0);
    auto put32 = [&](int o, uint32_t v) { memcpy(&b[o], &v, 4); };
    b[0] = 'B', b[1] = 'M';
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, W);
    put32(22, (uint32_t)-H); // top-down
    b[26] = 1, b[28] = 24;
    put32(34, row * H);
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++)
        {
            uint16_t c = tft_fb[y * W + x];
            uint8_t* p = &b[54 + y * row + x * 3];
            p[2] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            p[0] = (uint8_t)((c & 31) * 255 / 31);
        }
    std::lock_guard<std::mutex> lk(tft_m);
    tft_bmp.swap(b);
}

void Serve(int fd)
{
    char    req[2048];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if(n <= 0)
    {
        close(fd);
        return;
    }
    req[n] = 0;
    std::string r(req);
    std::string path = r.substr(r.find(' ') + 1);
    path             = path.substr(0, path.find(' '));

    if(path == "/" || path == "/index.html")
    {
        FILE* f = fopen(page_path.c_str(), "rb");
        if(!f)
        {
            SendAll(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nui/apcbox-ui.html not found");
        }
        else
        {
            std::string body;
            char        b[8192];
            size_t      k;
            while((k = fread(b, 1, sizeof(b), f)) > 0)
                body.append(b, k);
            fclose(f);
            SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: "
                            + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        }
    }
    else if(path == "/events")
    {
        if(!SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n"))
        {
            close(fd);
            return;
        }
        resync_req = true;
        uint64_t                 cursor = hub.End();
        std::vector<std::string> lines;
        for(;;)
        {
            std::string out;
            if(hub.Wait(cursor, lines))
                for(auto& l : lines)
                    out += "data: " + l + "\n\n";
            else
                out = ": ping\n\n";
            if(!SendAll(fd, out))
                break;
        }
    }
    else if(path.rfind("/tft.bmp", 0) == 0)
    {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(tft_m);
            body.assign((const char*)tft_bmp.data(), tft_bmp.size());
        }
        SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nCache-Control: no-store\r\nContent-Length: "
                        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }
    else if(path.rfind("/ui", 0) == 0)
    {
        std::string q = path.substr(path.find('?') == std::string::npos ? path.size() : path.find('?'));
        int kind = q.find("a=push") != std::string::npos ? 1 : (q.find("a=back") != std::string::npos ? 2 : 0);
        if(q.find("a=touch") != std::string::npos)
            kind = 3;
        {
            std::lock_guard<std::mutex> lk(inject_m);
            ui_events.push_back({kind, QueryInt(q, "d", 1), QueryInt(q, "x", 0), QueryInt(q, "y", 0)});
        }
        SendAll(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    }
    else if(path.rfind("/resync", 0) == 0)
    {
        resync_req = true;
        SendAll(fd, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    }
    else if(path.rfind("/midi", 0) == 0)
    {
        std::string q = path.substr(path.find('?') == std::string::npos ? path.size() : path.find('?'));
        {
            std::lock_guard<std::mutex> lk(inject_m);
            injected.push_back({QueryInt(q, "t", 1), QueryInt(q, "c", 0), QueryInt(q, "a", 0), QueryInt(q, "b", 127)});
        }
        SendAll(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    }
    else
    {
        SendAll(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    }
    close(fd);
}

void HttpServer(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 16) != 0)
    {
        printf("can't listen on port %d (already running?)\n", port);
        return;
    }
    printf("monitor: http://localhost:%d\n", port);
    for(;;)
    {
        int c = accept(s, nullptr, nullptr);
        if(c < 0)
            continue;
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        std::thread(Serve, c).detach();
    }
}

/* ================================================================== */

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    bool        silent = false;
    std::string root;
    for(int i = 1; i < argc; i++)
    {
        if(!strcmp(argv[i], "--silent"))
            silent = true;
        else
            root = argv[i];
    }
    if(root.empty())
    {
        const char* home = getenv("HOME");
        root             = std::string(home ? home : ".") + "/ApcBox";
    }
    plat.root = root;

    /* The page sits next to the build folder: mac/build/apcbox -> ui/ */
    std::string exe = argv[0];
    std::string dir = exe.find('/') == std::string::npos ? "." : exe.substr(0, exe.rfind('/'));
    page_path       = dir + "/../../ui/apcbox-ui.html";

    /* Engine memory, same sizes as the Daisy */
    const uint32_t kSlotCap = 768u * 1024u; // same as the Daisy
    static std::vector<int16_t> slots((size_t)kSlotCap * kTracks);
    static std::vector<float>   dl(96000), dr(96000), cl(192000), cr(192000);
    Memory mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slots.data() + (size_t)t * kSlotCap;
    mem.slot_capacity = kSlotCap;
    mem.delay_l       = dl.data();
    mem.delay_r       = dr.data();
    mem.delay_len     = (uint32_t)dl.size();
    mem.capture_l     = cl.data();
    mem.capture_r     = cr.data();
    mem.capture_len   = (uint32_t)cl.size();
#ifdef USE_DAISYSP_LGPL
    static ab::Reverb reverb;
    mem.reverb = &reverb;
#endif
    engine.Init(48000.f, mem);
    engine.RenderBuiltInKit();
    BoxInit(&plat, &engine);
    BoxLog("ApcBox (Mac) start");

    std::thread(HttpServer, 8040).detach();

    /* Audio */
    ma_device dev;
    bool      have_audio = false;
    if(!silent)
    {
        /* duplex: the Mac's input stands in for the Daisy's AUDIO IN L */
        for(int duplex = 1; duplex >= 0 && !have_audio; duplex--)
        {
            ma_device_config cfg   = ma_device_config_init(duplex ? ma_device_type_duplex
                                                                  : ma_device_type_playback);
            cfg.playback.format    = ma_format_f32;
            cfg.playback.channels  = 2;
            cfg.capture.format     = ma_format_f32;
            cfg.capture.channels   = 1;
            cfg.sampleRate         = 48000;
            cfg.periodSizeInFrames = 128;
            cfg.dataCallback       = AudioCallback;
            if(ma_device_init(nullptr, &cfg, &dev) == MA_SUCCESS && ma_device_start(&dev) == MA_SUCCESS)
            {
                have_audio = true;
                BoxLog("audio: %s%s%s", dev.playback.name, duplex ? ", input: " : " (no input)",
                       duplex ? dev.capture.name : "");
            }
        }
    }
    if(!have_audio)
    {
        BoxLog("no audio output, running silent");
        std::thread(SilentClock).detach();
    }
    plat.audio = true;

    /* Samples */
    struct stat sb;
    if(stat(root.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode))
        BoxStorageReady();
    else
        BoxLog("no sample folder at %s, using the built-in kit", root.c_str());

    /* MIDI */
    try
    {
        midi_in  = new RtMidiIn(RtMidi::UNSPECIFIED, "ApcBox");
        midi_out = new RtMidiOut(RtMidi::UNSPECIFIED, "ApcBox");
    }
    catch(RtMidiError& e)
    {
        BoxLog("MIDI unavailable: %s", e.getMessage().c_str());
        midi_in  = nullptr;
        midi_out = nullptr;
    }

    BoxTftInit(tft_fb);
    uint32_t tft_time = 0;
    for(;;)
    {
        uint32_t now = NowMs();
        CheckApc(now);
        PollMidi(now);
        {
            std::vector<Injected> ev;
            {
                std::lock_guard<std::mutex> lk(inject_m);
                ev.swap(injected);
            }
            for(auto& e : ev)
                BoxMidi(e.t, e.c, e.a, e.b, now);
            std::vector<UiEvent> ue;
            {
                std::lock_guard<std::mutex> lk(inject_m);
                ue.swap(ui_events);
            }
            for(auto& e : ue)
            {
                if(e.kind == 0)
                    BoxUiTurn(e.d, now);
                else if(e.kind == 1)
                    BoxUiPush(now);
                else if(e.kind == 3)
                    BoxUiTouch(e.x, e.y, now);
                else
                    BoxUiBack(now);
            }
        }
        if(now - tft_time >= 66) // 15 fps, like the Daisy
        {
            tft_time = now;
            if(BoxTftCompose(now))
                MakeBmp();
        }
        if(resync_req.exchange(false))
            BoxResync();
        BoxPoll(now);
        FlushLeds(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
