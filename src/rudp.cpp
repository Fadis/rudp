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
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <rudp/rudp.hpp>
#include <rudp/checksum.hpp>


namespace rudp {

const uint8_t *from_be16( const uint8_t *begin, uint16_t &value ) {
  value = ( uint16_t( begin[ 0 ] ) << 8 ) | uint16_t( begin[ 1 ] );
  return begin + 2;
}

const uint8_t *from_be32( const uint8_t *begin, uint32_t &value ) {
  value = ( uint32_t( begin[ 0 ] ) << 24 ) | ( uint32_t( begin[ 1 ] ) << 16 ) | ( uint32_t( begin[ 2 ] ) << 8 ) | uint32_t( begin[ 3 ] );
  return begin + 4;
}

uint8_t *to_be16( uint8_t *begin, uint16_t value ) {
  begin[ 0 ] = uint8_t( value >> 8 );
  begin[ 1 ] = uint8_t( value );
  return begin + 2;
}

uint8_t *to_be32( uint8_t *begin, uint32_t value ) {
  begin[ 0 ] = uint8_t( value >> 24 );
  begin[ 1 ] = uint8_t( value >> 16 );
  begin[ 2 ] = uint8_t( value >> 8 );
  begin[ 3 ] = uint8_t( value );
  return begin + 4;
}

session_config::session_config() :
  max_out_of_standing_segs( 64 ),
  option_flags( 0x02 ),
  maximum_segment_size( 1350 ),
  retransmission_timeout_value( 1000 ),
  cumulative_ack_timeout_value( 500 ),
  null_segment_timeout_value( 1000 ),
  transfer_state_timeout_value( 1000 ),
  max_retrans( 3 ),
  max_cum_ack( 32 ),
  max_out_of_seq( 32 ),
  max_auto_reset( 0 ),
  connection_identifier( rand() ) {}
session_config::session_config( const uint8_t *begin, const uint8_t *end ) {
  if( std::distance( begin, end ) != 22 ) throw unable_to_deserialize_session_config();
  auto iter = begin;
  if( *( iter++ ) != 0x10 ) throw invalid_packet();
  max_out_of_standing_segs = *( iter++ );
  option_flags = *( iter++ );
  ++iter;
  iter = from_be16( iter, maximum_segment_size );
  iter = from_be16( iter, retransmission_timeout_value );
  iter = from_be16( iter, cumulative_ack_timeout_value );
  iter = from_be16( iter, null_segment_timeout_value );
  iter = from_be16( iter, transfer_state_timeout_value );
  max_retrans = *( iter++ );
  max_cum_ack = *( iter++ );
  max_out_of_seq = *( iter++ );
  max_auto_reset = *( iter++ );
  iter = from_be32( iter, connection_identifier );
}
void session_config::dump( uint8_t *begin, uint8_t *end ) {
  if( std::distance( begin, end ) != 22 ) throw unable_to_serialize_session_config();
  auto iter = begin;
  *( iter++ ) = 0x10;
  *( iter++ ) = max_out_of_standing_segs;
  *( iter++ ) = option_flags;
  *( iter++ ) = 0x00;
  iter = to_be16( iter, maximum_segment_size );
  iter = to_be16( iter, retransmission_timeout_value );
  iter = to_be16( iter, cumulative_ack_timeout_value );
  iter = to_be16( iter, null_segment_timeout_value );
  iter = to_be16( iter, transfer_state_timeout_value );
  *( iter++ ) = max_retrans;
  *( iter++ ) = max_cum_ack;
  *( iter++ ) = max_out_of_seq;
  *( iter++ ) = max_auto_reset;
  iter = to_be32( iter, connection_identifier );
}

session_config &operator&=( session_config &l, const session_config &r ) {
  l.retransmission_timeout_value = std::min( l.retransmission_timeout_value, r.retransmission_timeout_value );
  l.cumulative_ack_timeout_value = std::min( l.cumulative_ack_timeout_value, r.cumulative_ack_timeout_value );
  l.null_segment_timeout_value = std::min( l.null_segment_timeout_value, r.null_segment_timeout_value );
  l.transfer_state_timeout_value = std::min( l.transfer_state_timeout_value, r.transfer_state_timeout_value );
  l.max_retrans = std::min( l.max_retrans, r.max_retrans );
  l.max_cum_ack = std::min( l.max_cum_ack, r.max_cum_ack );
  l.max_out_of_seq = std::min( l.max_out_of_seq, r.max_out_of_seq );
  l.max_auto_reset = std::min( l.max_auto_reset, r.max_auto_reset );
  return l;
}

