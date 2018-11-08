/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#include <memory>
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
#include <boost/spirit/include/karma.hpp>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <rudp/rudp.hpp>

template< typename Iterator >
void send( rudp::rudp_server &server, uint32_t identifier, Iterator begin, Iterator end, const std::function< void( bool ) > &cb ) {
  auto chunk_end = std::next( begin, std::min( 1344, std::distance( begin, end ) ) );
  server.send( identifier, begin, chunk_end, [&server=server,chunk_end,end]( bool status ){
  } );
}

struct sender {
  sender( rudp::rudp_server &session_ ) : session( session_ ), data( 1024 * 1024 ) {
    std::for_each( data.begin(), data.end(), []( auto &v ) { v = rand(); } );
    head = data.begin();
  }
  void operator()( uint32_t ident ) {
    if( head == data.end() ) {
      session.disconnect( ident );
      return;
    }
    session.send( ident, head, std::next( head, 1024 ),
      [this,ident]( bool status ) {
        if( status ) {
          head = std::next( head, 1024 );
          (*this)( ident );
        }
        else std::cout << "failed" << std::endl;
      }
    );
  }
  void check( const std::vector< uint8_t > &r ) {
    if( std::equal( data.begin(), data.end(), r.begin(), r.end() ) )
      std::cout << "ok" << std::endl;
    else
      std::cout << "corrupted" << std::endl;
  }
  std::vector< uint8_t >::const_iterator head;
  std::vector< uint8_t > data;
  rudp::rudp_server &session;
};

int main( int argc, char *argv[] ) {
  namespace po = boost::program_options;
  po::options_description opts( "Options" );
  opts.add_options()
    ( "remote_port,r", po::value< uint16_t >()->default_value( 8000 ), "port" )
    ( "self_port,s", po::value< uint16_t >()->default_value( 8001 ), "port" )
    ( "host,H", po::value< std::string >()->default_value( "localhost" ), "host" )
    ( "help,h", "display this message" );
  po::variables_map values;
  po::store( po::parse_command_line( argc, argv, opts ), values );
  if( values.count("help") ) {
    std::cout << opts << std::endl;
    return 0;
  }
  srand( time( nullptr ) );
  boost::asio::io_service io_service;
  using boost::asio::ip::udp;
  udp::resolver resolver( io_service );
  std::string port_name;
  boost::spirit::karma::generate( std::back_inserter( port_name ), boost::spirit::karma::ushort_, values[ "remote_port" ].as< uint16_t >() );
  udp::resolver::query query( values[ "host" ].as< std::string >(), port_name );
  const auto remote_endpoint = *resolver.resolve( query );
  std::vector< uint8_t > received;
  rudp::rudp_server server( io_service, values[ "self_port" ].as< uint16_t >(),
    [&]( rudp::rudp_server&, uint32_t ident, const rudp::buffers_ptr_t &bufs ) {
      std::cout << "received" << std::endl;
      for( const auto &buf: *bufs ) {
        const auto header_size = (*buf)[ 1 ];
        received.insert( received.end(), std::next( buf->begin(), header_size ), buf->end() );
      }
    }
  );
  sender s( server );
  std::string a( "abcde" );
  server.connect( remote_endpoint, [&]( bool status, uint32_t identifier ) {
    if( status ) {
      std::cout << "connected" << std::endl;
      s( identifier );
    }
    else std::cout << "failed" << std::endl;
  },
  [&]() {
    std::cout << "closed" << std::endl;
    s.check( received );
  } );
  std::thread worker( [&]() { io_service.run(); } );
  worker.join();
}

