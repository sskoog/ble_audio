#include "ble_audio_broadcast.hpp"
#include "status_led.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cmath>
#include <cstring>

static const char* TAG = "BLE_AUDIO";
static Bluetooth::BleAudioBroadcast* s_broadcast_instance = nullptr;

namespace Bluetooth {

/* Static value handles for GATT server characteristics on SINK */
static uint16_t s_bass_recv_state_val_handle = 0;
static uint16_t s_vcs_state_val_handle = 0;
static uint16_t s_vcs_flags_val_handle = 0;

const char* bluetoothStateToString(BluetoothState state) {
    switch (state) {
        case BluetoothState::OFF:          return "OFF";
        case BluetoothState::SCANNING:     return "SCANNING";
        case BluetoothState::STREAMING:    return "STREAMING";
        case BluetoothState::BROADCASTING: return "BROADCASTING";
        default:                           return "UNKNOWN";
    }
}

/* =====================================================================
 *            CONVENIENCE DEBUG FUNCTIONS FOR GATT STRUCTURES
 * ===================================================================== */

void printGATTnotification(const char* service_name, uint16_t chr_uuid, const uint8_t* data, size_t len) {
    if (!service_name || !data || len == 0) return;

    if (chr_uuid == BLE_GATT_CHR_BASS_RECV_STATE) {
        /* Decode BASS Receive State Notification (UUID 0x2BC8) */
        uint8_t src_id = data[0];
        uint8_t pa_sync = (len > 8) ? data[8] : 0;
        uint8_t enc = (len > 9) ? data[9] : 0;
        uint32_t bis_sync = 0;
        if (len >= 14) {
            bis_sync = data[10] | (data[11] << 8) | (data[12] << 16) | (data[13] << 24);
        }
        ESP_LOGI("GATT_NOTIF", "[BASS 0x2BC8 Notification] SrcID: %u | PA Sync: %s (0x%02X) | Encryption: 0x%02X | BIS Sync: 0x%08lX",
                 src_id, (pa_sync == 0x02) ? "PA_SYNCED" : (pa_sync == 0x01) ? "SYNC_REQ" : "NOT_SYNCED", pa_sync, enc, (unsigned long)bis_sync);
    } else if (chr_uuid == BLE_GATT_CHR_VCS_STATE) {
        /* Decode VCS Volume State Notification (UUID 0x2B7D) */
        uint8_t vol = data[0];
        uint8_t mute = (len > 1) ? data[1] : 0;
        uint8_t counter = (len > 2) ? data[2] : 0;
        float pct = (static_cast<float>(vol) * 100.0f) / 255.0f;
        ESP_LOGI("GATT_NOTIF", "[VCS 0x2B7D Notification] Volume: %.1f%% (%u/255) | Mute: %s | Counter: #%u",
                 pct, vol, mute ? "MUTED" : "UNMUTED", counter);
    } else {
        ESP_LOGI("GATT_NOTIF", "[%s 0x%04X Notification] Length: %u bytes", service_name, chr_uuid, (unsigned int)len);
    }
}

void printGATTcommand(const char* service_name, uint16_t chr_uuid, const uint8_t* data, size_t len) {
    if (!service_name || !data || len == 0) return;

    if (chr_uuid == BLE_GATT_CHR_BASS_CP) {
        /* Decode BASS Control Point Write (UUID 0x2BC7) */
        uint8_t opcode = data[0];
        const char* op_str = "UNKNOWN";
        if (opcode == 0x01) op_str = "SET_BROADCAST_CODE";
        else if (opcode == 0x02) op_str = "ADD_SOURCE";
        else if (opcode == 0x03) op_str = "MODIFY_SOURCE";
        else if (opcode == 0x04) op_str = "SET_BROADCAST_CODE_RSP";
        else if (opcode == 0x05) op_str = "REMOVE_SOURCE";

        if (opcode == 0x02 && len >= 15) {
            uint32_t b_id = data[8] | (data[9] << 8) | (data[10] << 16);
            uint8_t pa_sync = data[11];
            uint32_t bis_sync = data[14] | (data[15] << 8) | (data[16] << 16) | (data[17] << 24);
            ESP_LOGI("GATT_CMD", "[BASS 0x2BC7 Write: %s] Broadcast_ID: 0x%06lX | PA Sync: %u | BIS Sync: 0x%08lX",
                     op_str, (unsigned long)b_id, pa_sync, (unsigned long)bis_sync);
        } else {
            ESP_LOGI("GATT_CMD", "[BASS 0x2BC7 Write: %s] Opcode: 0x%02X | Length: %u bytes", op_str, opcode, (unsigned int)len);
        }
    } else if (chr_uuid == BLE_GATT_CHR_VCS_CP) {
        /* Decode VCS Control Point Write (UUID 0x2B7E) */
        uint8_t opcode = data[0];
        const char* op_str = "UNKNOWN";
        if (opcode == 0x00) op_str = "REL_VOL_DOWN";
        else if (opcode == 0x01) op_str = "REL_VOL_UP";
        else if (opcode == 0x02) op_str = "UNMUTE";
        else if (opcode == 0x03) op_str = "MUTE";
        else if (opcode == 0x04) op_str = "SET_ABSOLUTE_VOLUME";

        if (opcode == 0x04 && len >= 2) {
            uint8_t vol = data[1];
            float pct = (static_cast<float>(vol) * 100.0f) / 255.0f;
            ESP_LOGI("GATT_CMD", "[VCS 0x2B7E Write: %s] Target: %.1f%% (%u/255)", op_str, pct, vol);
        } else {
            ESP_LOGI("GATT_CMD", "[VCS 0x2B7E Write: %s] Opcode: 0x%02X", op_str, opcode);
        }
    } else {
        ESP_LOGI("GATT_CMD", "[%s 0x%04X Write] Length: %u bytes", service_name, chr_uuid, (unsigned int)len);
    }
}

/* =====================================================================
 *                GATT SERVER ACCESS CALLBACKS (SINK NODE)
 * ===================================================================== */

int BleAudioBroadcast::gattAccessPacs(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == BLE_GATT_CHR_PACS_SINK_PAC) {
        /*
         * Published Audio Capabilities (PACS) - Sink PAC Record (0x2BC9)
         * Standard Bluetooth SIG PAC Record LTV Structure:
         * - Num PAC Records: 1
         * - Codec ID: 0x06 (LC3), Company ID: 0x0000, Vendor Codec: 0x0000
         * - Capabilities LTVs:
         *   * 0x03, 0x01, 0x60, 0x00: Sampling Freq (44.1 kHz 0x0020 & 48 kHz 0x0040)
         *   * 0x02, 0x02, 0x03: Frame Duration (7.5 ms 0x01 | 10 ms 0x02 = 0x03)
         *   * 0x02, 0x03, 0x03: Channel Count (1 & 2 channels = 0x03 Stereo Capable)
         *   * 0x05, 0x04, 0x14, 0x00, 0x78, 0x00: Octets/Frame (Min 20 to Max 120 octets = 16 to 160 kbps)
         * - Metadata LTV:
         *   * 0x03, 0x02, 0x04, 0x00: Preferred Audio Context = Media (0x0004)
         */
        static const uint8_t sink_pac_record[] = {
            0x01,                                           /* Num PAC Records = 1 */
            0x06, 0x00, 0x00, 0x00, 0x00,                   /* Codec: LC3 (0x06) */
            16,                                             /* Codec Specific Capabilities Length = 16 */
            0x03, 0x01, 0x60, 0x00,                         /* Sampling Freq: 44.1 & 48 kHz */
            0x02, 0x02, 0x03,                               /* Frame Duration: 7.5 & 10 ms */
            0x02, 0x03, 0x03,                               /* Channels: 1 & 2 (Stereo) */
            0x05, 0x04, 0x14, 0x00, 0x78, 0x00,             /* Min 20 octets, Max 120 octets */
            0x04,                                           /* Metadata Length = 4 */
            0x03, 0x02, 0x04, 0x00                          /* Audio Context: Media (0x0004) */
        };
        int rc = os_mbuf_append(ctxt->om, sink_pac_record, sizeof(sink_pac_record));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    } 
    else if (uuid == BLE_GATT_CHR_PACS_SINK_LOC) {
        /*
         * Sink Audio Locations (0x2BCA)
         * 32-bit bitfield: Front Left (0x00000001) | Front Right (0x00000002) = 0x00000003 (Stereo)
         */
        static const uint32_t audio_locations = 0x00000003;
        int rc = os_mbuf_append(ctxt->om, &audio_locations, sizeof(audio_locations));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    else if (uuid == BLE_GATT_CHR_PACS_SINK_CTX) {
        /*
         * Sink Supported & Available Audio Contexts (0x2BCE)
         * 16-bit Supported (Media 0x0004) + 16-bit Available (Media 0x0004) = 0x00040004
         */
        static const uint32_t audio_contexts = 0x00040004;
        int rc = os_mbuf_append(ctxt->om, &audio_contexts, sizeof(audio_contexts));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

int BleAudioBroadcast::gattAccessBass(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!s_broadcast_instance) return BLE_ATT_ERR_UNLIKELY;
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == BLE_GATT_CHR_BASS_RECV_STATE) {
        /* Read Broadcast Receive State (0x2BC8) */
        int rc = os_mbuf_append(ctxt->om, &s_broadcast_instance->m_bass_state, sizeof(BassReceiveState));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    else if (uuid == BLE_GATT_CHR_BASS_CP) {
        /* Write to Broadcast Audio Scan Control Point (0x2BC7) */
        uint8_t buf[64] = {0};
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, len, buf);

        printGATTcommand("BASS", BLE_GATT_CHR_BASS_CP, buf, len);

        uint8_t opcode = buf[0];
        if (opcode == 0x02 && len >= 15) {
            /*
             * Opcode 0x02: Add Source
             * Format: [0x02, Adv_Addr_Type, Addr(6), Adv_SID, Broadcast_ID(3), PA_Sync_State, PA_Interval(2), Num_Subgroups, BIS_Sync(4)]
             */
            s_broadcast_instance->m_bass_state.source_addr_type = buf[1];
            memcpy(s_broadcast_instance->m_bass_state.source_addr, &buf[2], 6);
            s_broadcast_instance->m_bass_state.source_adv_sid = buf[8];
            s_broadcast_instance->m_bass_state.broadcast_id[0] = buf[9];
            s_broadcast_instance->m_bass_state.broadcast_id[1] = buf[10];
            s_broadcast_instance->m_bass_state.broadcast_id[2] = buf[11];
            s_broadcast_instance->m_bass_state.pa_sync_state = 0x02; /* PA Synced */
            s_broadcast_instance->m_bass_state.big_encryption = 0x00; /* Unencrypted */
            
            if (len >= 18) {
                s_broadcast_instance->m_bass_state.bis_sync_bitfield = buf[14] | (buf[15] << 8) | (buf[16] << 16) | (buf[17] << 24);
            }

            s_broadcast_instance->m_telemetry.is_synced = true;
            s_broadcast_instance->m_telemetry.bis_index = 1;
            s_broadcast_instance->m_last_adv_tick = xTaskGetTickCount();

            /* Notify connected GATT client of updated BASS Receive State */
            if (s_bass_recv_state_val_handle != 0) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_broadcast_instance->m_bass_state, sizeof(BassReceiveState));
                if (om) {
                    ble_gatts_notify_custom(conn_handle, s_bass_recv_state_val_handle, om);
                }
            }

            s_broadcast_instance->transitionTo(BluetoothState::STREAMING);
            ESP_LOGI(TAG, "SINK: BASS Add Source Accepted! Locked to Broadcast ID 0x%02X%02X%02X (BIS #%u)",
                     buf[11], buf[10], buf[9], (unsigned int)s_broadcast_instance->m_telemetry.bis_index);
        }
        else if (opcode == 0x05) {
            /* Opcode 0x05: Remove Source */
            s_broadcast_instance->m_bass_state.pa_sync_state = 0x00; /* Not Synced */
            s_broadcast_instance->m_bass_state.bis_sync_bitfield = 0x00000000;
            s_broadcast_instance->transitionTo(BluetoothState::SCANNING);
            ESP_LOGI(TAG, "SINK: BASS Remove Source Accepted. Returning to SCANNING.");
        }

        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

int BleAudioBroadcast::gattAccessVcs(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!s_broadcast_instance) return BLE_ATT_ERR_UNLIKELY;
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == BLE_GATT_CHR_VCS_STATE) {
        /* Read Volume State (0x2B7D) - 3 bytes: Volume Setting (0-255), Mute (0/1), Change Counter */
        int rc = os_mbuf_append(ctxt->om, &s_broadcast_instance->m_vcs_state, sizeof(VcsVolumeState));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    else if (uuid == BLE_GATT_CHR_VCS_FLAGS) {
        /* Read Volume Flags (0x2B7F) - 1 byte (0x01 = Persisted) */
        int rc = os_mbuf_append(ctxt->om, &s_broadcast_instance->m_vcs_flags, 1);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    else if (uuid == BLE_GATT_CHR_VCS_CP) {
        /* Write Volume Control Point (0x2B7E) */
        uint8_t buf[16] = {0};
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, len, buf);

        printGATTcommand("VCS", BLE_GATT_CHR_VCS_CP, buf, len);

        uint8_t opcode = buf[0];
        switch (opcode) {
            case 0x00: /* Relative Volume Down */
                if (s_broadcast_instance->m_vcs_state.volume_setting >= 10)
                    s_broadcast_instance->m_vcs_state.volume_setting -= 10;
                else
                    s_broadcast_instance->m_vcs_state.volume_setting = 0;
                break;
            case 0x01: /* Relative Volume Up */
                if (s_broadcast_instance->m_vcs_state.volume_setting <= 245)
                    s_broadcast_instance->m_vcs_state.volume_setting += 10;
                else
                    s_broadcast_instance->m_vcs_state.volume_setting = 255;
                break;
            case 0x02: /* Unmute */
                s_broadcast_instance->m_vcs_state.mute = 0;
                break;
            case 0x03: /* Mute */
                s_broadcast_instance->m_vcs_state.mute = 1;
                break;
            case 0x04: /* Set Absolute Volume: [0x04, Volume_Setting (0-255), Change_Counter] */
                if (len >= 2) {
                    s_broadcast_instance->m_vcs_state.volume_setting = buf[1];
                }
                break;
            default:
                break;
        }

        s_broadcast_instance->m_vcs_state.change_counter++;
        s_broadcast_instance->m_telemetry.volume_setting = s_broadcast_instance->m_vcs_state.volume_setting;
        s_broadcast_instance->m_telemetry.volume_percent = (s_broadcast_instance->m_vcs_state.volume_setting * 100) / 255;
        s_broadcast_instance->m_telemetry.is_muted = (s_broadcast_instance->m_vcs_state.mute != 0);

        /* Notify connected GATT clients of updated volume state */
        if (s_vcs_state_val_handle != 0) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_broadcast_instance->m_vcs_state, sizeof(VcsVolumeState));
            if (om) {
                ble_gatts_notify_custom(conn_handle, s_vcs_state_val_handle, om);
            }
        }

        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* =====================================================================
 *                GATT SERVER DEFINITIONS (NIMBLE SERVICES TABLE)
 * ===================================================================== */

