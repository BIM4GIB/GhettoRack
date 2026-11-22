/** Composite USB device on the Daisy's built-in port: CDC-ACM serial +
 *  USB Audio Class 1 input. See usbdev.h.
 *
 *  Interfaces:  0 CDC control (EP 0x82 interrupt)
 *               1 CDC data    (EP 0x01 out, 0x81 in, bulk 64)
 *               2 Audio control
 *               3 Audio streaming, alt 1 = EP 0x83 isochronous in
 *  One class struct handles all four; usbd_ctlreq_apc.c lets the ST core
 *  route requests for interfaces 2 and 3 to it.
 */
#include "usbdev.h"
#include "stm32h7xx_hal.h"
#include <cstring>
#include <cmath>

extern "C"
{
#include "usbd_def.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
    extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
}

namespace usbdev
{
namespace
{
/* ---------------- descriptors ---------------- */

constexpr uint16_t kVid = 0x1209; // pid.codes
constexpr uint16_t kPid = 0x0001; // pid.codes test PID, for private use
constexpr uint8_t  kEpCdcOut = 0x01, kEpCdcIn = 0x81, kEpCdcCmd = 0x82, kEpAudio = 0x83;
constexpr uint16_t kCdcPacket   = 64;
constexpr int      kRate        = 48000;
constexpr int      kFrameBytes  = 4; // 2 channels x 16 bit
constexpr int      kMaxFrames   = 49; // per 1 ms packet
constexpr uint16_t kAudioPacket = kMaxFrames * kFrameBytes;

#define LE16(x) (uint8_t)((x)&0xFF), (uint8_t)(((x) >> 8) & 0xFF)

__ALIGN_BEGIN uint8_t dev_desc[18] __ALIGN_END = {
    18, USB_DESC_TYPE_DEVICE, LE16(0x0200),
    0xEF, 0x02, 0x01,          // composite with association descriptors
    USB_MAX_EP0_SIZE, LE16(kVid), LE16(kPid), LE16(0x0100),
    USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR, 1};

constexpr uint16_t kAcTotal  = 9 + 12 + 9; // AC header + input + output terminal
constexpr uint16_t kCfgTotal = 9 + 66 + 99;

__ALIGN_BEGIN uint8_t cfg_desc[kCfgTotal] __ALIGN_END = {
    /* configuration */
    9, USB_DESC_TYPE_CONFIGURATION, LE16(kCfgTotal), 4, 1, 0, 0x80, 250, // bus powered, up to 500 mA

    /* ---- CDC serial: IAD + 2 interfaces ---- */
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
    9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, LE16(0x0110),           // header
    5, 0x24, 0x01, 0x00, 1,                // call management
    4, 0x24, 0x02, 0x02,                   // ACM: line coding
    5, 0x24, 0x06, 0, 1,                   // union
    7, USB_DESC_TYPE_ENDPOINT, kEpCdcCmd, 0x03, LE16(8), 16,
    9, USB_DESC_TYPE_INTERFACE, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, USB_DESC_TYPE_ENDPOINT, kEpCdcOut, 0x02, LE16(kCdcPacket), 0,
    7, USB_DESC_TYPE_ENDPOINT, kEpCdcIn, 0x02, LE16(kCdcPacket), 0,

    /* ---- audio: IAD + control + streaming ---- */
    8, 0x0B, 2, 2, 0x01, 0x00, 0x00, USBD_IDX_INTERFACE_STR,
    9, USB_DESC_TYPE_INTERFACE, 2, 0, 0, 0x01, 0x01, 0x00, 0,
    9, 0x24, 0x01, LE16(0x0100), LE16(kAcTotal), 1, 3,          // AC header -> AS 3
    12, 0x24, 0x02, 1, LE16(0x0603), 0, 2, LE16(0x0003), 0, 0,   // input: line, L+R
    9, 0x24, 0x03, 2, LE16(0x0101), 0, 1, 0,                     // output: USB stream
    9, USB_DESC_TYPE_INTERFACE, 3, 0, 0, 0x01, 0x02, 0x00, 0,    // alt 0: no bandwidth
    9, USB_DESC_TYPE_INTERFACE, 3, 1, 1, 0x01, 0x02, 0x00, 0,    // alt 1: streaming
    7, 0x24, 0x01, 2, 1, LE16(0x0001),                           // PCM from terminal 2
    11, 0x24, 0x02, 1, 2, 2, 16, 1, 0x80, 0xBB, 0x00,            // 2 ch 16 bit 48000
    9, USB_DESC_TYPE_ENDPOINT, kEpAudio, 0x05, LE16(kAudioPacket), 1, 0, 0, // iso async
    7, 0x25, 0x01, 0x00, 0, LE16(0),
};
static_assert(sizeof(cfg_desc) == kCfgTotal, "config descriptor length");

__ALIGN_BEGIN uint8_t qualifier_desc[10] __ALIGN_END
    = {10, USB_DESC_TYPE_DEVICE_QUALIFIER, LE16(0x0200), 0xEF, 0x02, 0x01, 64, 1, 0};

__ALIGN_BEGIN uint8_t langid_desc[4] __ALIGN_END = {4, USB_DESC_TYPE_STRING, LE16(0x0409)};
__ALIGN_BEGIN uint8_t str_buf[128] __ALIGN_END;

uint8_t* String(const char* s, uint16_t* len)
{
    USBD_GetString((uint8_t*)s, str_buf, len);
    return str_buf;
}
uint8_t* DevDesc(USBD_SpeedTypeDef, uint16_t* len)
{
    *len = sizeof(dev_desc);
    return dev_desc;
}
uint8_t* LangDesc(USBD_SpeedTypeDef, uint16_t* len)
{
    *len = sizeof(langid_desc);
    return langid_desc;
}
uint8_t* MfcDesc(USBD_SpeedTypeDef, uint16_t* len) { return String("ApcBox", len); }
uint8_t* ProductDesc(USBD_SpeedTypeDef, uint16_t* len) { return String("ApcBox", len); }
uint8_t* SerialDesc(USBD_SpeedTypeDef, uint16_t* len)
{
    char     s[25];
    uint32_t a = HAL_GetUIDw0(), b = HAL_GetUIDw1(), c = HAL_GetUIDw2();
    const char* hx = "0123456789ABCDEF";
    uint32_t w[3] = {a, b, c};
    for(int i = 0; i < 24; i++)
        s[i] = hx[(w[i / 8] >> (28 - 4 * (i % 8))) & 15];
    s[24] = 0;
    return String(s, len);
}
uint8_t* ConfigDesc(USBD_SpeedTypeDef, uint16_t* len) { return String("ApcBox", len); }
uint8_t* InterfaceDesc(USBD_SpeedTypeDef, uint16_t* len) { return String("ApcBox", len); }

USBD_DescriptorsTypeDef descriptors = {DevDesc, LangDesc, MfcDesc, ProductDesc,
                                       SerialDesc, ConfigDesc, InterfaceDesc};

/* ---------------- state ---------------- */

USBD_HandleTypeDef dev;
volatile bool      configured = false;
RxFn               rx_fn      = nullptr;

/* serial */
uint8_t          cdc_rx[kCdcPacket];
volatile bool    cdc_busy = false;
uint8_t          line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8}; // 115200 8N1
uint8_t          ep0_buf[64];
uint8_t          ep0_req = 0; // class request waiting for its data stage
uint8_t          ep0_itf = 0;

/* audio */
constexpr int     kRing   = 1024; // frames
constexpr int     kTarget = 96;   // frames kept queued (2 ms)
int16_t           ring[kRing * 2];
volatile uint32_t ring_w = 0, ring_r = 0;
volatile bool     streaming = false, inflight = false;
uint8_t           alt_setting[4] = {};
int16_t           packet[kMaxFrames * 2];
volatile uint32_t st_packets = 0, st_under = 0, st_over = 0, st_incomplete = 0;

void SendAudioPacket(USBD_HandleTypeDef* pdev)
{
    uint32_t w    = ring_w;
    int      fill = (int)(w - ring_r);
    if(fill > kRing - kMaxFrames) // the host stopped reading for a while
    {
        ring_r = w - kTarget;
        fill   = kTarget;
        st_over++;
    }
    int n = kRate / 1000;
    if(fill > kTarget + 16)
        n++;
    else if(fill < kTarget - 16)
        n--;
    if(fill < n)
    {
        memset(packet, 0, (size_t)n * kFrameBytes); // nothing yet: silence
        st_under++;
    }
    else
    {
        uint32_t r = ring_r;
        for(int i = 0; i < n; i++)
        {
            uint32_t k        = (r + i) % kRing;
            packet[2 * i]     = ring[2 * k];
            packet[2 * i + 1] = ring[2 * k + 1];
        }
        ring_r = r + n;
    }
    inflight = true;
    st_packets++;
    USBD_LL_Transmit(pdev, kEpAudio, (uint8_t*)packet, (uint32_t)n * kFrameBytes);
}

void StartStream(USBD_HandleTypeDef* pdev)
{
    if(streaming)
        return;
    USBD_LL_OpenEP(pdev, kEpAudio, USBD_EP_TYPE_ISOC, kAudioPacket);
    pdev->ep_in[kEpAudio & 0xF].is_used = 1U;
    pdev->ep_in[kEpAudio & 0xF].maxpacket = kAudioPacket;
    ring_r    = ring_w - kTarget;
    inflight  = false;
    streaming = true;
}

void StopStream(USBD_HandleTypeDef* pdev)
{
    if(!streaming)
        return;
    streaming = false;
    USBD_LL_FlushEP(pdev, kEpAudio);
    USBD_LL_CloseEP(pdev, kEpAudio);
    pdev->ep_in[kEpAudio & 0xF].is_used = 0U;
    inflight = false;
}

/* ---------------- class callbacks ---------------- */

uint8_t ClassInit(USBD_HandleTypeDef* pdev, uint8_t)
{
    USBD_LL_OpenEP(pdev, kEpCdcIn, USBD_EP_TYPE_BULK, kCdcPacket);
    pdev->ep_in[kEpCdcIn & 0xF].is_used = 1U;
    USBD_LL_OpenEP(pdev, kEpCdcOut, USBD_EP_TYPE_BULK, kCdcPacket);
    pdev->ep_out[kEpCdcOut & 0xF].is_used = 1U;
    USBD_LL_OpenEP(pdev, kEpCdcCmd, USBD_EP_TYPE_INTR, 8);
    pdev->ep_in[kEpCdcCmd & 0xF].is_used  = 1U;
    pdev->ep_in[kEpCdcCmd & 0xF].bInterval = 16;
    USBD_LL_PrepareReceive(pdev, kEpCdcOut, cdc_rx, kCdcPacket);
    memset(alt_setting, 0, sizeof(alt_setting));
    cdc_busy          = false;
    pdev->pClassData  = (void*)&configured;
    configured        = true;
    return (uint8_t)USBD_OK;
}

uint8_t ClassDeInit(USBD_HandleTypeDef* pdev, uint8_t)
{
    configured = false;
    StopStream(pdev);
    USBD_LL_CloseEP(pdev, kEpCdcIn);
    USBD_LL_CloseEP(pdev, kEpCdcOut);
    USBD_LL_CloseEP(pdev, kEpCdcCmd);
    pdev->ep_in[kEpCdcIn & 0xF].is_used   = 0U;
    pdev->ep_out[kEpCdcOut & 0xF].is_used = 0U;
    pdev->ep_in[kEpCdcCmd & 0xF].is_used  = 0U;
    pdev->pClassData = nullptr;
    cdc_busy         = false;
    return (uint8_t)USBD_OK;
}

uint8_t ClassSetup(USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req)
{
    static uint8_t status[2] = {0, 0};
    const uint8_t  itf       = LOBYTE(req->wIndex);
    const bool     to_host   = (req->bmRequest & 0x80U) != 0U;

    switch(req->bmRequest & USB_REQ_TYPE_MASK)
    {
        case USB_REQ_TYPE_STANDARD:
            switch(req->bRequest)
            {
                case USB_REQ_GET_STATUS:
                    USBD_CtlSendData(pdev, status, 2);
                    return (uint8_t)USBD_OK;
                case USB_REQ_GET_INTERFACE:
                    USBD_CtlSendData(pdev, &alt_setting[itf & 3], 1);
                    return (uint8_t)USBD_OK;
                case USB_REQ_SET_INTERFACE:
                {
                    uint8_t alt = LOBYTE(req->wValue);
                    if(pdev->dev_state != USBD_STATE_CONFIGURED || itf > 3
                       || (itf == 3 ? alt > 1 : alt != 0))
                        break;
                    alt_setting[itf] = alt;
                    if(itf == 3)
                    {
                        if(alt)
                            StartStream(pdev);
                        else
                            StopStream(pdev);
                    }
                    return (uint8_t)USBD_OK;
                }
                case USB_REQ_CLEAR_FEATURE: return (uint8_t)USBD_OK;
                default: break;
            }
            break;

        case USB_REQ_TYPE_CLASS:
        {
            const bool to_endpoint = (req->bmRequest & 0x1FU) == USB_REQ_RECIPIENT_ENDPOINT;
            if(!to_endpoint && itf == 0) // CDC
            {
                if(req->wLength == 0)
                    return (uint8_t)USBD_OK; // SET_CONTROL_LINE_STATE etc.
                if(to_host)
                {
                    uint16_t n = req->wLength < 7 ? req->wLength : 7;
                    USBD_CtlSendData(pdev, line_coding, n); // GET_LINE_CODING
                }
                else
                {
                    ep0_req = req->bRequest;
                    ep0_itf = 0;
                    USBD_CtlPrepareRx(pdev, ep0_buf,
                                      req->wLength < sizeof(ep0_buf) ? req->wLength
                                                                     : sizeof(ep0_buf));
                }
                return (uint8_t)USBD_OK;
            }
            /* Audio: nothing is adjustable. Answer reads (the sample rate
               is 48000), accept and ignore writes, rather than stall:
               some hosts set the rate even when told it's fixed. */
            if(req->wLength == 0)
                return (uint8_t)USBD_OK;
            if(to_host)
            {
                static uint8_t reply[8];
                memset(reply, 0, sizeof(reply));
                if(HIBYTE(req->wValue) == 0x01) // sampling frequency
                {
                    reply[0] = (uint8_t)(kRate & 0xFF);
                    reply[1] = (uint8_t)((kRate >> 8) & 0xFF);
                    reply[2] = (uint8_t)((kRate >> 16) & 0xFF);
                }
                USBD_CtlSendData(pdev, reply, req->wLength < 8 ? req->wLength : 8);
            }
            else
            {
                ep0_req = req->bRequest;
                ep0_itf = 2;
                USBD_CtlPrepareRx(pdev, ep0_buf,
                                  req->wLength < sizeof(ep0_buf) ? req->wLength : sizeof(ep0_buf));
            }
            return (uint8_t)USBD_OK;
        }
        default: break;
    }
    USBD_CtlError(pdev, req);
    return (uint8_t)USBD_FAIL;
}

uint8_t ClassEP0RxReady(USBD_HandleTypeDef*)
{
    if(ep0_itf == 0 && ep0_req == 0x20) // SET_LINE_CODING
        memcpy(line_coding, ep0_buf, sizeof(line_coding));
    ep0_req = 0;
    return (uint8_t)USBD_OK;
}

uint8_t ClassDataIn(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if(epnum == (kEpAudio & 0xF))
    {
        inflight = false;
        if(streaming)
            SendAudioPacket(pdev); // keep one packet in every frame
        return (uint8_t)USBD_OK;
    }
    if(epnum == (kEpCdcIn & 0xF))
    {
        USBD_EndpointTypeDef& ep = pdev->ep_in[epnum];
        if(ep.total_length > 0U && (ep.total_length % kCdcPacket) == 0U)
        {
            ep.total_length = 0U; // ended on a full packet: send a zero-length one
            USBD_LL_Transmit(pdev, kEpCdcIn, nullptr, 0U);
        }
        else
            cdc_busy = false;
    }
    return (uint8_t)USBD_OK;
}

uint8_t ClassDataOut(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if(epnum == kEpCdcOut)
    {
        uint32_t n = USBD_LL_GetRxDataSize(pdev, epnum);
        if(rx_fn && n)
            rx_fn(cdc_rx, n);
        USBD_LL_PrepareReceive(pdev, kEpCdcOut, cdc_rx, kCdcPacket);
    }
    return (uint8_t)USBD_OK;
}

uint8_t ClassSOF(USBD_HandleTypeDef* pdev)
{
    if(streaming && !inflight)
        SendAudioPacket(pdev); // start, or restart after a missed frame
    return (uint8_t)USBD_OK;
}

uint8_t ClassIsoInIncomplete(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if(epnum == (kEpAudio & 0xF) || epnum == kEpAudio)
    {
        USBD_LL_FlushEP(pdev, kEpAudio);
        inflight = false;
        st_incomplete++;
    }
    return (uint8_t)USBD_OK;
}

uint8_t ClassIsoOutIncomplete(USBD_HandleTypeDef*, uint8_t) { return (uint8_t)USBD_OK; }

uint8_t* GetCfg(uint16_t* len)
{
    *len = sizeof(cfg_desc);
    return cfg_desc;
}
uint8_t* GetQualifier(uint16_t* len)
{
    *len = sizeof(qualifier_desc);
    return qualifier_desc;
}

USBD_ClassTypeDef apcbox_class = {
    ClassInit,       ClassDeInit,   ClassSetup,         nullptr, ClassEP0RxReady,
    ClassDataIn,     ClassDataOut,  ClassSOF,           ClassIsoInIncomplete,
    ClassIsoOutIncomplete, GetCfg, GetCfg, GetCfg, GetQualifier,
#if(USBD_SUPPORT_USER_STRING_DESC == 1U)
    nullptr,
#endif
};

} // namespace

