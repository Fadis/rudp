/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#include <cstdio>
#include <iostream>
#include <iterator>
#include <numeric>
#include <algorithm>
#include <thread>
#include <chrono>
#include <queue>
#include <boost/container/static_vector.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <rudp/rudp.hpp>

int main( int argc, char *argv[] ) {
  namespace po = boost::program_options;
  po::options_description opts( "Options" );
  opts.add_options()
    ( "port,p", po::value< uint16_t >()->default_value( 8000 ), "port" )
    ( "help,h", "display this message" );
  po::variables_map values;
  po::store( po::parse_command_line( argc, argv, opts ), values );
  if( values.count("help") ) {
    std::cout << opts << std::endl;
    return 0;
  }
  srand( time( nullptr ) );
  boost::asio::io_service io_service;
  rudp::rudp_server server( io_service, values[ "port" ].as< uint16_t >(),
    []( rudp::rudp_server &self, uint32_t ident, const rudp::buffers_ptr_t &bufs ) {
      std::cout << "received" << std::endl;
      for( auto &buf: *bufs ) {
        if( buf->size() >= 2 ) {
          const auto header_size = (*buf)[ 1 ];
          if( size_t( header_size ) < buf->size() )
            self.send( ident, std::next( buf->data(), header_size ), std::next( buf->data(), buf->size() ), []( bool status ) {
	      if( status ) { std::cout << "responded" << std::endl; } else { std::cout << "failed" << std::endl; }
	    } );
        }
      }
    }
  );
  std::thread worker( [&]() { io_service.run(); } );
  worker.join();
}