/* Static 16-bit UUID structures for C++ compatibility */
static const ble_uuid16_t uuid_pacs_svc = BLE_UUID16_INIT(BLE_GATT_SVC_PACS_UUID16);
static const ble_uuid16_t uuid_pacs_sink_pac = BLE_UUID16_INIT(BLE_GATT_CHR_PACS_SINK_PAC);
static const ble_uuid16_t uuid_pacs_sink_loc = BLE_UUID16_INIT(BLE_GATT_CHR_PACS_SINK_LOC);
static const ble_uuid16_t uuid_pacs_sink_ctx = BLE_UUID16_INIT(BLE_GATT_CHR_PACS_SINK_CTX);

static const ble_uuid16_t uuid_bass_svc = BLE_UUID16_INIT(BLE_GATT_SVC_BASS_UUID16);
static const ble_uuid16_t uuid_bass_recv_state = BLE_UUID16_INIT(BLE_GATT_CHR_BASS_RECV_STATE);
static const ble_uuid16_t uuid_bass_cp = BLE_UUID16_INIT(BLE_GATT_CHR_BASS_CP);

static const ble_uuid16_t uuid_vcs_svc = BLE_UUID16_INIT(BLE_GATT_SVC_VCS_UUID16);
static const ble_uuid16_t uuid_vcs_state = BLE_UUID16_INIT(BLE_GATT_CHR_VCS_STATE);
static const ble_uuid16_t uuid_vcs_cp = BLE_UUID16_INIT(BLE_GATT_CHR_VCS_CP);
static const ble_uuid16_t uuid_vcs_flags = BLE_UUID16_INIT(BLE_GATT_CHR_VCS_FLAGS);

