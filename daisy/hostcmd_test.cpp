// Host command parser: echoed output must never trigger a command.
// c++ -std=c++17 -O1 -Wall -o /tmp/hostcmd_test hostcmd_test.cpp && /tmp/hostcmd_test
#include "hostcmd.h"
#include <cstdio>
#include <cstring>
#include <string>

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
    printf("%s (%d failures)\n", fails ? "FAILED" : "all ok", fails);
    return fails ? 1 : 0;
}
