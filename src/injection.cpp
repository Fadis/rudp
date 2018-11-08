/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#include <algorithm>
#include <cstdint>
#include <array>
#include <utility>
#include <numeric>
#include <iostream>
#include <boost/spirit/include/karma.hpp>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <rudp/checksum.hpp>

constexpr uint16_t port = 0x1f40;

struct pseudo_header_t {
  uint32_t saddr;
  uint32_t daddr;
  uint8_t zero;
  uint8_t protocol;
  uint16_t tot_len;
};

int main() {
  int sock = socket( AF_PACKET, SOCK_RAW, IPPROTO_RAW );
  if (sock < 0) {
    std::cout << "ソケットを開けない" << std::endl;
    return 1;
  }
  ifreq interface;
  memset( &interface, 0, sizeof( interface ) );
  strncpy( interface.ifr_name, "lo", IFNAMSIZ );
  if( ioctl( sock, SIOCGIFINDEX, &interface ) < 0 ) {
    std::cout << "インターフェースが見つからない" << std::endl;
    close( sock );
    return 1;
  }
  sockaddr_ll source_addr;
  memset( &source_addr, 0, sizeof( sockaddr_ll ) );
  source_addr.sll_family = AF_PACKET;
  source_addr.sll_protocol = htons( ETH_P_ALL );
  source_addr.sll_ifindex = interface.ifr_ifindex;
  const auto bind_result = bind( sock, reinterpret_cast< sockaddr* >( &source_addr ), sizeof( sockaddr_ll ) );
  if( bind_result < 0 ) {
    std::cout << strerror( errno ) << std::endl;
    std::cout << "bindできない " << bind_result << std::endl;
    close( sock );
    return 1;
  }
  constexpr size_t ether_header_size = 14;
  constexpr size_t ip_header_size = 20;
  constexpr size_t udp_header_size = 8;
  constexpr size_t rudp_header_size = 6;
  constexpr size_t pseudo_header_size = 12;
  constexpr size_t ether_offset = 0;
  constexpr size_t ip_offset = ether_header_size;
  constexpr size_t udp_offset = ip_offset + ip_header_size;
  constexpr size_t rudp_offset = udp_offset + udp_header_size;
  constexpr size_t total_packet_size = rudp_offset + rudp_header_size;
  constexpr size_t pseudo_ip_offset = 0;
  constexpr size_t pseudo_udp_offset = pseudo_ip_offset + pseudo_header_size;
  constexpr size_t pseudo_rudp_offset = pseudo_udp_offset + udp_header_size;
  constexpr size_t total_pseudo_packet_size = pseudo_rudp_offset + rudp_header_size;
  constexpr size_t ether_payload_size = ip_header_size + udp_header_size + rudp_header_size;
  constexpr size_t ip_payload_size = udp_header_size + rudp_header_size;
  constexpr size_t udp_payload_size = rudp_header_size;
  namespace karma = boost::spirit::karma;
  std::array< uint8_t, total_packet_size > buf;
  std::fill( buf.begin(), buf.end(), 0 );
  std::array< uint8_t, 6 > mac{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  std::copy( mac.begin(), mac.end(), buf.begin() );
  std::copy( mac.begin(), mac.end(), std::next( buf.begin(), 6 ) );
  buf[ 12 ] = 0x08;
  buf[ 13 ] = 0x00;ether_payload_size & 0xFF;
  auto ip_header = reinterpret_cast< iphdr* >( std::next( buf.data(), ip_offset ) );
  ip_header->version = 4;
  ip_header->ihl = 5;
  ip_header->tot_len = htons( ether_payload_size );
  ip_header->ttl = 0x40;
  ip_header->protocol = 0x11;
  ip_header->frag_off = htons( 0x02 << 13 );
  ip_header->saddr = htonl( ( 192 << 24 ) | ( 168 << 16 ) | ( 2 << 8 ) | 2 );
  ip_header->daddr = htonl( ( 192 << 24 ) | ( 168 << 16 ) | ( 2 << 8 ) | 1 );
  uint16_t c0 = rudp::checksum( std::next( buf.begin(), ip_offset ), std::next( buf.begin(), ip_offset + ip_header_size ) );
  ip_header->check = htons( c0 );
  std::array< uint8_t, total_pseudo_packet_size > pseudo;
  std::fill( pseudo.begin(), pseudo.end(), 0 );
  auto pseudo_header = reinterpret_cast< pseudo_header_t* >( std::next( pseudo.data(), pseudo_ip_offset ) );
  pseudo_header->saddr = ip_header->saddr;
  pseudo_header->daddr = ip_header->daddr;
  pseudo_header->protocol = ip_header->protocol;
  pseudo_header->tot_len = htons( udp_header_size + rudp_header_size );
  auto udp_header = reinterpret_cast< udphdr* >( std::next( buf.data(), udp_offset ) );
  udp_header->uh_sport = htons( port );
  udp_header->uh_dport = htons( port );
  udp_header->uh_ulen = htons( udp_header_size + rudp_header_size );
  auto rudp_header = std::next( buf.begin(), rudp_offset );
  for( unsigned int seq = 0; seq != 256u; ++seq ) {
    rudp_header[ 0 ] = 0x50;
    rudp_header[ 1 ] = 0x06;
    rudp_header[ 2 ] = uint8_t( seq );
    rudp_header[ 3 ] = uint8_t( seq - 1 );
    karma::generate( std::next( rudp_header, 4 ), karma::big_word, 0x0000 );
    uint16_t c2 = rudp::checksum( rudp_header, buf.end() );
    karma::generate( std::next( rudp_header, 4 ), karma::big_word, c2 );
    udp_header->uh_sum = 0;
    std::copy( std::next( buf.begin(), udp_offset ), buf.end(), std::next( pseudo.begin(), pseudo_udp_offset ) );
    uint16_t c1 = rudp::checksum( pseudo.begin(), pseudo.end() );
    udp_header->uh_sum = htons( c1 );
    ip_header->id = htons( seq );
    ip_header->check = 0;
    uint16_t c0 = rudp::checksum( std::next( buf.begin(), ip_offset ), std::next( buf.begin(), ip_offset + ip_header_size ) );
    ip_header->check = htons( c0 );
    std::string message;
    if( write( sock, buf.data(), buf.size() ) < 0 ) {
      std::cout << strerror( errno ) << std::endl;
      std::cout << "送信できない " << bind_result << std::endl;
      close( sock );
      return 1;
    }
  }
  close( sock );
  return 1;
}
