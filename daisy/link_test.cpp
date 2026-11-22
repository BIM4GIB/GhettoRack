// Link (USB serial out): a resync mid-line must not glue lines together.
// c++ -std=c++17 -O1 -Wall -Iteststub -o /tmp/link_test link_test.cpp && /tmp/link_test
#include "link.h"
#include <string>
#include <vector>

volatile bool Link::resync_ = false;
volatile bool Link::boot_   = false;
HostCmd       Link::cmd_;
uint8_t           Link::rx_[Link::kRx];
volatile uint32_t Link::rx_w_ = 0, Link::rx_r_ = 0, Link::rx_dropped_ = 0;

static usbdev::RxFn rx;
static std::string  wire; // what the host received
namespace usbdev
{
void Init() {}
void SetRx(RxFn fn) { rx = fn; }
bool SerialReady() { return true; }
bool SerialSend(const uint8_t* d, uint16_t n)
{
    wire.append((const char*)d, n);
    return true;
}
void AudioPush(const float*, const float*, size_t) {}
} // namespace usbdev

static int fails = 0;
static void Check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static std::vector<std::string> Lines(const std::string& s)
{
    std::vector<std::string> v;
    size_t                   a = 0, b;
    while((b = s.find('\n', a)) != std::string::npos)
    {
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

static bool Whole(const std::string& l)
{
    // Every line we queue is "X n ..." with a known shape
    if(l.rfind("D ", 0) == 0)
        return l.size() == 2 + 2048 && l.find_first_not_of("0123456789abcdef", 2) == std::string::npos;
    return l == "V 7" || l.rfind("S ", 0) == 0 || l.rfind("L ", 0) == 0;
}

static Link link_; // 16 KB ring: static, like on the Daisy

int main()
{
    link_.Init();
    std::string d = "D " + std::string(2048, 'a');
    bool        all_ok = true, glued = false;
    int         resyncs = 0, midline = 0;
    // Many rounds so the ring wraps and resyncs land at every offset
    for(int round = 0; round < 400; round++)
    {
        for(int i = 0; i < 1 + round % 5; i++)
            link_.Line(d.c_str());
        link_.Printf("S %d 1 2 3", round);
        link_.Flush(); // up to 2048 bytes out: usually mid-line
        if(!wire.empty() && wire.back() != '\n')
            midline++;
        if(round % 3 != 2)
        {
            const uint8_t esc_r[] = {0x1B, 'R'};
            rx(esc_r, 2);
            link_.Poll();
            resyncs += link_.TakeResync();
            link_.Printf("V 7");
            link_.Printf("L dump %d", round);
        }
        for(int k = 0; k < 20; k++)
            link_.Flush();
    }
    for(const std::string& l : Lines(wire))
        if(!Whole(l))
        {
            if(all_ok)
                printf("  bad line: %.40s... (%zu bytes)\n", l.c_str(), l.size());
            all_ok = false;
            glued |= l.find("V 7") != std::string::npos || l.find("L dump") != std::string::npos;
        }
    printf("  %d resyncs, %d of them with a line half sent\n", resyncs, midline);
    Check(resyncs > 200 && midline > 100, "test actually hit resyncs mid-line");
    Check(all_ok && !glued, "every line arrives whole across resyncs");
    Check(wire.back() == '\n', "stream ends on a line boundary");

    // Resync at a line boundary still drops everything queued
    wire.clear();
    link_.Printf("L a");
    link_.Flush();
    link_.Printf("L stale 1");
    link_.Printf("L stale 2");
    const uint8_t esc_r[] = {0x1B, 'R'};
    rx(esc_r, 2);
    link_.Poll();
    link_.TakeResync();
    link_.Printf("V 7");
    link_.Flush();
    link_.Flush();
    Check(wire == "L a\nV 7\n", "queued stale lines dropped on resync");

    printf("%s (%d failures)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