static const struct ble_gatt_chr_def s_pacs_characteristics[] = {
    {
        /* Sink PAC (0x2BC9) - Characteristic: Read */
        .uuid = &uuid_pacs_sink_pac.u,
        .access_cb = BleAudioBroadcast::gattAccessPacs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = nullptr,
    },
    {
        /* Sink Audio Locations (0x2BCA) - Characteristic: Read */
        .uuid = &uuid_pacs_sink_loc.u,
        .access_cb = BleAudioBroadcast::gattAccessPacs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = nullptr,
    },
    {
        /* Sink Audio Contexts (0x2BCE) - Characteristic: Read */
        .uuid = &uuid_pacs_sink_ctx.u,
        .access_cb = BleAudioBroadcast::gattAccessPacs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = nullptr,
    },
    { 0 } /* Terminating characteristic */
};

static const struct ble_gatt_chr_def s_bass_characteristics[] = {
    {
        /* Broadcast Receive State (0x2BC8) - Characteristic: Read | Notify */
        .uuid = &uuid_bass_recv_state.u,
        .access_cb = BleAudioBroadcast::gattAccessBass,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_bass_recv_state_val_handle,
    },
    {
        /* Broadcast Audio Scan Control Point (0x2BC7) - Characteristic: Write */
        .uuid = &uuid_bass_cp.u,
        .access_cb = BleAudioBroadcast::gattAccessBass,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = nullptr,
    },
    { 0 } /* Terminating characteristic */
};

static const struct ble_gatt_chr_def s_vcs_characteristics[] = {
    {
        /* Volume State (0x2B7D) - Characteristic: Read | Notify */
        .uuid = &uuid_vcs_state.u,
        .access_cb = BleAudioBroadcast::gattAccessVcs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_vcs_state_val_handle,
    },
    {
        /* Volume Control Point (0x2B7E) - Characteristic: Write */
        .uuid = &uuid_vcs_cp.u,
        .access_cb = BleAudioBroadcast::gattAccessVcs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = nullptr,
    },
    {
        /* Volume Flags (0x2B7F) - Characteristic: Read | Notify */
        .uuid = &uuid_vcs_flags.u,
        .access_cb = BleAudioBroadcast::gattAccessVcs,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_vcs_flags_val_handle,
    },
    { 0 } /* Terminating characteristic */
};

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        /* Service: Published Audio Capabilities Service (PACS - 0x184E) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_pacs_svc.u,
        .includes = nullptr,
        .characteristics = s_pacs_characteristics,
    },
    {
        /* Service: Broadcast Audio Scan Service (BASS - 0x184F) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_bass_svc.u,
        .includes = nullptr,
        .characteristics = s_bass_characteristics,
    },
    {
        /* Service: Volume Control Service (VCS - 0x1844) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_vcs_svc.u,
        .includes = nullptr,
        .characteristics = s_vcs_characteristics,
    },
    { 0 } /* Terminating service */
};

esp_err_t BleAudioBroadcast::registerGattServices() {
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GATT Server Services Registered: PACS (0x184E), BASS (0x184F), VCS (0x1844)");
    return ESP_OK;
}

/* =====================================================================
 *            GATT CLIENT & CENTRAL DISCOVERY (SOURCE NODE)
 * ===================================================================== */

void BleAudioBroadcast::addOrUpdateTrackedSink(uint16_t conn_handle, const ble_addr_t* addr, const char* name) {
    /* 1. Try to find existing sink by MAC address */
    if (addr) {
        for (auto& sink : m_tracked_sinks) {
            if (memcmp(sink.addr.val, addr->val, 6) == 0) {
                if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    sink.conn_handle = conn_handle;
                    sink.connected = true;
                }
                if (name && strlen(name) > 0) sink.device_name = name;
                sink.last_seen_tick = xTaskGetTickCount();
                return;
            }
        }
    }

    /* 2. Try to find existing sink by conn_handle */
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        for (auto& sink : m_tracked_sinks) {
            if (sink.conn_handle == conn_handle) {
                sink.connected = true;
                if (addr) sink.addr = *addr;
                if (name && strlen(name) > 0) sink.device_name = name;
                sink.last_seen_tick = xTaskGetTickCount();
                return;
            }
        }
    }

    /* 3. Add new sink entry if table not full */
    if (m_tracked_sinks.size() < MAX_GATT_SINK_NODES) {
        DiscoveredSinkNode new_sink;
        new_sink.conn_handle = conn_handle;
        if (addr) new_sink.addr = *addr;
        if (name) new_sink.device_name = name;
        new_sink.connected = (conn_handle != BLE_HS_CONN_HANDLE_NONE);
        new_sink.last_seen_tick = xTaskGetTickCount();
        m_tracked_sinks.push_back(new_sink);
        ESP_LOGI(TAG, "SOURCE: Added new SINK to tracking table (Total Tracked: %u / %u)",
                 (unsigned int)m_tracked_sinks.size(), MAX_GATT_SINK_NODES);
    }
}

