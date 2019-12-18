/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Streambuffer Type Definitions & Structures
 * - This header describes the base streambuffer-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#include <ddk/streambuffer.h>
#include <limits.h>
#include <internal/_syscalls.h>
#include <internal/_utils.h>
#include <os/futex.h>
#include <stdlib.h>
#include <string.h>

#define STREAMBUFFER_CAN_OVERWRITE(stream) (stream->options & STREAMBUFFER_OVERWRITE_ENABLED)

#define STREAMBUFFER_PARTIAL_OP(options)  (options & STREAMBUFFER_ALLOW_PARTIAL)
#define STREAMBUFFER_PARTIAL_OP_SAFE(options, bytes_available)  (STREAMBUFFER_PARTIAL_OP(options) && bytes_available != 0)

#define STREAMBUFFER_CAN_READ(options, bytes_available, length)           (bytes_available == length || STREAMBUFFER_PARTIAL_OP_SAFE(options, bytes_available))
#define STREAMBUFFER_CAN_WRITE(options, bytes_available, length)          (bytes_available == length || STREAMBUFFER_PARTIAL_OP(options))
#define STREAMBUFFER_CAN_STREAM(stream, options, bytes_available, length) (bytes_available == length || STREAMBUFFER_CAN_OVERWRITE(stream) || STREAMBUFFER_PARTIAL_OP_SAFE(options, bytes_available))

#define STREAMBUFFER_CAN_BLOCK(options)           ((options & STREAMBUFFER_NO_BLOCK) == 0)
#define STREAMBUFFER_HAS_MULTIPLE_READERS(stream) (stream->options & STREAMBUFFER_MULTIPLE_READERS)
#define STREAMBUFFER_HAS_MULTIPLE_WRITERS(stream) (stream->options & STREAMBUFFER_MULTIPLE_WRITERS)
#define STREAMBUFFER_WAIT_FLAGS(stream)           ((stream->options & STREAMBUFFER_GLOBAL) ? 0 : FUTEX_WAIT_PRIVATE)
#define STREAMBUFFER_WAKE_FLAGS(stream)           ((stream->options & STREAMBUFFER_GLOBAL) ? 0 : FUTEX_WAKE_PRIVATE)

typedef struct sb_packethdr {
    size_t packet_len;
} sb_packethdr_t;

void
streambuffer_construct(
    _In_ streambuffer_t* stream,
    _In_ size_t          capacity,
    _In_ unsigned int    options)
{
    memset(stream, 0, sizeof(streambuffer_t));
    stream->capacity = capacity;
    stream->options  = options;
}

OsStatus_t
streambuffer_create(
    _In_  size_t           capacity,
    _In_  unsigned int     options,
    _Out_ streambuffer_t** stream_out)
{
    // When calculating the number of bytes we want to actual structure size
    // without the buffer[1] and then capacity
    streambuffer_t* stream = (streambuffer_t*)malloc((sizeof(streambuffer_t) - 1) + capacity);
    if (!stream) {
        return OsOutOfMemory;
    }
    
    streambuffer_construct(stream, capacity, options);
    *stream_out = stream;
    return OsSuccess;
}

void
streambuffer_set_option(
    _In_ streambuffer_t* stream,
    _In_ unsigned int    option)
{
    stream->options |= option;
}

void
streambuffer_clear_option(
    _In_ streambuffer_t* stream,
    _In_ unsigned int    option)
{
    stream->options &= ~(option);
}

static inline size_t
bytes_writable(
    _In_ size_t capacity,
    _In_ size_t read_index,
    _In_ size_t write_index)
{
    // Handle wrap-around
    if (read_index > write_index) {
        if (read_index >= (UINT_MAX - capacity)) {
            return (read_index & (capacity - 1)) - (write_index & (capacity - 1));
        }
        else {
            return 0; // Overcommitted
        }
    }
    return capacity - (write_index - read_index);
}

