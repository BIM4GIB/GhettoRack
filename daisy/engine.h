/** ApcBox audio engine
 *
 *  Platform-independent: no libDaisy calls, so it compiles and runs on a
 *  desktop for testing. The firmware owns the memory and hands it in.
 *
 *  Threading model (firmware):
 *   - Process() runs in the audio interrupt.
 *   - Everything else is called from the main loop. Shared fields are
 *     single 32-bit values or bytes, written by one side only.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <stdint.h>
#include <stddef.h>
#include "daisysp.h"
#include "acid.h"
#ifdef USE_DAISYSP_LGPL
#include "daisysp-lgpl.h"
#endif

namespace ab
{
constexpr int kTracks   = 16; // two pages of 8 on the APC40
constexpr int kKitSounds = 8; // sounds per built-in kit (one page)
constexpr int kSteps    = 16;
constexpr int kBanks    = 8;
constexpr int kPatterns = kBanks * 8; // 8 banks of 8

/** Step values in a pattern */
enum : uint8_t
{
    kStepOff    = 0,
    kStepOn     = 1,
    kStepAccent = 2,
};

/** 303 tracks: each step also has a note byte. MIDI note in the low 7
 *  bits (0 = not set yet, plays kAcidDefaultNote), kNoteSlide = glide
 *  from this step into the next one. */
constexpr uint8_t kNoteSlide       = 0x80;
constexpr int     kAcidDefaultNote = 36; // C2
constexpr int     kAcidLowNote     = 24; // C1
constexpr int     kAcidHighNote    = 72; // C5

/** Momentary performance effects (APC40 grid row 4) */
enum PerfFx
{
    kFxRepeat4 = 0, // repeat last 1/4 note
    kFxRepeat8,     // 1/8
    kFxRepeat16,    // 1/16
    kFxRepeat32,    // 1/32
    kFxTapeStop,
    kFxReverse,
    kFxCrush,
    kFxGate,
    kNumPerfFx
};

/** One sample slot per track. Data is mono int16. */
struct Slot
{
    int16_t*          data     = nullptr;
    uint32_t          capacity = 0; // samples
    volatile uint32_t length   = 0; // samples, 0 = empty
    float             rate     = 1.f; // file sample rate / engine sample rate
};

/** Per-track controls, all 0..1 unless noted. Written by the main loop. */
struct TrackParams
{
    float vol     = 0.8f;
    float pan     = 0.5f;
    float rev     = 0.f;  // reverb send
    float dly     = 0.f;  // delay send
    float pitch   = 0.5f; // 0.5 = original, 0..1 = -12..+12 semitones
    float decay   = 1.f;  // 1 = play whole sample
    float filter  = 0.5f; // 0.5 = off, below = low-pass, above = high-pass
    float reso    = 0.2f;
    float start   = 0.f;  // sample start offset
    float reverse = 0.f;  // >= 0.5 plays backwards
    float drive   = 0.f;
    float prob    = 1.f;  // step probability
    bool  mute    = false;
    bool  solo    = false;
    AcidParams acid; // used while the track is a 303
    uint8_t xfade = 0; // crossfader side: 0 = both (AB), 1 = A (left), 2 = B (right)
};

/** Global controls. Written by the main loop. */
struct GlobalParams
{
    float bpm       = 120.f;
    float swing     = 0.f; // 0..1 = 50..75 %
    float rev_size  = 0.55f;
    float rev_tone  = 0.6f;
    float dly_time  = 0.5f; // picks a tempo division
    float dly_fb    = 0.35f;
    float drive     = 0.f;
    float fx_return = 0.8f;
    float master    = 0.8f;
    float xfader    = 0.5f; // 0 = A side only, 0.5 = both, 1 = B side only
    float djfilter  = 0.5f; // master LP/HP (CUE LEVEL), 0.5 = off
    bool  metronome = false;
};

/** How one set hands over to the next (over the last bar of the old one) */
enum Transition : uint8_t
{
    kTransCut = 0, // straight in on the bar line
    kTransFilter,  // master low-pass closes over the bar
    kTransTape,    // tape stop over the second half of the bar
    kTransEcho,    // dry fades, delay sends and feedback up, the tail rings on
    kNumTransitions
};

