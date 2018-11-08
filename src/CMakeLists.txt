add_library( checksum checksum.cpp )
add_library( rudp rudp.cpp )
target_link_libraries( rudp Threads::Threads
  ${Boost_PROGRAM_OPTIONS_LIBRARIES} ${Boost_SYSTEM_LIBRARIES} checksum )
add_executable( injection injection.cpp )
target_link_libraries( injection Threads::Threads
  ${Boost_PROGRAM_OPTIONS_LIBRARIES} ${Boost_SYSTEM_LIBRARIES} checksum )
add_executable( syn_flood syn_flood.cpp )
target_link_libraries( syn_flood Threads::Threads
  ${Boost_PROGRAM_OPTIONS_LIBRARIES} ${Boost_SYSTEM_LIBRARIES} checksum )
add_executable( rudp_server rudp_server.cpp )
target_link_libraries( rudp_server Threads::Threads
  ${Boost_PROGRAM_OPTIONS_LIBRARIES} ${Boost_SYSTEM_LIBRARIES} rudp )
add_executable( rudp_client rudp_client.cpp )
target_link_libraries( rudp_client Threads::Threads
  ${Boost_PROGRAM_OPTIONS_LIBRARIES} ${Boost_SYSTEM_LIBRARIES} rudp )