 session::session(
    boost::asio::io_service &ios,
    boost::asio::ip::udp::socket &sock,
    const boost::asio::ip::udp::endpoint &endpoint_,
    const std::function< void( const boost::asio::ip::udp::endpoint& ) > &on_closed_
  ) : io_service( ios ), socket( sock ), endpoint( endpoint_ ), receive_head( 0 ), send_head( 0 ), acknowledge_head( 0 ), cumulative_ack_count( 0 ), state( session_state::initial ), client( false ), on_closed( on_closed_ ) {
    std::fill( receive_buffer.begin(), receive_buffer.end(), buffer_ptr_t() );
    std::for_each( send_buffer.begin(), send_buffer.end(), []    ( auto &v ) { v.first = buffer_ptr_t(); } );
    std::for_each( retransmission_timer.begin(), retransmission_timer.end(), []( auto &v ) { v.second = 0u; } );
  }
  void session::connect( const std::function< void( bool, uint32_t ) > &cb ) {
    client = true;
    send( generate_syn( false ), false, [this,this_=shared_from_this(),cb]( bool status ) {
      cb( status, self_config.connection_identifier );
    } );
  }
  void session::receive( const buffer_ptr_t &incoming, outiter_t &received ) {
    if( !incoming ) return;
    const bool syn = (*incoming)[ 0 ] & 0x80;
    const bool ack = (*incoming)[ 0 ] & 0x40;
    const bool eak = (*incoming)[ 0 ] & 0x20;
    const bool nul = (*incoming)[ 0 ] & 0x08;
    const bool chk = (*incoming)[ 0 ] & 0x04;
    const bool tcs = (*incoming)[ 0 ] & 0x02;
    if( !check_common_header( incoming ) ) throw invalid_packet();
    const auto header_size = (*incoming)[ 1 ];
    if( header_size > incoming->size() ) throw invalid_packet();
    if( header_size < 4 ) throw invalid_packet();
    uint16_t expected_sum;
    from_be16( std::next( incoming->data(), header_size - 2 ), expected_sum );
    to_be16( std::next( incoming->data(), header_size - 2 ), 0 );
    const auto sum = rudp::checksum( incoming->data(), std::next( incoming->data(), chk ? incoming->size() : header_size ) );
    if( expected_sum != sum ) throw invalid_packet();
    const uint8_t sequence_number = (*incoming)[ 2 ];
    if( ack && !is_valid_sequence_number( (*incoming)[ 3 ] ) ) throw invalid_packet();
    bool has_data = header_size != incoming->size();
    if( syn ) {
      if( !client && state != session_state::initial ) throw invalid_packet();
      std::fill( receive_buffer.begin(), receive_buffer.end(), buffer_ptr_t() );
      receive_head = sequence_number;
      remote_config = session_config( std::next( incoming->data(), 4 ), std::next( incoming->data(), header_size - 2 ) );
      self_config &= remote_config;
      remote_config &= self_config;
      state = session_state::opened;
    }
    if( tcs ) {
      const uint8_t adj = (*incoming)[ 4 ];
      if( adj ) {
        receive_head = sequence_number;
        auto copied = std::move( receive_buffer );
        std::fill( receive_buffer.begin(), receive_buffer.end(), buffer_ptr_t() );
        for( size_t i = 0u; i != 256u; ++i )
          receive_buffer[ ( i + adj ) & 0xFF ] = copied[ i ];
      }
    }
    if( receive_buffer[ sequence_number ] ) return; // duplicated
    receive_buffer[ sequence_number ] = incoming;
    update_receive_head( received );
    std::vector< send_cb_t > cbs;
    if( ack ) cbs = update_ack( (*incoming)[ 3 ] );
    if( syn && !ack ) send( generate_syn( true ), false, []( bool ){} );
    if( syn && ack ) send( generate_ack(), false, []( bool ){} );
    if( has_data || tcs ) increment_cumulative_ack_counter();
    if( eak && header_size > 6 ) {
      auto cbs_ = update_eak( std::next( incoming->begin(), 4 ), std::next( incoming->begin(), header_size - 2 ) );
      cbs.insert( cbs.end(), cbs_.begin(), cbs_.end() );
      resend( acknowledge_head, *std::next( incoming->begin(), header_size - 3 ) );
    }
    if( nul ) send( generate_ack(), false, []( bool ){} );
    if( out_of_sequence_count >= self_config.max_out_of_seq ) {
      send( generate_eak(), false, []( bool ) {} );
    }
    while( ready_to_send() && !pending.empty() ) {
      send( pending.front().first, true, pending.front().second );
      pending.pop();
    }
    for( auto &cb: cbs ) {
      cb( true );
    }
  }
  void session::disconnect() {
    send( generate_rst(), false, [this,this_=shared_from_this()]( bool ) {
      close();
    } );
  }
  void session::send( const buffer_ptr_t &incoming, bool resend, const send_cb_t &cb ) {
    if( !incoming ) {
      cb( false );
      return;
    }
    const bool syn = (*incoming)[ 0 ] & 0x80;
    const bool rst = (*incoming)[ 0 ] & 0x10;
    const bool nul = (*incoming)[ 0 ] & 0x08;
    const bool tcs = (*incoming)[ 0 ] & 0x02;
    std::vector< send_cb_t > cbs;
    if( !resend && state != session_state::opened && !syn && !tcs ) {
      cb( false );
      return;
    }
    if( !ready_to_send() ) {
      pending.emplace( incoming, cb );
      return;
    }
    const bool ack = (*incoming)[ 0 ] & 0x40;
    if( !check_common_header( incoming ) ) throw invalid_packet();
    const auto header_size = (*incoming)[ 1 ];
    if( header_size > incoming->size() ) throw invalid_packet();
    if( header_size < 4 ) throw invalid_packet();
    bool has_data = header_size != incoming->size();
    if( syn ) {
      std::for_each( send_buffer.begin(), send_buffer.end(), [&]( auto &v ) {
        if( v.first ) cbs.push_back( v.second );
        v.first = buffer_ptr_t();
      } );
      send_head = rand();
      acknowledge_head = send_head;
      self_config = session_config( std::next( incoming->data(), 4 ), std::next( incoming->data(), header_size - 2 ) );
      state = session_state::opened;
    }
    if( rst ) {
      state = session_state::closed;
    }
    const auto sequence_number = send_head++;
    if( header_size != incoming->size() ) (*incoming)[ 0 ] = (*incoming)[ 0 ] | 0x04;
    else (*incoming)[ 0 ] = (*incoming)[ 0 ] & 0xFB;
    (*incoming)[ 2 ] = sequence_number;
    if( ack )  (*incoming)[ 3 ] = receive_head - 1;
    send_buffer[ sequence_number ] = std::make_pair( incoming, cb );
    to_be16( std::next( incoming->data(), header_size - 2 ), 0 );
    const auto sum = rudp::checksum( incoming->data(), std::next( incoming->data(), incoming->size() ) );
    to_be16( std::next( incoming->data(), header_size - 2 ), sum );
    send_packet( incoming, cb );
    ++unacknowledged_packet_count;
    reset_cumulative_ack_counter();
    set_null_segment_timer();
    if( has_data || nul || rst ) {
      //clear_retransmission_timer( sequence_number, sequence_number + 1u );
      set_retransmission_timer( sequence_number );
    }
    for( auto &cb: cbs ) {
      cb( false );
    }
  }
  void session::resend( uint8_t begin, uint8_t end ) {
    for( uint8_t seq = begin; seq != end; ++seq ) {
      if( send_buffer[ seq ].first ) {
        send( send_buffer[ seq ].first, false, send_buffer[ seq ].second );
      }
    }
  }
  void session::increment_cumulative_ack_counter() {
    ++cumulative_ack_count;
    if( cumulative_ack_count == 1 ) {
      cumulative_ack_timer.reset( new boost::asio::steady_timer( io_service ) );
      cumulative_ack_timer->expires_from_now( std::chrono::milliseconds( self_config.cumulative_ack_timeout_value ) );
      cumulative_ack_timer->async_wait(
        [this,this_=shared_from_this()]( const boost::system::error_code &error ) {
	         if( !error ) {
            if( cumulative_ack_timer ) {
              cumulative_ack_timer->cancel();
              cumulative_ack_timer.reset();
	           }
            cumulative_ack_count = 0;
	           send( generate_ack(), false, []( bool ){} );
	         }
	       }
      );
    }
    else if( cumulative_ack_count > self_config.max_cum_ack ) {
      if( cumulative_ack_timer ) {
        cumulative_ack_timer->cancel();
        cumulative_ack_timer.reset();
      }
      cumulative_ack_count = 0;
      send( generate_ack(), false, []( bool ){} );
    }
  }
  void session::reset_cumulative_ack_counter() {
    if( cumulative_ack_timer ) {
      cumulative_ack_timer->cancel();
      cumulative_ack_timer.reset();
    }
    cumulative_ack_count = 0;
  }
  void session::set_null_segment_timer() {
    if( null_segment_timer ) {
      null_segment_timer->cancel();
      null_segment_timer.reset();
    }
    null_segment_timer.reset( new boost::asio::steady_timer( io_service ) );
    null_segment_timer->expires_from_now( std::chrono::milliseconds( self_config.null_segment_timeout_value * ( client ? 1 : 2 ) ) );
    null_segment_timer->async_wait(
      [this,this_=shared_from_this(),null_segment_timer_=null_segment_timer]( const boost::system::error_code &error ) {
        if( !error ) {
          if( null_segment_timer ) {
            null_segment_timer.reset();
          }
          if( client ) {
            if( state == session_state::opened ) send( generate_nul(), false, []( bool ){} );
          }
          else {
            wait_for_tcs();
          }
        }
      }
    );
  }
  void session::set_retransmission_timer( uint8_t at ) {
    retransmission_timer[ at ].first.reset( new boost::asio::steady_timer( io_service ) );
    retransmission_timer[ at ].first->expires_from_now( std::chrono::milliseconds( self_config.retransmission_timeout_value ) );
    retransmission_timer[ at ].first->async_wait(
      [this,this_=shared_from_this(),retransmission_timer_=retransmission_timer[ at ],at]( const boost::system::error_code &error ) {
        if( !error ) {
          resend( at, at + 1u );
          ++retransmission_timer[ at ].second;
          if( retransmission_timer[ at ].second > self_config.max_retrans ) {
            close();
          }
          else {
            set_retransmission_timer( at );
          }
        }
      }
    );
  }
  void session::clear_retransmission_timer( uint8_t begin, uint8_t end ) {
    for( uint8_t i = begin; i != end; ++i ) {
      if( retransmission_timer[ i ].first ) {
        retransmission_timer[ i ].first->cancel();
        retransmission_timer[ i ].first.reset();
      }
      retransmission_timer[ i ].second = 0;
    }
  }
  void session::cancel_all_timers() {
    if( cumulative_ack_timer ) {
      cumulative_ack_timer->cancel();
      cumulative_ack_timer.reset();
    }
    if( null_segment_timer ) {
      null_segment_timer->cancel();
      null_segment_timer.reset();
    }
    clear_retransmission_timer( 0u, 255u );
  }
  void session::update_receive_head( outiter_t &received ) {
    const auto old_receive_head = receive_head;
    const auto head_iter = std::next( receive_buffer.begin(), receive_head );
    const auto end_iter = std::find( head_iter, receive_buffer.end(), buffer_ptr_t() );
    for( auto iter = head_iter; iter != end_iter; ++iter ) {
      if( iter && *iter && (*iter)->size() > 2 && (*iter)->size() > (**iter)[ 1 ] ) {
        *received = *iter;
	       ++received;
      }
    }
    std::fill( head_iter, end_iter, buffer_ptr_t() );
    receive_head = std::distance( receive_buffer.begin(), end_iter );
    if( end_iter == receive_buffer.end() ) {
      const auto head_iter = receive_buffer.begin();
      const auto end_iter = std::find( head_iter, receive_buffer.end(), buffer_ptr_t() );
      for( auto iter = head_iter; iter != end_iter; ++iter ) {
        if( iter && *iter && (*iter)->size() > 2 && (*iter)->size() > (**iter)[ 1 ] ) {
          *received = *iter;
          ++received;
        }
      }
      std::fill( head_iter, end_iter, buffer_ptr_t() );
      receive_head = std::distance( receive_buffer.begin(), end_iter );
    }
    if( old_receive_head == receive_head ) ++out_of_sequence_count;
    else out_of_sequence_count = 0;
  }
  std::vector< send_cb_t > session::update_ack( uint8_t new_acknowledge_head ) {
    std::vector< send_cb_t > cbs;
    if( new_acknowledge_head == uint8_t( acknowledge_head - 1u ) ) return cbs;
    if( !is_valid_sequence_number( new_acknowledge_head ) ) return cbs;
    ++new_acknowledge_head;
    size_t acknowledged_count = 0u;
    if( acknowledge_head < new_acknowledge_head ) {
      acknowledged_count = std::count_if( std::next( send_buffer.begin(), acknowledge_head ), std::next( send_buffer.begin(), new_acknowledge_head ), []( const auto &v ) -> bool { return static_cast< bool >( v.first ); } );
      std::for_each( std::next( send_buffer.begin(), acknowledge_head ), std::next( send_buffer.begin(), new_acknowledge_head ), [&]( auto &v ) {
        if( v.first ) cbs.push_back( v.second );
        v.first = buffer_ptr_t();
      } );
    }
    else if( acknowledge_head > new_acknowledge_head ) {
      acknowledged_count = std::count_if( std::next( send_buffer.begin(), acknowledge_head ), send_buffer.end(), []( const auto &v ) -> bool { return static_cast< bool >( v.first ); } );
      acknowledged_count += std::count_if( send_buffer.begin(), std::next( send_buffer.begin(), new_acknowledge_head ), []( const auto &v ) -> bool { return static_cast< bool >( v.first ); } );
      std::for_each( std::next( send_buffer.begin(), acknowledge_head ), send_buffer.end(), [&]( auto &v ) {
        if( v.first ) cbs.push_back( v.second );
        v.first = buffer_ptr_t();
      } );
      std::for_each( send_buffer.begin(), std::next( send_buffer.begin(), new_acknowledge_head ), [&]( auto &v ) {
        if( v.first ) cbs.push_back( v.second );
        v.first = buffer_ptr_t();
      } );
    }
    unacknowledged_packet_count -= acknowledged_count;
    clear_retransmission_timer( acknowledge_head, new_acknowledge_head );
    acknowledge_head = new_acknowledge_head;
    return cbs;
  }
  std::vector< send_cb_t > session::update_eak( buffer_t::const_iterator begin, buffer_t::const_iterator end ) {
    std::vector< send_cb_t > cbs;
    for( auto iter = begin; iter != end; ++iter ) {
      if( is_valid_sequence_number( *iter ) && send_buffer[ *iter ].first ) {
        send_buffer[ *iter ].first.reset();
        cbs.push_back( send_buffer[ *iter ].second );
       	unacknowledged_packet_count -= 1;
        clear_retransmission_timer( *iter, *iter + 1u );
      }
    }
    return cbs;
  }
  bool session::check_common_header( const buffer_ptr_t &incoming ) {
    const bool syn = (*incoming)[ 0 ] & 0x80;
    const bool ack = (*incoming)[ 0 ] & 0x40;
    const bool eak = (*incoming)[ 0 ] & 0x20;
    const bool rst = (*incoming)[ 0 ] & 0x10;
    const bool nul = (*incoming)[ 0 ] & 0x08;
    const bool chk = (*incoming)[ 0 ] & 0x04;
    const bool tcs = (*incoming)[ 0 ] & 0x02;
    unsigned int role_count = 0u;
    if( syn ) ++role_count;
    if( eak ) ++role_count;
    if( rst ) ++role_count;
    if( nul ) ++role_count;
    if( tcs ) ++role_count;
    if( ( role_count == 0 ) && ack ) ++role_count;
    if( ( eak || nul ) && !ack ) return false;
    return role_count == 1u;
  }
  buffer_ptr_t session::generate_syn( bool ack ) {
    buffer_ptr_t buffer( new buffer_t( 28 ) );
    (*buffer)[ 0 ] = ack ? 0xC0 : 0x80;
    (*buffer)[ 1 ] = 28;
    self_config.dump( std::next( buffer->data(), 4 ), std::next( buffer->data(), 26 ) );
    return buffer;
  }
  buffer_ptr_t session::generate_ack() {
    buffer_ptr_t buffer( new buffer_t( 6 ) );
    (*buffer)[ 0 ] = 0x40;
    (*buffer)[ 1 ] = 6;
    return buffer;
  }
  buffer_ptr_t session::generate_rst() {
    buffer_ptr_t buffer( new buffer_t( 6 ) );
    (*buffer)[ 0 ] = 0x10;
    (*buffer)[ 1 ] = 6;
    return buffer;
  }
  buffer_ptr_t session::generate_eak() {
    const auto oos_ack_count = get_out_of_sequence_acknowlege_count();
    buffer_ptr_t buffer( new buffer_t( 6 + oos_ack_count ) );
    (*buffer)[ 0 ] = 0x60;
    (*buffer)[ 1 ] = 6 + oos_ack_count;
    auto iter = std::next( buffer->begin(), 4u );
    for( unsigned int i = receive_head; i != 256u; ++i )
      if( receive_buffer[ i ] ) *( iter++ ) = i;
    for( unsigned int i = 0u; i != receive_head; ++i )
      if( receive_buffer[ i ] ) *( iter++ ) = i;
    return buffer;
    out_of_sequence_count = 0;
  }
  buffer_ptr_t session::generate_nul() {
    buffer_ptr_t buffer( new buffer_t( 6 ) );
    (*buffer)[ 0 ] = 0x48;
    (*buffer)[ 1 ] = 6;
    return buffer;
  }
  bool session::ready_to_send() {
    return send_head + 1 != acknowledge_head && unacknowledged_packet_count <= remote_config.max_out_of_standing_segs;
  }
  bool session::is_valid_sequence_number( uint8_t sequence_number ) {
    if( sequence_number == uint8_t( acknowledge_head - 1u ) ) return true;
    if( acknowledge_head < send_head ) return acknowledge_head <= sequence_number && sequence_number < send_head;
    else if( send_head < acknowledge_head )
      return sequence_number < send_head || acknowledge_head <= sequence_number;
    else return false;
  }
  size_t session::get_out_of_sequence_acknowlege_count() {
    std::count_if( receive_buffer.begin(), receive_buffer.end(), []( const auto &v ) -> bool { return static_cast< bool >( v ); } );
  }
  size_t session::get_waiting_for_acknowlege_count() {
    std::count_if( send_buffer.begin(), send_buffer.end(), []( const auto &v ) -> bool { return static_cast< bool >( v.first ); } );
  }
  void session::send_packet(
    const buffer_ptr_t &data,
    const send_cb_t &cb
  ) {
    socket.async_send_to( boost::asio::buffer( data->data(), data->size() ),
      endpoint,
      [this,this_=shared_from_this(),data,cb]( const boost::system::error_code &error, size_t ) {
        if( error ) {
          cb( false );
        }
      }
    );
  }
  void session::send_packets(
    const buffers_ptr_t &data,
    const send_cb_t &cb
  ) {
    std::reverse( data->begin(), data->end() );
    send_packets_internal( data, cb );
  }
  void session::send_packets_internal(
    const buffers_ptr_t &data,
    const send_cb_t &cb
  ) {
    socket.async_send_to( boost::asio::buffer( data->back()->data(), data->back()->size() ),
      endpoint,
      [this,this_=shared_from_this(),data,cb]( const boost::system::error_code &error, size_t ) {
        if( !error ) {
	         data->pop_back();
	         if( data->empty() );
	         else send_packets_internal( data, cb );
	       }
	       else {
          cb( false );
        }
      }
    );
  }
  void session::wait_for_tcs() {
    state = session_state::broken;
    tcs_timer.reset( new boost::asio::steady_timer( io_service ) );
    tcs_timer->expires_from_now( std::chrono::milliseconds( self_config.transfer_state_timeout_value ) );
    tcs_timer->async_wait(
      [this,this_=shared_from_this()]( const boost::system::error_code &error ) {
	       if( !error ) {
          close();
	       }
	     }
    );
  }
  void session::close() {
    state = session_state::opened;
    cancel_all_timers();
    on_closed( endpoint );
  }
/*
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
*/
}