/** Everything a set switch changes, prepared by the main loop and applied
 *  by the audio side at a bar line. Patterns and notes are pointers so the
 *  box can keep them in the buffer it read the set file into. */
struct SetSwap
{
    const uint8_t* patterns; // [kPatterns][kTracks][kSteps]
    const uint8_t* notes;    // same shape
    TrackParams    tracks[kTracks];
    GlobalParams   global;
    bool           synth[kTracks];
    bool           replace[kTracks]; // the slot gets new sample data
    int16_t*       data[kTracks];    // the new data (staging), nullptr = silent for now
    uint32_t       length[kTracks];
    float          rate[kTracks];
    uint8_t        loop_bars[kTracks]; // 0 = plays from steps
    float          loop_bpm[kTracks];
    int            pattern;
    uint8_t        transition;
};

/** Memory the platform provides (SDRAM on the Daisy) */
struct Memory
{
    int16_t* slot_mem[kTracks];
    uint32_t slot_capacity;      // samples per slot
    float*   delay_l;
    float*   delay_r;
    uint32_t delay_len;          // samples per channel
    float*   capture_l;
    float*   capture_r;
    uint32_t capture_len;        // samples per channel
#ifdef USE_DAISYSP_LGPL
    daisysp::ReverbSc* reverb;   // large object, lives in SDRAM
#endif
};

class Engine
{
  public:
    void Init(float sample_rate, const Memory& mem);

    /** Audio callback body. Non-interleaved stereo. */
    void Process(float* out_l, float* out_r, size_t size);

    /* ---- Transport (main loop) ---- */
    void Play();
    void Stop();
    void Panic();
    bool Playing() const { return playing_; }
    int  CurrentStep() const { return step_; }
    void SetRecord(bool on) { record_ = on; }
    bool Recording() const { return record_; }

    /* ---- Pads (main loop) ---- */
    void TriggerLive(int track);
    /** Plays a track once without recording it (sample preview) */
    void Audition(int track);
    void SetRoll(int track, bool on) { roll_[track] = on; }
    /** Held FX wait for the grid while playing (quant_fx) */
    void SetPerfFx(int fx, bool on) { fx_want_[fx] = on; }
    bool PerfFxActive(int fx) const { return fx_want_[fx]; }
    /** What is really running right now */
    bool PerfFxLive(int fx) const { return fx_held_[fx]; }

    /** Increments every time a track fires; the UI uses it to flash LEDs */
    uint32_t TriggerCount(int track) const { return trig_count_[track]; }

    /* ---- Patterns (main loop) ---- */
    uint8_t GetStep(int pat, int track, int step) const
    {
        return patterns_[pat][track][step];
    }
    void SetStep(int pat, int track, int step, uint8_t v)
    {
        patterns_[pat][track][step] = v;
    }
    void ClearTrack(int pat, int track);
    void CopyPattern(int from, int to);
    /** One track's steps and notes from one pattern into another */
    void CopyTrack(int from, int to, int track);
    void ClearPattern(int pat);
    bool PatternEmpty(int pat) const;
    /** A whole pattern (steps then notes) for undo */
    static constexpr int kPatternBytes = 2 * kTracks * kSteps;
    void SavePattern(int pat, uint8_t* dst) const;
    void RestorePattern(int pat, const uint8_t* src);
    void QueuePattern(int pat);
    int  CurrentPattern() const { return pattern_; }
    int  QueuedPattern() const { return queued_pattern_; }
    uint8_t (*PatternData())[kTracks][kSteps] { return patterns_; }

