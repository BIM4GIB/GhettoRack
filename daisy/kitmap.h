/** Guesses which track a drum sample belongs on from its file name.
 *
 *  Tracks: 1 kick, 2 snare, 3 closed hat, 4 open hat, 5 clap, 6 tom,
 *          7 rim, 8 perc.
 *  A name starting with a digit 1-8 ("3 hat.wav", "5_clap.wav") wins.
 */
#pragma once
#include <cstring>
#include <cctype>

namespace ab
{
namespace kitmap_detail
{
    /** Lowercase copy without the extension */
    inline void Prep(const char* in, char* out, size_t cap)
    {
        size_t n = 0;
        for(; in[n] && n < cap - 1; n++)
            out[n] = (char)tolower((unsigned char)in[n]);
        out[n]    = 0;
        char* dot = strrchr(out, '.');
        if(dot)
            *dot = 0;
    }

    inline bool Has(const char* s, const char* word)
    {
        return strstr(s, word) != nullptr;
    }

    /** True if `code` appears as a whole token (letters bounded by
     *  non-letters), optionally followed by digits: "bd", "BD_01", "bd2" */
    inline bool Token(const char* s, const char* code)
    {
        size_t      len = strlen(code);
        const char* p   = s;
        while((p = strstr(p, code)) != nullptr)
        {
            bool left_ok  = (p == s) || !isalpha((unsigned char)p[-1]);
            char r        = p[len];
            bool right_ok = !isalpha((unsigned char)r);
            if(left_ok && right_ok)
                return true;
            p++;
        }
        return false;
    }
} // namespace kitmap_detail

/** Returns 0-7, or -1 if the name gives no clue */
inline int KitTrackForName(const char* filename)
{
    using namespace kitmap_detail;
    if(filename[0] >= '1' && filename[0] <= '8' && !isdigit((unsigned char)filename[1]))
        return filename[0] - '1';

    char s[128];
    Prep(filename, s, sizeof(s));

    if(Has(s, "open") || Has(s, "ohh") || Token(s, "oh") || Token(s, "ophat"))
        return 3;
    if(Has(s, "clap") || Has(s, "snap") || Token(s, "cp") || Token(s, "clp"))
        return 4;
    if(Has(s, "kick") || Has(s, "kik") || Has(s, "bassdrum") || Has(s, "bass drum")
       || Token(s, "bd") || Token(s, "kck") || Token(s, "kd"))
        return 0;
    if(Has(s, "snare") || Has(s, "snr") || Token(s, "sd") || Token(s, "sn"))
        return 1;
    if(Has(s, "rim") || Has(s, "stick") || Token(s, "rs"))
        return 6;
    if(Has(s, "tom") || Token(s, "lt") || Token(s, "mt") || Token(s, "ht")
       || Token(s, "tm"))
        return 5;
    if(Has(s, "hat") || Has(s, "hh") || Token(s, "ch") || Token(s, "chh")
       || Token(s, "hihat"))
        return 2;
    if(Has(s, "perc") || Has(s, "cow") || Token(s, "cb") || Has(s, "shak")
       || Has(s, "conga") || Has(s, "bongo") || Has(s, "clave") || Has(s, "tamb")
       || Has(s, "cym") || Has(s, "crash") || Has(s, "ride") || Has(s, "bell")
       || Has(s, "block") || Token(s, "fx"))
        return 7;
    return -1;
}

/** Hidden files and macOS "._" resource forks must be skipped */
inline bool IsWavName(const char* name)
{
    if(name[0] == '.' || name[0] == 0)
        return false;
    const char* dot = strrchr(name, '.');
    if(!dot)
        return false;
    return (tolower((unsigned char)dot[1]) == 'w' && tolower((unsigned char)dot[2]) == 'a'
            && tolower((unsigned char)dot[3]) == 'v' && dot[4] == 0);
}

} // namespace ab
