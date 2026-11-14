#include "info_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "backbone.h"
#include "heap_probe.h"
#include "version.h"

#if CONFIG_THBR_ENABLE_BORDER_ROUTER
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/border_routing.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#endif

#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "thbr_info";

static httpd_handle_t s_server;
static volatile bool  s_ot_ready;

/* Reset diagnostics are captured once during boot, before other tasks start.
 * The counter lives in NVS and survives normal flashing and power cycles.
 * Keep diagnostics best-effort: NVS problems must never trigger another reset. */
#define DIAG_NVS_NS  "thbr_diag"
#define DIAG_NVS_BOOTS "boots"
#define DIAG_NVS_API_REBOOT "api_reboot"

static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static uint32_t s_boot_count;
static bool s_boot_count_valid;
static bool s_api_reboot_requested;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:    return "ESP_RST_POWERON";
    case ESP_RST_EXT:        return "ESP_RST_EXT";
    case ESP_RST_SW:         return "ESP_RST_SW";
    case ESP_RST_PANIC:      return "ESP_RST_PANIC";
    case ESP_RST_INT_WDT:    return "ESP_RST_INT_WDT";
    case ESP_RST_TASK_WDT:   return "ESP_RST_TASK_WDT";
    case ESP_RST_WDT:        return "ESP_RST_WDT";
    case ESP_RST_DEEPSLEEP:  return "ESP_RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "ESP_RST_BROWNOUT";
    case ESP_RST_SDIO:       return "ESP_RST_SDIO";
    case ESP_RST_USB:        return "ESP_RST_USB";
    case ESP_RST_JTAG:       return "ESP_RST_JTAG";
    case ESP_RST_EFUSE:      return "ESP_RST_EFUSE";
    case ESP_RST_PWR_GLITCH: return "ESP_RST_PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "ESP_RST_CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:                return "ESP_RST_UNKNOWN";
    }
}

void info_server_init_boot_diagnostics(void)
{
    s_reset_reason = esp_reset_reason();

    nvs_handle_t h;
    esp_err_t err = nvs_open(DIAG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot diagnostics NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t count = 0;
    err = nvs_get_u32(h, DIAG_NVS_BOOTS, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        if (count < UINT32_MAX) {
            ++count;
        }
        err = nvs_set_u32(h, DIAG_NVS_BOOTS, count);
    }

    /* The marker is set only by the HTTP reboot endpoint. Clear it on every
     * boot to avoid misattributing a later software reset to this request. */
    uint8_t api_marker = 0;
    esp_err_t marker_err = nvs_get_u8(h, DIAG_NVS_API_REBOOT, &api_marker);
    if (marker_err == ESP_OK) {
        s_api_reboot_requested = api_marker == 1 && s_reset_reason == ESP_RST_SW;
        esp_err_t erase_err = nvs_erase_key(h, DIAG_NVS_API_REBOOT);
        if (erase_err != ESP_OK) {
            ESP_LOGW(TAG, "could not clear reboot marker: %s", esp_err_to_name(erase_err));
        }
    } else if (marker_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "could not read reboot marker: %s", esp_err_to_name(marker_err));
    }

    /* Commit the marker removal even if updating the counter failed. */
    esp_err_t commit_err = nvs_commit(h);
    if (err == ESP_OK && commit_err == ESP_OK) {
        s_boot_count = count;
        s_boot_count_valid = true;
    } else {
        ESP_LOGW(TAG, "boot counter NVS update failed: %s",
                 esp_err_to_name(err != ESP_OK ? err : commit_err));
    }
    nvs_close(h);

    ESP_LOGI(TAG, "boot diagnostics: reason=%s (%d), count=%" PRIu32
                  ", valid=%d, api_reboot=%d",
             reset_reason_name(s_reset_reason), (int)s_reset_reason,
             s_boot_count, (int)s_boot_count_valid, (int)s_api_reboot_requested);
}


