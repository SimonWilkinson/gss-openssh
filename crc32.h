/*
 *
 * crc32.h
 *
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 *
 * Copyright (c) 1992 Tatu Ylonen, Espoo, Finland
 *                    All rights reserved
 *
 * Created: Tue Feb 11 14:37:27 1992 ylo
 *
 * Functions for computing 32-bit CRC.
 *
 */

/* RCSID("$OpenBSD: crc32.h,v 1.6 2000/06/20 01:39:40 markus Exp $"); */

#ifndef CRC32_H
#define CRC32_H

/*
 * This computes a 32 bit CRC of the data in the buffer, and returns the CRC.
 * The polynomial used is 0xedb88320.
 */
unsigned int crc32(const unsigned char *buf, unsigned int len);

#endif				/* CRC32_H */
