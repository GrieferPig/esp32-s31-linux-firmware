idf_component_register(SRCS "host_stub.c"
                       INCLUDE_DIRS "../../../shared"
                       REQUIRES ulp)

set(ulp_app_name s31_lp_main)
set(ulp_lp_core_sources
    "lp_core/main.c"
    "lp_core/s31_lp_i2c.c"
    "lp_core/s31_lp_spi.c"
    "lp_core/s31_lp_uart.c"
    "lp_core/s31_lp_gdma.c")
set(ulp_exp_dep_srcs "host_stub.c")
ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")
