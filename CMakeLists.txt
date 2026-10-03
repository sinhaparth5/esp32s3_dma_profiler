cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# TinyUSB vendor class: direct (unbuffered) transfers, 4 KB per IN transfer.
# The buffered default uses a 64 B FIFO that drains every packet and appends a ZLP each time.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_VENDOR_TXRX_BUFFERED=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_VENDOR_TX_EPSIZE=4096" APPEND)
project(esp32s3_dma_profiler)