static inline size_t
bytes_readable(
    _In_ size_t capacity,
    _In_ size_t read_index,
    _In_ size_t write_index)
{
    // Handle wrap-around
    if (read_index > write_index) {
        if (read_index >= (UINT_MAX - capacity)) {
            return (capacity - (read_index & (capacity- 1))) + 
                (write_index & (capacity - 1)) - 1;
        }
        else {
            return 0; // Overcommitted
        }
    }
    return write_index - read_index;
}

static void
streambuffer_try_truncate(
    _In_ streambuffer_t* stream,
    _In_ unsigned int    options,
    _In_ size_t          length)
{
    // when we check, we must check how many bytes are actually allocated, not currently comitted
    // as we have to take into account current readers. The write index however
    // we have to only take into account how many bytes are actually comitted
    unsigned int write_index     = atomic_load(&stream->producer_comitted_index);
    unsigned int read_index      = atomic_load(&stream->consumer_index);
    size_t       bytes_available = MIN(
        bytes_readable(stream->capacity, read_index, write_index), 
        length);
    size_t       bytes_comitted  = bytes_available;
    if (!STREAMBUFFER_CAN_READ(options, bytes_available, length)) {
        // should not happen but abort if this occurs
        return;
    }

    // Perform the actual allocation, if this fails someone else is truncating
    // or reading, abort
    if (!atomic_compare_exchange_strong(&stream->consumer_index, 
            &read_index, read_index + bytes_available)) {
        return;
    }
    
    // Synchronize with other consumers, we must wait for our turn to increament
    // the comitted index, otherwise we could end up telling writers that the wrong
    // index is writable. This can be skipped for single reader
    if (STREAMBUFFER_HAS_MULTIPLE_READERS(stream)) {
        unsigned int current_commit = atomic_load(&stream->consumer_comitted_index);
        while (current_commit < (read_index - bytes_comitted)) {
            current_commit = atomic_load(&stream->consumer_comitted_index);
        }
    }
    atomic_fetch_add(&stream->consumer_comitted_index, bytes_comitted);
}

size_t
streambuffer_stream_out(
    _In_ streambuffer_t* stream,
    _In_ void*           buffer,
    _In_ size_t          length,
    _In_ unsigned int    options)
{
    const uint8_t*    casted_ptr    = (const uint8_t*)buffer;
    size_t            bytes_written = 0;
    FutexParameters_t parameters;

    // Make sure we write all the bytes
    while (bytes_written < length) {
        // when we check, we must check how many bytes are actually allocated, not committed
        // as we have to take into account current writers. The read index however
        // we have to only take into account how many bytes are actually read
        unsigned int write_index     = atomic_load(&stream->producer_index);
        unsigned int read_index      = atomic_load(&stream->consumer_comitted_index);
        size_t       bytes_available = MIN(
            bytes_writable(stream->capacity, read_index, write_index),
            length - bytes_written);
        size_t       bytes_comitted  = bytes_available;
        if (!STREAMBUFFER_CAN_STREAM(stream, options, bytes_available, (length - bytes_written))) {
            if (!STREAMBUFFER_CAN_BLOCK(options)) {
                break;
            }
            
            parameters._futex0  = (atomic_int*)&stream->consumer_comitted_index;
            parameters._val0    = (int)read_index;
            parameters._timeout = 0;
            parameters._flags   = STREAMBUFFER_WAIT_FLAGS(stream);
            atomic_fetch_add(&stream->producer_count, 1);
            Syscall_FutexWait(&parameters);
            continue; // Start over
        }
        
        // Handle overwrite, empty the queue by an the needed amount of bytes
        if (bytes_available < (length - bytes_written)) {
            streambuffer_try_truncate(stream, options, (length - bytes_written));
            continue;
        }
        
        // Perform the actual allocation
        if (!atomic_compare_exchange_strong(&stream->producer_index, 
                &write_index, write_index + bytes_available)) {
            continue;
        }

        // Write the data to the internal buffer
        while (bytes_available--) {
            stream->buffer[(write_index++ & (stream->capacity - 1))] = casted_ptr[bytes_written++];
        }
        
        // Synchronize with other producers, we must wait for our turn to increament
        // the comitted index, otherwise we could end up telling readers that the wrong
        // index is readable. This can be skipped for single writer
        if (STREAMBUFFER_HAS_MULTIPLE_WRITERS(stream)) {
            unsigned int current_commit = atomic_load(&stream->producer_comitted_index);
            while (current_commit < (write_index - bytes_comitted)) {
                current_commit = atomic_load(&stream->producer_comitted_index);
            }
        }

        atomic_fetch_add(&stream->producer_comitted_index, bytes_comitted);
        parameters._val0 = atomic_exchange(&stream->consumer_count, 0);
        if (parameters._val0 != 0) {
            parameters._futex0 = (atomic_int*)&stream->producer_comitted_index;
            parameters._flags  = STREAMBUFFER_WAKE_FLAGS(stream);
            Syscall_FutexWake(&parameters);
        }
    }
    return bytes_written;
}

