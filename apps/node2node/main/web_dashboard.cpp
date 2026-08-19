#include "web_dashboard.hpp"
#include "dashboard_html_gz.h"
#include "esp_log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char* TAG = "WEB_DASH";

namespace Web {

WebDashboard& WebDashboard::getInstance() {
    static WebDashboard instance;
    return instance;
}

esp_err_t WebDashboard::start(uint16_t port) {
    if (m_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.task_priority = 3; /* Low priority: Audio tasks (18-20) take strict precedence */

    ESP_LOGI(TAG, "Starting Native Web Dashboard Server on port %d...", port);
    esp_err_t ret = httpd_start(&m_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 1. Register Index GET handler */
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = WebDashboard::indexGetHandler,
        .user_ctx  = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
    };
    httpd_register_uri_handler(m_server, &index_uri);

    /* 2. Register WebSocket URI handler */
    httpd_uri_t ws_uri = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = WebDashboard::wsHandler,
        .user_ctx  = this,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = nullptr
    };
    httpd_register_uri_handler(m_server, &ws_uri);

    ESP_LOGI(TAG, "Web Dashboard HTTP & WebSocket endpoints registered successfully.");
    return ESP_OK;
}

void WebDashboard::stop() {
    if (m_server) {
        httpd_stop(m_server);
        m_server = nullptr;
        m_client_fds.clear();
        ESP_LOGI(TAG, "Web Dashboard stopped.");
    }
}

esp_err_t WebDashboard::indexGetHandler(httpd_req_t* req) {
    /* Serve GZIP-compressed dashboard.html from Flash */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");

    httpd_resp_send(req, (const char*)dashboard_html_gz, DASHBOARD_HTML_GZ_LEN);
    return ESP_OK;
}

esp_err_t WebDashboard::wsHandler(httpd_req_t* req) {
    auto* self = static_cast<WebDashboard*>(req->user_ctx);

    if (req->method == HTTP_GET) {
        int client_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket Handshake Success! Client FD: %d", client_fd);

        /* Limit to 2 concurrent WebSocket clients */
        if (self->m_client_fds.size() >= 2) {
            ESP_LOGW(TAG, "Max WebSocket clients reached. Rejecting client FD %d", client_fd);
            return ESP_FAIL;
        }

        if (std::find(self->m_client_fds.begin(), self->m_client_fds.end(), client_fd) == self->m_client_fds.end()) {
            self->m_client_fds.push_back(client_fd);
        }

        self->sendInitialData(client_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {};
    uint8_t buf[256] = {0};
    ws_pkt.payload = buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';
        self->handleIncomingAction((const char*)buf, ws_pkt.len);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int client_fd = httpd_req_to_sockfd(req);
        auto it = std::find(self->m_client_fds.begin(), self->m_client_fds.end(), client_fd);
        if (it != self->m_client_fds.end()) {
            self->m_client_fds.erase(it);
        }
        ESP_LOGI(TAG, "WebSocket Client Closed FD: %d (Remaining: %u)", client_fd, (unsigned int)self->m_client_fds.size());
    }

    return ESP_OK;
}

void WebDashboard::sendInitialData(int client_fd) {
    /*
     * Build standard ESP-DashboardPlus init payload defining card layout
     */
    static const char init_manifest[] = 
    "{"
      "\"type\":\"init\","
      "\"title\":\"Auracast Audio Source\","
      "\"subtitle\":\"ESP32-C6 LE Audio Broadcaster & GATT Orchestrator\","
      "\"cards\":["
        "{\"id\":\"card_node\",\"type\":\"stat\",\"config\":{\"title\":\"Broadcast Node\",\"value\":\"ESP32-C6-21\",\"unit\":\"Active\",\"icon\":\"radio\"}},"
        "{\"id\":\"card_cpu\",\"type\":\"stat\",\"config\":{\"title\":\"CPU Load (5s Avg/Peak)\",\"value\":\"12 / 16%\",\"unit\":\"%\",\"icon\":\"cpu\"}},"
        "{\"id\":\"card_heap\",\"type\":\"stat\",\"config\":{\"title\":\"Free DRAM\",\"value\":\"345 KB\",\"icon\":\"database\"}},"
        "{\"id\":\"card_temp\",\"type\":\"stat\",\"config\":{\"title\":\"Core Temp\",\"value\":\"24 °C\",\"icon\":\"thermometer\"}},"
        "{\"id\":\"card_uptime\",\"type\":\"stat\",\"config\":{\"title\":\"Uptime\",\"value\":\"0 s\",\"icon\":\"clock\"}},"
        "{\"id\":\"card_vco\",\"type\":\"stat\",\"config\":{\"title\":\"VCO Tone Freq\",\"value\":\"440.0 Hz\",\"icon\":\"activity\"}},"
        "{\"id\":\"card_vfo\",\"type\":\"stat\",\"config\":{\"title\":\"VFO Mod Rate\",\"value\":\"1.00 Hz\",\"icon\":\"zap\"}},"
        "{\"id\":\"card_pkts\",\"type\":\"stat\",\"config\":{\"title\":\"Transmitted BIS Frames\",\"value\":\"0\",\"icon\":\"send\"}},"
        "{\"id\":\"ctrl_lfo\",\"type\":\"toggle\",\"config\":{\"title\":\"0.10 Hz Sine VCS LFO\",\"value\":true}},"
        "{\"id\":\"ctrl_vol\",\"type\":\"slider\",\"config\":{\"title\":\"Manual SINK Volume Override\",\"value\":30,\"min\":10,\"max\":100}},"
        "{\"id\":\"ctrl_gain\",\"type\":\"slider\",\"config\":{\"title\":\"Tone Generator Gain\",\"value\":30,\"min\":0,\"max\":100}},"
        "{\"id\":\"card_sinks\",\"type\":\"stat\",\"config\":{\"title\":\"Connected SINK Nodes\",\"value\":\"Scanning...\",\"icon\":\"headphones\"}}"
      "]"
    "}";

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)init_manifest;
    ws_pkt.len = strlen(init_manifest);

    httpd_ws_send_frame_async(m_server, client_fd, &ws_pkt);
    ESP_LOGI(TAG, "Sent Initial Dashboard Manifest to Client FD %d", client_fd);
}

void WebDashboard::handleIncomingAction(const char* json_str, size_t len) {
    ESP_LOGI(TAG, "Received WebSocket Action: %s", json_str);

    /* Quick parser for control card interactions */
    if (strstr(json_str, "ctrl_lfo")) {
        bool state = (strstr(json_str, "true") != nullptr);
        if (m_lfo_cb) m_lfo_cb(state);
        ESP_LOGI(TAG, "Action: LFO Toggle set to %s", state ? "ON" : "OFF");
    } else if (strstr(json_str, "ctrl_vol")) {
        const char* val_ptr = strstr(json_str, "\"value\":");
        if (val_ptr) {
            int vol = atoi(val_ptr + 8);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            if (m_vol_cb) m_vol_cb(static_cast<uint8_t>(vol));
            ESP_LOGI(TAG, "Action: Manual Volume Override set to %d%%", vol);
        }
    } else if (strstr(json_str, "ctrl_gain")) {
        const char* val_ptr = strstr(json_str, "\"value\":");
        if (val_ptr) {
            float gain = (float)atof(val_ptr + 8);
            if (gain < 0.0f) gain = 0.0f;
            if (gain > 100.0f) gain = 100.0f;
            if (m_gain_cb) m_gain_cb(gain);
            ESP_LOGI(TAG, "Action: Tone Gain set to %.1f%%", gain);
        }
    }
}

void WebDashboard::broadcastTelemetry(const DashboardTelemetry& data) {
    if (m_client_fds.empty() || !m_server) return;

    TickType_t now = xTaskGetTickCount();
    /* Rate limit to 3 Hz (every 333 ms) */
    if ((now - m_last_broadcast_tick) < pdMS_TO_TICKS(333)) {
        return;
    }
    m_last_broadcast_tick = now;

    char buf[512];

    /* Format SINKs summary string */
    char sinks_str[128] = "No SINKs connected";
    if (!data.sinks.empty()) {
        snprintf(sinks_str, sizeof(sinks_str), "%u Connected (Vol: %.0f%%)",
                 (unsigned int)data.sinks.size(), data.sinks[0].volume_percent);
    }

    /* Send batch updates */
    auto sendCardUpdate = [this](const char* card_id, const char* val_str) {
        char payload[192];
        int len = snprintf(payload, sizeof(payload), 
                           "{\"type\":\"update\",\"cardId\":\"%s\",\"data\":{\"value\":\"%s\"}}",
                           card_id, val_str);
        httpd_ws_frame_t ws_pkt = {};
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)payload;
        ws_pkt.len = len;

        for (auto it = m_client_fds.begin(); it != m_client_fds.end(); ) {
            esp_err_t ret = httpd_ws_send_frame_async(m_server, *it, &ws_pkt);
            if (ret != ESP_OK) {
                it = m_client_fds.erase(it);
            } else {
                ++it;
            }
        }
    };

    snprintf(buf, sizeof(buf), "%d / %d%%", data.cpu_mean_pct, data.cpu_peak_pct);
    sendCardUpdate("card_cpu", buf);

    snprintf(buf, sizeof(buf), "%lu KB", data.free_heap_kb);
    sendCardUpdate("card_heap", buf);

    snprintf(buf, sizeof(buf), "%d °C", data.cpu_temp_c);
    sendCardUpdate("card_temp", buf);

    snprintf(buf, sizeof(buf), "%lu s", data.uptime_sec);
    sendCardUpdate("card_uptime", buf);

    snprintf(buf, sizeof(buf), "%.1f Hz", data.vco_freq_hz);
    sendCardUpdate("card_vco", buf);

    snprintf(buf, sizeof(buf), "%.2f Hz", data.vfo_mod_rate_hz);
    sendCardUpdate("card_vfo", buf);

    snprintf(buf, sizeof(buf), "%lu", data.packets_count);
    sendCardUpdate("card_pkts", buf);

    sendCardUpdate("card_sinks", sinks_str);
}

} // namespace Web
