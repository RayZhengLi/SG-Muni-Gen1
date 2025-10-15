/***************************************************************************************
 * @file data_logging.c
 * @brief Functions of Fileringbuffer lib
 ***************************************************************************************/

#define _POSIX_C_SOURCE 200809L
#include "sg-file-ringbuffer.h"
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

// --- 小工具：把任意payload封装成固定RECORD_SIZE的缓冲 ---
static int pack_record(void *fixed_rec_out /*RECORD_SIZE*/, const void *data, size_t len, uint16_t type) {
    if (len > SG_RB_MAX_PAYLOAD) return -5;       // 太长
    SgRbHdr hdr = { .len = (uint16_t)len, .type = type };
    memcpy((uint8_t*)fixed_rec_out, &hdr, SG_RB_HDR_SIZE);
    if (len) memcpy((uint8_t*)fixed_rec_out + SG_RB_HDR_SIZE, data, len);
    // 其余区域保持未定义/零填均可（不影响读取，因为按len解析）
    return 0;
}

// --- 小工具：把固定RECORD_SIZE的缓冲解包成payload ---
static int unpack_record(const void *fixed_rec_in /*RECORD_SIZE*/, void *out, size_t *inout_len, uint16_t *out_type, bool do_copy) {
    SgRbHdr hdr;
    memcpy(&hdr, fixed_rec_in, SG_RB_HDR_SIZE);
    size_t need = hdr.len;
    if (out_type) *out_type = hdr.type;

    if (*inout_len < need) {
        // 告知所需大小，不复制，不消费
        *inout_len = need;
        return -5; // buffer too small
    }
    if (do_copy && need) {
        memcpy(out, (const uint8_t*)fixed_rec_in + SG_RB_HDR_SIZE, need);
    }
    *inout_len = need;
    return 0;
}

// --- 内部工具函数 ---
// 计算第 index 条记录的文件偏移
static inline long rec_offset(FileRingBuffer *b, int index) {
    return (long) META_DATA_SIZE + (long) index * (long) RECORD_SIZE;
}

// 内部：刷新元数据到文件（调用方须持锁）
static inline void meta_flush(FileRingBuffer *buffer) {
    fflush(buffer->file);
    if (buffer->enable_fsync) {
        int fd = fileno(buffer->file);
        if (fd >= 0) fsync(fd);
    }
}

// 内部：构造“当前时刻 + timeout_ms”的绝对时间
static void make_abs_timespec(struct timespec *ts, int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += timeout_ms / 1000;
    long nsec = ts->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts->tv_sec  += nsec / 1000000000L;
    ts->tv_nsec  = nsec % 1000000000L;
}

// 内部：向 tail 写入一条记录（要求：调用方已持锁，并确保有空位或已处理覆盖逻辑）
static int write_at_tail_unlocked(FileRingBuffer *buffer, const char *record, bool increase_count) {
    fseek(buffer->file, (long)META_DATA_SIZE + (long)buffer->tail * (long)RECORD_SIZE, SEEK_SET);
    size_t n = fwrite(record, 1, RECORD_SIZE, buffer->file);
    if (n != RECORD_SIZE) return -1;

    buffer->tail = (buffer->tail + 1) % buffer->max_record_num;
    if (increase_count) buffer->count++;
    save_meta_data(buffer);               // 刷新元数据
    pthread_cond_signal(&buffer->not_empty);
    return 0;
}

// 内部：保存元数据到文件（调用方须持锁）
void save_meta_data(FileRingBuffer *buffer){
    fseek(buffer->file, 0, SEEK_SET);
    fwrite(MAGIC_NUMBER, sizeof(char), sizeof(MAGIC_NUMBER), buffer->file);
    int version = VERSION;
    fwrite(&version, sizeof(int), 1, buffer->file);
    fwrite(&(buffer->head), sizeof(int), 1, buffer->file);
    fwrite(&(buffer->tail), sizeof(int), 1, buffer->file);
    fwrite(&(buffer->count), sizeof(int), 1, buffer->file);
    meta_flush(buffer);
}

