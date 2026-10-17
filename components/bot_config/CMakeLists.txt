idf_component_register(SRCS "src/bot_config.c"
                    INCLUDE_DIRS "include"
                    REQUIRES discord_scheduler discord_rest
                             discord_memory)