int BleAudioBroadcast::onGattDiscSvcCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                       const struct ble_gatt_svc *service, void *arg) {
    if (!s_broadcast_instance) return 0;
    if (error->status == 0 && service) {
        uint16_t uuid = ble_uuid_u16(&service->uuid.u);
        if (uuid == BLE_GATT_SVC_BASS_UUID16 || uuid == BLE_GATT_SVC_VCS_UUID16) {
            /* Discover characteristics in this service */
            ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle, onGattDiscChrCb, s_broadcast_instance);
        }
    }
    return 0;
}

int BleAudioBroadcast::onGattDiscChrCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                       const struct ble_gatt_chr *chr, void *arg) {
    if (!s_broadcast_instance) return 0;
    if (error->status == 0 && chr) {
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);

        for (auto& sink : s_broadcast_instance->m_tracked_sinks) {
            if (sink.conn_handle == conn_handle) {
                if (uuid == BLE_GATT_CHR_BASS_RECV_STATE) {
                    sink.bass_state_handle = chr->val_handle;
                    /* Subscribe to BASS Receive State notifications by writing 0x0001 to CCCD (val_handle + 1) */
                    uint16_t cccd_val = 0x0001;
                    ble_gattc_write_flat(conn_handle, chr->val_handle + 1, &cccd_val, sizeof(cccd_val), nullptr, nullptr);
                }
                else if (uuid == BLE_GATT_CHR_BASS_CP) {
                    sink.bass_cp_handle = chr->val_handle;
                    /* Immediately assign our active BIS Auracast stream to this SINK */
                    s_broadcast_instance->configureSinkBass(sink);
                }
                else if (uuid == BLE_GATT_CHR_VCS_STATE) {
                    sink.vcs_state_handle = chr->val_handle;
                    /* Subscribe to VCS Volume State notifications by writing 0x0001 to CCCD (val_handle + 1) */
                    uint16_t cccd_val = 0x0001;
                    ble_gattc_write_flat(conn_handle, chr->val_handle + 1, &cccd_val, sizeof(cccd_val), nullptr, nullptr);
                }
                else if (uuid == BLE_GATT_CHR_VCS_CP) {
                    sink.vcs_cp_handle = chr->val_handle;
                }
                break;
            }
        }
    }
    return 0;
}

void BleAudioBroadcast::handleIncomingNotification(uint16_t conn_handle, uint16_t val_handle, const uint8_t* data, uint16_t len) {
    for (auto& sink : m_tracked_sinks) {
        if (sink.conn_handle == conn_handle) {
            sink.last_seen_tick = xTaskGetTickCount();
            if (val_handle == sink.bass_state_handle) {
                printGATTnotification("BASS", BLE_GATT_CHR_BASS_RECV_STATE, data, len);
                if (len > 8) sink.pa_sync_state = data[8];
                if (len >= 14) sink.bis_sync_bitfield = data[10] | (data[11] << 8) | (data[12] << 16) | (data[13] << 24);
            }
            else if (val_handle == sink.vcs_state_handle) {
                printGATTnotification("VCS", BLE_GATT_CHR_VCS_STATE, data, len);
                sink.volume_setting = data[0];
                sink.volume_percent = (static_cast<float>(data[0]) * 100.0f) / 255.0f;
                sink.is_muted = (len > 1 && data[1] != 0);
            }
            break;
        }
    }
}

void BleAudioBroadcast::configureSinkBass(DiscoveredSinkNode& sink) {
    if (sink.conn_handle == BLE_HS_CONN_HANDLE_NONE || sink.bass_cp_handle == 0) return;

    /*
     * Build standard BASS Opcode 0x02: Add Source command
     * [0x02, Adv_Addr_Type, Addr(6), Adv_SID, Broadcast_ID(3), PA_Sync_State, PA_Interval(2), Num_Subgroups, BIS_Sync(4)]
     */
    uint8_t cmd[18] = {0};
    cmd[0] = 0x02; /* Add Source */
    cmd[1] = 0x00; /* Public Address */
    /* Addr */
    cmd[8] = 0x01; /* Adv SID = 1 */
    cmd[9] = 0x56; cmd[10] = 0x34; cmd[11] = 0x12; /* Broadcast ID: 0x123456 */
    cmd[12] = 0x02; /* PA_Sync = Synchronize to PA */
    cmd[13] = 0x00; cmd[14] = 0x00; /* PA Interval */
    cmd[15] = 0x01; /* Num Subgroups = 1 */
    cmd[16] = 0x01; cmd[17] = 0x00; /* BIS Sync: BIS #1 (bit 0) */

    printGATTcommand("BASS", BLE_GATT_CHR_BASS_CP, cmd, sizeof(cmd));

    int rc = ble_gattc_write_flat(sink.conn_handle, sink.bass_cp_handle, cmd, sizeof(cmd), nullptr, nullptr);
    if (rc == 0) {
        sink.bass_configured = true;
        ESP_LOGI(TAG, "SOURCE: Sent BASS Add Source to SINK (Handle: %u | Broadcast ID: 0x123456 | BIS #1)", sink.conn_handle);
    } else {
        ESP_LOGW(TAG, "SOURCE: Failed to send BASS Add Source command (rc = %d)", rc);
    }
}

void BleAudioBroadcast::sendManualVolumeToAllSinks(uint8_t vol_pct) {
    uint8_t vol_setting = (static_cast<uint16_t>(vol_pct) * 255) / 100;
    ESP_LOGI(TAG, "Manual SINK Volume broadcast: %d%% (%d/255) to %u sinks", 
             vol_pct, vol_setting, (unsigned int)m_tracked_sinks.size());
    for (auto& sink : m_tracked_sinks) {
        if (sink.connected && sink.vcs_cp_handle != 0) {
            sendVcsVolumeToSink(sink, vol_setting);
        }
    }
}

void BleAudioBroadcast::sendVcsVolumeToSink(DiscoveredSinkNode& sink, uint8_t volume_setting) {
    if (sink.conn_handle == BLE_HS_CONN_HANDLE_NONE || sink.vcs_cp_handle == 0) return;

    /*
     * Build standard VCS Opcode 0x04: Set Absolute Volume command
     * [0x04, Volume_Setting (0-255)]
     */
    uint8_t cmd[2] = { 0x04, volume_setting };

    ble_gattc_write_flat(sink.conn_handle, sink.vcs_cp_handle, cmd, sizeof(cmd), nullptr, nullptr);
    sink.volume_setting = volume_setting;
    sink.volume_percent = (static_cast<float>(volume_setting) * 100.0f) / 255.0f;
    sink.last_seen_tick = xTaskGetTickCount();
}

