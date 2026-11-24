/** Samples sent from the T95 (v13.8, control protocol 3). Included by box.cpp.
 *
 *  The T95 keeps the sample library; the Daisy only needs the sound in a
 *  track's memory. A sample arrives as a whole WAV file (the T95 converts
 *  it), chunk by chunk, into the staging pool that set loading uses (so no
 *  extra SDRAM), while the track keeps playing its old sound. Once every
 *  chunk is in, the normal loader reads it from there like a file
 *  ("t95/<name>"): same WAV parsing, mono mix-down, rate, naming, and a set
 *  saved later keeps it (sets write their WAVs from memory).
 *
 *  Frames (text after ESC F, one per line, see protocol/serial.md):
 *    O id track size name      start: track 1-16, size in bytes
 *    D id seq crc32 base64     chunk seq (512 bytes, the last may be shorter)
 *    C id                      all sent: check every chunk arrived, load it
 *    A id                      abort
 *  Replies: FO id ok capacity | FO id err why, FA id seq ok|err why,
 *           FC id ok | FC id err why, FL id ok|err (loaded into the track).
 *  Chunks can come in any order and twice: each one carries its CRC32, a
 *  bitmap says which are in, and nothing loads until all are.
 */
#pragma once

constexpr uint32_t kXferChunk     = 512;
constexpr uint32_t kXferMaxChunks = 20000; // ~10 MB, more than the staging pool
constexpr uint32_t kXferTimeout   = 10000; // ms without a frame: give up

enum XferState : uint8_t
{
    kXferIdle,
    kXferRecv,
    kXferLoading,
};

XferState xfer_state = kXferIdle;
int       xfer_id    = 0;
int       xfer_track = 0;
uint32_t  xfer_size = 0, xfer_chunks = 0, xfer_have = 0, xfer_time = 0;
uint8_t*  xfer_buf = nullptr;
uint8_t   xfer_got[kXferMaxChunks / 8 + 1];
uint8_t   xfer_tmp[kXferChunk + 4];

bool XferBusy() { return xfer_state != kXferIdle; }

void XferReset()
{
    xfer_state = kXferIdle;
    xfer_buf   = nullptr;
    mem_file   = nullptr;
    mem_size   = 0;
    mem_open   = false;
}

