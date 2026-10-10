#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void remote_endpoint;
enum { REMOTE_CONTROL=1, REMOTE_INPUT=2, REMOTE_CURSOR=3, REMOTE_VIDEO=4 };
/* Start is asynchronous. Status: 0 connecting/listening, 1 connected, -1 error. */
remote_endpoint* remote_server_start(const char* bind_addr, const char* token, const char* identity_dir);
remote_endpoint* remote_client_start(const char* server_addr, const char* token, const char* cert_sha256);
/* cert_sha256 may be NULL/empty: SPAKE2 authenticates the peer without sending the token. */
int remote_status(remote_endpoint* endpoint);
/* Optional independent synchronization connection. 0 waiting, 1 ready, -1 unavailable.
   Failure never changes remote_status or consumes pairing-token guesses. */
int remote_bulk_status(remote_endpoint* endpoint);
/* FIFO reliable frames, 1..1 MiB. 1 queued, 0 backpressure (retry), -1 unavailable/invalid.
   Never pass clipboard/file payloads to remote_send. Delivery still needs application ACK. */
int remote_bulk_send(remote_endpoint* endpoint, const uint8_t* data, size_t len);
/* Payload length, 0 empty, -1 unavailable/invalid/insufficient capacity. Small buffer
   preserves the queued frame. Allocate 1 MiB; separate from remote_poll. */
int remote_bulk_poll(remote_endpoint* endpoint, uint8_t* data, size_t capacity);
uint32_t remote_bulk_pending(remote_endpoint* endpoint);
double remote_rtt_ms(remote_endpoint* endpoint);
uint32_t remote_pending(remote_endpoint* endpoint, uint32_t channel);
int remote_send(remote_endpoint* endpoint, uint32_t channel, const uint8_t* data, size_t len);
/* Returns payload length, 0 when empty, -1 on error. Capacity must be >= 8 MiB for video. */
int remote_poll(remote_endpoint* endpoint, uint32_t* channel, uint8_t* data, size_t capacity);
int remote_last_error(remote_endpoint* endpoint, char* data, size_t capacity);
/* Server identity SHA256; on clients available only after successful mutual PAKE proof. */
int remote_fingerprint(remote_endpoint* endpoint, char* data, size_t capacity);
/* Server-only. JSON {threshold:10,entries:[{ip,failures,blocked}]}, length excludes NUL. */
int remote_auth_policy_json(remote_endpoint* endpoint, char* data, size_t capacity);
/* Returns 1 on success (also for an absent IP), -1 on invalid IP/persistence failure. */
int remote_unblock_ip(remote_endpoint* endpoint, const char* ip);
/* 6–128 Unicode characters, no control characters. Host persists its private token file.
   Returns 1 on success; changes disconnect all current/pending client connections. */
int remote_set_token(remote_endpoint* endpoint, const char* token);
void remote_close(remote_endpoint* endpoint);
#ifdef __cplusplus
}
#endif

/* All wire integers and float32 are little endian; receivers validate lengths. */
#pragma pack(push,1)
typedef struct RemoteFrameHeader {
    uint32_t magic; /* 0x31524652 = RFR1 */
    uint32_t width;
    uint32_t height;
    uint32_t flags; /* bit 0 IDR; H.264 Annex B payload with SPS/PPS on IDR */
    uint64_t sequence;
    uint64_t capture_time_us;
} RemoteFrameHeader;
enum { INPUT_MOVE=1, INPUT_BUTTON=2, INPUT_SCROLL=3, INPUT_KEY=4, INPUT_RELEASE_ALL=5, INPUT_TEXT=6, INPUT_KEYFRAME=7, INPUT_PASTE=8 };
typedef struct RemoteInputEvent {
    uint32_t type;
    uint32_t flags; /* bit 0 down; bit 1 Windows shortcut mode; bit 2 extended key */
    float x; /* normalized 0..1 within active Mac display */
    float y;
    int32_t a; /* button: 1 left 2 right 3 middle; key: Windows VK; scroll: vertical delta/120 */
    int32_t b; /* key: scan code; scroll: horizontal delta/120 */
    uint64_t sequence; /* INPUT_PASTE: clipboard/offer transaction ID; a=1 content, a=2 files */
    /* INPUT_TEXT appends UTF-8 text; no NUL required */
} RemoteInputEvent;
typedef struct RemoteCursorHeader {
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t flags; /* bit 0 hidden; bit 1 unavailable shape / baked-video fallback */
    uint32_t rgba_bytes;
    /* RGBA8 straight alpha, rows top-to-bottom; hotspot origin is top-left. */
} RemoteCursorHeader;
#pragma pack(pop)