    /* ---- 303 tracks (main loop) ---- */
    /** Switches a track between its sample slot and the 303 voice */
    void SetSynth(int track, bool on) { synth_[track] = on; }
    bool IsSynth(int track) const { return synth_[track]; }
    /** MIDI note of a step (kAcidDefaultNote if never set) */
    int GetNote(int pat, int track, int step) const
    {
        int n = notes_[pat][track][step] & 0x7F;
        return n ? n : kAcidDefaultNote;
    }
    void SetNote(int pat, int track, int step, int note)
    {
        uint8_t& b = notes_[pat][track][step];
        b          = (uint8_t)((b & kNoteSlide) | (note & 0x7F));
    }
    bool GetSlide(int pat, int track, int step) const
    {
        return notes_[pat][track][step] & kNoteSlide;
    }
    void SetSlide(int pat, int track, int step, bool on)
    {
        uint8_t& b = notes_[pat][track][step];
        b          = on ? (b | kNoteSlide) : (b & ~kNoteSlide);
    }
    uint8_t (*NoteData())[kTracks][kSteps] { return notes_; }
    /** Keyboard on a 303 track: held notes stack, last one wins, playing
     *  a second key while one is held slides to it. Records into the
     *  pattern while recording. */
    void LiveNote(int track, int note, bool on);
    /** A MIDI keyboard note on any track: 303 tracks play the note (velocity
     *  100+ = accent), sample tracks play the sample pitched, note 60 = as is.
     *  Recorded like pads when recording. vel 1..127 */
    void KeyNote(int track, int note, int vel, bool on);

    /* ---- live sampling from the audio input (mic / line into L) ---- */
    /** Audio callback, before Process: this block's input */
    void SetInput(const float* l, const float* r)
    {
        in_l_ = l;
        in_r_ = r;
    }
    /** Record into a track from the next bar line (starts the transport
     *  if it's stopped). LiveRecStop ends it at the end of the bar it's
     *  in; the track then loops it in sync. */
    void LiveRecArm(int track, int16_t* buf = nullptr, uint32_t cap = 0)
    {
        lr_req_buf_   = buf;
        lr_req_cap_   = cap;
        lr_req_track_ = track;
        lr_req_       = 1;
    }
    /* `buf`, `cap` (LiveRecArm, LiveRecShot): where the take goes. nullptr = where
       the track's sound is now. Another buffer: the track's sound stays whole
       where it is, and a take that is cancelled puts the track back on it. */
    /** Record on top of the track's loop, in place and in time with it
     *  (only at the tempo the loop was recorded at). Ends on LiveRecStop
     *  (at once) or after lr_len_bars. */
    void LiveRecOverdub(int track)
    {
        lr_req_track_ = track;
        lr_req_       = 4;
    }
    /** Record a one-shot sample into a track: starts when the input gets
     *  loud enough, ends on LiveRecStop or when the slot is full. Nothing
     *  to do with the transport; the track then plays it from its steps. */
    void LiveRecShot(int track, int16_t* buf = nullptr, uint32_t cap = 0)
    {
        lr_req_buf_   = buf;
        lr_req_cap_   = cap;
        lr_req_track_ = track;
        lr_req_       = 5;
    }
    /** A take is asked for, armed or running */
    bool LiveRecBusy() const { return lr_req_ != 0 || lr_state_ != 0; }
    void LiveRecStop() { lr_req_ = 2; }
    void LiveRecCancel() { lr_req_ = 3; }
    int  LiveRecState() const { return lr_state_; } // 0 idle, 1 armed, 2 recording
    enum { kRecLoop = 0, kRecOverdub = 1, kRecShot = 2 };
    int  LiveRecMode() const { return lr_mode_; }
    /** What the mixer is using right now (mutes can wait for the beat or bar) */
    bool MuteLive(int t) const { return mute_eff_[t]; }
    /** Where a loop starts in the bar (sixteenths; beat-quantised takes) */
    int  LoopOffset(int t) const { return loop_off_[t]; }
    /** Looper and quantise settings (main loop writes) */
    volatile uint8_t lr_len_bars = 0;     // a take stops by itself after this many bars, 0 = when told
    volatile bool    lr_countin  = false; // clicks while armed; from stopped, one bar of them first
    volatile uint8_t quant_rec   = 0;     // a take starts on: 0 the bar, 1 the beat
    volatile uint8_t quant_pat   = 0;     // a pattern switch lands on: 0 the bar, 1 the beat, 2 the next step
    volatile uint8_t quant_mute  = 0;     // mute/solo land: 0 at once, 1 on the beat, 2 on the bar
    volatile uint8_t quant_fx    = 1;     // held FX start and end: 0 at once, 1 on the step, 2 on the beat
    volatile float   in_gain     = 1.f;   // input level, 0..4
    int  LiveRecTrack() const { return lr_track_; }
    int  LiveRecBars() const { return lr_bars_done_; }
    /** True once when a recording has finished */
    bool TakeLiveRecDone(int* track, int* bars, int* mode = nullptr)
    {
        if(!lr_done_)
            return false;
        *track   = lr_done_track_;
        *bars    = lr_done_bars_;
        if(mode)
            *mode = lr_done_mode_;
        lr_done_ = false;
        return true;
    }
    bool IsLoop(int t) const { return loop_on_[t]; }
    int  LoopBars(int t) const { return loop_bars_[t]; }
    void ClearLoop(int t) { loop_clear_req_[t] = true; }
    /** The slot's data is a recorded loop of `bars` at `bpm` (restoring a
     *  saved one); it starts from its top on the next bar line */
    void SetLoop(int t, int bars, float bpm, int off = 0)
    {
        loop_set_bars_[t] = bars;
        loop_set_bpm_[t]  = bpm;
        loop_set_off_[t]  = (uint8_t)off;
        __sync_synchronize();
        loop_set_req_[t] = true;
    }
    float LoopBpm(int t) const { return loop_bpm_[t]; }

