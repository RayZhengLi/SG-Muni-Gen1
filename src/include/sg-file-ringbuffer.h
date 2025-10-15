/***************************************************************************************
 * @file data_logging.h
 * @brief These are the functions of the Fileringbuffer
 ***************************************************************************************/

#ifndef SG_FILE_RINGBUFFER_H
#define SG_FILE_RINGBUFFER_H

#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

#ifndef RECORD_SIZE
#define RECORD_SIZE     1024
#endif
#define MAGIC_NUMBER    "RBUF"
#define VERSION         1
#define META_DATA_SIZE  ((int)sizeof(MAGIC_NUMBER) + (int)sizeof(int) * 4)

typedef struct {
    FILE *file;
    int head;   // Head of the ring buffer, points to the position to write new record
    int tail;   // Tail of the ring buffer, points to the position to read the oldest record
    int count;  // The current number of records in the buffer
    int max_record_num; // Max number of records

    /* Thread safety addtions */
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    /* Durability options*/
    bool enable_fsync; // If true, fsync after every write
    bool stop_requested;   // Let the blokcing read be able to exit
} FileRingBuffer;

typedef struct __attribute__((packed)) {
    uint16_t len;
    uint16_t type;
} SgRbHdr;

#define SG_RB_HDR_SIZE   ((size_t)sizeof(SgRbHdr))
#define SG_RB_MAX_PAYLOAD (RECORD_SIZE - SG_RB_HDR_SIZE)


/***************************************************************************************
* @brief This is the Fileringbuffer initialiser
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param filename The file to store the Fileringbuffer
* @return -1: The file doesn't exist;
*          0: The Fileringbuffer successfully registered
***************************************************************************************/
int file_ringbuffer_init(FileRingBuffer *buffer, int max_record_num, const char *filename);


/***************************************************************************************
* @brief This is the function to enable or disable fsync
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param on True to enable fsync
* @return None
***************************************************************************************/
void file_ringbuffer_enable_fsync(FileRingBuffer *buffer, bool on);


/***************************************************************************************
* @brief This is the function to close the Fileringbuffer
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return None
***************************************************************************************/
void file_ringbuffer_destroy(FileRingBuffer *buffer);


/***************************************************************************************
* @brief This is the function to exit the blocking read function, it should be called 
*        before destroyiing the buffer
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return None
***************************************************************************************/
void file_ringbuffer_request_stop(FileRingBuffer *buffer);


/***************************************************************************************
* @brief This is the Fileringbuffer write function
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully registered
***************************************************************************************/
int file_ringbuffer_write(FileRingBuffer *buffer, const char *record);


/***************************************************************************************
* @brief This is the Fileringbuffer peek function, it only read but doesn't delete messages
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully peeked;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_peek(FileRingBuffer *buffer, char *record);


/***************************************************************************************
* @brief This is the function to delete one oldest message
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The message is successfully deleted;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_delete(FileRingBuffer *buffer);


/***************************************************************************************
* @brief This is the function to read and then delete one oldest message
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully read;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_read(FileRingBuffer *buffer, char *record);


// blocking ops

/***************************************************************************************
* @brief This is the Fileringbuffer write function (blobking), it stubs when the buffer
*        is full
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully registered;
*        -1: Error happened;
*        -3: The buffer is requested to stopl;
***************************************************************************************/
int  file_ringbuffer_write_blocking(FileRingBuffer *buffer, const char *record);


/***************************************************************************************
* @brief This is the Fileringbuffer write function (blobking) with timeout, it stubs when
*         the buffer is full
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @param timeout_ms The timeout in milliseconds
* @return 0: The Fileringbuffer successfully registered;
*        -1: Error happened;
*        -2: Timeout
*        -3: The buffer is requested to stop;
***************************************************************************************/
int file_ringbuffer_write_timed(FileRingBuffer *buffer, const char *record, int timeout_ms);


/***************************************************************************************
* @brief This is the Fileringbuffer read function (blocking), it stubs when the buffer
*        is empty
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0:  Successfully read The Fileringbuffer
*        -1:  Error happened;
*        -3:  The buffer is requested to stop;
***************************************************************************************/
int  file_ringbuffer_read_blocking(FileRingBuffer *buffer, char *record);


/***************************************************************************************
* @brief This is the Fileringbuffer read function (blocking) with timeout, it stubs when
*        the buffer is empty
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @param timeout_ms The timeout in milliseconds
* @return 0:  Successfully read The Fileringbuffer
*        -1:  Error happened;
*        -2:  Timeout;
*        -3:  The buffer is requested to stop;
***************************************************************************************/
int file_ringbuffer_read_timed(FileRingBuffer *buffer, char *record, int timeout_ms);

// Methods to store structs
// 任意字节序列写入（满则覆盖 / 不覆盖两种都支持）
/***************************************************************************************
* @brief This is the Fileringbuffer write function for arbitrary byte sequence (overwrite mode).
*        It packs the payload and type header into a fixed-size record and writes it to file.
*        When the buffer is full, the oldest record will be overwritten automatically.
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param data   The pointer to the payload data to be written
* @param len    The payload data length in bytes
* @param type   The custom type identifier for this record
* @return 0:  Successfully wrote data into the Fileringbuffer
*        -1:  I/O error occurred
*        -5:  Payload length exceeds SG_RB_MAX_PAYLOAD
***************************************************************************************/
int file_ringbuffer_write_bytes        (FileRingBuffer *buffer, const void *data, size_t len, uint16_t type);


