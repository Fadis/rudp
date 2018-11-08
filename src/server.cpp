/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#include <cstdio>
#include <iostream>
#include <iterator>
#include <thread>
#include <boost/container/static_vector.hpp>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

class udp_server {
public:
  udp_server( boost::asio::io_service &ios, uint16_t p ) : io_service( ios ), socket( ios, boost::asio::ip::udp::endpoint( boost::asio::ip::udp::v4(), p ) ), port( p ) {
    receive();
  }
private:
  void receive() {
    std::shared_ptr< boost::container::static_vector< uint8_t, 2048u > > data( new boost::container::static_vector< uint8_t, 2048u >( 2048u ) );
    using boost::asio::ip::udp;
    std::shared_ptr< udp::endpoint > from( new udp::endpoint() );
    socket.async_receive_from(
      boost::asio::buffer( data->data(), data->size() ),
      *from,
      [this,data,from](
        const boost::system::error_code& error,
        size_t size
      ) {
        if( likely( !error ) ) {
          data->resize( size );
          receive();
          socket.async_send_to( boost::asio::buffer( data->data(), data->size() ),
            *from,
            []( const boost::system::error_code&, size_t ) {}
          );
        }
      }
    );
  };
  boost::asio::io_service &io_service;
  uint16_t port;
  boost::asio::ip::udp::socket socket;
};

int main( int argc, char *argv[] ) {
  namespace po = boost::program_options;
  po::options_description opts( "Options" );
  opts.add_options()
    ( "port,p", po::value< uint16_t >()->default_value( 7 ), "port" )
    ( "help,h", "display this message" );
  po::variables_map values;
  po::store( po::parse_command_line( argc, argv, opts ), values );
  if( values.count("help") ) {
    std::cout << opts << std::endl;
    return 0;
  }
  boost::asio::io_service io_service;
  udp_server server( io_service, values[ "port" ].as< uint16_t >() );
  std::thread worker( [&]() { io_service.run(); } );
  worker.join();
}