/* =====================================================================
 *                       GAP EVENT HANDLERS
 * ===================================================================== */

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (!s_broadcast_instance) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_EXT_DISC: {
            const auto &disc = event->ext_disc;
            s_broadcast_instance->parseAdvReport(disc.data, disc.length_data, disc.rssi, &disc.addr);
            for (auto& s : s_broadcast_instance->getTrackedSinksMutable()) {
                if (memcmp(s.addr.val, disc.addr.val, 6) == 0) {
                    s.rssi = disc.rssi;
                    s.last_seen_tick = xTaskGetTickCount();
                }
            }
            break;
        }
        case BLE_GAP_EVENT_DISC: {
            const auto &disc = event->disc;
            s_broadcast_instance->parseAdvReport(disc.data, disc.length_data, disc.rssi, &disc.addr);
            break;
        }
                case BLE_GAP_EVENT_CONNECT: {
            s_broadcast_instance->m_is_connecting = false;
            if (event->connect.status == 0) {
                uint16_t conn_handle = event->connect.conn_handle;
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(conn_handle, &desc) == 0) {
                    ESP_LOGI(TAG, "BLE GAP Connect Success! Conn Handle: %u, Peer: %02x:%02x:%02x:%02x:%02x:%02x",
                             conn_handle, desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                             desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
                    s_broadcast_instance->addOrUpdateTrackedSink(conn_handle, &desc.peer_id_addr, nullptr);
                } else {
                    ESP_LOGI(TAG, "BLE GAP Connect Success! Conn Handle: %u", conn_handle);
                    s_broadcast_instance->addOrUpdateTrackedSink(conn_handle, nullptr, nullptr);
                }

                for (auto& s : s_broadcast_instance->getTrackedSinksMutable()) {
                    if (s.conn_handle == conn_handle) {
                        s.connected = true;
                        s.connecting = false;
                        break;
                    }
                }

                /* If SOURCE, initiate GATT service discovery on the connected SINK */
                if (get_system_config()->node_role == NODE_ROLE_SOURCE) {
                    int drc = ble_gattc_disc_all_svcs(conn_handle, BleAudioBroadcast::onGattDiscSvcCb, s_broadcast_instance);
                    ESP_LOGI(TAG, "SOURCE: Initiated GATT service discovery on conn_handle %u (rc = %d)", conn_handle, drc);
                }
            } else {
                ESP_LOGW(TAG, "BLE GAP Connect Failed, status = %d", event->connect.status);
                for (auto& s : s_broadcast_instance->getTrackedSinksMutable()) {
                    s.connecting = false;
                }
            }
            break;
        }
                case BLE_GAP_EVENT_DISCONNECT: {
            uint16_t conn_handle = event->disconnect.conn.conn_handle;
            ESP_LOGW(TAG, "BLE GAP Disconnected! Conn Handle: %u, Reason: %d", conn_handle, event->disconnect.reason);
            s_broadcast_instance->m_is_connecting = false;
            for (auto it = s_broadcast_instance->getTrackedSinksMutable().begin(); it != s_broadcast_instance->getTrackedSinksMutable().end(); ) {
                if (it->conn_handle == conn_handle) {
                    ESP_LOGI(TAG, "Removing disconnected SINK node '%s' from tracking table", it->device_name.c_str());
                    it = s_broadcast_instance->getTrackedSinksMutable().erase(it);
                } else {
                    ++it;
                }
            }

            if (get_system_config()->node_role == NODE_ROLE_SOURCE) {
                /* SOURCE: resume scanning to rediscover and reconnect to SINK */
                s_broadcast_instance->startScanning();
            } else {
                /* SINK: restart connectable advertising so SOURCE can discover and reconnect! */
                ble_gap_ext_adv_start(1, 0, 0);
                ESP_LOGI(TAG, "SINK: Restarted Connectable Extended Advertising (Instance 1)");
            }
            break;
        }
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint16_t conn_handle = event->notify_rx.conn_handle;
            uint16_t attr_handle = event->notify_rx.attr_handle;
            uint8_t data[64] = {0};
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof(data)) len = sizeof(data);
            os_mbuf_copydata(event->notify_rx.om, 0, len, data);

            s_broadcast_instance->handleIncomingNotification(conn_handle, attr_handle, data, len);
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE: {
            s_broadcast_instance->startScanning();
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE: {
            if (get_system_config()->node_role == NODE_ROLE_SINK) {
                /* SINK: Always keep connectable advertising active */
                ble_gap_ext_adv_start(1, 0, 0);
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

/* =====================================================================
 *                  LIFECYCLE & STATE MACHINE
 * ===================================================================== */

BleAudioBroadcast::BleAudioBroadcast(Codec::Lc3CodecEngine& lc3_codec, 
                                     Audio::ToneGenerator* tone_gen, 
                                     Hardware::I2sAudioDriver* i2s_dac)
    : m_lc3_codec(lc3_codec), m_tone_gen(tone_gen), m_i2s_dac(i2s_dac) {
    s_broadcast_instance = this;
}

BleAudioBroadcast::~BleAudioBroadcast() {
    transitionTo(BluetoothState::OFF);
    if (s_broadcast_instance == this) {
        s_broadcast_instance = nullptr;
    }
}

esp_err_t BleAudioBroadcast::init(uint8_t node_role) {
    m_node_role = node_role;
    m_state = BluetoothState::OFF;
    m_ble_hw_enabled = false;
    m_initialized = true;

    m_vcs_state.volume_setting = 77; /* Default ~30% volume */
    m_vcs_state.mute = 0;
    m_vcs_state.change_counter = 1;

    m_telemetry.volume_setting = 77;
    m_telemetry.volume_percent = 30;
    m_telemetry.is_muted = false;

    ESP_LOGI(TAG, "BleAudioBroadcast initialized in state OFF (Node Role: %d).", m_node_role);
    return ESP_OK;
}

void BleAudioBroadcast::setVolumeSetting(uint8_t vol, bool notify) {
    m_vcs_state.volume_setting = vol;
    m_vcs_state.change_counter++;
    m_telemetry.volume_setting = vol;
    m_telemetry.volume_percent = (vol * 100) / 255;

    if (notify && s_vcs_state_val_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&m_vcs_state, sizeof(VcsVolumeState));
        if (om) {
            ble_gatts_notify_custom(0, s_vcs_state_val_handle, om);
        }
    }
}

void BleAudioBroadcast::setMute(bool mute, bool notify) {
    m_vcs_state.mute = mute ? 1 : 0;
    m_vcs_state.change_counter++;
    m_telemetry.is_muted = mute;

    if (notify && s_vcs_state_val_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&m_vcs_state, sizeof(VcsVolumeState));
        if (om) {
            ble_gatts_notify_custom(0, s_vcs_state_val_handle, om);
        }
    }
}

esp_err_t BleAudioBroadcast::enableBluetoothHardware() {
    if (m_ble_hw_enabled) return ESP_OK;

    ESP_LOGI(TAG, "Configuring system parameters and enabling Bluetooth hardware...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize nimble port: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = onResetCb;
    ble_hs_cfg.sync_cb = onSyncCb;

    /* Set standard LE Audio Headphone Appearance (0x0841) */
    ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_HEADPHONES);
    ble_svc_gap_device_name_set(get_system_config()->device_name);

    /* Register full GATT Server (PACS, BASS, VCS) on SINK */
    if (m_node_role == NODE_ROLE_SINK) {
        registerGattServices();
    } else {
        ble_svc_gap_init();
        ble_svc_gatt_init();
    }

    /* Initialize LC3 Codec Engine */
    if (m_node_role == NODE_ROLE_SOURCE) {
        m_lc3_codec.initEncoder(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS_NUM, AUDIO_FRAME_DURATION_MS * 1000, AUDIO_LC3_OCTETS_PER_FRAME);
    } else {
        m_lc3_codec.initDecoder(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS_NUM, AUDIO_FRAME_DURATION_MS * 1000, AUDIO_LC3_OCTETS_PER_FRAME);
    }

    nimble_port_freertos_init(hostTaskRoutine);

    m_ble_hw_enabled = true;
    ESP_LOGI(TAG, "Bluetooth Hardware & NimBLE Host Stack successfully enabled.");
    return ESP_OK;
}

esp_err_t BleAudioBroadcast::disableBluetoothHardware() {
    if (!m_ble_hw_enabled) return ESP_OK;

    ESP_LOGI(TAG, "Disabling Bluetooth Hardware & NimBLE Host...");
    nimble_port_stop();
    nimble_port_deinit();
    m_ble_hw_enabled = false;
    return ESP_OK;
}

esp_err_t BleAudioBroadcast::transitionTo(BluetoothState target_state) {
    if (m_state == target_state) return ESP_OK;

    ESP_LOGI(TAG, "Bluetooth State Transition: [%s] ---> [%s]", 
             bluetoothStateToString(m_state), bluetoothStateToString(target_state));

    BluetoothState old_state = m_state;

    if (old_state == BluetoothState::OFF && target_state != BluetoothState::OFF) {
        esp_err_t err = enableBluetoothHardware();
        if (err != ESP_OK) return err;
    }

    switch (target_state) {
        case BluetoothState::OFF:
            disableBluetoothHardware();
            m_telemetry.is_synced = false;
            m_telemetry.source_name = "N/A";
            m_telemetry.rssi_dbm = 0;
            m_telemetry.packets_count = 0;
            break;

        case BluetoothState::SCANNING:
            m_telemetry.is_synced = false;
            m_telemetry.source_name = "SEARCHING...";
            m_telemetry.rssi_dbm = 0;
            m_telemetry.packets_count = 0;
            if (m_ble_hw_enabled && ble_hs_is_enabled()) {
                startScanning();
                if (m_node_role == NODE_ROLE_SINK) {
                    /* SINK: ensure connectable advertising is running */
                    ble_gap_ext_adv_start(1, 0, 0);
                }
            }
            break;

        case BluetoothState::STREAMING:
            m_telemetry.is_synced = true;
            break;

        case BluetoothState::BROADCASTING:
            m_telemetry.is_synced = true;
            break;
    }

    m_state = target_state;
    return ESP_OK;
}

void BleAudioBroadcast::notifyAdvReceived(const char* name, int8_t rssi) {
    if (name) m_telemetry.source_name = name;
    m_telemetry.rssi_dbm = rssi;
    m_last_adv_tick = xTaskGetTickCount();

    if (m_state == BluetoothState::SCANNING) {
        transitionTo(BluetoothState::STREAMING);
    }
}

void BleAudioBroadcast::parseAdvReport(const uint8_t* data, uint8_t length_data, int8_t rssi, const ble_addr_t* addr) {
    if (!data || length_data == 0) return;

    bool is_le_audio_broadcast = false;
    char found_name[48] = {0};



    size_t offset = 0;
    while (offset < length_data) {
        uint8_t ad_len = data[offset];
        if (ad_len == 0 || (offset + 1 + ad_len) > length_data) break;

        uint8_t ad_type = data[offset + 1];
        const uint8_t *ad_payload = &data[offset + 2];
        uint8_t payload_len = (ad_len > 0) ? (ad_len - 1) : 0;

        /* Extract Complete or Shortened Local Name (AD Types 0x09, 0x08) */
        if (ad_type == 0x09 || ad_type == 0x08) {
            size_t copy_len = (payload_len < sizeof(found_name) - 1) ? payload_len : (sizeof(found_name) - 1);
            memcpy(found_name, ad_payload, copy_len);
            found_name[copy_len] = '\0';
        }

        /* Filter for 16-bit Service UUIDs (AD Types 0x02, 0x03): BASE (0x1851), BAA/PBA (0x1852), BASS (0x184F), PACS (0x184E), PBA (0x1856) */
        if (ad_type == 0x02 || ad_type == 0x03) {
            for (size_t i = 0; i + 1 < payload_len; i += 2) {
                uint16_t uuid = ad_payload[i] | (ad_payload[i + 1] << 8);
                if (uuid == 0x1851 || uuid == 0x1852 || uuid == 0x184F || uuid == 0x184E || uuid == 0x1856 || uuid == 0x1850) {
                    is_le_audio_broadcast = true;
                }
            }
        }

        /* Filter for Service Data (AD Type 0x16): BASE (0x1851), BAA/PBA (0x1852), BASS (0x184F), PBA (0x1856) */
        if (ad_type == 0x16 && payload_len >= 2) {
            uint16_t uuid = ad_payload[0] | (ad_payload[1] << 8);
            if (uuid == 0x1851 || uuid == 0x1852 || uuid == 0x184F || uuid == 0x1856 || uuid == 0x1850) {
                is_le_audio_broadcast = true;
            }
        }

        offset += (1 + ad_len);
    }

    if (found_name[0] != '\0') {
        if (strstr(found_name, "ESP32") != nullptr || 
            strstr(found_name, "Auracast") != nullptr || 
            strstr(found_name, "ForestChirp") != nullptr ||
            strstr(found_name, "Bumble") != nullptr ||
            strstr(found_name, "Pixel") != nullptr ||
            strstr(found_name, "Broadcast") != nullptr) {
            is_le_audio_broadcast = true;
        }
    }

    /* SINK Node: Listen for ANY BLE Audio / Auracast / Bumble broadcast and lock on */
    if (m_node_role == NODE_ROLE_SINK) {
        if (is_le_audio_broadcast || 
            (found_name[0] != '\0' && (strstr(found_name, "ForestChirp") != nullptr || strstr(found_name, "Auracast") != nullptr || strstr(found_name, "ESP32") != nullptr || strstr(found_name, "Source") != nullptr))) {
            if (found_name[0] == '\0') {
                if (addr) {
                    snprintf(found_name, sizeof(found_name), "SRC-%02X%02X", addr->val[1], addr->val[0]);
                } else {
                    strncpy(found_name, "Auracast-SRC", sizeof(found_name) - 1);
                }
            }
            notifyAdvReceived(found_name, rssi);
        }
    }
    /* SOURCE Node: If SINK found (name contains "ESP32"), connect over GATT */
    else if (m_node_role == NODE_ROLE_SOURCE && addr) {
        if (strstr(found_name, "ESP32") != nullptr || strstr(found_name, "Node20") != nullptr) {
            /* Check if already connected or currently connecting */
            bool already_active = false;
            for (const auto& sink : m_tracked_sinks) {
                if (memcmp(sink.addr.val, addr->val, 6) == 0) {
                    if (sink.connected || sink.connecting) {
                        already_active = true;
                        break;
                    }
                }
            }

            if (!already_active && !m_is_connecting && m_tracked_sinks.size() < MAX_GATT_SINK_NODES) {
                ESP_LOGI(TAG, "SOURCE: Discovered compatible SINK '%s'! Connecting via BLE GAP...", found_name);
                m_is_connecting = true;
                addOrUpdateTrackedSink(BLE_HS_CONN_HANDLE_NONE, addr, found_name);

                for (auto& s : m_tracked_sinks) {
                    if (memcmp(s.addr.val, addr->val, 6) == 0) {
                        s.connecting = true;
                        break;
                    }
                }

                struct ble_gap_conn_params conn_params = {};
                conn_params.scan_itvl = 32;
                conn_params.scan_window = 16;
                conn_params.itvl_min = 24;  /* 30 ms */
                conn_params.itvl_max = 40;  /* 50 ms */
                conn_params.latency = 0;
                conn_params.supervision_timeout = 200; /* 2.0 s */

                ble_gap_disc_cancel();
                int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, 10000, &conn_params, ble_gap_event_cb, this);
                ESP_LOGI(TAG, "ble_gap_connect initiated, return code: %d", rc);
                if (rc != 0) {
                    ESP_LOGW(TAG, "ble_gap_connect failed: %d, resuming discovery", rc);
                    m_is_connecting = false;
                    startScanning();
                }
            }
        }
    }
}

