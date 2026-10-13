cmake_minimum_required(VERSION 3.30)

project(riot-tee-bl VERSION 0.1 LANGUAGES C ASM)

if (NOT DEFINED TOOLCHAINPATH)
    message(WARNING "Using default toolchain, may lead to errors.")
    set(TOOLCHAINPATH       /usr/bin)
endif()

set(CMAKE_C_COMPILER    ${TOOLCHAINPATH}/arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER  ${TOOLCHAINPATH}/arm-none-eabi-gcc)

# The cc3xx driver comes from TF-M. RIOT's pkg/riot-tee-bl passes the path of
# its tf-m package.
if(NOT DEFINED TFM_PATH)
    message(FATAL_ERROR "Set TFM_PATH to a TF-M source tree")
endif()
set(CC3XXPATH           "${TFM_PATH}/platform/ext/target/arm/drivers/cc3xx")
set(NRFXPATH            "${PROJECT_SOURCE_DIR}/3rd-party/nrfx")
set(CMSISPATH           "${PROJECT_SOURCE_DIR}/3rd-party/CMSIS_5/CMSIS")
set(CMSISDEVPATH        "${PROJECT_SOURCE_DIR}/3rd-party/CMSIS_5/Device/ARM/ARMCM33/Include")

# Memory layout — passed from RIOT build system or use defaults
if(NOT DEFINED BL_FLASH_SIZE)
    set(BL_FLASH_SIZE 0x8000)
endif()
if(NOT DEFINED TEE_FLASH_START)
    set(TEE_FLASH_START 0x8000)
endif()
if(NOT DEFINED TEE_FLASH_SIZE)
    set(TEE_FLASH_SIZE 0x20000)
endif()

# Generate linker script from template
configure_file(
    "${PROJECT_SOURCE_DIR}/target/nrf9160/nrf9160.ld.in"
    "${CMAKE_BINARY_DIR}/nrf9160.ld"
    @ONLY
)

set(LINKER_SCRIPT       "${CMAKE_SOURCE_DIR}/ld/common.ld")
set(MAP_FILE            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map")

set(FLAGS
    "-mcpu=cortex-m33"
    "-mthumb"
    "-mfloat-abi=hard"
    "-mabi=aapcs"
    "-mfpu=fpv5-sp-d16"
)

set(SOURCES
    src/main.c
    ${NRFXPATH}/mdk/system_nrf9160.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_hash.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_dma.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_engine_state.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_init.c
    ${CC3XXPATH}/common/cc3xx_stdlib.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_rng.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_hmac.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_drbg.c
    ${CC3XXPATH}/low_level_driver/src/cc3xx_drbg_hmac.c
)

set(ASM_SOURCES
    ${NRFXPATH}/mdk/gcc_startup_nrf9160.S
)

set_source_files_properties(${ASM_SOURCES}
    PROPERTIES
    COMPILE_FLAGS
    ${FLAGS}
    "-x"
    "assembler-with-cpp"
)

set(EXECUTABLE_NAME ${PROJECT_NAME}.elf)

add_executable(${EXECUTABLE_NAME}
    ${SOURCES}
    ${ASM_SOURCES}
)

target_include_directories(${EXECUTABLE_NAME}
    PUBLIC
    ${PROJECT_SOURCE_DIR}/target/nrf9160
    ${TFM_PATH}/platform/include
    ${CC3XXPATH}/common
    ${CC3XXPATH}/low_level_driver/include
    ${CMSISDEVPATH}
    ${CMSISPATH}/Core/Include
    ${NRFXPATH}
    ${NRFXPATH}/mdk
    ${NRFXPATH}/hal
    ${NRFXPATH}/templates
    ${NRFXPATH}/drivers/include
)

target_compile_definitions(${EXECUTABLE_NAME}
    PUBLIC
    NRF9160_XXAA
    FLOAT_ABI_HARD
    BL_FLASH_SIZE=${BL_FLASH_SIZE}
    TEE_FLASH_START=${TEE_FLASH_START}
    TEE_FLASH_SIZE=${TEE_FLASH_SIZE}
)

target_compile_options(${EXECUTABLE_NAME} PUBLIC
    ${FLAGS}
    "-mcmse"
    "-Wall"
    "-Werror"
)

target_link_options(${EXECUTABLE_NAME}
    PUBLIC
    ${FLAGS}
    "-T${PROJECT_SOURCE_DIR}/ld/common.ld"
    "-Wl,-Map=${MAP_FILE}"
    "--specs=nosys.specs"
    "-L${NRFXPATH}/mdk/"
    "-L${CMAKE_BINARY_DIR}/"
)

target_link_libraries(${EXECUTABLE_NAME}
    PUBLIC
    "-Wl,--start-group -lgcc -lc -lnosys"
    "-Wl,--end-group"
)