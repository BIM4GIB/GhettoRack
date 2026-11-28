/** deckd: the deck engine as a program on the companion box.
 *
 *  Three kinds of thread. The audio thread runs Engine::Process and writes
 *  the sound card (ALSA, or nothing at all with --null: the desk and the
 *  bridge's tests). A loader thread per load decodes a file with ffmpeg into
 *  memory. The main thread does the rest: a Unix socket the bridge talks to
 *  in text lines, the Numark's messages, lights, reports, freeing tracks.
 *
 *  Lines in:  M hex (MIDI from the controller)   C 0|1 (controller gone / here)
 *             load d path   eject d   play d 0|1|2   cue d 0|1   seek d seconds
 *             set name [d] value   browse steps   pick d   R (say everything again)
 *  Lines out: V version   X hex (MIDI for the controller)   L text
 *             D d playing cue_held pos_ms len_ms speed*1000 cue_ms peak*1000 track
 *             T d rate*10000 range*100 gain hi mid lo filter volume pfl scratch  (0..1000)
 *             G xfader master cue_mix cue_gain peak_l peak_r                     (0..1000)
 *             N d track name   F d track path   W d track peaks-hex   B cursor count path
 *             Q d h1..h8 loop_in loop_out loop_on bpm*100 beat0 pad_mode beats*10000 roll sync synced rate*10000 fx   (ms; -1: none)
 *             E fx_kind wet beats*10000
 *             H xruns worst_us period buffer rt channels dropped_events
 *  d is 1 or 2. See README.md.
 *
 *  Build: decks/build.sh */
#include "engine.h"
#include "numark.h"
#include "tempo.h"
#include <map>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sys/mman.h>
#endif
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

extern char** environ;
using namespace dk;