// 内部：从文件加载元数据（调用方须持锁）
int load_meta_data(FileRingBuffer *buffer) {
    fseek(buffer->file, 0, SEEK_SET);
    char magic[sizeof(MAGIC_NUMBER)];
    if (fread(magic, 1, sizeof(MAGIC_NUMBER), buffer->file) != sizeof(MAGIC_NUMBER)) return -1;
    if (strncmp(magic, MAGIC_NUMBER, sizeof(MAGIC_NUMBER)) != 0) return -1;
    int version=0;
    if (fread(&version, sizeof(int), 1, buffer->file) != 1) return -1;
    if (version != VERSION) return -1;
    if (fread(&buffer->head,  sizeof(int), 1, buffer->file) != 1) return -1;
    if (fread(&buffer->tail,  sizeof(int), 1, buffer->file) != 1) return -1;
    if (fread(&buffer->count, sizeof(int), 1, buffer->file) != 1) return -1;
    if (buffer->count < 0 || buffer->count > buffer->max_record_num) return -1;
    if (buffer->head  < 0 || buffer->head  >= buffer->max_record_num) return -1;
    if (buffer->tail  < 0 || buffer->tail  >= buffer->max_record_num) return -1;
    return 0;
}

/***************************************************************************************
* @brief This is the Fileringbuffer initialiser
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param filename The file to store the Fileringbuffer
* @return -1: The file doesn't exist or failed to rebuild;
*          0: The Fileringbuffer successfully registered
***************************************************************************************/
int file_ringbuffer_init(FileRingBuffer *buffer, int max_record_num, const char *filename) {
    buffer->file = fopen(filename, "r+b");
    buffer->max_record_num = max_record_num;
    buffer->enable_fsync = false;
    buffer->stop_requested = false;

    if (!buffer->file) {
        buffer->file = fopen(filename, "w+b");
        if (!buffer->file) return -1;
        buffer->head = buffer->tail = buffer->count = 0;
        save_meta_data(buffer);
    } else if (load_meta_data(buffer) != 0) {
        fclose(buffer->file);
        remove(filename);
        buffer->file = fopen(filename, "w+b");
        if (!buffer->file) return -1;
        buffer->head = buffer->tail = buffer->count = 0;
        save_meta_data(buffer);
    }

    pthread_mutex_init(&buffer->mu, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    return 0;
}

/***************************************************************************************
* @brief This is the function to enable or disable fsync
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param on True to enable fsync
* @return None
***************************************************************************************/
void file_ringbuffer_enable_fsync(FileRingBuffer *buffer, bool on) {
    pthread_mutex_lock(&buffer->mu);
    buffer->enable_fsync = on;
    pthread_mutex_unlock(&buffer->mu);
}

/***************************************************************************************
* @brief This is the function to exit the blocking read function
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return None
***************************************************************************************/
void file_ringbuffer_request_stop(FileRingBuffer *buffer) {
    pthread_mutex_lock(&buffer->mu);
    buffer->stop_requested = true;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mu);
}

/***************************************************************************************
* @brief This is the Fileringbuffer write function
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully registered
***************************************************************************************/
int file_ringbuffer_write(FileRingBuffer *buffer, const char *record) {
    pthread_mutex_lock(&buffer->mu);

    // 满则覆盖：推进 head；否则计数+1
    if (buffer->count == buffer->max_record_num) {
        buffer->head = (buffer->head + 1) % buffer->max_record_num;
    } else {
        buffer->count++;
    }

    fseek(buffer->file, rec_offset(buffer, buffer->tail), SEEK_SET);
    size_t n = fwrite(record, 1, RECORD_SIZE, buffer->file);
    if (n != RECORD_SIZE) {
        // 尽力回滚
        if (buffer->count < buffer->max_record_num) buffer->count--;
        else buffer->head = (buffer->head - 1 + buffer->max_record_num) % buffer->max_record_num;
        pthread_mutex_unlock(&buffer->mu);
        return -1;
    }
    buffer->tail = (buffer->tail + 1) % buffer->max_record_num;
    save_meta_data(buffer);

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mu);
    return 0;
}

