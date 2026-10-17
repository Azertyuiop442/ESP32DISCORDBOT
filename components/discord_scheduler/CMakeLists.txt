idf_component_register(SRCS "src/discord_jobq.c"
                    "src/discord_watchdog.c"
                    "src/discord_intq.c"
                    INCLUDE_DIRS "include"
                    REQUIRES discord_etf discord_core)
