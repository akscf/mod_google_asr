/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include "mod_google_asr.h"

extern globals_t globals;

static size_t curl_io_write_callback(char *buffer, size_t size, size_t nitems, void *user_data) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *)user_data;
    size_t len = (size * nitems);

    if(len > 0 && asr_ctx->curl_recv_buffer_ref) {
        switch_buffer_write(asr_ctx->curl_recv_buffer_ref, buffer, len);
    }

    return len;
}

static size_t curl_io_read_callback(char *buffer, size_t size, size_t nitems, void *user_data) {
    gasr_ctx_t *asr_ctx = (gasr_ctx_t *)user_data;
    size_t nmax = (size * nitems);
    size_t ncur = (asr_ctx->curl_send_buffer_len > nmax) ? nmax : asr_ctx->curl_send_buffer_len;

    if(ncur > 0) {
        memmove(buffer, asr_ctx->curl_send_buffer_ref, ncur);
        asr_ctx->curl_send_buffer_ref += ncur;
        asr_ctx->curl_send_buffer_len -= ncur;
    }

    return ncur;
}

switch_status_t curl_perform(gasr_ctx_t *asr_ctx) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    CURL *chnd = NULL;
    switch_curl_slist_t *headers = NULL;
    switch_CURLcode http_resp = 0;

#ifndef CURL_CRASHBUG_DBG
    chnd = switch_curl_easy_init();
    headers = switch_curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    switch_curl_easy_setopt(chnd, CURLOPT_HTTPHEADER, headers);
    switch_curl_easy_setopt(chnd, CURLOPT_POST, 1);
    switch_curl_easy_setopt(chnd, CURLOPT_NOSIGNAL, 1);
    switch_curl_easy_setopt(chnd, CURLOPT_READFUNCTION, curl_io_read_callback);
    switch_curl_easy_setopt(chnd, CURLOPT_READDATA, (void *) asr_ctx);
    switch_curl_easy_setopt(chnd, CURLOPT_WRITEFUNCTION, curl_io_write_callback);
    switch_curl_easy_setopt(chnd, CURLOPT_WRITEDATA, (void *) asr_ctx);

    if(globals.request_timeout > 0) {
        switch_curl_easy_setopt(chnd, CURLOPT_TIMEOUT, globals.request_timeout);
        //switch_curl_easy_setopt(chnd, CURLOPT_CONNECTTIMEOUT, globals.request_timeout);
    }
    if(globals.user_agent) {
        switch_curl_easy_setopt(chnd, CURLOPT_USERAGENT, globals.user_agent);
    }

    switch_curl_easy_setopt(chnd, CURLOPT_URL, globals.api_url_ep);

    if(strncasecmp(globals.api_url_ep, "https", 5) == 0) {
        switch_curl_easy_setopt(chnd, CURLOPT_SSL_VERIFYPEER, 0);
        switch_curl_easy_setopt(chnd, CURLOPT_SSL_VERIFYHOST, 0);
    }

    switch_curl_easy_perform(chnd);
    switch_curl_easy_getinfo(chnd, CURLINFO_RESPONSE_CODE, &http_resp);

    if(http_resp != 200) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "http-error=[%d] (%s)\n", http_resp, globals.api_url);
        status = SWITCH_STATUS_FALSE;
    }
out:
    if(chnd)    { switch_curl_easy_cleanup(chnd); }
    if(headers) { switch_curl_slist_free_all(headers); }

#else
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Emulate GCP response\n");

    const char  *resp ="{ \"results\": [ { \"alternatives\": [ { \"transcript\": \"how old is the Brooklyn Bridge\", \"confidence\": 0.98267895 } ] } ] }";
    switch_buffer_write(asr_ctx->curl_recv_buffer_ref, resp, strlen(resp));
    switch_buffer_write(asr_ctx->curl_recv_buffer_ref, '\0', 1);
#endif

    return status;
}