    /* ---- set switching ---- */
    /** Hand over to another set: at the next bar line (after a bar of
     *  transition unless it's a cut), or in the next block if stopped.
     *  `s` must stay valid until TakeSwapDone. */
    void ArmSwap(const SetSwap* s)
    {
        swap_done_ = false;
        __sync_synchronize();
        swap_      = s;
        swap_state_ = 1;
    }
    /** Same thing from the main loop when audio isn't running */
    void ApplySwapNow(const SetSwap* s)
    {
        swap_ = s;
        ApplySwap();
    }
    /** 0 idle, 1 waiting for the bar line, 2 in the transition bar */
    int  SwapState() const { return swap_state_; }
    bool TakeSwapDone()
    {
        if(!swap_done_)
            return false;
        swap_done_ = false;
        return true;
    }
    /** After copying a staged slot back into its own memory */
    void SlotHome(int t, uint32_t length)
    {
        (void)length;
        SlotPoint(t, home_[t], slot_capacity_);
    }
    /** The track's sound is at `data` from now on (room for `cap` samples).
     *  Either the same samples are there already, so a playing voice doesn't
     *  notice, or the slot is empty or locked. */
    void SlotPoint(int t, int16_t* data, uint32_t cap)
    {
        Slot& s    = slots_[t];
        s.capacity = cap;
        __sync_synchronize();
        s.data = data;
    }
    /** Samples a track's own slot holds, and a bar at the tempo now */
    uint32_t SlotCapacity() const { return slot_capacity_; }
    uint32_t BarSamples() const { return (uint32_t)(kSteps * BaseStepLength()); }
    int16_t* HomeData(int t) const { return home_[t]; }
    /** Pitch bend from a keyboard, in semitones, per track */
    volatile float bend_st[kTracks] = {};
    /** Hear the input all the time (it's always heard while recording) */
    volatile bool input_monitor = false;
    /** Input level 0..1, peak with a slow fall */
    /** Following an outside clock (the T95's clock module). Call once per
     *  sixteenth or so with where the outside clock is in the bar right now,
     *  in sixteenths (0..16, straight, already corrected for the time the
     *  message took). The sequencer stays on the audio clock: this only
     *  nudges it. A small error is slewed out over a second or two (so
     *  jitter in the messages doesn't shake the tempo); more than half a
     *  step off, it jumps there. Set the tempo with global.bpm as usual. */
    void ExtSync(float bar_pos16)
    {
        ext_pos_ = bar_pos16;
        ext_req_ = true;
    }
    /** How far the sequencer was from the outside clock at the last ExtSync,
     *  smoothed, in ms (+ = we were behind) */
    float ExtErrMs() const { return ext_err_ * 1000.f / sr_; }
    void  ExtRelease() { ext_slew_ = 0.f; ext_err_ = 0.f; }
    /** Samples since the last step fired, and that step */
    uint32_t SinceStep() const { return since_step_; }
    int      ClockStep() const { return clock_step_; }

