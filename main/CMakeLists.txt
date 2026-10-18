idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES discord_core discord_etf discord_gateway
                             discord_json discord_memory discord_rest
                             discord_scheduler discord_security
                             discord_transport bot_config xp
                             esp_event esp_netif esp_wifi lwip
                             heap nvs_flash freertos log)
