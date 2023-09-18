/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include "mod_google_asr.h"

extern globals_t globals;


/**
 ** https://cloud.google.com/speech-to-text/docs/reference/rest/v1/RecognitionConfig
 ** https://cloud.google.com/speech-to-text/docs/speech-to-text-supported-languages
 **
 **/
char *gcp_get_language(const char *val) {
    if(strcasecmp(val, "en") == 0) { return "en-US"; }
    if(strcasecmp(val, "de") == 0) { return "de-DE"; }
    if(strcasecmp(val, "es") == 0) { return "es-US"; }
    if(strcasecmp(val, "it") == 0) { return "it-IT"; }
    if(strcasecmp(val, "ru") == 0) { return "ru-RU"; }
    return (char *)val;
}

char *gcp_get_encoding(const char *val) {
    if(strcasecmp(val, "unspecified") == 0)  { return "ENCODING_UNSPECIFIED"; }
    if(strcasecmp(val, "l16") == 0)  { return "LINEAR16"; }
    if(strcasecmp(val, "flac") == 0) { return "FLAC"; }
    if(strcasecmp(val, "ulaw") == 0) { return "MULAW"; }
    if(strcasecmp(val, "amr") == 0)  { return "AMR"; }
    return (char *)val;
}

// RecognitionMetadata
char *gcp_get_microphone_distance(const char *val) {
    if(strcasecmp(val, "unspecified") == 0)  { return "MICROPHONE_DISTANCE_UNSPECIFIED"; }
    if(strcasecmp(val, "nearfield") == 0)  { return "NEARFIELD"; }
    if(strcasecmp(val, "midfield") == 0)  { return "MIDFIELD"; }
    if(strcasecmp(val, "farfield") == 0)  { return "FARFIELD"; }
    return (char *)val;
}

// RecognitionMetadata
char *gcp_get_recording_device(const char *val) {
    if(strcasecmp(val, "unspecified") == 0)  { return "RECORDING_DEVICE_TYPE_UNSPECIFIED"; }
    if(strcasecmp(val, "smartphone") == 0)  { return "SMARTPHONE"; }
    if(strcasecmp(val, "pc") == 0)          { return "PC"; }
    if(strcasecmp(val, "phone_line") == 0)  { return "PHONE_LINE"; }
    if(strcasecmp(val, "vehicle") == 0)     { return "VEHICLE"; }
    if(strcasecmp(val, "other_outdoor_device") == 0) { return "OTHER_OUTDOOR_DEVICE"; }
    if(strcasecmp(val, "other_indoor_device") == 0)  { return "OTHER_INDOOR_DEVICE"; }
    return (char *)val;
}

// RecognitionMetadata
char *gcp_get_interaction(const char *val) {
    if(strcasecmp(val, "unspecified") == 0)  { return "INTERACTION_TYPE_UNSPECIFIED"; }
    if(strcasecmp(val, "discussion") == 0)  { return "DISCUSSION"; }
    if(strcasecmp(val, "presentation") == 0)  { return "PRESENTATION"; }
    if(strcasecmp(val, "phone_call") == 0)  { return "PHONE_CALL"; }
    if(strcasecmp(val, "voicemal") == 0)  { return "VOICEMAIL"; }
    if(strcasecmp(val, "professionally_produced") == 0)  { return "PROFESSIONALLY_PRODUCED"; }
    if(strcasecmp(val, "voice_search") == 0)  { return "VOICE_SEARCH"; }
    if(strcasecmp(val, "voice_command") == 0)  { return "VOICE_COMMAND"; }
    if(strcasecmp(val, "dictation") == 0)  { return "DICTATION"; }
    return (char *)val;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
void thread_finished() {
    switch_mutex_lock(globals.mutex);
    if(globals.active_threads > 0) { globals.active_threads--; }
    switch_mutex_unlock(globals.mutex);
}

void thread_launch(switch_memory_pool_t *pool, switch_thread_start_t fun, void *data) {
    switch_threadattr_t *attr = NULL;
    switch_thread_t *thread = NULL;

    switch_mutex_lock(globals.mutex);
    globals.active_threads++;
    switch_mutex_unlock(globals.mutex);

    switch_threadattr_create(&attr, pool);
    switch_threadattr_detach_set(attr, 1);
    switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&thread, attr, fun, data, pool);

    return;
}