    float InputPeak() const { return in_peak_; }
    /** Level meters for the UI: the highest post-fader sample of a track
     *  (0..15) or of the master output (16 left, 17 right) since the last
     *  call. The audio thread only raises it; reading clears it. */
    float TakePeak(int i)
    {
        float v  = peak_[i];
        peak_[i] = 0.f;
        return v;
    }

    /* ---- Samples (main loop) ---- */
    Slot& GetSlot(int track) { return slots_[track]; }
    /** Ask the audio side to release a slot before overwriting it */
    void RequestSlotLock(int track) { lock_req_[track] = true; }
    bool SlotLocked(int track) const { return locked_[track]; }
    void ReleaseSlotLock(int track) { lock_req_[track] = false; }

    /** Synthesized kits that need no SD card */
    void               RenderBuiltInKit(int kit = 0);
    void               RenderBuiltInSound(int track, int kit);
    /** Same sound into any buffer; returns its length */
    uint32_t           RenderBuiltInTo(int track, int kit, int16_t* dst, uint32_t cap);
    static int         NumBuiltInKits();
    static const char* BuiltInKitName(int kit);
    static const char* BuiltInSoundName(int kit, int track);

    TrackParams  track[kTracks];
    GlobalParams global;

    /** Samples where the limiter pulled more than 1 dB (clip indicator) */
    volatile uint32_t limit_count = 0;
    /** Times the output went non-finite and the engine reset its state */
    volatile uint32_t nan_count = 0;

    /** Seconds of audio a slot can hold */
    float SlotSeconds() const { return slot_capacity_ / sr_; }
    float SampleRate() const { return sr_; }

  private:
    struct Voice
    {
        bool    active    = false;
        int32_t idx       = 0;   // integer read position
        float   frac      = 0.f; // fractional part
        int32_t dir       = 1;   // +1 forward, -1 reverse
        float   amp       = 0.f; // decay envelope
        float   decay     = 1.f; // per-sample multiplier
        float   fade      = 1.f; // retrigger declick
        float   fade_step = 0.f;
        float   att       = 1.f; // short attack when starting mid-sample
        float   att_step  = 0.f;
        float   gain      = 1.f; // velocity
        float   rate      = 1.f; // keyboard pitch on top of the track's
    };

    struct Ramp
    {
        float cur = 0.f, step = 0.f;
        void  Set(float target, size_t n)
        {
            step = (target - cur) / (float)n;
        }
        inline float Next()
        {
            cur += step;
            return cur;
        }
    };

    void  StartVoice(int t, float vel, float rate = 1.f);
    void  RecordHit(int t);
    void  AcidStep(int t, int s);
    void  LiveRecStep(int s);
    void  LiveRecFinish();
    void  LiveRecBuffer(int t);
    bool  LiveRecRestore();
    void  ShotFinish();
    void  AcidLiveEvents();
    void  AcidRecord(int t, int note);
    float RenderVoice(Voice& v, const Slot& sl, float inc);
    void  LiveHit(int t, bool record);
    void  OnStep(int s);
    void  ApplySwap();
    float SwapPhase() const;
    void  FxPress(int f);
    void  FxRelease(int f);
    void  Click(bool downbeat);
    float StepLength(int s) const;
    float BaseStepLength() const;
    void  HandleRequests();
    void  UpdateBlockParams(size_t size);
    void  PerfFxEdges();
    void  ProcessPerfFx(float& l, float& r);
    float CaptureRead(const float* buf, double pos) const;

    float sr_ = 48000.f;

    // samples
    Slot     slots_[kTracks];
    uint32_t slot_capacity_ = 0;

    // voices: two per track (current + fading out)
    Voice voices_[kTracks][2];
    int   cur_voice_[kTracks] = {};

