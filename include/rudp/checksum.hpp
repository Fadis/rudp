/*
 *  Copyright (C) 2018 Naomasa Matsubayashi
 *  Licensed under MIT license, see file LICENSE in this source tree.
 */
#ifndef RUDP_CHECKSUM_H
#define RUDP_CHECKSUM_H

namespace rudp {
  uint16_t checksum( const uint8_t *begin, const uint8_t *end );
}

#endif