switch_status_t xdata_buffer_alloc(xdata_buffer_t **out, switch_byte_t *data, uint32_t data_len) {
    xdata_buffer_t *buf = NULL;

    switch_zmalloc(buf, sizeof(xdata_buffer_t));

    if(data_len) {
        switch_malloc(buf->data, data_len);
        switch_assert(buf->data);

        buf->len = data_len;
        memcpy(buf->data, data, data_len);
    }

    *out = buf;
    return SWITCH_STATUS_SUCCESS;
}

void xdata_buffer_free(xdata_buffer_t *buf) {
    if(buf) {
        switch_safe_free(buf->data);
        switch_safe_free(buf);
        buf = NULL;
    }
}

void xdata_buffer_queue_clean(switch_queue_t *queue) {
    void *data = NULL;

    if(!queue || !switch_queue_size(queue)) { return; }

    while(switch_queue_trypop(queue, &data) == SWITCH_STATUS_SUCCESS) {
        if(data) { xdata_buffer_free((xdata_buffer_t *) data); }
    }
}

switch_status_t xdata_buffer_push(switch_queue_t *queue, switch_byte_t *data, uint32_t data_len) {
    xdata_buffer_t *buff = NULL;

    if(xdata_buffer_alloc(&buff, data, data_len) == SWITCH_STATUS_SUCCESS) {
        if(switch_queue_trypush(queue, buff) == SWITCH_STATUS_SUCCESS) {
            return SWITCH_STATUS_SUCCESS;
        }
        xdata_buffer_free(buff);
    }
    return SWITCH_STATUS_FALSE;
}

char *audio_file_write(switch_byte_t *buf, uint32_t buf_len, uint32_t channels, uint32_t samplerate) {
    switch_status_t status = SWITCH_STATUS_FALSE;
    switch_size_t len = buf_len;
    switch_file_handle_t fh = { 0 };
    char *file_name = NULL;
    char name_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
    int flags = (SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT);

    switch_uuid_str((char *)name_uuid, sizeof(name_uuid));
    file_name = switch_mprintf("%s%s%s.wav", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, name_uuid);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "audio-file: %s\n", file_name);

    if((status = switch_core_file_open(&fh, file_name, channels, samplerate, flags, NULL)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open fail: %s\n", file_name);
        goto out;
    }

    if((status = switch_core_file_write(&fh, buf, &len)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Write fail (%s)\n", file_name);
        goto out;
    }

    switch_core_file_close(&fh);
out:
    if(status != SWITCH_STATUS_SUCCESS) {
        if(file_name) {
            unlink(file_name);
            switch_safe_free(file_name);
        }
        return NULL;
    }
    return file_name;
}

void data_file_write(switch_byte_t *buf, uint32_t buf_len) {
    switch_status_t status = SWITCH_STATUS_FALSE;
    switch_memory_pool_t *pool = NULL;
    switch_size_t len = buf_len;
    switch_file_t *fd = NULL;
    char *file_name = NULL;
    char name_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };

    switch_uuid_str((char *)name_uuid, sizeof(name_uuid));
    file_name = switch_mprintf("%s%s%s.txt", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, name_uuid);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "data-file: %s, len=%u\n", file_name, buf_len);

    if(switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_new_memory_pool() fail\n");
        goto out;
    }
    if(switch_file_open(&fd, file_name, (SWITCH_FOPEN_WRITE | SWITCH_FOPEN_TRUNCATE | SWITCH_FOPEN_CREATE), SWITCH_FPROT_OS_DEFAULT, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open fail: %s\n", file_name);
        goto out;
    }
    if(switch_file_write(fd, buf, &len) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Write fail (%s)\n", file_name);
    }
    switch_file_close(fd);

out:
    if(pool) {
        switch_core_destroy_memory_pool(&pool);
    }
    switch_safe_free(file_name);
}