void BleAudioBroadcast::startScanning() {
    ESP_LOGI(TAG, "Starting BLE Extended Discovery (Node Role: %d)...", m_node_role);

    struct ble_gap_ext_disc_params uncoded_params = {};
    uncoded_params.itvl = 160;   /* 100 ms */
    uncoded_params.window = 160; /* 100 ms continuous scan */
    uncoded_params.passive = (m_node_role == NODE_ROLE_SINK) ? 1 : 0; /* Active scan for Central */

    int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0, 0, 0, &uncoded_params, nullptr, ble_gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Failed to start ble_gap_ext_disc, rc = %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE Extended Discovery Active.");
    }
}

void BleAudioBroadcast::checkSyncState() {
    if (m_node_role == NODE_ROLE_SINK) {
        if (m_state == BluetoothState::STREAMING) {
            TickType_t now = xTaskGetTickCount();
            if ((now - m_last_adv_tick) > pdMS_TO_TICKS(AUDIO_SYNC_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "Node20: Broadcast Sync Lost (Source Inactive/Powered Off). Transitioning to SCANNING...");
                transitionTo(BluetoothState::SCANNING);
            }
        }
    }
}

void BleAudioBroadcast::onResetCb(int reason) {
    ESP_LOGE(TAG, "NimBLE Host Reset! Reason = %d", reason);
}

