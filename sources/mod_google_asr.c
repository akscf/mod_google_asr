/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include "mod_google_asr.h"

globals_t globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_google_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_google_asr_shutdown);
SWITCH_MODULE_DEFINITION(mod_google_asr, mod_google_asr_load, mod_google_asr_shutdown, NULL);

/**
 ** https://cloud.google.com/speech-to-text/docs/sync-recognize#speech-sync-recognize-drest
 ** https://cloud.google.com/speech-to-text/docs/reference/rest/v1/RecognitionConfig
 ** https://cloud.google.com/speech-to-text/docs/reference/rest/v1/RecognitionConfig#AudioEncoding
 **/

// ---------------------------------------------------------------------------------------------------------------------------------------------
static void *SWITCH_THREAD_FUNC transcript_thread(switch_thread_t *thread, void *obj) {
    volatile gasr_ctx_t *_ref = (gasr_ctx_t *) obj;
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) _ref;
    switch_status_t status;
    switch_byte_t *base64_buffer = NULL;
    switch_byte_t *curl_send_buffer = NULL;
    switch_buffer_t *chunk_buffer = NULL;
    switch_buffer_t *curl_recv_buffer = NULL;
    switch_memory_pool_t *pool = NULL;
    cJSON *json = NULL;
    uint32_t base64_buffer_size = 0, chunk_buffer_size = 0, recv_len = 0;
    uint8_t fl_do_transcript = false;
    const void *curl_recv_buffer_ptr = NULL;
    void *pop = NULL;

    switch_mutex_lock(asr_ctx->mutex);
    asr_ctx->deps++;
    switch_mutex_unlock(asr_ctx->mutex);

    if(switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "pool fail\n");
        goto out;
    }
    if(switch_buffer_create_dynamic(&curl_recv_buffer, 1024, 4096, 16384) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail\n");
        goto out;
    }

    while(true) {
        if(globals.fl_shutdown || asr_ctx->fl_destroyed ) {
            break;
        }

        if(chunk_buffer_size == 0) {
            switch_mutex_lock(asr_ctx->mutex);
            chunk_buffer_size = asr_ctx->chunk_buffer_size;
            switch_mutex_unlock(asr_ctx->mutex);

            if(chunk_buffer_size > 0) {
                if(switch_buffer_create(pool, &chunk_buffer, chunk_buffer_size) != SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "mem fail\n");
                    break;
                }
                switch_buffer_zero(chunk_buffer);
            }

            goto timer_next;
        }

        fl_do_transcript = false;
        while(switch_queue_trypop(asr_ctx->q_audio, &pop) == SWITCH_STATUS_SUCCESS) {
            xdata_buffer_t *audio_buffer = (xdata_buffer_t *)pop;
            if(globals.fl_shutdown || asr_ctx->fl_destroyed ) {
                xdata_buffer_free(audio_buffer);
                break;
            }
            if(audio_buffer && audio_buffer->len) {
                if(switch_buffer_write(chunk_buffer, audio_buffer->data, audio_buffer->len) >= chunk_buffer_size) {
                    fl_do_transcript = true;
                    break;
                }
            }
            xdata_buffer_free(audio_buffer);
        }
        if(!fl_do_transcript) {
            fl_do_transcript = (switch_buffer_inuse(chunk_buffer) > 0 && asr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING);
        }

        if(fl_do_transcript) {
            const void *chunk_buffer_ptr = NULL;
            uint32_t buf_len = switch_buffer_peek_zerocopy(chunk_buffer, &chunk_buffer_ptr);
            uint32_t b64_len = BASE64_ENC_SZ(buf_len) + 1;

            if(base64_buffer_size == 0 || base64_buffer_size < b64_len) {
                if(base64_buffer_size > 0) { switch_safe_free(base64_buffer); }
                switch_zmalloc(base64_buffer, b64_len);
                base64_buffer_size = b64_len;
            } else {
                memset(base64_buffer, 0x0, b64_len);
            }

            if(switch_b64_encode((uint8_t *)chunk_buffer_ptr, buf_len, base64_buffer, base64_buffer_size) == SWITCH_STATUS_SUCCESS) {
                curl_send_buffer = (char*) switch_mprintf(
                                "{'config':{" \
                                "'languageCode':'%s', 'encoding':'%s', 'sampleRateHertz':'%u', 'audioChannelCount':'%u', 'maxAlternatives':'%u', 'profanityFilter':'%s', 'enableWordTimeOffsets':'%s', 'enableWordConfidence':'%s', " \
                                "'enableAutomaticPunctuation':'%s', 'enableSpokenPunctuation':'%s', 'enableSpokenEmojis':'%s', 'model':'%s', 'useEnhanced':'%s', " \
                                "'diarizationConfig':{'enableSpeakerDiarization': '%s', 'minSpeakerCount': '%u', 'maxSpeakerCount': '%u'}, " \
                                "'metadata':{'interactionType':'%s', 'microphoneDistance':'%s', 'recordingDeviceType':'%s'}" \
                                "}, 'audio':{'content':'%s'}}",
                                asr_ctx->lang, asr_ctx->opt_encoding, asr_ctx->samplerate, asr_ctx->channels,
                                asr_ctx->opt_max_alternatives,
                                BOOL2STR(asr_ctx->opt_enable_profanity_filter), BOOL2STR(asr_ctx->opt_enable_word_time_offsets), BOOL2STR(asr_ctx->opt_enable_word_confidence), BOOL2STR(asr_ctx->opt_enable_automatic_punctuation),
                                BOOL2STR(asr_ctx->opt_enable_spoken_punctuation), BOOL2STR(asr_ctx->opt_enable_spoken_emojis), asr_ctx->opt_speech_model, BOOL2STR(asr_ctx->opt_use_enhanced_model),
                                BOOL2STR(asr_ctx->opt_enable_speaker_diarization), asr_ctx->opt_diarization_min_speaker_count, asr_ctx->opt_diarization_max_speaker_count,
                                asr_ctx->opt_meta_interaction_type, asr_ctx->opt_meta_microphone_distance, asr_ctx->opt_meta_recording_device_type,
                                base64_buffer );

                asr_ctx->curl_send_buffer_ref = curl_send_buffer;
                asr_ctx->curl_send_buffer_len = strlen((const char *)curl_send_buffer);

                switch_buffer_zero(curl_recv_buffer);
                asr_ctx->curl_recv_buffer_ref = curl_recv_buffer;

                status = curl_perform(asr_ctx);
                recv_len = switch_buffer_peek_zerocopy(curl_recv_buffer, &curl_recv_buffer_ptr);

                if(status == SWITCH_STATUS_SUCCESS) {
                    if((json = cJSON_Parse((char *) curl_recv_buffer_ptr)) != NULL) {
                        cJSON *jres = cJSON_GetObjectItem(json, "results");
                        if(jres && cJSON_GetArraySize(jres) > 0) {
                            cJSON *jelem = cJSON_GetArrayItem(jres, 0);
                            if(jelem) {
                                jres = cJSON_GetObjectItem(jelem, "alternatives");
                                if(jres && cJSON_GetArraySize(jres) > 0) {
                                    jelem = cJSON_GetArrayItem(jres, 0);
                                    if(jelem) {
                                        cJSON *jt = cJSON_GetObjectItem(jelem, "transcript"); // jt->valuestring
                                        cJSON *jc = cJSON_GetObjectItem(jelem, "confidence"); // jc->valuedouble
                                        if(jt && jt->valuestring) {
                                            xdata_buffer_t *tbuff = NULL;
                                            if(xdata_buffer_alloc(&tbuff, jt->valuestring, strlen(jt->valuestring)) == SWITCH_STATUS_SUCCESS) {
                                                if(switch_queue_trypush(asr_ctx->q_text, tbuff) == SWITCH_STATUS_SUCCESS) {
                                                    switch_mutex_lock(asr_ctx->mutex);
                                                    asr_ctx->transcript_results++;
                                                    switch_mutex_unlock(asr_ctx->mutex);
                                                } else {
                                                    xdata_buffer_free(tbuff);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if(json) {
                        cJSON_Delete(json);
                        json = NULL;
                    }
                } else {
                    if(recv_len > 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GCP-RESPONSE: %s\n", (char *) curl_recv_buffer_ptr);
                    }
                }
                switch_safe_free(curl_send_buffer);
            } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "b64_encode fail\n");
            }
            switch_buffer_zero(chunk_buffer);
        }
        timer_next:
        switch_yield(10000);
    }

out:
    switch_safe_free(base64_buffer);
    switch_safe_free(curl_send_buffer);

    if(json != NULL) {
        cJSON_Delete(json);
    }
    if(curl_recv_buffer) {
        switch_buffer_destroy(&curl_recv_buffer);
    }
    if(chunk_buffer) {
        switch_buffer_destroy(&chunk_buffer);
    }
    if(pool) {
        switch_core_destroy_memory_pool(&pool);
    }

    switch_mutex_lock(asr_ctx->mutex);
    if(asr_ctx->deps > 0) asr_ctx->deps--;
    switch_mutex_unlock(asr_ctx->mutex);

    thread_finished();

    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
static switch_status_t asr_open(switch_asr_handle_t *ah, const char *codec, int samplerate, const char *dest, switch_asr_flag_t *flags) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_memory_pool_t *pool = NULL;
    gasr_ctx_t *asr_ctx = NULL;

    if(strcmp(codec, "L16") !=0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unsupported codec: %s\n", codec);
        switch_goto_status(SWITCH_STATUS_FALSE, out);
    }

    if((asr_ctx = switch_core_alloc(ah->memory_pool, sizeof(gasr_ctx_t))) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    asr_ctx->chunk_buffer_size = 0;
    asr_ctx->samplerate = samplerate;
    asr_ctx->channels = 1;
    asr_ctx->lang = (char *) globals.default_lang;
    asr_ctx->opt_max_alternatives = globals.opt_max_alternatives;
    asr_ctx->opt_enable_profanity_filter = globals.opt_enable_profanity_filter;
    asr_ctx->opt_enable_word_time_offsets = globals.opt_enable_word_time_offsets;
    asr_ctx->opt_enable_word_confidence = globals.opt_enable_word_confidence;
    asr_ctx->opt_enable_automatic_punctuation = globals.opt_enable_automatic_punctuation;
    asr_ctx->opt_enable_spoken_punctuation = globals.opt_enable_spoken_punctuation;
    asr_ctx->opt_enable_spoken_emojis = globals.opt_enable_spoken_emojis;
    asr_ctx->opt_meta_interaction_type = globals.opt_meta_interaction_type;
    asr_ctx->opt_meta_microphone_distance = globals.opt_meta_microphone_distance;
    asr_ctx->opt_meta_recording_device_type = globals.opt_meta_recording_device_type;
    asr_ctx->opt_speech_model = globals.opt_speech_model;
    asr_ctx->opt_use_enhanced_model = globals.opt_use_enhanced_model;
    asr_ctx->opt_enable_speaker_diarization = false;
    asr_ctx->opt_diarization_min_speaker_count = 1;
    asr_ctx->opt_diarization_max_speaker_count = 1;

   if((status = switch_mutex_init(&asr_ctx->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    switch_queue_create(&asr_ctx->q_audio, QUEUE_SIZE, ah->memory_pool);
    switch_queue_create(&asr_ctx->q_text, QUEUE_SIZE, ah->memory_pool);

    // VAD
    asr_ctx->fl_vad_enabled = globals.fl_vad_enabled;
    asr_ctx->vad_buffer = NULL;
    asr_ctx->frame_len = 0;
    asr_ctx->vad_buffer_offs = 0;
    asr_ctx->vad_buffer_size = 0; // will be calculated in the feed function
    asr_ctx->vad_stored_frames = 0;

    if((asr_ctx->vad = switch_vad_init(asr_ctx->samplerate, asr_ctx->channels)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't init VAD\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    switch_vad_set_mode(asr_ctx->vad, -1);
    switch_vad_set_param(asr_ctx->vad, "debug", globals.fl_vad_debug);
    if(globals.vad_silence_ms > 0) { switch_vad_set_param(asr_ctx->vad, "silence_ms", globals.vad_silence_ms); }
    if(globals.vad_voice_ms > 0) { switch_vad_set_param(asr_ctx->vad, "voice_ms", globals.vad_voice_ms); }
    if(globals.vad_threshold > 0) { switch_vad_set_param(asr_ctx->vad, "thresh", globals.vad_threshold); }

    ah->private_info = asr_ctx;

    thread_launch(ah->memory_pool, transcript_thread, asr_ctx);
out:
    return status;
}

static switch_status_t asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    int32_t wc = 0;

    assert(asr_ctx != NULL);

    asr_ctx->fl_abort = true;
    asr_ctx->fl_destroyed = true;
    while(true) {
        if(asr_ctx->deps <= 0) { break; }
        switch_yield(100000);
        if(++wc % 100 == 0) {
            if(asr_ctx->deps > 0) { switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "asr_ctx is used (deps=%u)!\n", asr_ctx->deps); }
            wc = 0;
        }
    }

    if(asr_ctx->q_audio) {
        xdata_buffer_queue_clean(asr_ctx->q_audio);
        switch_queue_term(asr_ctx->q_audio);
    }
    if(asr_ctx->q_text) {
        xdata_buffer_queue_clean(asr_ctx->q_text);
        switch_queue_term(asr_ctx->q_text);
    }
    if(asr_ctx->vad) {
        switch_vad_destroy(&asr_ctx->vad);
    }

    switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_feed(switch_asr_handle_t *ah, void *data, unsigned int data_len, switch_asr_flag_t *flags) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    switch_vad_state_t vad_state = 0;
    uint8_t fl_has_audio = false;

    assert(asr_ctx != NULL);

    if(switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
        return SWITCH_STATUS_BREAK;
    }
    if(asr_ctx->fl_destroyed || asr_ctx->fl_abort) {
        return SWITCH_STATUS_BREAK;
    }
    if(asr_ctx->fl_pause) {
        return SWITCH_STATUS_SUCCESS;
    }
    if(!data || !data_len) {
        return SWITCH_STATUS_BREAK;
    }

    if(data_len > 0 && asr_ctx->frame_len == 0) {
        switch_mutex_lock(asr_ctx->mutex);
        asr_ctx->frame_len = data_len;
        asr_ctx->ptime = (data_len / sizeof(int16_t)) / (asr_ctx->samplerate / 1000);
        asr_ctx->chunk_buffer_size = ((globals.chunk_size_sec * 1000) * data_len) / asr_ctx->ptime;
        asr_ctx->vad_buffer_size = (asr_ctx->frame_len * VAD_STORE_FRAMES);
        switch_mutex_unlock(asr_ctx->mutex);

        if((asr_ctx->vad_buffer = switch_core_alloc(ah->memory_pool, asr_ctx->vad_buffer_size)) == NULL) {
            asr_ctx->vad_buffer_size = 0; // force disable
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (vad_buffer)\n");
        }
    }

    if(asr_ctx->fl_vad_enabled && asr_ctx->vad_buffer_size) {
        if(asr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING || (asr_ctx->vad_state == vad_state && vad_state == SWITCH_VAD_STATE_NONE)) {
            if(asr_ctx->frame_len >= data_len) {
                if(asr_ctx->vad_buffer_offs >= asr_ctx->vad_buffer_size) { asr_ctx->vad_buffer_offs = 0; asr_ctx->vad_stored_frames = 0; }
                memcpy((void *)(asr_ctx->vad_buffer + asr_ctx->vad_buffer_offs), data, MIN(asr_ctx->frame_len, data_len));
                asr_ctx->vad_buffer_offs += asr_ctx->frame_len;
                asr_ctx->vad_stored_frames++;
            }
        }

        vad_state = switch_vad_process(asr_ctx->vad, (int16_t *)data, (data_len / sizeof(int16_t)));
        if(vad_state == SWITCH_VAD_STATE_START_TALKING) {
            asr_ctx->vad_state = vad_state;
            fl_has_audio = true;
        } else if (vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
            asr_ctx->vad_state = vad_state;
            fl_has_audio = false;
            switch_vad_reset(asr_ctx->vad);
        } else if (vad_state == SWITCH_VAD_STATE_TALKING) {
            asr_ctx->vad_state = vad_state;
            fl_has_audio = true;
        }
    } else {
        fl_has_audio = true;
    }

    if(fl_has_audio) {
        if(vad_state == SWITCH_VAD_STATE_START_TALKING && asr_ctx->vad_buffer_offs > 0) {
            xdata_buffer_t *tau_buf = NULL;
            uint32_t tdata_len = 0;

            if(asr_ctx->vad_stored_frames >= VAD_RECOVERY_FRAMES) { asr_ctx->vad_stored_frames = VAD_RECOVERY_FRAMES; }

            asr_ctx->vad_buffer_offs -= (asr_ctx->vad_stored_frames * asr_ctx->frame_len);
            if(asr_ctx->vad_buffer_offs < 0 ) { asr_ctx->vad_buffer_offs = 0; }

            tdata_len = (asr_ctx->vad_stored_frames * asr_ctx->frame_len) + data_len;

            switch_zmalloc(tau_buf, sizeof(xdata_buffer_t));
            switch_malloc(tau_buf->data, tdata_len);
            tau_buf->len = tdata_len;

            tdata_len = (asr_ctx->vad_stored_frames * asr_ctx->frame_len);
            memcpy(tau_buf->data, asr_ctx->vad_buffer + asr_ctx->vad_buffer_offs, tdata_len);
            memcpy(tau_buf->data + tdata_len, data, data_len);

            if(switch_queue_trypush(asr_ctx->q_audio, tau_buf) != SWITCH_STATUS_SUCCESS) {
                xdata_buffer_free(tau_buf);
            }

            asr_ctx->vad_stored_frames = 0;
            asr_ctx->vad_buffer_offs = 0;
        } else {
            xdata_buffer_push(asr_ctx->q_audio, data, data_len);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    assert(asr_ctx != NULL);

    return (asr_ctx->transcript_results > 0 ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
}

static switch_status_t asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    char *result = NULL;
    void *pop = NULL;

    assert(asr_ctx != NULL);

    if(switch_queue_trypop(asr_ctx->q_text, &pop) == SWITCH_STATUS_SUCCESS) {
        xdata_buffer_t *tbuff = (xdata_buffer_t *)pop;
        if(tbuff->len > 0) {
            switch_zmalloc(result, tbuff->len + 1);
            memcpy(result, tbuff->data, tbuff->len);
        }
        xdata_buffer_free(tbuff);

        switch_mutex_lock(asr_ctx->mutex);
        if(asr_ctx->transcript_results > 0) asr_ctx->transcript_results--;
        switch_mutex_unlock(asr_ctx->mutex);
    }

    *xmlstr = result;
    return (result ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
}

static switch_status_t asr_start_input_timers(switch_asr_handle_t *ah) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    assert(asr_ctx != NULL);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_pause(switch_asr_handle_t *ah) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    assert(asr_ctx != NULL);

    if(!asr_ctx->fl_pause) {
        asr_ctx->fl_pause = true;
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_resume(switch_asr_handle_t *ah) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    assert(asr_ctx != NULL);

    if(asr_ctx->fl_pause) {
        asr_ctx->fl_pause = false;
    }

    return SWITCH_STATUS_SUCCESS;
}

static void asr_text_param(switch_asr_handle_t *ah, char *param, const char *val) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *) ah->private_info;
    assert(asr_ctx != NULL);

    if(strcasecmp(param, "vad") == 0) {
        if(val) asr_ctx->fl_vad_enabled = switch_true(val);
    } else if(strcasecmp(param, "lang") == 0) {
        if(val) asr_ctx->lang = switch_core_strdup(ah->memory_pool, gcp_get_language(val));
    } else if(!strcasecmp(param, "speech-model")) {
        if(val) asr_ctx->opt_speech_model = switch_core_strdup(ah->memory_pool, val);
    } else if(!strcasecmp(param, "use-enhanced-model")) {
        if(val) asr_ctx->opt_use_enhanced_model = switch_true(val);
    } else if(!strcasecmp(param, "max-alternatives")) {
        if(val) asr_ctx->opt_max_alternatives = atoi(val);
    } else if(!strcasecmp(param, "enable-word-time-offsets")) {
        if(val) asr_ctx->opt_enable_word_time_offsets = switch_true(val);
    } else if(!strcasecmp(param, "enable-enable-word-confidence;")) {
        if(val) asr_ctx->opt_enable_word_confidence = switch_true(val);
    } else if(!strcasecmp(param, "enable-profanity-filter")) {
        if(val) asr_ctx->opt_enable_profanity_filter = switch_true(val);
    } else if(!strcasecmp(param, "enable-automatic-punctuation")) {
        if(val) asr_ctx->opt_enable_automatic_punctuation = switch_true(val);
    } else if(!strcasecmp(param, "enable-spoken-punctuation")) {
        if(val) asr_ctx->opt_enable_spoken_punctuation = switch_true(val);
    } else if(!strcasecmp(param, "enable-spoken-emojis")) {
        if(val) asr_ctx->opt_enable_spoken_emojis = switch_true(val);
    } else if(!strcasecmp(param, "microphone-distance")) {
        if(val) asr_ctx->opt_meta_microphone_distance = switch_core_strdup(ah->memory_pool, gcp_get_microphone_distance(val));
    } else if(!strcasecmp(param, "recording-device-type")) {
        if(val) asr_ctx->opt_meta_recording_device_type = switch_core_strdup(ah->memory_pool, gcp_get_recording_device(val));
    } else if(!strcasecmp(param, "interaction-type")) {
        if(val) asr_ctx->opt_meta_interaction_type = switch_core_strdup(ah->memory_pool, gcp_get_interaction(val));
    } else if(!strcasecmp(param, "enable-speaker-diarizatio")) {
        if(val) asr_ctx->opt_enable_speaker_diarization = switch_true(val);
    } else if(!strcasecmp(param, "diarization-min-speakers")) {
        if(val) asr_ctx->opt_diarization_min_speaker_count = atoi(val);
    } else if(!strcasecmp(param, "diarization-max-speakers")) {
        if(val) asr_ctx->opt_diarization_max_speaker_count = atoi(val);
    }

}

static void asr_numeric_param(switch_asr_handle_t *ah, char *param, int val) {
}

static void asr_float_param(switch_asr_handle_t *ah, char *param, double val) {
}

static switch_status_t asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name) {
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_unload_grammar(switch_asr_handle_t *ah, const char *name) {
    return SWITCH_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------------------------------------------------------
#define CONFIG_NAME "google_asr.conf"
SWITCH_MODULE_LOAD_FUNCTION(mod_google_asr_load) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_xml_t cfg, xml, settings, param;
    switch_asr_interface_t *asr_interface;

    memset(&globals, 0, sizeof(globals));

    switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);

    if((xml = switch_xml_open_cfg(CONFIG_NAME, &cfg, NULL)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't open configuration file: %s\n", CONFIG_NAME);
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    if((settings = switch_xml_child(cfg, "settings"))) {
        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if(!strcasecmp(var, "vad-silence-ms")) {
                if(val) globals.vad_silence_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-voice-ms")) {
                if(val) globals.vad_voice_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-threshold")) {
                if(val) globals.vad_threshold = atoi (val);
            } else if(!strcasecmp(var, "vad-enable")) {
                if(val) globals.fl_vad_enabled = switch_true(val);
            } else if(!strcasecmp(var, "vad-debug")) {
                if(val) globals.fl_vad_debug = switch_true(val);
            } else if(!strcasecmp(var, "api-key")) {
                if(val) globals.api_key = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "api-url")) {
                if(val) globals.api_url = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "user-agent")) {
                if(val) globals.user_agent = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "proxy")) {
                if(val) globals.proxy = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "proxy-credentials")) {
                if(val) globals.proxy_credentials = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "default-language")) {
                if(val) globals.default_lang = switch_core_strdup(pool, gcp_get_language(val));
            } else if(!strcasecmp(var, "encoding")) {
                if(val) globals.opt_encoding = switch_core_strdup(pool, gcp_get_encoding(val));
            } else if(!strcasecmp(var, "chunk-size-sec")) {
                if(val) globals.chunk_size_sec = atoi(val);
            } else if(!strcasecmp(var, "request-timeout")) {
                if(val) globals.request_timeout = atoi(val);
            } else if(!strcasecmp(var, "connect-timeout")) {
                if(val) globals.connect_timeout = atoi(val);
            } else if(!strcasecmp(var, "speech-model")) {
                if(val) globals.opt_speech_model = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "use-enhanced-model")) {
                if(val) globals.opt_use_enhanced_model = switch_true(val);
            } else if(!strcasecmp(var, "max-alternatives")) {
                if(val) globals.opt_max_alternatives = atoi(val);
            } else if(!strcasecmp(var, "enable-word-time-offsets")) {
                if(val) globals.opt_enable_word_time_offsets = switch_true(val);
            } else if(!strcasecmp(var, "enable-word-confidence")) {
                if(val) globals.opt_enable_word_confidence = switch_true(val);
            } else if(!strcasecmp(var, "enable-profanity-filter")) {
                if(val) globals.opt_enable_profanity_filter = switch_true(val);
            } else if(!strcasecmp(var, "enable-automatic-punctuation")) {
                if(val) globals.opt_enable_automatic_punctuation = switch_true(val);
            } else if(!strcasecmp(var, "enable-spoken-punctuation")) {
                if(val) globals.opt_enable_spoken_punctuation = switch_true(val);
            } else if(!strcasecmp(var, "enable-spoken-emojis")) {
                if(val) globals.opt_enable_spoken_emojis = switch_true(val);
            } else if(!strcasecmp(var, "microphone-distance")) {
                if(val) globals.opt_meta_microphone_distance = switch_core_strdup(pool, gcp_get_microphone_distance(val));
            } else if(!strcasecmp(var, "recording-device-type")) {
                if(val) globals.opt_meta_recording_device_type = switch_core_strdup(pool, gcp_get_recording_device(val));
            } else if(!strcasecmp(var, "interaction-type")) {
                if(val) globals.opt_meta_interaction_type = switch_core_strdup(pool, gcp_get_interaction(val));
            }
        }
    }

    if(!globals.api_url) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid parameter: api-url\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    if(!globals.api_key) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid parameter: api-key\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    globals.api_url_ep = switch_string_replace(globals.api_url, "${api-key}", globals.api_key);
    if(!globals.api_url_ep) {
        globals.api_url_ep = strdup(globals.api_key);
    }

    globals.chunk_size_sec = globals.chunk_size_sec > DEF_CHUNK_SZ_SEC ? globals.chunk_size_sec : DEF_CHUNK_SZ_SEC;
    globals.opt_encoding = globals.opt_encoding ?  globals.opt_encoding : gcp_get_encoding("l16");
    globals.opt_speech_model = globals.opt_speech_model ?  globals.opt_speech_model : "phone_call";
    globals.opt_max_alternatives = globals.opt_max_alternatives > 0 ? globals.opt_max_alternatives : 1;
    globals.opt_meta_microphone_distance = globals.opt_meta_microphone_distance ? globals.opt_meta_microphone_distance : gcp_get_microphone_distance("unspecified");
    globals.opt_meta_recording_device_type = globals.opt_meta_recording_device_type ? globals.opt_meta_recording_device_type : gcp_get_recording_device("unspecified");
    globals.opt_meta_interaction_type = globals.opt_meta_interaction_type ? globals.opt_meta_interaction_type : gcp_get_interaction("unspecified");

    // -------------------------
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
    asr_interface->interface_name = "google";
    asr_interface->asr_open = asr_open;
    asr_interface->asr_close = asr_close;
    asr_interface->asr_feed = asr_feed;
    asr_interface->asr_pause = asr_pause;
    asr_interface->asr_resume = asr_resume;
    asr_interface->asr_check_results = asr_check_results;
    asr_interface->asr_get_results = asr_get_results;
    asr_interface->asr_start_input_timers = asr_start_input_timers;
    asr_interface->asr_text_param = asr_text_param;
    asr_interface->asr_numeric_param = asr_numeric_param;
    asr_interface->asr_float_param = asr_float_param;
    asr_interface->asr_load_grammar = asr_load_grammar;
    asr_interface->asr_unload_grammar = asr_unload_grammar;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "GoogleASR-%s\n", VERSION);
out:
    if(xml) {
        switch_xml_free(xml);
    }
    return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_google_asr_shutdown) {
    globals.fl_shutdown = true;

    while(globals.active_threads > 0) {
        switch_yield(100000);
    }

    switch_safe_free(globals.api_url_ep);

    return SWITCH_STATUS_SUCCESS;
}
