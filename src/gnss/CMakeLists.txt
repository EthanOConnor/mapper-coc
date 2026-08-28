#
#    Copyright 2026 Ethan O'Connor
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    OpenOrienteering is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.

# External GNSS receiver support: protocol parsers, transports, NTRIP
# corrections, and session management.
#
# This is the Qt 5 port of the same subsystem in the forward product line. Keep
# the two in step: the protocol and session layers are meant to be diffable.

find_package(Qt5Core REQUIRED)
find_package(Qt5Network REQUIRED)
find_package(Qt5Widgets REQUIRED)
find_package(Qt5Bluetooth QUIET)
find_package(Qt5SerialPort QUIET)
if(ANDROID)
	find_package(Qt5AndroidExtras REQUIRED)
endif()

set(CMAKE_AUTOMOC ON)

set(MAPPER_GNSS_DEFINITIONS
  PUBLIC
    MAPPER_GNSS_AVAILABLE
)

# minmea (vendored NMEA parser, WTFPL license)
add_library(minmea STATIC 3rdparty/minmea/minmea.c)
target_include_directories(minmea PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/minmea")
set_target_properties(minmea PROPERTIES C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(minmea PRIVATE -w)
endif()

set(MAPPER_GNSS_SOURCES
  # Protocol layer
  protocol/ubx_parser.cpp
  protocol/ubx_config.cpp
  protocol/nmea_parser.cpp
  protocol/protocol_detector.cpp
  protocol/rtcm_framer.cpp
  protocol/hyfix_protocol.cpp
  # Transport layer
  transport/gnss_transport.cpp
  transport/tcp_transport.cpp
  # Corrections
  correction/ntrip_client.cpp
  correction/ntrip_profile_store.cpp
  gga_generator.cpp
  # Session & integration
  gnss_fusion_engine.cpp
  gnss_session.cpp
  gnss_raw_logger.cpp
  hyfix_receiver.cpp
  gnss_controller.cpp
  transport/ble_device_model.cpp
  # UI
  ui/gnss_status_overlay.cpp
  ui/gnss_detail_panel.cpp
  ui/gnss_device_dialog.cpp
  # ntrip_settings_widget.cpp and gnss_settings_page.cpp are built in
  # Mapper_Common so the linker resolves GnssSettingsPage from settings_dialog.
)

set(MAPPER_GNSS_PRIVATE_LIBRARIES Qt5::Network minmea)

if(TARGET Qt5::Bluetooth)
	list(APPEND MAPPER_GNSS_SOURCES transport/ble_transport.cpp)
	list(APPEND MAPPER_GNSS_DEFINITIONS PUBLIC MAPPER_GNSS_BLE)
endif()

if(TARGET Qt5::SerialPort)
	list(APPEND MAPPER_GNSS_SOURCES transport/serial_transport.cpp)
	list(APPEND MAPPER_GNSS_DEFINITIONS PUBLIC MAPPER_GNSS_SERIAL)
endif()

if(ANDROID)
	# USB-host serial receivers, bridged through MapperUsbSerial.java and the
	# usb-serial-for-android driver library (see packaging/android).
	list(APPEND MAPPER_GNSS_SOURCES transport/android_usb_serial_transport.cpp)
	list(APPEND MAPPER_GNSS_DEFINITIONS PUBLIC MAPPER_GNSS_ANDROID_USB_SERIAL)
endif()

add_library(mapper-gnss STATIC ${MAPPER_GNSS_SOURCES})
target_compile_definitions(mapper-gnss ${MAPPER_GNSS_DEFINITIONS})
target_include_directories(mapper-gnss
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(mapper-gnss
  PUBLIC Qt5::Core Qt5::Widgets
  PRIVATE ${MAPPER_GNSS_PRIVATE_LIBRARIES}
)
if(TARGET Qt5::Bluetooth)
	target_link_libraries(mapper-gnss PUBLIC Qt5::Bluetooth)
endif()
if(TARGET Qt5::SerialPort)
	target_link_libraries(mapper-gnss PUBLIC Qt5::SerialPort)
endif()
if(ANDROID)
	target_link_libraries(mapper-gnss PUBLIC Qt5::AndroidExtras)
endif()
if(APPLE)
	# NTRIP passwords are stored in the login keychain.
	target_link_libraries(mapper-gnss PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