uint32_t Crc32(const uint8_t* d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < n; i++)
    {
        c ^= d[i];
        for(int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/** Standard base64; returns bytes written, -1 on bad input or overflow */
int B64Decode(const char* s, uint8_t* out, int cap)
{
    auto val = [](char ch) -> int {
        if(ch >= 'A' && ch <= 'Z')
            return ch - 'A';
        if(ch >= 'a' && ch <= 'z')
            return ch - 'a' + 26;
        if(ch >= '0' && ch <= '9')
            return ch - '0' + 52;
        if(ch == '+')
            return 62;
        if(ch == '/')
            return 63;
        return -1;
    };
    int      n = 0, bits = 0;
    uint32_t acc = 0;
    for(; *s && *s != '='; s++)
    {
        int v = val(*s);
        if(v < 0)
            return -1;
        acc  = (acc << 6) | (uint32_t)v;
        bits += 6;
        if(bits >= 8)
        {
            bits -= 8;
            if(n >= cap)
                return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

void XferOpen(const char* t, uint32_t now)
{
    int           id = 0, track = 0, off = 0;
    unsigned long size = 0;
    if(sscanf(t, "O %d %d %lu %n", &id, &track, &size, &off) < 3 || off == 0)
        return;
    const char* why = nullptr;
    uint32_t    n   = 0;
    int16_t*    st  = plat->Staging(&n);
    if(XferBusy())
        why = "busy";
    else if(set_op)
        why = "set-busy";
    else if(track < 1 || track > kTracks)
        why = "track";
    else if(!st || size == 0 || size > n * 2u || (size + kXferChunk - 1) / kXferChunk > kXferMaxChunks)
        why = "too-big";
    if(why)
    {
        Out("FO %d err %s", id, why);
        return;
    }
    /* name: keep it to safe characters, it becomes the track's name */
    char        name[40];
    int         k = 0;
    const char* p = t + off;
    for(; *p && k < 32; p++)
        name[k++] = (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') ? *p : '_';
    name[k] = 0;
    if(k == 0)
        snprintf(name, sizeof(name), "sample.wav");
    snprintf(mem_name, sizeof(mem_name), "t95/%s", name);
    xfer_id     = id;
    xfer_track  = track - 1;
    xfer_size   = (uint32_t)size;
    xfer_chunks = (xfer_size + kXferChunk - 1) / kXferChunk;
    xfer_have   = 0;
    xfer_buf    = (uint8_t*)st;
    xfer_time   = now;
    xfer_state  = kXferRecv;
    memset(xfer_got, 0, sizeof(xfer_got));
    Out("FO %d ok %lu", id, (unsigned long)eng->SlotCapacity());
}

void XferData(const char* t, uint32_t now)
{
    int           id = 0, off = 0;
    unsigned long seq = 0, crc = 0;
    if(sscanf(t, "D %d %lu %lx %n", &id, &seq, &crc, &off) < 3 || off == 0)
        return;
    const char* why = nullptr;
    int         n   = -1;
    if(xfer_state != kXferRecv || id != xfer_id)
        why = "stale";
    else if(seq >= xfer_chunks)
        why = "range";
    else
    {
        uint32_t want = seq == xfer_chunks - 1 ? xfer_size - (uint32_t)seq * kXferChunk : kXferChunk;
        n             = B64Decode(t + off, xfer_tmp, sizeof(xfer_tmp));
        if(n != (int)want)
            why = "length";
        else if(Crc32(xfer_tmp, (uint32_t)n) != (uint32_t)crc)
            why = "crc";
    }
    if(why)
    {
        Out("FA %d %lu err %s", id, seq, why);
        return;
    }
    memcpy(xfer_buf + seq * kXferChunk, xfer_tmp, (size_t)n);
    if(!(xfer_got[seq >> 3] & (1u << (seq & 7))))
    {
        xfer_got[seq >> 3] |= (uint8_t)(1u << (seq & 7));
        xfer_have++;
    }
    xfer_time = now;
    Out("FA %d %lu ok", id, seq);
}

void XferCommit(const char* t)
{
    int id = 0;
    if(sscanf(t, "C %d", &id) != 1)
        return;
    if(xfer_state != kXferRecv || id != xfer_id)
    {
        Out("FC %d err stale", id);
        return;
    }
    if(xfer_have != xfer_chunks)
    {
        Out("FC %d err missing %lu", id, (unsigned long)(xfer_chunks - xfer_have));
        return;
    }
    if(set_op)
    {
        Out("FC %d err set-busy", id);
        return;
    }
    mem_file   = xfer_buf;
    mem_size   = xfer_size;
    xfer_state = kXferLoading;
    QueueLoad(xfer_track, mem_name, false);
    Out("FC %d ok", id);
}

/** From FinishJob: the loader is done with the received file */
void XferLoaded(bool ok)
{
    if(xfer_state != kXferLoading)
        return;
    Out("FL %d %s", xfer_id, ok ? "ok" : "err");
    Log("track %d <- %s from the T95%s", xfer_track + 1, mem_name + 4, ok ? "" : " (not a playable wav)");
    XferReset();
}

void XferFrame(const char* t, uint32_t now)
{
    switch(t[0])
    {
        case 'O': XferOpen(t, now); break;
        case 'D': XferData(t, now); break;
        case 'C': XferCommit(t); break;
        case 'A':
        {
            int id = 0;
            if(sscanf(t, "A %d", &id) == 1 && id == xfer_id && xfer_state == kXferRecv)
            {
                XferReset();
                Out("FC %d err aborted", id);
            }
            break;
        }
        default: break;
    }
}

void XferPoll(uint32_t now)
{
    /* Signed: frames are stamped when parsed, a moment after the main loop
       read `now`; unsigned, that wrapped to ~49 days and dropped every
       transfer at once (caught by t95sim, like the v13.5 keepalive bug) */
    if(xfer_state == kXferRecv && (int32_t)(now - xfer_time) > (int32_t)kXferTimeout)
    {
        Log("sample from the T95: nothing for 10 s, dropped");
        XferReset();
    }
}