/***************************************************************************************
* @brief This is the Fileringbuffer peek function, it only read but doesn't delete messages
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully peeked;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_peek(FileRingBuffer *buffer, char *record) {
    pthread_mutex_lock(&buffer->mu);
    if (buffer->count == 0) {
        // Nothing to peek
        pthread_mutex_unlock(&buffer->mu);
        return -1;
    }

    // Find the position of the oldest record
    fseek(buffer->file, rec_offset(buffer, buffer->head), SEEK_SET);
    // Read the recording without deleting
    size_t n = fread(record, sizeof(char), RECORD_SIZE, buffer->file);
    pthread_mutex_unlock(&buffer->mu);
    return (n == RECORD_SIZE) ? 0 : -1;
}

/***************************************************************************************
* @brief This is the function to delete one oldest message
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The message is successfully deleted;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_delete(FileRingBuffer *buffer) {
    pthread_mutex_lock(&buffer->mu);
    if (buffer->count == 0) { pthread_mutex_unlock(&buffer->mu); return -1; }

    buffer->head = (buffer->head + 1) % buffer->max_record_num;
    buffer->count--;
    
    save_meta_data(buffer);
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mu);
    return 0;
}

/***************************************************************************************
* @brief This is the function to read and then delete one oldest message
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully read;
*        -1: No message is in the buffer
***************************************************************************************/
int file_ringbuffer_read(FileRingBuffer *buffer, char *record) {
    pthread_mutex_lock(&buffer->mu);
    if (buffer->count == 0) { pthread_mutex_unlock(&buffer->mu); return -1; }

    fseek(buffer->file, rec_offset(buffer, buffer->head), SEEK_SET);
    size_t n = fread(record, 1, RECORD_SIZE, buffer->file);

    if (n != RECORD_SIZE) { pthread_mutex_unlock(&buffer->mu); return -1; }
    buffer->head = (buffer->head + 1) % buffer->max_record_num;
    buffer->count--;

    save_meta_data(buffer);
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mu);
    return 0;
}

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
int file_ringbuffer_write_blocking(FileRingBuffer *buffer, const char *record) {
    pthread_mutex_lock(&buffer->mu);

    while (buffer->count == buffer->max_record_num && !buffer->stop_requested) {
        pthread_cond_wait(&buffer->not_full, &buffer->mu);
    }
    if (buffer->stop_requested) {
        pthread_mutex_unlock(&buffer->mu);
        return -3;  // 被请求停止
    }

    // 现在一定有空位（count < max）
    int rc = write_at_tail_unlocked(buffer, record, /*increase_count=*/true);

    pthread_mutex_unlock(&buffer->mu);
    return (rc == 0) ? 0 : -1;
}

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
int file_ringbuffer_write_timed(FileRingBuffer *buffer, const char *record, int timeout_ms) {
    pthread_mutex_lock(&buffer->mu);

    if (buffer->count == buffer->max_record_num && !buffer->stop_requested) {
        struct timespec ts; make_abs_timespec(&ts, timeout_ms);
        int rc = pthread_cond_timedwait(&buffer->not_full, &buffer->mu, &ts);
        (void)rc; // 真正以谓词为准
    }

    if (buffer->stop_requested) {
        pthread_mutex_unlock(&buffer->mu);
        return -3;  // 被请求停止
    }
    if (buffer->count == buffer->max_record_num) {
        // 仍然满：超时或虚假唤醒但没空位
        pthread_mutex_unlock(&buffer->mu);
        return -2;  // 超时
    }

    int rc = write_at_tail_unlocked(buffer, record, /*increase_count=*/true);

    pthread_mutex_unlock(&buffer->mu);
    return (rc == 0) ? 0 : -1;
}

/***************************************************************************************
* @brief This is the Fileringbuffer read function (blocking), wait if empty
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @param record The char array to be written in
* @return 0: The Fileringbuffer successfully registered
***************************************************************************************/
int file_ringbuffer_read_blocking(FileRingBuffer *buffer, char *record) {
    pthread_mutex_lock(&buffer->mu);
    while (buffer->count == 0 && !buffer->stop_requested) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mu);
    }
    if (buffer->stop_requested && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mu);
        return -3; // stop
    }
    // 此时 count>0 或 stop+有残留数据，放锁后用非阻塞读取
    pthread_mutex_unlock(&buffer->mu);
    return file_ringbuffer_read(buffer, record);
}

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
int file_ringbuffer_read_timed(FileRingBuffer *buffer, char *record, int timeout_ms) {
    pthread_mutex_lock(&buffer->mu);
    if (buffer->count == 0 && !buffer->stop_requested) {
        struct timespec ts; make_abs_timespec(&ts, timeout_ms);
        int rc = pthread_cond_timedwait(&buffer->not_empty, &buffer->mu, &ts);
        (void)rc; // 超时由谓词判断
    }
    if (buffer->stop_requested && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mu);
        return -3; // stop
    }
    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mu);
        return -2; // timeout（或虚假唤醒但依旧无数据）
    }
    pthread_mutex_unlock(&buffer->mu);
    return file_ringbuffer_read(buffer, record);
}