    // per-track processing
    daisysp::Svf filt_[kTracks];
    bool         filt_on_[kTracks] = {};
    bool         filt_hp_[kTracks] = {};
    Ramp         gain_l_[kTracks], gain_r_[kTracks];
    Ramp         send_rev_[kTracks], send_dly_[kTracks];
    float        drive_gain_[kTracks] = {}, drive_comp_[kTracks] = {};
    float        pitch_ratio_[kTracks] = {};
    uint32_t     tail_[kTracks]        = {}; // filter ring-out after voices end
    float        last_filter_[kTracks], last_reso_[kTracks];

    // live sampling
    const float*      in_l_ = nullptr, *in_r_ = nullptr;
    volatile uint8_t  lr_req_       = 0; // 1 arm, 2 stop at bar end, 3 cancel
    volatile int      lr_req_track_ = 0;
    int16_t* volatile lr_req_buf_   = nullptr; // where the take goes (nullptr: in place)
    volatile uint32_t lr_req_cap_   = 0;
    bool              lr_swapped_   = false;   // the take has its own buffer; what the track held:
    int16_t*          lr_prev_data_ = nullptr;
    uint32_t          lr_prev_cap_  = 0, lr_prev_len_ = 0;
    bool              lr_prev_loop_ = false, lr_prev_synth_ = false;
    volatile uint8_t  lr_state_     = 0;
    volatile int      lr_track_     = -1;
    volatile int      lr_bars_done_ = 0;
    bool              lr_stop_      = false;
    uint32_t          lr_pos_       = 0;
    volatile bool     lr_done_      = false;
    int               lr_done_track_ = 0, lr_done_bars_ = 0, lr_done_mode_ = 0;
    volatile uint8_t  lr_mode_      = 0; // kRecLoop / kRecOverdub / kRecShot
    int               lr_off_       = 0; // step in the bar the take started on
    int               lr_wait_      = 0; // bar lines still to pass (count-in)
    uint8_t           loop_off_[kTracks] = {};
    bool              mute_eff_[kTracks] = {}, solo_eff_[kTracks] = {};
    bool              mute_latch_   = true;
    uint32_t          bar_count_    = 0;
    volatile bool     loop_on_[kTracks]        = {};
    volatile bool     loop_clear_req_[kTracks] = {};
    int               loop_bars_[kTracks]      = {};
    uint32_t          rl_len_[kTracks]         = {};
    float             loop_bpm_[kTracks]       = {};
    uint32_t          loop_start_bar_[kTracks] = {};
    double            rl_pos_[kTracks]         = {};
    float             loop_rate_[kTracks]      = {};
    volatile float    in_peak_ = 0.f;
    volatile float    peak_[kTracks + 2] = {};
    volatile bool     loop_set_req_[kTracks]   = {};
    int               loop_set_bars_[kTracks]  = {};
    float             loop_set_bpm_[kTracks]   = {};
    uint8_t           loop_set_off_[kTracks]   = {};

    // set switching
    int16_t*                home_[kTracks] = {};
    const SetSwap* volatile swap_       = nullptr;
    volatile int            swap_state_ = 0;
    volatile bool           swap_done_  = false;
    uint8_t                 trans_      = kTransCut; // running transition
    bool                    trans_tape_ = false;
    float                   echo_post_  = 0.f; // feedback boost after an echo out, falls to 0

    // 303 voices
    AcidVoice     acid_[kTracks];
    volatile bool synth_[kTracks]      = {};
    bool          synth_prev_[kTracks] = {};
    bool          acid_tie_[kTracks]   = {}; // last step slides into the next
    int32_t       acid_gate_[kTracks]  = {}; // samples until gate off, -1 = held
    int           acid_last_[kTracks]  = {}; // last note played (rolls)
    int8_t        live_stack_[kTracks][8];
    int           live_n_[kTracks] = {};
    struct NoteEvent
    {
        int8_t  track, note;
        bool    on;
        uint8_t vel; // 0 = from the pads (no velocity)
    };
    static constexpr int kNoteQueue = 32;
    NoteEvent            note_q_[kNoteQueue];
    volatile uint32_t    note_w_ = 0, note_r_ = 0; // main loop writes, audio reads

