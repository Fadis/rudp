/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#ifndef RUDP_RUDP_HPP
#define RUDP_RUDP_HPP

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
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

namespace rudp {
const uint8_t *from_be16( const uint8_t *begin, uint16_t &value );
const uint8_t *from_be32( const uint8_t *begin, uint32_t &value );
uint8_t *to_be16( uint8_t *begin, uint16_t value );
uint8_t *to_be32( uint8_t *begin, uint32_t value );

struct unable_to_deserialize_session_config {};
struct unable_to_serialize_session_config {};
struct invalid_ack {};
struct invalid_packet {};
struct no_space_left_on_send_buffer {};

struct session_config {
  session_config();
  session_config( const uint8_t *begin, const uint8_t *end );
  void dump( uint8_t *begin, uint8_t *end );
  uint8_t max_out_of_standing_segs;
  uint8_t option_flags;
  uint16_t maximum_segment_size;
  uint16_t retransmission_timeout_value;
  uint16_t cumulative_ack_timeout_value;
  uint16_t null_segment_timeout_value;
  uint16_t transfer_state_timeout_value;
  uint8_t max_retrans;
  uint8_t max_cum_ack;
  uint8_t max_out_of_seq;
  uint8_t max_auto_reset;
  uint32_t connection_identifier;
};

session_config &operator&=( session_config &l, const session_config &r );
constexpr static const size_t buffer_size = 2048u;
constexpr static const size_t ring_size = 256u;
using buffer_t = boost::container::static_vector< uint8_t, buffer_size >;
using buffer_ptr_t = std::shared_ptr< buffer_t >;
using ring_t = std::array< buffer_ptr_t, ring_size >;
using buffers_t = boost::container::static_vector< buffer_ptr_t, ring_size >;
using buffers_ptr_t = std::shared_ptr< buffers_t >;
using outiter_t = std::back_insert_iterator< buffers_t >;
using send_cb_t = std::function< void( bool ) >;

template< typename T >
bool is_syn( const T &buf ) {
  return !buf.empty() && ( buf[ 0 ] & 0x80 );
}

template< typename T >
bool is_rst( const T &buf ) {
  return !buf.empty() && ( buf[ 0 ] & 0x10 );
}

enum class session_state {
  initial,
  opened,
  broken,
  closed
};

class session : public std::enable_shared_from_this< session > {
public:
  session(
    boost::asio::io_service &ios,
    boost::asio::ip::udp::socket &sock,
    const boost::asio::ip::udp::endpoint &endpoint_,
    const std::function< void( const boost::asio::ip::udp::endpoint& ) > &on_closed_
  );
  void connect( const std::function< void( bool, uint32_t ) > &cb );
  const session_config &get_remote_config() const { return remote_config; }
  const session_config &get_self_config() const { return self_config; }
  void receive( const buffer_ptr_t &incoming, outiter_t &received );
  void disconnect();
  template< typename Iterator >
  void send( Iterator begin, Iterator end, const send_cb_t &cb ) {
    send( generate_ack( begin, end ), false, cb );
  }
  void send( const buffer_ptr_t &incoming, bool resend, const send_cb_t &cb );
  void resend( uint8_t begin, uint8_t end );
private:
  void increment_cumulative_ack_counter();
  void reset_cumulative_ack_counter();
  void set_null_segment_timer();
  void set_retransmission_timer( uint8_t at );
  void clear_retransmission_timer( uint8_t begin, uint8_t end );
  void cancel_all_timers();
  void update_receive_head( outiter_t &received );
  std::vector< send_cb_t > update_ack( uint8_t new_acknowledge_head );
  std::vector< send_cb_t > update_eak( buffer_t::const_iterator begin, buffer_t::const_iterator end );
  bool check_common_header( const buffer_ptr_t &incoming );
  buffer_ptr_t generate_syn( bool ack );
  buffer_ptr_t generate_ack();
  template< typename Iterator >
  buffer_ptr_t generate_ack( Iterator begin, Iterator end ) {
    const auto size = std::distance( begin, end );
    buffer_ptr_t buffer( new buffer_t( 6 + size ) );
    (*buffer)[ 0 ] = 0x40;
    (*buffer)[ 1 ] = 6;
    std::copy( begin, end, std::next( buffer->data(), 6 ) );
    return buffer;
  }
  buffer_ptr_t generate_rst();
  buffer_ptr_t generate_eak();
  buffer_ptr_t generate_nul();
  bool ready_to_send();
  bool is_valid_sequence_number( uint8_t sequence_number );
  size_t get_out_of_sequence_acknowlege_count();
  size_t get_waiting_for_acknowlege_count();
  void send_packet(
    const buffer_ptr_t &data,
    const send_cb_t &cb
  );
  void send_packets(
    const buffers_ptr_t &data,
    const send_cb_t &cb
  );
  void send_packets_internal(
    const buffers_ptr_t &data,
    const send_cb_t &cb
  );
  void wait_for_tcs();
  void close();
  boost::asio::io_service &io_service;
  boost::asio::ip::udp::socket &socket;
  boost::asio::ip::udp::endpoint endpoint;
  session_config self_config;
  session_config remote_config;
  std::array< buffer_ptr_t, 256 > receive_buffer;
  uint8_t receive_head;
  std::array< std::pair< buffer_ptr_t, send_cb_t >, 256 > send_buffer;
  uint8_t send_head;
  uint8_t acknowledge_head;
  uint8_t out_of_sequence_count;
  size_t unacknowledged_packet_count;
  std::queue< std::pair< buffer_ptr_t, send_cb_t > > pending;
  size_t cumulative_ack_count;
  std::shared_ptr< boost::asio::steady_timer > cumulative_ack_timer;
  std::shared_ptr< boost::asio::steady_timer > null_segment_timer;
  std::shared_ptr< boost::asio::steady_timer > tcs_timer;
  std::array< std::pair< std::shared_ptr< boost::asio::steady_timer >, size_t >, 256 > retransmission_timer;
  session_state state;
  bool client;
  std::function< void( const boost::asio::ip::udp::endpoint& ) > on_closed;
};

class rudp_server {
public:
  rudp_server( boost::asio::io_service &ios, uint16_t p, const std::function< void( rudp_server&, uint32_t, const buffers_ptr_t& ) > &cb_ ) : io_service( ios ), socket( ios, boost::asio::ip::udp::endpoint( boost::asio::ip::udp::v4(), p ) ), port( p ), cb( cb_ ) {
    receive();
  }
  void connect(
    const boost::asio::ip::udp::endpoint &endpoint,
    const std::function< void( bool, uint32_t ) > &cb,
    const std::function< void() > &on_close
  ) {
	   auto &sess = sessions[ endpoint ];
	   if( !sess ) sess.reset( new session( io_service, socket, endpoint, [this,on_close]( const boost::asio::ip::udp::endpoint &endpoint ) {
      auto s = sessions.find( endpoint );
      if( s != sessions.end() ) {
        session_bindings.erase( s->second->get_self_config().connection_identifier );
        sessions.erase( s );
        on_close();
      }
    } ) );
    sess->connect( [this,cb,endpoint]( bool status, uint32_t identifier ) {
      session_bindings.insert( std::make_pair( identifier, endpoint ) );
      cb( status, identifier );
    } );
  }
  void disconnect( uint32_t session_id ) {
    auto endpoint = session_bindings.find( session_id );
    if( endpoint != session_bindings.end() ) {
      auto sess = sessions.find( endpoint->second );
      if( sess != sessions.end() && sess->second )
        sess->second->disconnect();
    }
  }
  template< typename Iterator >
  void send(
    uint32_t session_id,
    Iterator begin, Iterator end,
    const std::function< void( bool ) > &cb
  ) {
    auto endpoint = session_bindings.find( session_id );
    if( endpoint != session_bindings.end() ) {
      auto sess = sessions.find( endpoint->second );
      if( sess != sessions.end() && sess->second )
        sess->second->send( begin, end, cb );
      else cb( false );
    }
    else cb( false );
  }
private:
  void receive() {
    buffer_ptr_t data( new buffer_t( buffer_size ) );
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
	         auto &sess = sessions[ *from ];
	         if( !sess && is_syn( *data ) ) {
            sess.reset( new session( io_service, socket, *from, [this]( const boost::asio::ip::udp::endpoint &endpoint ) {
              auto s = sessions.find( endpoint );
              if( s != sessions.end() ) {
                session_bindings.erase( s->second->get_self_config().connection_identifier );
                sessions.erase( s );
              }
            } ) );
            session_bindings.insert( std::make_pair( sess->get_self_config().connection_identifier, *from ) );
          }
          if( sess ) {
            buffers_ptr_t received( new buffers_t() );
	           buffers_t response;
	           auto received_iter = std::back_inserter( *received );
            try {
	             sess->receive( data, received_iter );
	             if( !received->empty() )
                cb( *this, sess->get_self_config().connection_identifier, received );
              if( is_rst( *data ) ) {
                auto s = sessions.find( *from );
                if( s != sessions.end() ) {
                  session_bindings.erase( s->second->get_self_config().connection_identifier );
                  sessions.erase( s );
                }
              }
            } catch( const invalid_packet& ) {
              std::cout << "invalid packet" << std::endl;
            }
          }
        }
      }
    );
  }
  void send(
    const std::shared_ptr< boost::asio::ip::udp::endpoint > &from,  const buffers_ptr_t &data,
    const std::function< void( bool ) > &cb
  ) {
    std::reverse( data->begin(), data->end() );
    send_internal( from, data, cb );
  }
  void send_internal(
    const std::shared_ptr< boost::asio::ip::udp::endpoint > &from,  const buffers_ptr_t &data,
    const std::function< void( bool ) > &cb
  ) {
    socket.async_send_to( boost::asio::buffer( data->back()->data(), data->back()->size() ),
      *from,
      [this,from,data,cb]( const boost::system::error_code &error, size_t ) {
        if( !error ) {
	         data->pop_back();
	         if( data->empty() ) cb( true );
	         else send_internal( from, data, cb );
	       }
	       else cb( false );
      }
    );
  }
  boost::asio::io_service &io_service;
  uint16_t port;
  boost::asio::ip::udp::socket socket;
  std::function< void( rudp_server&, uint32_t, const buffers_ptr_t& ) > cb;
  std::map< boost::asio::ip::udp::endpoint, std::shared_ptr< session > > sessions;
  std::map< uint32_t, boost::asio::ip::udp::endpoint > session_bindings;
};

}

#endif