/***************************************************************************************
* @brief This is the Fileringbuffer write function (blocking). It waits when the buffer 
*        is full until there is space or a stop is requested.
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array (fixed RECORD_SIZE) to be written
* @return 0:  Successfully wrote into the Fileringbuffer
*        -1:  Error happened during I/O
*        -3:  The buffer is requested to stop
***************************************************************************************/
int file_ringbuffer_write_bytes_blocking(FileRingBuffer *buffer, const void *data, size_t len, uint16_t type);


/***************************************************************************************
* @brief This is the Fileringbuffer write function (blocking with timeout) for arbitrary
*        byte sequence. It waits when the buffer is full until space becomes available,
*        timeout expires, or stop is requested.
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param data   The pointer to the payload data to be written
* @param len    The payload data length in bytes
* @param type   The custom type identifier for this record
* @param timeout_ms The timeout duration in milliseconds
* @return 0:  Successfully wrote data into the Fileringbuffer
*        -1:  I/O error occurred
*        -2:  Timeout expired before space was available
*        -3:  The Fileringbuffer is requested to stop
*        -5:  Payload length exceeds SG_RB_MAX_PAYLOAD
***************************************************************************************/
int file_ringbuffer_write_bytes_timed  (FileRingBuffer *buffer, const void *data, size_t len, uint16_t type, int timeout_ms);

// 任意字节序列读取（返回0成功；*inout_len 为传入缓冲区大小，返回时写入真实长度）
// 当 *inout_len 小于真实载荷时，返回 -5 并把 *inout_len 设为所需大小，不消费记录（保持在队首）
/***************************************************************************************
* @brief This is the Fileringbuffer peek function for arbitrary byte sequence.
*        It reads and unpacks the next record without consuming it from the buffer.
*        When the provided buffer is too small, the function will not consume the record
*        and will return the required size.
****************************************************************************************
* @param buffer     The Fileringbuffer struct
* @param out        The pointer to the output buffer
* @param inout_len  Input: available buffer size; Output: actual payload length or required size
* @param out_type   The pointer to store the record type identifier
* @return 0:  Successfully peeked one record
*        -1:  The buffer is empty or I/O error
*        -5:  Output buffer too small, record not consumed
***************************************************************************************/
int file_ringbuffer_peek_bytes (FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type);


/***************************************************************************************
* @brief This is the Fileringbuffer read function for arbitrary byte sequence (non-blocking).
*        It reads and unpacks one record from the buffer and removes it from the queue.
*        When the provided buffer is too small, the record will not be consumed and
*        the function returns the required size.
****************************************************************************************
* @param buffer     The Fileringbuffer struct
* @param out        The pointer to the output buffer
* @param inout_len  Input: available buffer size; Output: actual payload length or required size
* @param out_type   The pointer to store the record type identifier
* @return 0:  Successfully read one record
*        -1:  The buffer is empty or I/O error
*        -5:  Output buffer too small, record not consumed
***************************************************************************************/
int file_ringbuffer_read_bytes (FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type);


/***************************************************************************************
* @brief This is the Fileringbuffer read function (blocking) for arbitrary byte sequence.
*        It waits when the buffer is empty until a record is available or stop is requested,
*        then unpacks the record and removes it from the queue.
****************************************************************************************
* @param buffer     The Fileringbuffer struct
* @param out        The pointer to the output buffer
* @param inout_len  Input: available buffer size; Output: actual payload length
* @param out_type   The pointer to store the record type identifier
* @return 0:  Successfully read one record
*        -1:  I/O error occurred
*        -3:  The Fileringbuffer is requested to stop
*        -5:  Output buffer too small, record not consumed
***************************************************************************************/
int file_ringbuffer_read_bytes_blocking(FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type);


/***************************************************************************************
* @brief This is the Fileringbuffer read function (blocking with timeout) for arbitrary
*        byte sequence. It waits when the buffer is empty until a record is available,
*        timeout expires, or stop is requested, then unpacks and removes the record.
****************************************************************************************
* @param buffer     The Fileringbuffer struct
* @param out        The pointer to the output buffer
* @param inout_len  Input: available buffer size; Output: actual payload length
* @param out_type   The pointer to store the record type identifier
* @param timeout_ms The timeout duration in milliseconds
* @return 0:  Successfully read one record
*        -1:  I/O error occurred
*        -2:  Timeout expired before data was available
*        -3:  The Fileringbuffer is requested to stop
*        -5:  Output buffer too small, record not consumed
***************************************************************************************/
int file_ringbuffer_read_bytes_timed  (FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type, int timeout_ms);


// Basic tools

/***************************************************************************************
* @brief This is the function to save the index of the Fileringbuffer file
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return None
***************************************************************************************/
void save_meta_data(FileRingBuffer *buffer);

/***************************************************************************************
* @brief This is the function to load the index of the Fileringbuffer file
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return 0: Successfully loaded the buffer index
         -1: Invalid file
***************************************************************************************/
int load_meta_data(FileRingBuffer *buffer);

#endif