size_t
streambuffer_write_packet_start(
    _In_  streambuffer_t* stream,
    _In_  size_t          length,
    _In_  unsigned int    options,
    _Out_ unsigned int*   base_out,
    _Out_ unsigned int*   state_out)
{
    size_t            bytes_allocated = 0;
    FutexParameters_t parameters;
    sb_packethdr_t    header = { .packet_len = length };
    
    length += sizeof(sb_packethdr_t);
    
    // Make sure we write all the bytes in one go
    while (!bytes_allocated) {
        // when we check, we must check how many bytes are actually allocated, not committed
        // as we have to take into account current writers. The read index however
        // we have to only take into account how many bytes are actually read
        unsigned int write_index     = atomic_load(&stream->producer_index);
        unsigned int read_index      = atomic_load(&stream->consumer_comitted_index);
        size_t       bytes_available = MIN(
            bytes_writable(stream->capacity, read_index, write_index),
            length);
        
        if (bytes_available < length) {
            if (!STREAMBUFFER_CAN_BLOCK(options)) {
                break;
            }
            
            parameters._futex0  = (atomic_int*)&stream->consumer_comitted_index;
            parameters._val0    = (int)read_index;
            parameters._timeout = 0;
            parameters._flags   = STREAMBUFFER_WAIT_FLAGS(stream);
            atomic_fetch_add(&stream->producer_count, 1);
            Syscall_FutexWait(&parameters);
            continue; // Start over
        }
        
        // Perform the actual allocation
        if (!atomic_compare_exchange_strong(&stream->producer_index, 
                &write_index, write_index + bytes_available)) {
            continue;
        }
        
        // Initialize the header
        streambuffer_write_packet_data(stream, &header, sizeof(sb_packethdr_t), &write_index);
        
        *base_out       = write_index;
        *state_out      = write_index;
        bytes_allocated = bytes_available;
    }
    return bytes_allocated;
}

void
streambuffer_write_packet_data(
    _In_  streambuffer_t* stream,
    _In_  void*           buffer,
    _In_  size_t          length,
    _Out_ unsigned int*   state)
{
    uint8_t*     casted_ptr    = (uint8_t*)buffer;
    unsigned int write_index   = *state;
    size_t       bytes_written = 0;
    
    while (length--) {
        stream->buffer[(write_index++ & (stream->capacity - 1))] = casted_ptr[bytes_written++];
    }
    
    *state = write_index;
}