/***************************************************************************************
* @brief This is the function to close the Fileringbuffer
****************************************************************************************
* @param buffer The Fileringbuffer struct
* @return None
***************************************************************************************/
void file_ringbuffer_destroy(FileRingBuffer *buffer) {
    pthread_mutex_lock(&buffer->mu);
    FILE *f = buffer->file;
    buffer->file = NULL;
    pthread_mutex_unlock(&buffer->mu);

    if (f) fclose(f);
    pthread_mutex_destroy(&buffer->mu);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_cond_destroy(&buffer->not_full);
}

/* Methods to read and write structs */
// --- 写入（封装后复用现有的 write / write_blocking / write_timed） ---
int file_ringbuffer_write_bytes(FileRingBuffer *buffer, const void *data, size_t len, uint16_t type) {
    uint8_t rec[RECORD_SIZE];
    int rc = pack_record(rec, data, len, type);
    if (rc != 0) return rc;
    return file_ringbuffer_write(buffer, (const char*)rec); // 走“满则覆盖”路径
}

int file_ringbuffer_write_bytes_blocking(FileRingBuffer *buffer, const void *data, size_t len, uint16_t type) {
    uint8_t rec[RECORD_SIZE];
    int rc = pack_record(rec, data, len, type);
    if (rc != 0) return rc;
    return file_ringbuffer_write_blocking(buffer, (const char*)rec);  // 不覆盖，等空位
}

int file_ringbuffer_write_bytes_timed(FileRingBuffer *buffer, const void *data, size_t len, uint16_t type, int timeout_ms) {
    uint8_t rec[RECORD_SIZE];
    int rc = pack_record(rec, data, len, type);
    if (rc != 0) return rc;
    return file_ringbuffer_write_timed(buffer, (const char*)rec, timeout_ms); // 不覆盖，定时等待
}

// --- 读取（peek/read 两种；blocking/timed 封装现有函数） ---
int file_ringbuffer_peek_bytes(FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type) {
    uint8_t rec[RECORD_SIZE];
    int rc = file_ringbuffer_peek(buffer, (char*)rec);
    if (rc != 0) return rc; // -1 空
    // peek 不消费：只做解析与可能的拷贝
    return unpack_record(rec, out, inout_len, out_type, /*do_copy=*/true);
}

int file_ringbuffer_read_bytes(FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type) {
    uint8_t rec[RECORD_SIZE];
    // 先 peek 看看大小够不够；不够则报 -5 并不消费
    int rc = file_ringbuffer_peek(buffer, (char*)rec);
    if (rc != 0) return rc;        // -1 空
    rc = unpack_record(rec, out, inout_len, out_type, /*do_copy=*/false);
    if (rc == -5) return -5;       // 调用者需要扩大 out 缓冲

    // 够了：真正读出并消费
    rc = file_ringbuffer_read(buffer, (char*)rec);
    if (rc != 0) return rc;        // 理论上不该失败
    // 此时再把数据拷到 out
    return unpack_record(rec, out, inout_len, out_type, /*do_copy=*/true);
}

int file_ringbuffer_read_bytes_blocking(FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type) {
    uint8_t rec[RECORD_SIZE];
    int rc = file_ringbuffer_read_blocking(buffer, (char*)rec);
    if (rc != 0) return rc;  // -3 stop, -1 I/O
    return unpack_record(rec, out, inout_len, out_type, /*do_copy=*/true);
}

int file_ringbuffer_read_bytes_timed(FileRingBuffer *buffer, void *out, size_t *inout_len, uint16_t *out_type, int timeout_ms) {
    uint8_t rec[RECORD_SIZE];
    int rc = file_ringbuffer_read_timed(buffer, (char*)rec, timeout_ms);
    if (rc != 0) return rc;  // -2 超时, -3 stop, -1 I/O
    return unpack_record(rec, out, inout_len, out_type, /*do_copy=*/true);
}
