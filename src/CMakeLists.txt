#############################
# CMake - Fluid Manager - root/src
#############################

set(SPI_SOURCES spi_proto/src/crc16.c spi_proto/src/spi_proto.c spi_proto/src/spi_proto.h)
set(SPI_REMOTE_SOURCES spi_proto/src/spi_remote_host.c spi_proto/src/binary_semaphore.c spi_proto/src/spi_proto_master.cpp spi_proto/src/spi_chunks.cpp)
set(SPI_DATAGRAM_SOURCES spi_proto/src/spi_proto_master_datagram.cpp)

set(FLUID_MANAGER_MODULE_SOURCES FluidManager.cpp ${SPI_SOURCES} ${SPI_REMOTE_SOURCES})

add_executable(amm_fluid_manager ${FLUID_MANAGER_MODULE_SOURCES})
target_include_directories(amm_fluid_manager PUBLIC spi_proto/src)

target_link_libraries(
   amm_fluid_manager
   PUBLIC amm_std
   PUBLIC Boost::thread
   ${TinyXML2_LIBRARIES}
)

install(TARGETS amm_fluid_manager RUNTIME DESTINATION bin)
install(DIRECTORY ../config DESTINATION bin)