void BleAudioBroadcast::onSyncCb(void) {
    ESP_LOGI(TAG, "NimBLE Host Synced.");
    if (s_broadcast_instance) {
        if (s_broadcast_instance->m_node_role == NODE_ROLE_SOURCE) {
            ESP_LOGI(TAG, "Node21: Starting BLE 5.3 Audio Broadcast & PBP Announcement...");

            struct ble_gap_ext_adv_params ext_params = {};
            ext_params.legacy_pdu = 0;
            ext_params.connectable = 0;
            ext_params.scannable = 0;
            ext_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
            ext_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
            ext_params.primary_phy = BLE_HCI_LE_PHY_1M;
            ext_params.secondary_phy = BLE_HCI_LE_PHY_1M;
            ext_params.sid = 1;
            ext_params.own_addr_type = BLE_OWN_ADDR_PUBLIC;

            int rc = ble_gap_ext_adv_configure(0, &ext_params, nullptr, ble_gap_event_cb, nullptr);
            if (rc != 0) ESP_LOGE(TAG, "ble_gap_ext_adv_configure failed: %d", rc);

            uint8_t raw_adv[64] = {0};
            size_t idx = 0;
            raw_adv[idx++] = 0x02; raw_adv[idx++] = 0x01; raw_adv[idx++] = 0x06;

            const char* name_str = "ESP32-C6-21";
            uint8_t nlen = static_cast<uint8_t>(strlen(name_str));
            raw_adv[idx++] = nlen + 1; raw_adv[idx++] = 0x09;
            memcpy(&raw_adv[idx], name_str, nlen);
            idx += nlen;

            raw_adv[idx++] = 0x03; raw_adv[idx++] = 0x03; raw_adv[idx++] = 0x52; raw_adv[idx++] = 0x18;

            struct os_mbuf *data = os_msys_get_pkthdr(idx, 0);
            if (data) {
                os_mbuf_append(data, raw_adv, idx);
                ble_gap_ext_adv_set_data(0, data);
            }

            ble_gap_ext_adv_start(0, 0, 0);
            ESP_LOGI(TAG, "Node21: BLE Extended Audio Broadcaster Active with Name: %s", name_str);

            /* SOURCE also starts scanning to discover SINK nodes */
            s_broadcast_instance->startScanning();
        } else {
            /* SINK Node: Start Extended Connectable Advertising to be discoverable by SOURCE */
            struct ble_gap_ext_adv_params adv_params = {};
            adv_params.legacy_pdu = 1;
            adv_params.connectable = 1;
            adv_params.scannable = 1;
            adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
            adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
            adv_params.primary_phy = BLE_HCI_LE_PHY_1M;
            adv_params.secondary_phy = BLE_HCI_LE_PHY_1M;
            adv_params.sid = 2;
            adv_params.own_addr_type = BLE_OWN_ADDR_PUBLIC;

            ble_gap_ext_adv_configure(1, &adv_params, nullptr, ble_gap_event_cb, nullptr);

            uint8_t adv_data[64] = {0};
            size_t a_idx = 0;
            adv_data[a_idx++] = 0x02; adv_data[a_idx++] = 0x01; adv_data[a_idx++] = 0x06;
            
            /* Appearance: 0x0841 (Stereo Headphones) */
            adv_data[a_idx++] = 0x03; adv_data[a_idx++] = 0x19; adv_data[a_idx++] = 0x41; adv_data[a_idx++] = 0x08;

            const char* sink_name = "ESP32-C6-20";
            uint8_t slen = static_cast<uint8_t>(strlen(sink_name));
            adv_data[a_idx++] = slen + 1; adv_data[a_idx++] = 0x09;
            memcpy(&adv_data[a_idx], sink_name, slen);
            a_idx += slen;

            /* Service UUIDs: PACS (0x184E), BASS (0x184F), VCS (0x1844) */
            adv_data[a_idx++] = 0x07; adv_data[a_idx++] = 0x03;
            adv_data[a_idx++] = 0x4E; adv_data[a_idx++] = 0x18;
            adv_data[a_idx++] = 0x4F; adv_data[a_idx++] = 0x18;
            adv_data[a_idx++] = 0x44; adv_data[a_idx++] = 0x18;

            struct os_mbuf *s_data = os_msys_get_pkthdr(a_idx, 0);
            if (s_data) {
                os_mbuf_append(s_data, adv_data, a_idx);
                ble_gap_ext_adv_set_data(1, s_data);
            }

            ble_gap_ext_adv_start(1, 0, 0);
            ESP_LOGI(TAG, "Node20: SINK GATT Server Advertising (Appearance: Stereo Headphones 0x0841, Services: PACS/BASS/VCS)");

            s_broadcast_instance->startScanning();
        }
    }
}

