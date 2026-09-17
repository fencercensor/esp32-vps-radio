#pragma once

// Copy this file to radio_config.h, then replace every placeholder.
// Keep radio_config.h private: it is ignored by Git.

// Host or origin address that the ESP32 can reach over HTTP.
#define RADIO_GATEWAY_HOST "YOUR_SERVER_HOST_OR_IP"

// HTTP Host header used by Apache/LiteSpeed to select the correct website.
#define RADIO_GATEWAY_HTTP_HOST "radio.example.com"

#define RADIO_GATEWAY_PORT 80

// Use a unique password of at least 8 characters for the temporary setup AP.
#define RADIO_SETUP_AP_PASSWORD "CHANGE_ME_TO_A_UNIQUE_PASSWORD"