void
streambuffer_write_packet_end(
    _In_ streambuffer_t* stream,
    _In_ unsigned int    base,
    _In_ size_t          length)
{
    FutexParameters_t parameters;
    
    // Synchronize with other producers, we must wait for our turn to increament
    // the comitted index, otherwise we could end up telling readers that the wrong
    // index is readable. This can be skipped for single writer
    if (STREAMBUFFER_HAS_MULTIPLE_WRITERS(stream)) {
        unsigned int write_index    = base - sizeof(sb_packethdr_t);
        unsigned int current_commit = atomic_load(&stream->producer_comitted_index);
        while (current_commit < write_index) {
            current_commit = atomic_load(&stream->producer_comitted_index);
        }
    }

    length += sizeof(sb_packethdr_t);
    atomic_fetch_add(&stream->producer_comitted_index, length);
    parameters._val0 = atomic_exchange(&stream->consumer_count, 0);
    if (parameters._val0 != 0) {
        parameters._futex0 = (atomic_int*)&stream->producer_comitted_index;
        parameters._flags  = STREAMBUFFER_WAKE_FLAGS(stream);
        Syscall_FutexWake(&parameters);
    }
}

size_t
streambuffer_stream_in(
    _In_ streambuffer_t* stream,
    _In_ void*           buffer,
    _In_ size_t          length,
    _In_  unsigned int   options)
{
    uint8_t*          casted_ptr = (uint8_t*)buffer;
    size_t            bytes_read = 0;
    FutexParameters_t parameters;
    
    // Make sure there are bytes to read
    while (bytes_read < length) {
        // when we check, we must check how many bytes are actually allocated, not currently comitted
        // as we have to take into account current readers. The write index however
        // we have to only take into account how many bytes are actually comitted
        unsigned int write_index     = atomic_load(&stream->producer_comitted_index);
        unsigned int read_index      = atomic_load(&stream->consumer_index);
        size_t       bytes_available = MIN(
            bytes_readable(stream->capacity, read_index, write_index), 
            length - bytes_read);
        size_t       bytes_comitted  = bytes_available;
        if (!STREAMBUFFER_CAN_READ(options, bytes_available, (length - bytes_read))) {
            if (!STREAMBUFFER_CAN_BLOCK(options)) {
                break;
            }
            
            parameters._futex0  = (atomic_int*)&stream->producer_comitted_index;
            parameters._val0    = (int)write_index;
            parameters._timeout = 0;
            parameters._flags   = STREAMBUFFER_WAIT_FLAGS(stream);
            atomic_fetch_add(&stream->consumer_count, 1);
            Syscall_FutexWait(&parameters);
            continue; // Start over
        }

        // Perform the actual allocation
        if (!atomic_compare_exchange_strong(&stream->consumer_index, 
                &read_index, read_index + bytes_available)) {
            continue;
        }
        
        // Write the data to the provided buffer
        while (bytes_available--) {
            casted_ptr[bytes_read++] = stream->buffer[(read_index++ & (stream->capacity - 1))];
        }
        
        // Synchronize with other consumers, we must wait for our turn to increament
        // the comitted index, otherwise we could end up telling writers that the wrong
        // index is writable. This can be skipped for single reader
        if (STREAMBUFFER_HAS_MULTIPLE_READERS(stream)) {
            unsigned int current_commit = atomic_load(&stream->consumer_comitted_index);
            while (current_commit < (read_index - bytes_comitted)) {
                current_commit = atomic_load(&stream->consumer_comitted_index);
            }
        }

        atomic_fetch_add(&stream->consumer_comitted_index, bytes_comitted);
        parameters._val0 = atomic_exchange(&stream->producer_count, 0);
        if (parameters._val0 != 0) {
            parameters._futex0 = (atomic_int*)&stream->consumer_comitted_index;
            parameters._flags  = STREAMBUFFER_WAKE_FLAGS(stream);
            Syscall_FutexWake(&parameters);
        }
        break;
    }
    return bytes_read;
}

