idf_component_register(SRCS "src/discord_feature.c"
                    "src/discord_interaction_parse.c"
                    "src/discord_perms.c"
                    INCLUDE_DIRS "include"
                    REQUIRES discord_etf discord_json)