void info_server_set_ot_ready(bool ready)
{
    s_ot_ready = ready;
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t version_get(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"product\":\"THBR\",\"fw\":\"%s\",\"build\":\"%s\","
             "\"chip\":\"%s\",\"idf\":\"%s\","
             "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
             FW_VERSION_STRING, FW_BUILD_DATE, CONFIG_IDF_TARGET, IDF_VER,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return send_json(req, body);
}

static esp_err_t status_get(httpd_req_t *req)
{
    const char *br = "n/a", *role = "n/a";

#if CONFIG_THBR_ENABLE_BORDER_ROUTER
    static const char *const br_state[] = { "uninitialized", "disabled", "stopped", "running" };
    static const char *const roles[]    = { "disabled", "detached", "child", "router", "leader" };
    if (s_ot_ready) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstance *inst = esp_openthread_get_instance();
        otBorderRoutingState brs = otBorderRoutingGetState(inst);
        otDeviceRole r = otThreadGetDeviceRole(inst);
        esp_openthread_lock_release();
        br   = (brs < 4) ? br_state[brs] : "?";
        role = (r < 5) ? roles[r] : "?";
    }
#endif

    char body[384];
    snprintf(body, sizeof(body),
             "{\"fw\":\"%s\",\"uptime_s\":%" PRId64 ",\"heap\":%" PRIu32
             ",\"heap_min\":%" PRIu32 ",\"link\":\"%s\",\"br\":\"%s\",\"role\":\"%s\""
             ",\"reset_reason\":\"%s\",\"reset_reason_code\":%d"
             ",\"boot_count\":%" PRIu32 ",\"boot_count_valid\":%s"
             ",\"reboot_requested\":%s}",
             FW_VERSION_STRING, esp_timer_get_time() / 1000000,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             backbone_is_up() ? "up" : "down", br, role,
             reset_reason_name(s_reset_reason), (int)s_reset_reason,
             s_boot_count, s_boot_count_valid ? "true" : "false",
             s_api_reboot_requested ? "true" : "false");
    return send_json(req, body);
}

/* The prefixes the border router advertises, plus the address it advertises
 * them from — everything the host needs to route into the mesh by hand. */