size_t
streambuffer_read_packet_start(
    _In_  streambuffer_t* stream,
    _In_  unsigned int    options,
    _Out_ unsigned int*   base_out,
    _Out_ unsigned int*   state_out)
{
    size_t            bytes_read = 0;
    sb_packethdr_t    header;
    FutexParameters_t parameters;
    
    // Make sure there are bytes to read
    while (!bytes_read) {
        // when we check, we must check how many bytes are actually allocated, not currently comitted
        // as we have to take into account current readers. The write index however
        // we have to only take into account how many bytes are actually comitted
        unsigned int write_index     = atomic_load(&stream->producer_comitted_index);
        unsigned int read_index      = atomic_load(&stream->consumer_index);
        size_t       bytes_available = bytes_readable(stream->capacity, read_index, write_index);
        size_t       length          = sizeof(sb_packethdr_t);
        
        // Validate that it is indeed a header we are looking at, and then readjust
        // the number of bytes available
        if (bytes_available) {
            unsigned int temp_read_index = read_index;
            streambuffer_read_packet_data(stream, &header, sizeof(sb_packethdr_t), &temp_read_index);
            length = header.packet_len + sizeof(sb_packethdr_t);
        }
        bytes_available = MIN(bytes_available, length);
        
        if (bytes_available < length) {
            if (!STREAMBUFFER_CAN_BLOCK(options)) {
                break;
            }
            
            parameters._futex0  = (atomic_int*)&stream->producer_comitted_index;
            parameters._val0    = (int)write_index;
            parameters._timeout = 0;
            parameters._flags   = STREAMBUFFER_WAIT_FLAGS(stream);
            atomic_fetch_add(&stream->consumer_count, 1);
            Syscall_FutexWait(&parameters);
            continue; // Start over
        }
        
        // Perform the actual allocation if PEEK was not specified.
        if (!(options & STREAMBUFFER_PEEK)) {
            if (!atomic_compare_exchange_strong(&stream->consumer_index, 
                    &read_index, read_index + bytes_available)) {
                continue;
            }
        }
        
        read_index += sizeof(sb_packethdr_t);
        *base_out = read_index;
        *state_out = read_index;
        bytes_read = bytes_available - sizeof(sb_packethdr_t);
    }
    return bytes_read;
}

void
streambuffer_read_packet_data(
    _In_    streambuffer_t* stream,
    _In_    void*           buffer,
    _In_    size_t          length,
    _InOut_ unsigned int*   state)
{
    uint8_t*     casted_ptr = (uint8_t*)buffer;
    unsigned int read_index = *state;
    size_t       bytes_read = 0;
    
    // Write the data to the provided buffer
    while (length--) {
        casted_ptr[bytes_read++] = stream->buffer[(read_index++ & (stream->capacity - 1))];
    }
    
    *state = read_index;
}

void
streambuffer_read_packet_end(
    _In_ streambuffer_t* stream,
    _In_ unsigned int    base,
    _In_ size_t          length)
{
    FutexParameters_t parameters;

    // Synchronize with other consumers, we must wait for our turn to increament
    // the comitted index, otherwise we could end up telling writers that the wrong
    // index is writable. This can be skipped for single reader
    if (STREAMBUFFER_HAS_MULTIPLE_READERS(stream)) {
        unsigned int read_index     = base - sizeof(sb_packethdr_t);
        unsigned int current_commit = atomic_load(&stream->consumer_comitted_index);
        while (current_commit < (read_index - length)) {
            current_commit = atomic_load(&stream->consumer_comitted_index);
        }
    }

    length += sizeof(sb_packethdr_t);
    atomic_fetch_add(&stream->consumer_comitted_index, length);
    parameters._val0 = atomic_exchange(&stream->producer_count, 0);
    if (parameters._val0 != 0) {
        parameters._futex0 = (atomic_int*)&stream->consumer_comitted_index;
        parameters._flags  = STREAMBUFFER_WAKE_FLAGS(stream);
        Syscall_FutexWake(&parameters);
    }
}