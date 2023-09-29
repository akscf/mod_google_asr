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
    CURL *curl_handle = NULL;
    switch_curl_slist_t *headers = NULL;
    switch_CURLcode curl_ret = 0;
    long http_resp = 0;

    curl_handle = switch_curl_easy_init();
    headers = switch_curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    switch_curl_easy_setopt(curl_handle, CURLOPT_POST, 1);
    switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);
    switch_curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, curl_io_read_callback);
    switch_curl_easy_setopt(curl_handle, CURLOPT_READDATA, (void *) asr_ctx);
    switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_io_write_callback);
    switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) asr_ctx);

    if(globals.connect_timeout > 0) {
        switch_curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, globals.connect_timeout);
    }
    if(globals.request_timeout > 0) {
        switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, globals.request_timeout);
    }
    if(globals.user_agent) {
        switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, globals.user_agent);
    }
    if(strncasecmp(globals.api_url_ep, "https", 5) == 0) {
        switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
        switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
    }
    if(globals.proxy) {
        if(globals.proxy_credentials != NULL) {
            switch_curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
            switch_curl_easy_setopt(curl_handle, CURLOPT_PROXYUSERPWD, globals.proxy_credentials);
        }
        if(strncasecmp(globals.proxy, "https", 5) == 0) {
            switch_curl_easy_setopt(curl_handle, CURLOPT_PROXY_SSL_VERIFYPEER, 0);
        }
        switch_curl_easy_setopt(curl_handle, CURLOPT_PROXY, globals.proxy);
    }

    switch_curl_easy_setopt(curl_handle, CURLOPT_URL, globals.api_url_ep);

    curl_ret = switch_curl_easy_perform(curl_handle);
    if(!curl_ret) {
        switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_resp);
        if(!http_resp) { switch_curl_easy_getinfo(curl_handle, CURLINFO_HTTP_CONNECTCODE, &http_resp); }
    } else {
        http_resp = curl_ret;
    }

    if(http_resp != 200) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "http-error=[%ld] (%s)\n", http_resp, globals.api_url);
        status = SWITCH_STATUS_FALSE;
    }

    if(asr_ctx->curl_recv_buffer_ref) {
        if(switch_buffer_inuse(asr_ctx->curl_recv_buffer_ref) > 0) {
            switch_buffer_write(asr_ctx->curl_recv_buffer_ref, "\0", 1);
        }
    }

    if(curl_handle) {
        switch_curl_easy_cleanup(curl_handle);
    }

    if(headers) {
        switch_curl_slist_free_all(headers);
    }

    return status;
}
