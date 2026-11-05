if(SB_CONFIG_NET_CORE_IMAGE_HCI_IPC)
  ExternalZephyrProject_Add(
    APPLICATION hci_ipc
    SOURCE_DIR  ${ZEPHYR_BASE}/samples/bluetooth/hci_ipc
    BOARD       ${SB_CONFIG_NET_CORE_BOARD}
  )
endif()

# nRF7002 DK: the update slot is on the external flash, for MCUboot too.
if((BOARD STREQUAL "nrf7002dk" OR PS_SLOT1_EXTERNAL) AND SB_CONFIG_BOOTLOADER_MCUBOOT)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_nrf7002dk.overlay CACHE INTERNAL "")
  set(mcuboot_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_nrf7002dk.conf CACHE INTERNAL "")
endif()

# W55RP20-EVB-Pico: MCUboot needs the same partition layout as the application.
if(BOARD STREQUAL "w55rp20_pinscope" AND SB_CONFIG_BOOTLOADER_MCUBOOT)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_w55rp20.overlay CACHE INTERNAL "")
  set(conf ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_w55rp20.conf)
  # PS_MCUBOOT_DEBUG_CONF: an extra MCUboot config, e.g. its log over USB, for bring-up.
  if(DEFINED ENV{PS_MCUBOOT_DEBUG_CONF})
    list(APPEND conf $ENV{PS_MCUBOOT_DEBUG_CONF})
  endif()
  set(mcuboot_EXTRA_CONF_FILE "${conf}" CACHE INTERNAL "")
endif()
