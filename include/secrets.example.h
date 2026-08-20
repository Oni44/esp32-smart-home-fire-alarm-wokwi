#define SECRETS_EXAMPLE_H

// =====================================================
// WIFI CONFIGURATION
// =====================================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"


// =====================================================
// TELEGRAM CONFIGURATION
// =====================================================
#define TELEGRAM_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define TELEGRAM_CHAT_ID "YOUR_TELEGRAM_CHAT_ID"


// =====================================================
// THINGSBOARD CONFIGURATION
// =====================================================
#define TB_HOST "YOUR_THINGSBOARD_HOST"

// Sesuaikan dengan environment ThingsBoard/Wokwi.
// Project ini menggunakan MQTT port yang dikonfigurasi
// melalui environment ThingsBoard yang digunakan.
#define TB_PORT 11883

#define TB_ACCESS_TOKEN "YOUR_THINGSBOARD_DEVICE_ACCESS_TOKEN"


// =====================================================
// APPLICATION-LEVEL RPC SECURITY
// =====================================================
#define RPC_SECURITY_TOKEN "YOUR_RPC_SECURITY_TOKEN"


#endif