/* ---------------- public ---------------- */

void Init()
{
    USBD_Init(&dev, &descriptors, DEVICE_FS);
    /* FIFOs for the extra endpoints (libDaisy sets up RX, EP0 and EP1) */
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 2, 0x10);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 3, 0x80);
    USBD_RegisterClass(&dev, &apcbox_class);
    USBD_Start(&dev);
    USB_OTG_FS->GINTMSK |= USB_OTG_GINTMSK_SOFM; // 1 kHz tick for the audio
}

void SetRx(RxFn fn)
{
    rx_fn = fn;
}

bool SerialReady()
{
    return configured && dev.dev_state == USBD_STATE_CONFIGURED;
}

bool SerialSend(const uint8_t* data, uint16_t len)
{
    if(!SerialReady() || cdc_busy)
        return false;
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    cdc_busy                                 = true;
    dev.ep_in[kEpCdcIn & 0xF].total_length  = len;
    USBD_StatusTypeDef r = USBD_LL_Transmit(&dev, kEpCdcIn, (uint8_t*)data, len);
    if(r != USBD_OK)
        cdc_busy = false;
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
    return r == USBD_OK;
}

void AudioPush(const float* l, const float* r, size_t n)
{
    uint32_t w = ring_w;
    if(streaming && (int)(w - ring_r) + (int)n > kRing)
    {
        st_over++;
        return; // nobody reading fast enough: drop this block
    }
    for(size_t i = 0; i < n; i++)
    {
        float a = l[i] * 32767.f, b = r[i] * 32767.f;
        a = a > 32767.f ? 32767.f : (a < -32768.f ? -32768.f : a);
        b = b > 32767.f ? 32767.f : (b < -32768.f ? -32768.f : b);
        uint32_t k      = (w + i) % kRing;
        ring[2 * k]     = (int16_t)lrintf(a);
        ring[2 * k + 1] = (int16_t)lrintf(b);
    }
    ring_w = w + (uint32_t)n;
}

bool AudioStreaming()
{
    return streaming;
}

AudioStats Stats()
{
    AudioStats s;
    s.packets    = st_packets;
    s.underruns  = st_under;
    s.overruns   = st_over;
    s.incomplete = st_incomplete;
    s.fill       = (int)(ring_w - ring_r);
    return s;
}

} // namespace usbdev
