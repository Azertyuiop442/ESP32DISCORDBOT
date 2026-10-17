idf_component_register(SRCS "discord_xp_curve.c"
                             "discord_xp_cooldown.c"
                             "discord_xp_roles.c"
                             "discord_xp.c"
                     INCLUDE_DIRS "."
                     REQUIRES discord_core discord_memory discord_scheduler discord_transport)
