/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <iterator>
#include <arpa/inet.h>
#include <rudp/checksum.hpp>

namespace rudp {
  uint16_t checksum( const uint8_t *begin, const uint8_t *end ) {
    size_t length = std::distance( begin, end );
    if( length == 0 ) return 0xFFFF;
    const auto wbegin = reinterpret_cast< const uint16_t* >( begin );
    const auto wend = std::next( wbegin, length / 2u );
    auto sum = std::accumulate( wbegin, wend, uint32_t( 0xFFFF ), []( uint32_t sum, uint16_t v ) {
      sum += ntohs( v );
      if( sum > 0xFFFF ) sum -= 0xFFFF;
      return sum;
    } );
    if( length % 2 ) {
      sum += uint16_t( begin[ length - 1 ] ) << 8;
      if( sum > 0xFFFF ) sum -= 0xFFFF;
    }
    sum = ~sum;
    return ( uint16_t( sum ) == 0x0000 ) ? 0xFFFF : sum;
  }
}
