/*
 * Physical Ethernet backbone for ESP32-C6-Bug + Eth-Bug-PoE.
 * W5500 uses SPI2; USB-Serial/JTAG remains the diagnostic console.
 */
#include "backbone.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

static const char *TAG = "w5500_link";
#define ETH_LINK_BIT BIT0
#define ETH_IPV4_BIT BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static esp_eth_netif_glue_handle_t s_glue;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;
    if (handle != s_eth) return;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {0};
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        xEventGroupSetBits(s_events, ETH_LINK_BIT);
        esp_err_t err = esp_netif_create_ip6_linklocal(s_netif);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "IPv6 link-local failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "link up, mac %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_events, ETH_LINK_BIT | ETH_IPV4_BIT);
        ESP_LOGW(TAG, "link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "driver started; waiting for cable and DHCP");
        break;
    case ETHERNET_EVENT_STOP:
        xEventGroupClearBits(s_events, ETH_LINK_BIT | ETH_IPV4_BIT);
        ESP_LOGI(TAG, "driver stopped");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        if (event->esp_netif != s_netif) return;
        xEventGroupSetBits(s_events, ETH_IPV4_BIT);
        ESP_LOGI(TAG, "DHCP address " IPSTR ", netmask " IPSTR ", gateway " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        xEventGroupClearBits(s_events, ETH_IPV4_BIT);
        ESP_LOGW(TAG, "DHCP address lost");
    }
}

esp_err_t backbone_start(void)
{
    if (s_netif) return ESP_ERR_INVALID_STATE;

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "GPIO ISR service");
    }

    spi_bus_config_t bus_config = {
        .miso_io_num = CONFIG_THBR_W5500_MISO_GPIO,
        .mosi_io_num = CONFIG_THBR_W5500_MOSI_GPIO,
        .sclk_io_num = CONFIG_THBR_W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI2 initialization");

    spi_device_interface_config_t device_config = {
        .mode = 0,
        .clock_speed_hz = CONFIG_THBR_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_THBR_W5500_CS_GPIO,
    };
    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &device_config);
    w5500_config.base.int_gpio_num = CONFIG_THBR_W5500_INT_GPIO;
    w5500_config.base.poll_period_ms = 0;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = CONFIG_THBR_W5500_RESET_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG, "W5500 MAC/PHY creation");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth),
                        TAG, "Ethernet driver install");

    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_BASE), TAG, "base MAC");
    mac_addr[0] = (uint8_t)((mac_addr[0] | 0x02) & 0xfe);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, mac_addr),
                        TAG, "set Ethernet MAC");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_NO_MEM, TAG, "Ethernet netif creation");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_netif, CONFIG_THBR_W5500_HOSTNAME),
                        TAG, "set hostname");

    s_glue = esp_eth_new_netif_glue(s_eth);
    ESP_RETURN_ON_FALSE(s_glue, ESP_ERR_NO_MEM, TAG, "Ethernet netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, s_glue), TAG, "attach netif");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "ETH events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL), TAG, "DHCP event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_handler, NULL), TAG, "lost-IP event");

    ESP_LOGI(TAG,
             "SPI2 W5500: MISO=%d INT=%d CS=%d SCLK=%d MOSI=%d RESET=%d clock=%d MHz",
             CONFIG_THBR_W5500_MISO_GPIO, CONFIG_THBR_W5500_INT_GPIO,
             CONFIG_THBR_W5500_CS_GPIO, CONFIG_THBR_W5500_SCLK_GPIO,
             CONFIG_THBR_W5500_MOSI_GPIO, CONFIG_THBR_W5500_RESET_GPIO,
             CONFIG_THBR_W5500_SPI_CLOCK_MHZ);
    return esp_eth_start(s_eth);
}

bool backbone_is_up(void)
{
    return s_events && (xEventGroupGetBits(s_events) & ETH_IPV4_BIT);
}

esp_err_t backbone_wait_up(uint32_t timeout_ms)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, ETH_IPV4_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & ETH_IPV4_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_netif_t *backbone_netif(void) { return s_netif; }
uint32_t backbone_peer_ipv4(void) { return 0; }

esp_err_t backbone_announce_got_ip(void)
{
    if (!s_netif || !backbone_is_up()) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t ip = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_netif, &ip), TAG, "get IP");
    ip_event_got_ip_t event = {
        .esp_netif = s_netif,
        .ip_info = ip,
        .ip_changed = false,
    };
    return esp_event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event, sizeof(event),
                          pdMS_TO_TICKS(100));
}
