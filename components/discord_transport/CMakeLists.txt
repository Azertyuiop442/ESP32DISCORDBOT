idf_component_register(SRCS "src/discord_ackq.c"
                    "src/discord_identify.c"
                    "src/discord_nvs.c"
                    "src/discord_provision.c"
                    "src/discord_ws.c"
                    "src/discord_ws_decode.c"
                    "src/discord_rest_client.c"
                    "src/discord_reply.c"
                    "src/discord_v2.c"
                    "src/discord_transport.c"
                    INCLUDE_DIRS "include"
                    REQUIRES discord_core discord_etf discord_gateway
                             discord_json discord_memory discord_rest
                             discord_scheduler discord_security
                             bot_config
                             log esp_system esp-tls
                             esp_http_client esp_websocket_client
                             heap
                             nvs_flash freertos)