void BleAudioBroadcast::hostTaskRoutine(void* param) {
    ESP_LOGI(TAG, "NimBLE Host Task Started.");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleAudioBroadcast::startAudioTask() {
    if (!m_audio_task_running) {
        m_audio_task_running = true;
        xTaskCreate(audioTaskRoutine, "ble_audio_task", AUDIO_TASK_STACK_SIZE, this, 3, nullptr);
    }

    if (m_node_role == NODE_ROLE_SOURCE && !m_vcs_task_running) {
        m_vcs_task_running = true;
        xTaskCreate(vcsOscillatorTaskRoutine, "vcs_lfo_task", 3072, this, 2, nullptr);
    }
}

void BleAudioBroadcast::audioTaskRoutine(void* param) {
    auto* self = static_cast<BleAudioBroadcast*>(param);
    if (self->m_node_role == NODE_ROLE_SOURCE) {
        self->runSourceLoop();
    } else {
        self->runSinkLoop();
    }
}

void BleAudioBroadcast::vcsOscillatorTaskRoutine(void* param) {
    auto* self = static_cast<BleAudioBroadcast*>(param);
    self->runVcsOscillatorLoop();
}

void BleAudioBroadcast::runSourceLoop() {
    ESP_LOGI(TAG, "Node21: Audio Source Processing Loop Started (10 ms Frame Interval)...");

    static int16_t pcm_buffer[AUDIO_SAMPLES_PER_FRAME] = {0};
    static uint8_t lc3_buffer[AUDIO_LC3_OCTETS_PER_FRAME] = {0};
    size_t actual_encoded_bytes = 0;

    const TickType_t interval = pdMS_TO_TICKS(AUDIO_FRAME_DURATION_MS);

    while (m_audio_task_running) {
        if (m_state == BluetoothState::BROADCASTING) {
            if (m_tone_gen) {
                m_tone_gen->generateFrame(pcm_buffer, AUDIO_SAMPLES_PER_FRAME);
            }
            m_lc3_codec.encodeFrame(pcm_buffer, AUDIO_SAMPLES_PER_FRAME, lc3_buffer, sizeof(lc3_buffer), &actual_encoded_bytes);
            m_telemetry.packets_count++;
        }
        vTaskDelay(interval > 0 ? interval : 1);
    }
}

void BleAudioBroadcast::runSinkLoop() {
    ESP_LOGI(TAG, "Node20: Audio Sink Processing Loop Started (Google liblc3 Audio Decoder)...");

    static uint8_t lc3_frame_buf[AUDIO_LC3_OCTETS_PER_FRAME] = {0};
    static int16_t decoded_pcm[AUDIO_SAMPLES_PER_FRAME] = {0};
    static int16_t stereo_pcm[AUDIO_SAMPLES_PER_FRAME * 2] = {0}; // L + R interleaved for MAX98357A DAC
    size_t actual_samples = 0;
    size_t bytes_written = 0;
    uint32_t frame_count = 0;

    // Initialize an audible LC3 encoded test pattern using Google liblc3
    static Codec::Lc3CodecEngine test_encoder;
    test_encoder.initEncoder(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS_NUM, 10000, AUDIO_LC3_OCTETS_PER_FRAME);

    float synth_phase = 0.0f;
    float synth_lfo = 0.0f;
    size_t encoded_bytes = 0;

    while (m_audio_task_running) {
        checkSyncState();

        if (m_state == BluetoothState::STREAMING) {
            frame_count++;

            /* 1. Generate live broadcast LC3 frame (or receive from ISO stream) */
            float lfo_val = 0.5f * (sinf(synth_lfo) + 1.0f);
            float cur_freq = 330.0f + (lfo_val * 330.0f); // 330 Hz - 660 Hz sweep
            float phase_inc = (2.0f * M_PI * cur_freq) / static_cast<float>(AUDIO_SAMPLE_RATE_HZ);
            synth_lfo += (2.0f * M_PI * 0.5f) * (AUDIO_FRAME_DURATION_MS / 1000.0f);
            if (synth_lfo >= 2.0f * M_PI) synth_lfo -= 2.0f * M_PI;

            for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; ++i) {
                float s = sinf(synth_phase);
                synth_phase += phase_inc;
                if (synth_phase >= 2.0f * M_PI) synth_phase -= 2.0f * M_PI;
                decoded_pcm[i] = static_cast<int16_t>(s * 22000.0f);
            }

            // Encode to LC3 bitstream
            test_encoder.encodeFrame(decoded_pcm, AUDIO_SAMPLES_PER_FRAME, lc3_frame_buf, sizeof(lc3_frame_buf), &encoded_bytes);

            /* 2. Decode the LC3 bitstream back into PCM using Google liblc3 */
            m_lc3_codec.decodeFrame(lc3_frame_buf, encoded_bytes, decoded_pcm, AUDIO_SAMPLES_PER_FRAME, &actual_samples);

            /* 3. Apply VCS Volume Control & Interleave to Stereo Slots for MAX98357A */
            uint8_t vol_scale = (m_vcs_state.mute != 0) ? 0 : m_vcs_state.volume_setting;
            for (size_t i = 0; i < actual_samples; ++i) {
                int32_t scaled = (static_cast<int32_t>(decoded_pcm[i]) * vol_scale) / 255;
                int16_t sample = static_cast<int16_t>(scaled);
                stereo_pcm[i * 2] = sample;     // Left Channel
                stereo_pcm[i * 2 + 1] = sample; // Right Channel
            }

            /* 4. Stream to MAX98357A I2S DAC */
            if (m_i2s_dac && m_i2s_dac->isInitialized()) {
                esp_err_t err = m_i2s_dac->write(stereo_pcm, actual_samples * 2, &bytes_written, 50);
                if (err != ESP_OK && frame_count % 100 == 0) {
                    ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(err));
                }
            }
            m_telemetry.packets_count++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void BleAudioBroadcast::runVcsOscillatorLoop() {
    ESP_LOGI(TAG, "Node21: 0.33 Hz Sine VCS Oscillator Task Started (3 Hz Modulation Rate)...");

    const TickType_t interval = pdMS_TO_TICKS(VCS_UPDATE_INTERVAL_MS); // 333 ms
    uint32_t step_count = 0;

    while (m_vcs_task_running) {
        step_count++;
        float t = static_cast<float>(step_count) * (1.0f / VCS_UPDATE_RATE_HZ);
        
        /* 0.33 Hz Sine Wave: Period = 1.0 / 0.33 = ~3.03 seconds */
        float phase = 2.0f * M_PI * VCS_SINE_MOD_FREQ_HZ * t;
        float norm_sine = (sinf(phase) + 1.0f) * 0.5f; /* 0.0 to 1.0 */
        float vol_pct = VCS_VOLUME_MIN_PCT + norm_sine * (VCS_VOLUME_MAX_PCT - VCS_VOLUME_MIN_PCT); /* 10% to 50% */
        uint8_t vol_units = static_cast<uint8_t>((vol_pct * 255.0f) / 100.0f + 0.5f); /* ~25 to 128 */

        /* Transmit VCS Absolute Volume to all connected SINKs */
        for (auto& sink : m_tracked_sinks) {
            if (sink.connected && sink.vcs_cp_handle != 0) {
                sendVcsVolumeToSink(sink, vol_units);
            }
        }

        vTaskDelay(interval > 0 ? interval : 1);
    }
}

} // namespace Bluetooth
