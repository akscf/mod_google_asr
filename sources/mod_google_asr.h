/**
 * (C)2023 aks
 * https://github.com/akscf/
 **/
#ifndef MOD_GOOGLE_ASR_H
#define MOD_GOOGLE_ASR_H

#include <switch.h>
#include <switch_stun.h>
#include <switch_curl.h>
#include <switch_json.h>
#include <stdint.h>
#include <string.h>

#ifndef true
#define true SWITCH_TRUE
#endif
#ifndef false
#define false SWITCH_FALSE
#endif

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define VERSION             "1.0 (gcp-asr-api-v1_http)"
#define QUEUE_SIZE          64
#define VAD_STORE_FRAMES    32
#define VAD_RECOVERY_FRAMES 10
#define DEF_CHUNK_SZ_SEC    15
#define BASE64_ENC_SZ(n)    (4*(n/3))
#define BOOL2STR(v)         (v ? "true" : "false")

typedef struct {
    switch_mutex_t          *mutex;
    uint32_t                active_threads;
    uint32_t                chunk_size_sec;
    uint32_t                vad_silence_ms;
    uint32_t                vad_voice_ms;
    uint32_t                vad_threshold;
    uint32_t                request_timeout; // seconds
    uint32_t                connect_timeout; // seconds
    uint8_t                 fl_vad_debug;
    uint8_t                 fl_vad_enabled;
    uint8_t                 fl_shutdown;
    char                    *api_url_ep;
    const char              *api_key;
    const char              *api_url;
    const char              *user_agent;
    const char              *default_lang;
    const char              *proxy;
    const char              *proxy_credentials;
    const char              *opt_encoding;
    const char              *opt_speech_model;
    const char              *opt_meta_microphone_distance;
    const char              *opt_meta_recording_device_type;
    const char              *opt_meta_interaction_type;
    uint32_t                opt_max_alternatives;
    uint32_t                opt_use_enhanced_model;
    uint32_t                opt_enable_word_time_offsets;
    uint32_t                opt_enable_word_confidence;
    uint32_t                opt_enable_profanity_filter;
    uint32_t                opt_enable_automatic_punctuation;
    uint32_t                opt_enable_spoken_punctuation;
    uint32_t                opt_enable_spoken_emojis;
} globals_t;


typedef struct {
    switch_memory_pool_t    *pool;
    switch_vad_t            *vad;
    switch_buffer_t         *vad_buffer;
    switch_mutex_t          *mutex;
    switch_queue_t          *q_audio;
    switch_queue_t          *q_text;
    switch_buffer_t         *curl_recv_buffer_ref;
    switch_byte_t           *curl_send_buffer_ref;
    char                    *lang;
    switch_vad_state_t      vad_state;
    uint32_t                curl_send_buffer_len;
    int32_t                 transcript_results;
    uint32_t                vad_buffer_size;
    uint32_t                vad_stored_frames;
    uint32_t                chunk_buffer_size;
    uint32_t                deps;
    uint32_t                samplerate;
    uint32_t                channels;
    uint32_t                frame_len;
    uint32_t                ptime;
    uint8_t                 fl_pause;
    uint8_t                 fl_vad_enabled;
    uint8_t                 fl_vad_first_cycle;
    uint8_t                 fl_destroyed;
    uint8_t                 fl_abort;
    //
    const char              *opt_encoding;
    const char              *opt_speech_model;
    const char              *opt_meta_microphone_distance;
    const char              *opt_meta_recording_device_type;
    const char              *opt_meta_interaction_type;
    uint32_t                opt_max_alternatives;
    uint32_t                opt_use_enhanced_model;
    uint32_t                opt_enable_word_time_offsets;
    uint32_t                opt_enable_word_confidence;
    uint32_t                opt_enable_profanity_filter;
    uint32_t                opt_enable_automatic_punctuation;
    uint32_t                opt_enable_spoken_punctuation;
    uint32_t                opt_enable_spoken_emojis;
    uint32_t                opt_enable_speaker_diarization;
    uint32_t                opt_diarization_min_speaker_count;
    uint32_t                opt_diarization_max_speaker_count;
} gasr_ctx_t;

typedef struct {
    uint32_t                len;
    switch_byte_t           *data;
} xdata_buffer_t;

/* utils.c */
void thread_finished();
void thread_launch(switch_memory_pool_t *pool, switch_thread_start_t fun, void *data);
switch_status_t xdata_buffer_push(switch_queue_t *queue, switch_byte_t *data, uint32_t data_len);
switch_status_t xdata_buffer_alloc(xdata_buffer_t **out, switch_byte_t *data, uint32_t data_len);
void xdata_buffer_free(xdata_buffer_t **buf);
void xdata_buffer_queue_clean(switch_queue_t *queue);

char *audio_file_write(switch_byte_t *buf, uint32_t buf_len, uint32_t channels, uint32_t samplerate);
void data_file_write(switch_byte_t *buf, uint32_t buf_len);

char *gcp_get_language(const char *val);
char *gcp_get_encoding(const char *val);
char *gcp_get_microphone_distance(const char *val);
char *gcp_get_recording_device(const char *val);
char *gcp_get_interaction(const char *val);

/* curl.c */
switch_status_t curl_perform(gasr_ctx_t *asr_ctx);

#endif