namespace
{
constexpr int      kVersion   = 1;
constexpr uint32_t kMaxFrames = 30 * 60 * kRate; // half an hour a track: 318 MB
constexpr int      kPeaks     = 800;
constexpr size_t   kOutMax    = 256 * 1024; // a client this far behind is dropped
constexpr int      kClients   = 4;

struct Loaded : Track
{
    std::vector<int16_t> data;
    std::string          name, peaks;
};

/** What is remembered of a file between loads: its tempo (found once) and its hot cues */
struct Kept
{
    long long size  = -1; // the file's, when this was written: another file under the name starts afresh
    float     bpm   = -1; // under 0: not looked for yet. 0: looked, no steady beat
    double    beat0 = 0;
    double    hot[kHot];
    Kept()
    {
        for(auto& h : hot)
            h = -1;
    }
};

Engine                eng;
std::atomic<bool>     quit{false};
std::atomic<uint32_t> xruns{0}, worst_us{0};
int                   out_period = 128, out_buffer = 384, out_channels = 0, out_rt = 0;

void OnSignal(int)
{
    quit = true;
}

/* ---------- the audio thread ---------- */

void NoDenormals()
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    v |= (1u << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#elif defined(__x86_64__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
}

void Timed(float* buf, int frames)
{
    auto t0 = std::chrono::steady_clock::now();
    eng.Process(buf, frames);
    uint32_t us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if(us > worst_us.load(std::memory_order_relaxed))
        worst_us.store(us, std::memory_order_relaxed);
}

void NullAudio()
{
    std::vector<float> buf((size_t)out_period * kOuts);
    auto               next = std::chrono::steady_clock::now();
    NoDenormals();
    while(!quit)
    {
        Timed(buf.data(), out_period);
        next += std::chrono::nanoseconds((int64_t)out_period * 1000000000 / kRate);
        std::this_thread::sleep_until(next);
    }
}

#ifdef HAVE_ALSA
snd_pcm_t*       pcm     = nullptr;
snd_pcm_format_t pcm_fmt = SND_PCM_FORMAT_S24_3LE;

bool OpenAlsa(const char* dev, std::string& err)
{
    int r = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if(r < 0)
    {
        err = std::string("open ") + dev + ": " + snd_strerror(r);
        return false;
    }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    const snd_pcm_format_t fmts[] = {SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE};
    bool                   ok     = false;
    for(auto f : fmts)
        if(snd_pcm_hw_params_set_format(pcm, hw, f) == 0)
        {
            pcm_fmt = f;
            ok      = true;
            break;
        }
    if(!ok)
    {
        err = "no sample format this card and the engine share";
        return false;
    }
    out_channels = snd_pcm_hw_params_set_channels(pcm, hw, 4) == 0 ? 4 : 2;
    if(out_channels == 2 && snd_pcm_hw_params_set_channels(pcm, hw, 2) < 0)
    {
        err = "the card takes neither 4 nor 2 channels";
        return false;
    }
    if(snd_pcm_hw_params_set_rate(pcm, hw, kRate, 0) < 0)
    {
        err = "the card does not run at 44100 Hz";
        return false;
    }
    snd_pcm_uframes_t per = out_period, buf = out_buffer;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
    if((r = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        err = std::string("hw params: ") + snd_strerror(r);
        return false;
    }
    snd_pcm_hw_params_get_period_size(hw, &per, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw, &buf);
    out_period = (int)per, out_buffer = (int)buf;
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, buf - per);
    snd_pcm_sw_params_set_avail_min(pcm, sw, per);
    snd_pcm_sw_params(pcm, sw);
    return true;
}

void AlsaAudio()
{
    sched_param sp{};
    sp.sched_priority = 70;
    out_rt            = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    NoDenormals();
    const int            bytes = pcm_fmt == SND_PCM_FORMAT_S24_3LE ? 3 : pcm_fmt == SND_PCM_FORMAT_S32_LE ? 4 : 2;
    std::vector<float>   buf((size_t)out_period * kOuts);
    std::vector<uint8_t> raw((size_t)out_period * out_channels * bytes);
    while(!quit)
    {
        Timed(buf.data(), out_period);
        uint8_t* o = raw.data();
        for(int i = 0; i < out_period; i++)
            for(int c = 0; c < out_channels; c++)
            {
                float   x = buf[(size_t)i * kOuts + c];
                x         = x > 1.f ? 1.f : (x < -1.f ? -1.f : x);
                int32_t v = (int32_t)lrintf(x * 8388607.f); // 24 bits
                if(bytes == 3)
                    o[0] = v, o[1] = v >> 8, o[2] = v >> 16;
                else if(bytes == 4)
                    v = (int32_t)((uint32_t)v << 8), memcpy(o, &v, 4);
                else
                    o[0] = v >> 8, o[1] = v >> 16;
                o += bytes;
            }
        snd_pcm_sframes_t r = snd_pcm_writei(pcm, raw.data(), out_period);
        if(r < 0)
        {
            xruns++;
            if(snd_pcm_recover(pcm, (int)r, 1) < 0)
            {
                quit = true; // the card is gone
                break;
            }
        }
    }
    snd_pcm_close(pcm);
}
#endif

/* ---------- loading ---------- */

bool IsAudio(const std::string& p)
{
    static const char* ext[] = {".mp3", ".wav", ".flac", ".aif", ".aiff", ".m4a", ".ogg", ".opus", ".aac", ".wave"};
    std::string        e     = std::filesystem::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    for(auto x : ext)
        if(e == x)
            return true;
    return false;
}

/** A file, whatever it is, as 44.1 kHz stereo in memory. nullptr and `err` if not. */
Loaded* Decode(const std::string& path, std::string& err, float known_bpm, double known_beat0)
{
    int fd[2];
    if(pipe(fd) < 0)
    {
        err = "pipe";
        return nullptr;
    }
    const char* argv[] = {"ffmpeg", "-v", "error", "-nostdin", "-i", path.c_str(), "-vn", "-f", "s16le", "-ac", "2", "-ar", "44100", "-", nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int   r = posix_spawnp(&pid, "ffmpeg", &fa, nullptr, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if(r != 0)
    {
        close(fd[0]);
        err = "ffmpeg is not installed";
        return nullptr;
    }
    Loaded* t = new Loaded;
    size_t  n = 0; // bytes
    t->data.resize(1 << 20);
    bool too_long = false;
    for(;;)
    {
        if(n == t->data.size() * 2)
            t->data.resize(t->data.size() * 2);
        ssize_t k = read(fd[0], (char*)t->data.data() + n, t->data.size() * 2 - n);
        if(k <= 0)
            break;
        n += (size_t)k;
        if(n / 4 > kMaxFrames)
        {
            too_long = true;
            kill(pid, SIGKILL);
            break;
        }
    }
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    t->frames = (uint32_t)(n / 4);
    if(too_long || t->frames < kRate / 10)
    {
        err = too_long ? "longer than half an hour" : "not a sound file ffmpeg can read";
        delete t;
        return nullptr;
    }
    t->data.resize((size_t)t->frames * 2);
    t->data.shrink_to_fit();
    t->pcm  = t->data.data();
#ifdef HAVE_ALSA
    (void)!mlock(t->data.data(), t->data.size() * 2); // keep it out of swap if the limits allow; it plays either way
#endif
    t->name = std::filesystem::path(path).stem().string();
    if(known_bpm >= 0)
        t->bpm = known_bpm, t->beat0 = known_beat0;
    else // about a tenth of a second a minute of music on a Pi 4: before the track is handed over, so it arrives whole
    {
        const Tempo r = FindTempo(t->pcm, t->frames);
        t->bpm = r.bpm, t->beat0 = r.beat0;
    }
    static const char hex[] = "0123456789abcdef";
    for(int b = 0; b < kPeaks; b++)
    {
        size_t a = (size_t)b * t->frames / kPeaks, z = (size_t)(b + 1) * t->frames / kPeaks;
        int    m = 0;
        for(size_t i = a * 2; i < z * 2; i++)
            m = std::max(m, std::abs((int)t->data[i]));
        m = std::min(255, m >> 7);
        t->peaks += hex[m >> 4];
        t->peaks += hex[m & 15];
    }
    return t;
}

struct Done
{
    int         deck;
    uint32_t    gen;
    Loaded*     track;
    std::string err, path;
    long long   size;
};
std::mutex           done_mu;
std::vector<Done>    done;
std::atomic<uint32_t> load_gen[kDecks];
int                  wake[2] = {-1, -1};
uint32_t             next_track = 1;

long long FileSize(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

std::map<std::string, Kept> kept; // by path; the main thread's
std::string                 store_path;
bool                        store_dirty = false;
std::chrono::steady_clock::time_point store_at;

void StartLoad(int deck, const std::string& path)
{
    uint32_t        g    = ++load_gen[deck];
    const long long size = FileSize(path);
    auto            it   = kept.find(path);
    const bool      have = it != kept.end() && it->second.size == size && it->second.bpm > 0; // none found last time: look again
    const float     bpm  = have ? it->second.bpm : -1;
    const double    b0   = have ? it->second.beat0 : 0;
    std::thread([deck, path, g, size, bpm, b0] {
        Done d{deck, g, nullptr, "", path, size};
        d.track = Decode(path, d.err, bpm, b0);
        {
            std::lock_guard<std::mutex> l(done_mu);
            done.push_back(d);
        }
        char c = 1;
        (void)!write(wake[1], &c, 1);
    }).detach();
}

/* ---------- the main thread ---------- */

struct Client
{
    int         fd = -1;
    std::string in, out;
};
Client                   clients[kClients];
int                      midi_fd = -1;
std::vector<std::string> library;
int                      cursor = 0;
std::string              music_dir;
std::string              deck_name[kDecks], deck_peaks[kDecks], deck_path[kDecks];
bool                     hot_sync[kDecks]; // the deck shows what the file remembered: from here on a change is the player's
std::string              last_q[kDecks], last_e;
uint32_t                 deck_track[kDecks];
std::string              last_d[kDecks], last_t[kDecks], last_g;

void Send(const char* fmt, ...)
{
    char    line[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if(n < 0)
        return;
    n         = std::min(n, (int)sizeof(line) - 2);
    line[n++] = '\n';
    for(auto& c : clients)
        if(c.fd >= 0)
        {
            if(c.out.size() + n > kOutMax) // not reading: let it go rather than fall behind or block
            {
                close(c.fd);
                c = Client();
                continue;
            }
            c.out.append(line, n);
        }
}
void Flush()
{
    for(auto& c : clients)
        while(c.fd >= 0 && !c.out.empty())
        {
            ssize_t k = send(c.fd, c.out.data(), c.out.size(), MSG_DONTWAIT);
            if(k > 0)
                c.out.erase(0, (size_t)k);
            else if(k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            else
            {
                close(c.fd);
                c = Client();
            }
        }
}

/** MIDI for the controller: to the bridge as an X line, and to the port itself in --midi mode */
void ToController(const uint8_t* m, int n)
{
    if(n <= 0)
        return;
    std::string h;
    char        b[4];
    for(int i = 0; i < n; i++)
    {
        snprintf(b, sizeof(b), "%02x", m[i]);
        h += b;
    }
    Send("X %s", h.c_str());
    if(midi_fd >= 0)
        (void)!write(midi_fd, m, n);
}

void Scan(const std::string& dir);

void SayBrowse()
{
    Send("B %d %d %s", cursor, (int)library.size(), library.empty() ? "-" : library[cursor].c_str());
}

void Load(int deck, const std::string& path)
{
    if(deck < 0 || deck >= kDecks)
        return;
    if(eng.state[deck].playing.load()) // a playing deck keeps its track: the wrong LOAD button must not stop the music
    {
        Send("L deck %d is playing: not loaded", deck + 1);
        return;
    }
    Send("L deck %d loading %s", deck + 1, std::filesystem::path(path).filename().c_str());
    StartLoad(deck, path);
}

struct Host : NumarkHost
{
    void Browse(int steps, bool shift) override
    {
        if(library.empty())
            return;
        int n  = (int)library.size();
        cursor = std::max(0, std::min(n - 1, cursor + steps * (shift ? 10 : 1)));
        SayBrowse();
    }
    void BrowsePress(bool) override {}
    void LoadDeck(int d) override
    {
        if(!library.empty())
            Load(d, library[cursor]);
    }
} host;
Numark numark(eng, &host);

void Post(uint8_t type, int deck, float a, double d = 0, const Track* t = nullptr)
{
    Ev e;
    e.type = type, e.deck = (int8_t)deck, e.a = a, e.d = d, e.track = t;
    eng.Post(e);
}

/** Raw MIDI bytes to whole messages (channel messages only; the rest is dropped) */
struct MidiIn
{
    uint8_t st = 0, d[2];
    int     n = 0;
    bool    sysex = false;
    void    Feed(const uint8_t* b, int len)
    {
        for(int i = 0; i < len; i++)
        {
            uint8_t c = b[i];
            if(c >= 0xF8)
                continue;
            if(c & 0x80)
            {
                sysex = c == 0xF0;
                st    = c < 0xF0 ? c : 0;
                n     = 0;
                continue;
            }
            if(sysex || !st)
                continue;
            d[n++]   = c;
            int need = (st & 0xE0) == 0xC0 ? 1 : 2;
            if(n == need)
            {
                uint8_t m[3] = {st, d[0], need == 2 ? d[1] : (uint8_t)0};
                numark.Feed(m, 3);
                n = 0;
            }
        }
    }
} midi_in, line_midi;

void ControllerHere()
{
    uint8_t m[32];
    ToController(m, Numark::Hello(m));
    numark.ForgetLights();
}

void SayAll()
{
    Send("V %d", kVersion);
    for(int d = 0; d < kDecks; d++)
    {
        last_d[d].clear(), last_t[d].clear();
        if(deck_track[d])
        {
            Send("N %d %u %s", d + 1, deck_track[d], deck_name[d].c_str());
            Send("F %d %u %s", d + 1, deck_track[d], deck_path[d].c_str());
            Send("W %d %u %s", d + 1, deck_track[d], deck_peaks[d].c_str());
        }
    }
    last_g.clear();
    for(auto& q : last_q)
        q.clear();
    last_e.clear();
    SayBrowse();
}

bool SetKnob(const std::string& name, int deck, float v)
{
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    if(name == "xfader")
        eng.xfader = v;
    else if(name == "master")
        eng.master = v;
    else if(name == "cuemix")
        eng.cue_mix = v;
    else if(name == "cuegain")
        eng.cue_gain = v;
    else if(deck < 0 || deck >= kDecks)
        return false;
    else
    {
        DeckKnobs& k = eng.knobs[deck];
        if(name == "gain")
            k.gain = v;
        else if(name == "hi")
            k.hi = v;
        else if(name == "mid")
            k.mid = v;
        else if(name == "lo")
            k.lo = v;
        else if(name == "filter")
            k.filter = v;
        else if(name == "volume")
            k.volume = v;
        else if(name == "pfl")
            k.pfl = v;
        else if(name == "scratch")
            k.scratch = v;
        else if(name == "fx")
            k.fx = v > 0.5f ? 1.f : 0.f;
        else if(name == "sync") // as the SYNC button: not without both tempos, and the other deck's goes off
        {
            const bool on = v > 0.5f;
            if(on && (eng.state[deck].bpm.load() <= 0 || eng.state[1 - deck].bpm.load() <= 0))
            {
                Send("L deck %d: no SYNC without a tempo on both decks", deck + 1);
                return true;
            }
            k.sync = on ? 1.f : 0.f;
            if(on)
                eng.knobs[1 - deck].sync = 0.f;
        }
        else
            return false;
    }
    return true;
}

void Command(const std::string& line)
{
    char   w[32] = "";
    int    used  = 0;
    double a = 0, b = 0, c = 0;
    int    nargs = 0;
    if(sscanf(line.c_str(), "%31s %n", w, &used) < 1)
        return;
    const char*       rest = line.c_str() + used;
    const std::string cmd  = w;
    if(cmd == "M")
    {
        uint8_t bytes[256];
        int     n = 0;
        for(const char* p = rest; p[0] && p[1] && n < (int)sizeof(bytes); p += 2)
        {
            unsigned v;
            if(sscanf(p, "%2x", &v) != 1)
                break;
            bytes[n++] = (uint8_t)v;
        }
        line_midi.Feed(bytes, n);
    }
    else if(cmd == "C")
    {
        if(atoi(rest))
            ControllerHere();
        else // the module is being left: the controller back to how it was
        {
            uint8_t m[32];
            ToController(m, Numark::Bye(m));
        }
    }
    else if(cmd == "scan") // the music folder has changed
    {
        const std::string was = library.empty() ? "" : library[cursor];
        library.clear();
        if(!music_dir.empty())
            Scan(music_dir);
        auto it = std::find(library.begin(), library.end(), was);
        cursor  = it == library.end() ? 0 : (int)(it - library.begin());
        SayBrowse();
    }
    else if(cmd == "R")
        SayAll();
    else if(cmd == "load")
    {
        int d = 0, at = 0;
        if(sscanf(rest, "%d %n", &d, &at) >= 1 && rest[at])
            Load(d - 1, rest + at);
    }
    else if(cmd == "eject")
    {
        int d = atoi(rest) - 1;
        if(d >= 0 && d < kDecks && !eng.state[d].playing.load())
        {
            load_gen[d]++;
            Post(kEvLoad, d, 0);
            deck_track[d] = 0;
            Send("N %d 0 -", d + 1);
        }
    }
    else if(cmd == "play" && sscanf(rest, "%lf %lf", &a, &b) == 2)
        Post(kEvPlay, (int)a - 1, (float)b);
    else if(cmd == "cue" && sscanf(rest, "%lf %lf", &a, &b) == 2)
        Post(kEvCue, (int)a - 1, (float)b);
    else if(cmd == "seek" && sscanf(rest, "%lf %lf", &a, &b) == 2)
        Post(kEvSeek, (int)a - 1, 0, b * kRate);
    else if(cmd == "browse")
        host.Browse(atoi(rest), false);
    else if(cmd == "pick")
        host.LoadDeck(atoi(rest) - 1);
    else if(cmd == "at") // the cursor onto this file of the list
    {
        auto it = std::find(library.begin(), library.end(), std::string(rest));
        if(it != library.end())
        {
            cursor = (int)(it - library.begin());
            SayBrowse();
        }
    }
    else if(cmd == "hot" && sscanf(rest, "%lf %lf %lf", &a, &b, &c) == 3) // hot d n 1|0: pad n (1..8) pressed, let go
        Post(kEvHot, (int)a - 1, (float)(b - 1), c);
    else if(cmd == "hotclear" && sscanf(rest, "%lf %lf", &a, &b) == 2)
        Post(kEvHotClear, (int)a - 1, (float)(b - 1));
    else if(cmd == "loopin")
        Post(kEvLoopIn, atoi(rest) - 1, 0);
    else if(cmd == "loopout")
        Post(kEvLoopOut, atoi(rest) - 1, 0);
    else if(cmd == "loop" && (nargs = sscanf(rest, "%lf %lf %lf", &a, &b, &c)) >= 2) // loop d 0|1|2 [beats]
        Post(kEvLoop, (int)a - 1, (float)b, nargs == 3 && c > 0 ? c : 0);
    else if(cmd == "roll" && sscanf(rest, "%lf %lf %lf", &a, &b, &c) == 3) // roll d beats 1|0
        Post(kEvRoll, (int)a - 1, (float)c, b);
    else if(cmd == "fxkind" && sscanf(rest, "%lf", &a) == 1 && a >= 0 && a < kFxCount)
        eng.fx_kind = (int)a;
    else if(cmd == "fxwet" && sscanf(rest, "%lf", &a) == 1)
        eng.fx_wet = (float)std::max(0.0, std::min(1.0, a));
    else if(cmd == "fxbeats" && sscanf(rest, "%lf", &a) == 1)
        eng.fx_beats = (float)std::max(1.0 / 16, std::min(8.0, a));
    else if(cmd == "loopx" && sscanf(rest, "%lf %lf", &a, &b) == 2 && (b == 0.5 || b == 2))
        Post(kEvLoopScale, (int)a - 1, (float)b);
    else if(cmd == "bpmx" && sscanf(rest, "%lf %lf", &a, &b) == 2 && (b == 0.5 || b == 2)) // the tempo found is half or twice the real one
    {
        const int d = (int)a - 1;
        if(d >= 0 && d < kDecks && deck_track[d])
        {
            const float bpm = eng.state[d].bpm.load() * (float)b;
            auto        it  = kept.find(deck_path[d]);
            if(bpm >= 40 && bpm <= 320)
            {
                Post(kEvGrid, d, bpm, eng.state[d].beat0.load());
                if(it != kept.end())
                    it->second.bpm = bpm, store_dirty = true, store_at = std::chrono::steady_clock::now();
            }
        }
    }
    else if(cmd == "set")
    {
        char name[32] = "";
        int  n        = sscanf(rest, "%31s %lf %lf", name, &a, &b);
        bool ok       = false;
        if(n == 3 && std::string(name) == "rate")
        {
            int d = (int)a - 1;
            if(d >= 0 && d < kDecks && b >= -0.5 && b <= 0.5)
                eng.knobs[d].rate = (float)b, ok = true;
        }
        else if(n == 3)
            ok = SetKnob(name, (int)a - 1, (float)b);
        else if(n == 2)
            ok = SetKnob(name, -1, (float)a);
        if(!ok)
            Send("L set: not understood: %s", rest);
    }
    else
        Send("L not understood: %s", w);
}

void Loads()
{
    std::vector<Done> got;
    {
        std::lock_guard<std::mutex> l(done_mu);
        got.swap(done);
    }
    for(auto& d : got)
    {
        if(!d.track)
        {
            Send("L deck %d: %s: %s", d.deck + 1, std::filesystem::path(d.path).filename().c_str(), d.err.c_str());
            continue;
        }
        if(d.gen != load_gen[d.deck].load() || eng.state[d.deck].playing.load())
        {
            delete d.track; // a newer load was asked for, or the deck started playing meanwhile
            continue;
        }
        d.track->id        = next_track++;
        deck_track[d.deck] = d.track->id;
        deck_name[d.deck]  = d.track->name;
        deck_path[d.deck]  = d.path;
        Kept& k = kept[d.path];
        if(k.size != d.size) // not seen before, or another file under this name
            k = Kept(), k.size = d.size;
        if(k.bpm <= 0)
            k.bpm = d.track->bpm, k.beat0 = d.track->beat0, store_dirty = true, store_at = std::chrono::steady_clock::now();
        hot_sync[d.deck] = false;
        deck_peaks[d.deck] = d.track->peaks;
        Post(kEvLoad, d.deck, 0, 0, d.track);
        for(int i = 0; i < kHot; i++)
            if(k.hot[i] >= 0)
                Post(kEvHotSet, d.deck, (float)i, k.hot[i]);
        Send("N %d %u %s", d.deck + 1, d.track->id, d.track->name.c_str());
        Send("F %d %u %s", d.deck + 1, d.track->id, d.path.c_str());
        Send("W %d %u %s", d.deck + 1, d.track->id, d.track->peaks.c_str());
    }
}

int K(float v)
{
    return (int)lrintf(v * 1000.f);
}

void Reports()
{
    char line[256];
    for(int d = 0; d < kDecks; d++)
    {
        const DeckState& s = eng.state[d];
        snprintf(line, sizeof(line), "D %d %d %d %ld %ld %d %ld %d %u", d + 1, s.playing.load(), s.cue_held.load(), lrint(s.pos.load() * 1000 / kRate),
                 lrint((double)s.frames.load() * 1000 / kRate), K(s.speed.load()), lrint(s.cue.load() * 1000 / kRate), K(std::min(1.f, s.peak.load())),
                 s.track.load());
        if(last_d[d] != line)
            Send("%s", line), last_d[d] = line;
        const DeckKnobs& k = eng.knobs[d];
        snprintf(line, sizeof(line), "T %d %d %d %d %d %d %d %d %d %d %d", d + 1, (int)lrintf(k.rate.load() * 10000), (int)lrintf(Numark::kRanges[numark.range[d]] * 100),
                 K(k.gain), K(k.hi), K(k.mid), K(k.lo), K(k.filter), K(k.volume), K(k.pfl), K(k.scratch));
        if(last_t[d] != line)
            Send("%s", line), last_t[d] = line;
    }
    snprintf(line, sizeof(line), "G %d %d %d %d %d %d", K(eng.xfader), K(eng.master), K(eng.cue_mix), K(eng.cue_gain), K(std::min(1.f, eng.peak_l.load())),
             K(std::min(1.f, eng.peak_r.load())));
    if(last_g != line)
        Send("%s", line), last_g = line;
}

/* ---------- what is remembered of each file ---------- */

void StoreLoad()
{
    FILE* f = store_path.empty() ? nullptr : fopen(store_path.c_str(), "r");
    if(!f)
        return;
    char line[4096];
    while(fgets(line, sizeof(line), f))
    {
        Kept  k;
        int   used = 0;
        float bpm  = 0;
        if(sscanf(line, "%lld %f %lf %lf %lf %lf %lf %lf %lf %lf %lf %n", &k.size, &bpm, &k.beat0, &k.hot[0], &k.hot[1], &k.hot[2], &k.hot[3], &k.hot[4], &k.hot[5],
                  &k.hot[6], &k.hot[7], &used)
               < 11
           || !line[used])
            continue;
        k.bpm = bpm;
        std::string path = line + used;
        while(!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        if(!path.empty())
            kept[path] = k;
    }
    fclose(f);
}

void StoreSave()
{
    store_dirty = false;
    if(store_path.empty())
        return;
    const std::string tmp = store_path + ".tmp";
    FILE*             f   = fopen(tmp.c_str(), "w");
    if(!f)
        return;
    for(const auto& kv : kept)
    {
        const Kept& k = kv.second;
        if(kv.first.find('\n') != std::string::npos)
            continue;
        fprintf(f, "%lld %.4f %.1f", k.size, k.bpm, k.beat0);
        for(double h : k.hot)
            fprintf(f, " %.0f", h);
        fprintf(f, " %s\n", kv.first.c_str());
    }
    const bool ok = fflush(f) == 0;
    fclose(f);
    if(ok)
        rename(tmp.c_str(), store_path.c_str());
}

/** Hot cues, loop, tempo and the pad mode, on change; and a changed hot cue goes to what is remembered */
void PadReports()
{
    char line[320];
    snprintf(line, sizeof(line), "E %d %d %ld", eng.fx_kind.load(), K(eng.fx_wet), lrint(eng.fx_beats.load() * 10000));
    if(last_e != line)
        Send("%s", line), last_e = line;
    for(int d = 0; d < kDecks; d++)
    {
        const DeckState& s = eng.state[d];
        auto             ms = [](double frames) { return frames < 0 ? -1L : lrint(frames * 1000 / kRate); };
        int              n  = snprintf(line, sizeof(line), "Q %d", d + 1);
        double           hot[kHot];
        for(int i = 0; i < kHot; i++)
        {
            hot[i] = s.hot[i].load();
            n += snprintf(line + n, sizeof(line) - n, " %ld", ms(hot[i]));
        }
        snprintf(line + n, sizeof(line) - n, " %ld %ld %d %ld %ld %d %ld %d %d %d %ld %d", ms(s.loop_in.load()), ms(s.loop_out.load()), s.loop_on.load(),
                 lrint(s.bpm.load() * 100), ms(s.beat0.load()), numark.pads[d], lrint(s.beats.load() * 10000), s.roll.load(),
                 eng.knobs[d].sync.load() > 0.5f, s.synced.load(), lrint(s.rate.load() * 10000), eng.knobs[d].fx.load() > 0.5f);
        if(last_q[d] != line)
            Send("%s", line), last_q[d] = line;
        if(!deck_track[d] || s.track.load() != deck_track[d])
            continue;
        auto it = kept.find(deck_path[d]);
        if(it == kept.end())
            continue;
        bool same = true;
        for(int i = 0; i < kHot; i++)
            same = same && it->second.hot[i] == hot[i];
        if(same)
            hot_sync[d] = true;
        else if(hot_sync[d])
        {
            for(int i = 0; i < kHot; i++)
                it->second.hot[i] = hot[i];
            store_dirty = true, store_at = std::chrono::steady_clock::now();
        }
    }
}

void Scan(const std::string& dir)
{
    std::error_code ec;
    for(auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
        !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if(it->is_regular_file(ec) && IsAudio(it->path().string()) && it->path().filename().string()[0] != '.')
            library.push_back(it->path().string());
    std::sort(library.begin(), library.end());
}

} // namespace

int main(int argc, char** argv)
{
    std::string device = "hw:CARD=FX,DEV=0", sock_path = "/tmp/deckd.sock", music, midi_path;
    bool        null_out = false;
    for(int i = 1; i < argc; i++)
    {
        std::string a    = argv[i];
        auto        next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if(a == "--device")
            device = next();
        else if(a == "--socket")
            sock_path = next();
        else if(a == "--music")
            music = next();
        else if(a == "--store") // a file: each track's tempo and hot cues, kept between loads and runs
            store_path = next();
        else if(a == "--midi") // the controller's port, opened here: for trying the engine without the bridge only
            midi_path = next();
        else if(a == "--period")
            out_period = atoi(next().c_str());
        else if(a == "--buffer")
            out_buffer = atoi(next().c_str());
        else if(a == "--null")
            null_out = true;
        else
        {
            fprintf(stderr, "deckd [--device hw:CARD=FX,DEV=0 | --null] [--socket path] [--music folder] [--store file] [--period 128] [--buffer 384] [--midi /dev/snd/midiCxD0]\n");
            return 2;
        }
    }
    if(out_period < 16 || out_period > 4096 || out_buffer < out_period * 2)
    {
        fprintf(stderr, "deckd: period 16..4096, buffer at least two periods\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    if(pipe(wake) < 0)
        return 1;
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    StoreLoad();
    music_dir = music;
    if(!music.empty())
        Scan(music);

    std::thread audio;
#ifdef HAVE_ALSA
    if(!null_out)
    {
        std::string err;
        if(!OpenAlsa(device.c_str(), err))
        {
            fprintf(stderr, "deckd: %s\n", err.c_str());
            return 1;
        }
        if(mlockall(MCL_CURRENT) < 0) // what is here now; each track is locked as it is loaded
            fprintf(stderr, "deckd: memory not locked (%s)\n", strerror(errno));
        audio = std::thread(AlsaAudio);
    }
#else
    null_out = true;
#endif
    if(null_out)
        audio = std::thread(NullAudio);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if(sock_path.size() >= sizeof(sa.sun_path))
    {
        fprintf(stderr, "deckd: socket path too long (%d characters at most)\n", (int)sizeof(sa.sun_path) - 1);
        quit = true;
        audio.join();
        return 1;
    }
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path.c_str());
    unlink(sock_path.c_str());
    if(ls < 0 || bind(ls, (sockaddr*)&sa, sizeof(sa)) < 0 || listen(ls, 4) < 0)
    {
        fprintf(stderr, "deckd: socket %s: %s\n", sock_path.c_str(), strerror(errno));
        quit = true;
        audio.join();
        return 1;
    }
    chmod(sock_path.c_str(), 0660);
    if(!midi_path.empty())
    {
        midi_fd = open(midi_path.c_str(), O_RDWR | O_NONBLOCK);
        if(midi_fd < 0)
            fprintf(stderr, "deckd: %s: %s\n", midi_path.c_str(), strerror(errno));
        else
            ControllerHere();
    }
    fprintf(stderr, "deckd: %s, period %d, buffer %d (%.1f ms), %d channels, %d files, socket %s\n", null_out ? "no sound card" : device.c_str(), out_period,
            out_buffer, out_buffer * 1000.0 / kRate, out_channels, (int)library.size(), sock_path.c_str());

    auto last_report = std::chrono::steady_clock::now(), last_health = last_report;
    while(!quit)
    {
        pollfd p[3 + kClients];
        int    n = 0;
        p[n++]   = {ls, POLLIN, 0};
        p[n++]   = {wake[0], POLLIN, 0};
        p[n++]   = {midi_fd, POLLIN, 0}; // -1 is skipped by poll
        for(auto& c : clients)
            p[n++] = {c.fd, (short)(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0};
        poll(p, n, 15);

        if(p[0].revents & POLLIN)
        {
            int fd = accept(ls, nullptr, nullptr);
            int at = -1;
            for(int i = 0; i < kClients; i++)
                if(clients[i].fd < 0)
                    at = i;
            if(fd >= 0 && at < 0)
                close(fd);
            else if(fd >= 0)
            {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                clients[at].fd = fd;
                SayAll();
            }
        }
        if(p[1].revents & POLLIN)
        {
            char junk[64];
            while(read(wake[0], junk, sizeof(junk)) > 0) {}
            Loads();
        }
        if(midi_fd >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            uint8_t b[256];
            ssize_t k = read(midi_fd, b, sizeof(b));
            if(k > 0)
                midi_in.Feed(b, (int)k);
            else if(k == 0 || (errno != EAGAIN && errno != EINTR))
            {
                close(midi_fd);
                midi_fd = -1;
                Send("L controller port closed");
            }
        }
        for(int i = 0; i < kClients; i++)
        {
            Client& c = clients[i];
            if(c.fd < 0 || !(p[3 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char    b[4096];
            ssize_t k = recv(c.fd, b, sizeof(b), 0);
            if(k <= 0)
            {
                if(k < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                close(c.fd);
                c = Client();
                continue;
            }
            c.in.append(b, (size_t)k);
            size_t nl;
            while((nl = c.in.find('\n')) != std::string::npos)
            {
                std::string line = c.in.substr(0, nl);
                c.in.erase(0, nl + 1);
                if(!line.empty() && line.back() == '\r')
                    line.pop_back();
                Command(line);
            }
            if(c.in.size() > 8192) // a line this long is not one of ours
                c.in.clear();
        }

        while(const Track* t = eng.Trash())
            delete static_cast<const Loaded*>(t);

        auto now = std::chrono::steady_clock::now();
        if(now - last_report >= std::chrono::milliseconds(33))
        {
            last_report = now;
            uint8_t m[192];
            ToController(m, numark.Lights(m, sizeof(m)));
            Reports();
            PadReports();
            if(store_dirty && now - store_at >= std::chrono::seconds(1))
                StoreSave();
        }
        if(now - last_health >= std::chrono::seconds(1))
        {
            last_health = now;
            Send("H %u %u %d %d %d %d %u", xruns.load(), worst_us.exchange(0), out_period, out_buffer, out_rt, out_channels, eng.ev_dropped.load());
        }
        Flush();
    }

    if(store_dirty)
        StoreSave();
    uint8_t m[16];
    ToController(m, Numark::Bye(m));
    Flush();
    quit = true;
    audio.join();
    unlink(sock_path.c_str());
    return 0;
}