    // sequencer
    uint8_t           patterns_[kPatterns][kTracks][kSteps] = {};
    uint8_t           notes_[kPatterns][kTracks][kSteps]    = {};
    volatile int      pattern_        = 0;
    volatile int      queued_pattern_ = -1;
    volatile bool     playing_        = false;
    volatile bool     play_req_       = false;
    volatile bool     stop_req_       = false;
    volatile bool     panic_req_      = false;
    volatile bool     record_         = false;
    volatile int      step_           = 0;
    float             to_next_step_   = 0.f; // samples until next step
    float             cur_step_len_   = 1.f;
    int               clock_step_     = 0;   // free-running clock when stopped
    uint32_t          since_step_     = 0;   // samples since last step boundary
    volatile float    ext_pos_        = 0.f;
    volatile bool     ext_req_        = false;
    float             ext_err_        = 0.f; // smoothed error, samples
    float             ext_slew_       = 0.f; // extra samples taken off to_next_step_ per sample
    volatile uint8_t  live_req_[kTracks] = {}; // 1 live hit, 2 audition
    volatile bool     roll_[kTracks]     = {};
    int               skip_once_[kTracks];
    volatile uint32_t trig_count_[kTracks] = {};
    uint32_t          rng_ = 22222;

    // slot locking
    volatile bool lock_req_[kTracks] = {};
    volatile bool locked_[kTracks]   = {};

    // sends and master
    float*   dly_l_ = nullptr;
    float*   dly_r_ = nullptr;
    uint32_t dly_len_ = 0, dly_w_ = 0;
    float    dly_time_smooth_ = 0.f;
    float    dly_fb_lp_l_ = 0.f, dly_fb_lp_r_ = 0.f;
    float    dly_target_ = 0.f, dly_fb_ = 0.f;
    uint32_t dly_kill_ = 0; // samples left of muting after a panic
#ifdef USE_DAISYSP_LGPL
    daisysp::ReverbSc* reverb_ = nullptr;
#endif
    daisysp::Svf dj_l_, dj_r_;
    int          dj_mode_ = 0; // 0 off, 1 low-pass, 2 high-pass
    float        last_dj_ = -1.f;
    Ramp         master_gain_, fx_return_;
    float        master_drive_ = 1.f, master_drive_comp_ = 1.f;
    float        lim_env_ = 0.f;
    bool         nan_reset_ = false;

    // metronome
    float click_y1_ = 0.f, click_y2_ = 0.f, click_coef_ = 0.f, click_env_ = 0.f;

    // performance FX
    enum CapMode
    {
        kCapNone,
        kCapRepeat,
        kCapTape,
        kCapReverse
    };
    volatile bool fx_held_[kNumPerfFx] = {};
    volatile bool fx_want_[kNumPerfFx] = {};
    bool          fx_latch_            = true;
    bool          fx_prev_[kNumPerfFx] = {};
    float*        cap_l_ = nullptr;
    float*        cap_r_ = nullptr;
    uint32_t      cap_len_ = 0, cap_w_ = 0;
    bool          cap_writing_ = true;
    CapMode       cap_mode_    = kCapNone;
    int           cap_fx_      = -1; // which pad owns the capture mode
    uint32_t      loop_start_ = 0, loop_len_ = 1, loop_pos_ = 0;
    uint32_t      cap_have_ = 0; // samples captured since loop_start_
    double        tape_pos_ = 0.0;
    float         tape_rate_ = 1.f, tape_dec_ = 0.f;
    float         fx_mix_ = 0.f, fx_mix_target_ = 0.f;
    daisysp::Decimator crush_l_, crush_r_;
    float         crush_mix_ = 0.f, gate_gain_ = 1.f;
};

/** Fast soft clip, roughly tanh for |x| < 3 */
inline float SoftClip(float x)
{
    if(x > 3.f)
        return 1.f;
    if(x < -3.f)
        return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

} // namespace ab
