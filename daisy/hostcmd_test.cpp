// Host command parser: echoed output must never trigger a command.
// c++ -std=c++17 -O1 -Wall -o /tmp/hostcmd_test hostcmd_test.cpp && /tmp/hostcmd_test
#include "hostcmd.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void Check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok)
        fails++;
}

struct Count
{
    int resync = 0, boot = 0;
};

static Count Run(HostCmd& p, const std::string& s)
{
    Count c;
    for(unsigned char b : s)
    {
        HostCmd::Cmd r = p.Feed(b);
        if(r == HostCmd::kResync)
            c.resync++;
        if(r == HostCmd::kBoot)
            c.boot++;
    }
    return c;
}

int main()
{
    // What the Daisy sends, as an echoing tty would hand it straight back
    std::string echo
        = "V 7\nS 1 3 0 -1 0 1200 0 0 0 0 0 120 340 1 1 1\n"
          "B 2 14 5 120\nBF Breaks\nBN Amen_Break.wav\nR\nB\nBOOT\n"
          "L Rebooting? no. BOOTLOADER, RESET, R B R B\n"
          "D 00FFBB0B1B2BBBBBBBBB0000BOOTB00T\nK 0 1 2 3\nKN 0 1 RnB\n";
    std::string lots;
    for(int i = 0; i < 200; i++)
        lots += echo;
    {
        HostCmd p;
        Count   c = Run(p, lots);
        Check(c.resync == 0 && c.boot == 0, "echoed report lines trigger nothing");
    }
    {
        HostCmd p;
        Count   c = Run(p, "R B BOOT RRRR BBBB");
        Check(c.resync == 0 && c.boot == 0, "bare R and B are ignored");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1bR");
        Check(c.resync == 1 && c.boot == 0, "ESC R = resync");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1b" "BOOT");
        Check(c.resync == 0 && c.boot == 1, "ESC BOOT = bootloader");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1b" "BOOX\x1b" "BOO\x1b" "B\x1b" "BOT");
        Check(c.boot == 0, "broken BOOT words don't boot");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1b" "BO\x1bR");
        Check(c.resync == 1 && c.boot == 0, "ESC restarts a half-typed command");
    }
    {
        HostCmd p;
        Count c = Run(p, "\x1b");
        Count d = Run(p, "BO");
        Count e = Run(p, "OT");
        Check(c.boot + d.boot == 0 && e.boot == 1, "command split across USB packets");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1bZ R \x1bQB");
        Check(c.resync == 0 && c.boot == 0, "unknown ESC letters are ignored");
    }
    {
        HostCmd p;
        Count   c = Run(p, "\x1bRR\x1bR" + lots + "\x1bR");
        Check(c.resync == 3 && c.boot == 0, "commands mixed into an echo stream");
    }
    /* ---- T95 control frames (v13.5) ---- */
    struct Got
    {
        HostCmd::Cmd cmd;
        int          port;
        std::string  data;
    };
    auto frames = [](const std::string& s) {
        HostCmd          p;
        std::vector<Got> v;
        for(unsigned char b : s)
        {
            HostCmd::Cmd c = p.Feed(b);
            if(c == HostCmd::kNone)
                continue;
            Got g{c, p.Port(), ""};
            if(c == HostCmd::kMidi)
                for(int i = 0; i < p.MidiLen(); i++)
                {
                    char h[3];
                    snprintf(h, 3, "%02x", p.Midi()[i]);
                    g.data += h;
                }
            else if(c == HostCmd::kName || c == HostCmd::kPing || c == HostCmd::kUi || c == HostCmd::kFile)
                g.data = p.Text();
            v.push_back(g);
        }
        return v;
    };
    {
        auto v = frames("\x1bM1 903C64\n\x1bN2 LPK25 mk2\n\x1bK\x1bP 77\n\x1bM8F0 7E 7F 06 01 F7\r\n");
        Check(v.size() == 5, "five frames parsed");
        Check(v.size() == 5 && v[0].cmd == HostCmd::kMidi && v[0].port == 1 && v[0].data == "903c64", "M frame");
        Check(v.size() == 5 && v[1].cmd == HostCmd::kName && v[1].port == 2 && v[1].data == "LPK25 mk2", "N frame");
        Check(v.size() == 5 && v[2].cmd == HostCmd::kKeepalive, "K");
        Check(v.size() == 5 && v[3].cmd == HostCmd::kPing && v[3].data == "77", "P frame");
        Check(v.size() == 5 && v[4].port == 8 && v[4].data == "f07e7f0601f7", "sysex, spaces and CR");
    }
    {
        auto v = frames("\x1bM0 903c64\n\x1bM9 90\n\x1bM1 90z1\n\x1bM1 903\n\x1bM1\n\x1bN1\n");
        Check(v.empty(), "bad port, bad hex, odd digits, empty frames dropped");
    }
    {
        std::string big = "\x1bM1 ";
        for(int i = 0; i < 200; i++)
            big += "f7";
        auto v = frames(big + "\n\x1bR");
        Check(v.size() == 1 && v[0].cmd == HostCmd::kResync, "over-long frame dropped, next command fine");
    }
    {
        // A frame cut short by an ESC: the ESC starts the next command
        auto v = frames("\x1bM1 903c\x1bR\x1bN3 MPK249\n");
        Check(v.size() == 2 && v[0].cmd == HostCmd::kResync && v[1].data == "MPK249", "ESC inside a frame starts over");
    }
    {
        auto v = frames("\x1bU turn -2\n\x1bUplay\n\x1bU\n\x1bU track 12\n");
        Check(v.size() == 3 && v[0].cmd == HostCmd::kUi && v[0].data == "turn -2" && v[1].data == "play"
                  && v[2].data == "track 12",
              "U frames (empty one dropped)");
    }
    {
        std::string body(780, 'A');
        auto v = frames("\x1b" "FD 1 0 0 " + body + "\n\x1b" "FD 1 0 0 " + std::string(820, 'A') + "\n\x1bR");
        Check(v.size() == 2 && v[0].cmd == HostCmd::kFile && v[1].cmd == HostCmd::kResync,
              "F frame up to 800 characters, longer dropped");
    }
    {
        // Payloads are text: no frame content can make a BOOT
        auto v = frames("\x1bN1 BOOT\n\x1bP BOOT\n\x1bM1 B0 0B\n");
        bool boot = false;
        for(auto& g : v)
            boot |= g.cmd == HostCmd::kBoot;
        Check(!boot && v.size() == 3, "BOOT inside payloads is just text");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "all ok", fails);
    return fails ? 1 : 0;
}