static esp_err_t backbone_get(httpd_req_t *req)
{
    char omr[64] = "", onlink[64] = "", ll[48] = "";

#if CONFIG_THBR_ENABLE_BORDER_ROUTER
    if (s_ot_ready) {
        otIp6Prefix pfx;
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstance *inst = esp_openthread_get_instance();
        if (otBorderRoutingGetOmrPrefix(inst, &pfx) == OT_ERROR_NONE) {
            otIp6PrefixToString(&pfx, omr, sizeof(omr));
        }
        if (otBorderRoutingGetOnLinkPrefix(inst, &pfx) == OT_ERROR_NONE) {
            otIp6PrefixToString(&pfx, onlink, sizeof(onlink));
        }
        esp_openthread_lock_release();
    }
#endif

    esp_ip6_addr_t a;
    if (esp_netif_get_ip6_linklocal(backbone_netif(), &a) == ESP_OK) {
        const uint8_t *b = (const uint8_t *)a.addr;
        snprintf(ll, sizeof(ll),
                 "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }

    char body[224];
    snprintf(body, sizeof(body),
             "{\"omr_prefix\":\"%s\",\"onlink_prefix\":\"%s\",\"ll\":\"%s\"}",
             omr, onlink, ll);
    return send_json(req, body);
}

/* Restart the chip.  Without this the stick cannot be brought back to a known
 * state from the host: a wedged border router keeps answering /status while
 * nothing reaches the mesh, and there is no other way in — the add-on has no
 * shell, and reflashing needs the version to differ. */
static void reboot_task(void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(500));      /* let the HTTP response leave first */
    /* Persist the source before esp_restart(), so the next boot can
     * distinguish an API request from another ESP_RST_SW reset. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(DIAG_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, DIAG_NVS_API_REBOOT, 1);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist API reboot marker: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "reboot requested over the info API");
    esp_restart();
}

/* ---------------------------------------------------------------- BLE proxy

   The stick can lend its Bluetooth radio to a Matter server, and where it
   dials used to be compiled in.  That was the wrong place twice over: a host
   with its own Bluetooth has no use for the offer and could not turn it off,
   so the firmware dialled a port nobody served once a minute forever; and a
   stale generated sdkconfig once shipped a bench address to users (0.1.42).
   The address now lives in NVS, with the compiled value as the default, and
   an empty string switches the radio offer off entirely.
*/
#define BLE_NVS_NS   "thbr"
#define BLE_NVS_KEY  "ble_uri"

static bool s_ble_enabled;

void info_server_set_ble_proxy(bool enabled) { s_ble_enabled = enabled; }

void thbr_ble_uri_get(char *out, size_t len, bool *from_nvs)
{
    nvs_handle_t h;
    if (from_nvs) {
        *from_nvs = false;
    }
    if (nvs_open(BLE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        esp_err_t err = nvs_get_str(h, BLE_NVS_KEY, out, &n);
        nvs_close(h);
        if (err == ESP_OK) {
            if (from_nvs) {
                *from_nvs = true;
            }
            return;
        }
    }
    snprintf(out, len, "%s", CONFIG_THBR_BLE_PROXY_URI);
}

esp_err_t thbr_ble_uri_set(const char *uri)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BLE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, BLE_NVS_KEY, uri ? uri : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t ble_proxy_get(httpd_req_t *req)
{
    char uri[128];
    bool from_nvs = false;
    thbr_ble_uri_get(uri, sizeof(uri), &from_nvs);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"uri\":\"%s\",\"source\":\"%s\",\"enabled\":%s}",
             uri, from_nvs ? "nvs" : "built-in", s_ble_enabled ? "true" : "false");
    return send_json(req, body);
}

static esp_err_t ble_proxy_post(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    int got = len > 0 ? httpd_req_recv(req, buf, len) : 0;
    if (got < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *item = root ? cJSON_GetObjectItem(root, "uri") : NULL;
    if (!item || !cJSON_IsString(item)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "expected {\"uri\": \"ws://host:port/ble\"} "
                                   "or an empty string to switch the radio offer off");
    }
    esp_err_t err = thbr_ble_uri_set(item->valuestring);
    char stored[128];
    snprintf(stored, sizeof(stored), "%s", item->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not store it");
    }
    ESP_LOGI(TAG, "BLE proxy endpoint set to '%s' — takes effect on the next restart", stored);
    /* Deliberately NOT applied live: bringing the NimBLE host and the
       websocket client up and down again is a bigger operation than this
       setting is worth, and a border router should not restart itself because
       a setting changed.  The caller decides when the stick restarts. */
    char body[256];
    snprintf(body, sizeof(body),
             "{\"uri\":\"%s\",\"stored\":true,\"applies\":\"next restart\"}", stored);
    return send_json(req, body);
}


static esp_err_t reboot_post(httpd_req_t *req)
{
    esp_err_t err = send_json(req, "{\"rebooting\":true}");
    xTaskCreate(reboot_task, "thbr_reboot", 2048, NULL, 5, NULL);
    return err;
}

esp_err_t info_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_THBR_INFO_PORT;
    /* esp_ot_br_server's instance owns the default control port (32768); a
     * second httpd on the same control port fails to start. */
    cfg.ctrl_port = 32769;
    /* One slot per handler, and httpd_register_uri_handler() FAILS QUIETLY
     * when they run out -- the endpoints registered last simply answer 404
     * afterwards.  Adding /ble_proxy without raising this cost /backbone and
     * /reboot exactly that way, which would have taken the host's route into
     * the mesh with it on any system that cannot learn it from a router
     * advertisement.  Count the registrations below before changing this.
     * Six are registered here; the heap probe adds three more when it is
     * compiled in, and it logs an error rather than failing quietly if the
     * count is ever wrong again. */
#if CONFIG_THBR_HEAP_PROBE
    cfg.max_uri_handlers = 11;
    /* The per-task census walks the task table and the heap on the handler's
     * stack (about 3 KB of locals); the default 4 KB leaves no margin. */
    cfg.stack_size = 6144;
#else
    cfg.max_uri_handlers = 8;
#endif
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t version_uri = { .uri = "/version", .method = HTTP_GET, .handler = version_get };
    const httpd_uri_t status_uri  = { .uri = "/status",  .method = HTTP_GET, .handler = status_get };
    const httpd_uri_t backbone_uri = { .uri = "/backbone", .method = HTTP_GET, .handler = backbone_get };
#define REG(u) do { \
        if (httpd_register_uri_handler(s_server, &(u)) != ESP_OK) { \
            ESP_LOGE(TAG, "could not register %s — raise max_uri_handlers", (u).uri); \
        } \
    } while (0)
    REG(version_uri);
    REG(status_uri);
    const httpd_uri_t reboot_uri = { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post };
    const httpd_uri_t ble_get_uri  = { .uri = "/ble_proxy", .method = HTTP_GET,  .handler = ble_proxy_get };
    const httpd_uri_t ble_post_uri = { .uri = "/ble_proxy", .method = HTTP_POST, .handler = ble_proxy_post };
    REG(ble_get_uri);
    REG(ble_post_uri);
    REG(backbone_uri);
    REG(reboot_uri);
    heap_probe_register(s_server);   /* no-op unless CONFIG_THBR_HEAP_PROBE */
    ESP_LOGI(TAG, "info API on port %d: /version /status /backbone /ble_proxy, POST /reboot (fw %s)",
             CONFIG_THBR_INFO_PORT, FW_VERSION_STRING);
    return ESP_OK;
}
