/*
 *
 * buffer.h
 *
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 *
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 *
 * Created: Sat Mar 18 04:12:25 1995 ylo
 *
 * Code for manipulating FIFO buffers.
 *
 */

/* RCSID("$OpenBSD: buffer.h,v 1.5 2000/06/20 01:39:39 markus Exp $"); */

#ifndef BUFFER_H
#define BUFFER_H

typedef struct {
	char   *buf;		/* Buffer for data. */
	unsigned int alloc;	/* Number of bytes allocated for data. */
	unsigned int offset;	/* Offset of first byte containing data. */
	unsigned int end;	/* Offset of last byte containing data. */
}       Buffer;
/* Initializes the buffer structure. */
void    buffer_init(Buffer * buffer);

/* Frees any memory used for the buffer. */
void    buffer_free(Buffer * buffer);

/* Clears any data from the buffer, making it empty.  This does not actually
   zero the memory. */
void    buffer_clear(Buffer * buffer);

/* Appends data to the buffer, expanding it if necessary. */
void    buffer_append(Buffer * buffer, const char *data, unsigned int len);

/*
 * Appends space to the buffer, expanding the buffer if necessary. This does
 * not actually copy the data into the buffer, but instead returns a pointer
 * to the allocated region.
 */
void    buffer_append_space(Buffer * buffer, char **datap, unsigned int len);

/* Returns the number of bytes of data in the buffer. */
unsigned int buffer_len(Buffer * buffer);

/* Gets data from the beginning of the buffer. */
void    buffer_get(Buffer * buffer, char *buf, unsigned int len);

/* Consumes the given number of bytes from the beginning of the buffer. */
void    buffer_consume(Buffer * buffer, unsigned int bytes);

/* Consumes the given number of bytes from the end of the buffer. */
void    buffer_consume_end(Buffer * buffer, unsigned int bytes);

/* Returns a pointer to the first used byte in the buffer. */
char   *buffer_ptr(Buffer * buffer);

/*
 * Dumps the contents of the buffer to stderr in hex.  This intended for
 * debugging purposes only.
 */
void    buffer_dump(Buffer * buffer);

#endif				/* BUFFER_H */
