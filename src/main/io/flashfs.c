/*
 * This file is part of Cleanflight and Betaflight and EmuFlight.
 *
 * Cleanflight and Betaflight and EmuFlight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight and EmuFlight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * This provides a stream interface to a flash chip if one is present.
 *
 * On statup, call flashfsInit() after initialising the flash chip in order to init the filesystem. This will
 * result in the file pointer being pointed at the first free block found, or at the end of the device if the
 * flash chip is full.
 *
 * Note that bits can only be set to 0 when writing, not back to 1 from 0. You must erase sectors in order
 * to bring bits back to 1 again.
 *
 * In future, we can add support for multiple different flash chips by adding a flash device driver vtable
 * and make calls through that, at the moment flashfs just calls m25p16_* routines explicitly.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "platform.h"

#include "common/printf.h"
#include "drivers/flash.h"

#include "io/flashfs.h"

static const flashPartition_t *flashPartition = NULL;
static const flashGeometry_t *flashGeometry = NULL;
static uint32_t flashfsSize = 0;

static uint8_t flashWriteBuffer[FLASHFS_WRITE_BUFFER_SIZE];

/* The position of our head and tail in the circular flash write buffer.
 *
 * The head is the index that a byte would be inserted into on writing, while the tail is the index of the
 * oldest byte that has yet to be written to flash.
 *
 * When the circular buffer is empty, head == tail
 */
static uint8_t bufferHead = 0, bufferTail = 0;

// The position of the buffer's tail in the overall flash address space:
static uint32_t tailAddress = 0;

static void flashfsClearBuffer(void)
{
    bufferTail = bufferHead = 0;
}

static bool flashfsBufferIsEmpty(void)
{
    return bufferTail == bufferHead;
}

static void flashfsSetTailAddress(uint32_t address)
{
    tailAddress = address;
}

void flashfsEraseCompletely(void)
{
    if (flashGeometry->sectors > 0 && flashPartitionCount() > 0) {
        // if there's a single FLASHFS partition and it uses the entire flash then do a full erase
        const bool doFullErase = (flashPartitionCount() == 1) && (FLASH_PARTITION_SECTOR_COUNT(flashPartition) == flashGeometry->sectors);
        if (doFullErase) {
            flashEraseCompletely();
        } else {

            // TODO - the partial sector-based erase needs to be completely reworked.
            // All calls to flashfsEraseCompletely() currently expect the erase to run
            // asynchronously and return immediately. The current implementation performs
            // the erase synchronously and doesn't return until complete. This breaks calls
            // from MSP and runtime mode-switched erasing.

            for (flashSector_t sectorIndex = flashPartition->startSector; sectorIndex <= flashPartition->endSector; sectorIndex++) {
                uint32_t sectorAddress = sectorIndex * flashGeometry->sectorSize;
                flashEraseSector(sectorAddress);
            }
        }
    }

    flashfsClearBuffer();

    flashfsSetTailAddress(0);
}

/**
 * Start and end must lie on sector boundaries, or they will be rounded out to sector boundaries such that
 * all the bytes in the range [start...end) are erased.
 */
void flashfsEraseRange(uint32_t start, uint32_t end)
{
    if (flashGeometry->sectorSize <= 0)
        return;

    // Round the start down to a sector boundary
    int startSector = start / flashGeometry->sectorSize;

    // And the end upward
    int endSector = end / flashGeometry->sectorSize;
    int endRemainder = end % flashGeometry->sectorSize;

    if (endRemainder > 0) {
        endSector++;
    }

    for (int sectorIndex = startSector; sectorIndex < endSector; sectorIndex++) {
        uint32_t sectorAddress = sectorIndex * flashGeometry->sectorSize;
        flashEraseSector(sectorAddress);
    }
}

/**
 * Return true if the flash is not currently occupied with an operation.
 */
bool flashfsIsReady(void)
{
    // Check for flash chip existence first, then check if ready.

    return (flashfsIsSupported() && flashIsReady());
}

bool flashfsIsSupported(void)
{
    return flashfsSize > 0;
}

uint32_t flashfsGetSize(void)
{
    return flashfsSize;
}

static uint32_t flashfsTransmitBufferUsed(void)
{
    if (bufferHead >= bufferTail)
        return bufferHead - bufferTail;

    return FLASHFS_WRITE_BUFFER_SIZE - bufferTail + bufferHead;
}

/**
 * Get the size of the largest single write that flashfs could ever accept without blocking or data loss.
 */
uint32_t flashfsGetWriteBufferSize(void)
{
    return FLASHFS_WRITE_BUFFER_USABLE;
}

/**
 * Get the number of bytes that can currently be written to flashfs without any blocking or data loss.
 */
uint32_t flashfsGetWriteBufferFreeSpace(void)
{
    return flashfsGetWriteBufferSize() - flashfsTransmitBufferUsed();
}

/**
 * Write the given buffers to flash sequentially at the current tail address, advancing the tail address after
 * each write.
 *
 * In synchronous mode, waits for the flash to become ready before writing so that every byte requested can be written.
 *
 * In asynchronous mode, if the flash is busy, then the write is aborted and the routine returns immediately.
 * In this case the returned number of bytes written will be less than the total amount requested.
 *
 * Modifies the supplied buffer pointers and sizes to reflect how many bytes remain in each of them.
 *
 * bufferCount: the number of buffers provided
 * buffers: an array of pointers to the beginning of buffers
 * bufferSizes: an array of the sizes of those buffers
 * sync: true if we should wait for the device to be idle before writes, otherwise if the device is busy the
 *       write will be aborted and this routine will return immediately.
 *
 * Returns the number of bytes written
 */
static uint32_t flashfsWriteBuffers(uint8_t const **buffers, uint32_t *bufferSizes, int bufferCount, bool sync)
{
    uint32_t bytesTotal = 0;

    int i;

    for (i = 0; i < bufferCount; i++) {
        bytesTotal += bufferSizes[i];
    }

    if (!sync && !flashIsReady()) {
        return 0;
    }

    uint32_t bytesTotalRemaining = bytesTotal;

    uint16_t pageSize = flashGeometry->pageSize;

    while (bytesTotalRemaining > 0) {
        uint32_t bytesTotalThisIteration;
        uint32_t bytesRemainThisIteration;

        /*
         * Each page needs to be saved in a separate program operation, so
         * if we would cross a page boundary, only write up to the boundary in this iteration:
         */
        if (tailAddress % pageSize + bytesTotalRemaining > pageSize) {
            bytesTotalThisIteration = pageSize - tailAddress % pageSize;
        } else {
            bytesTotalThisIteration = bytesTotalRemaining;
        }

        // Are we at EOF already? Abort.
        if (flashfsIsEOF()) {
            // May as well throw away any buffered data
            flashfsClearBuffer();

            break;
        }

        flashPageProgramBegin(tailAddress);

        bytesRemainThisIteration = bytesTotalThisIteration;

        for (i = 0; i < bufferCount; i++) {
            if (bufferSizes[i] > 0) {
                // Is buffer larger than our write limit? Write our limit out of it
                if (bufferSizes[i] >= bytesRemainThisIteration) {
                    flashPageProgramContinue(buffers[i], bytesRemainThisIteration);

                    buffers[i] += bytesRemainThisIteration;
                    bufferSizes[i] -= bytesRemainThisIteration;

                    bytesRemainThisIteration = 0;
                    break;
                } else {
                    // We'll still have more to write after finishing this buffer off
                    flashPageProgramContinue(buffers[i], bufferSizes[i]);

                    bytesRemainThisIteration -= bufferSizes[i];

                    buffers[i] += bufferSizes[i];
                    bufferSizes[i] = 0;
                }
            }
        }

        flashPageProgramFinish();

        bytesTotalRemaining -= bytesTotalThisIteration;

        // Advance the cursor in the file system to match the bytes we wrote
        flashfsSetTailAddress(tailAddress + bytesTotalThisIteration);

        /*
         * We'll have to wait for that write to complete before we can issue the next one, so if
         * the user requested asynchronous writes, break now.
         */
        if (!sync)
            break;
    }

    return bytesTotal - bytesTotalRemaining;
}

/*
 * Since the buffered data might wrap around the end of the circular buffer, we can have two segments of data to write,
 * an initial portion and a possible wrapped portion.
 *
 * This routine will fill the details of those buffers into the provided arrays, which must be at least 2 elements long.
 */
static void flashfsGetDirtyDataBuffers(uint8_t const *buffers[], uint32_t bufferSizes[])
{
    buffers[0] = flashWriteBuffer + bufferTail;
    buffers[1] = flashWriteBuffer + 0;

    if (bufferHead >= bufferTail) {
        bufferSizes[0] = bufferHead - bufferTail;
        bufferSizes[1] = 0;
    } else {
        bufferSizes[0] = FLASHFS_WRITE_BUFFER_SIZE - bufferTail;
        bufferSizes[1] = bufferHead;
    }
}

/**
 * Get the current offset of the file pointer within the volume.
 */
uint32_t flashfsGetOffset(void)
{
    uint8_t const * buffers[2];
    uint32_t bufferSizes[2];

    // Dirty data in the buffers contributes to the offset

    flashfsGetDirtyDataBuffers(buffers, bufferSizes);

    return tailAddress + bufferSizes[0] + bufferSizes[1];
}

/**
 * Called after bytes have been written from the buffer to advance the position of the tail by the given amount.
 */
static void flashfsAdvanceTailInBuffer(uint32_t delta)
{
    bufferTail += delta;

    // Wrap tail around the end of the buffer
    if (bufferTail >= FLASHFS_WRITE_BUFFER_SIZE) {
        bufferTail -= FLASHFS_WRITE_BUFFER_SIZE;
    }

    if (flashfsBufferIsEmpty()) {
        flashfsClearBuffer(); // Bring buffer pointers back to the start to be tidier
    }
}

/**
 * If the flash is ready to accept writes, flush the buffer to it.
 *
 * Returns true if all data in the buffer has been flushed to the device, or false if
 * there is still data to be written (call flush again later).
 */
bool flashfsFlushAsync(void)
{
    if (flashfsBufferIsEmpty()) {
        return true; // Nothing to flush
    }

    uint8_t const * buffers[2];
    uint32_t bufferSizes[2];
    uint32_t bytesWritten;

    flashfsGetDirtyDataBuffers(buffers, bufferSizes);
    bytesWritten = flashfsWriteBuffers(buffers, bufferSizes, 2, false);
    flashfsAdvanceTailInBuffer(bytesWritten);

    return flashfsBufferIsEmpty();
}

/**
 * Wait for the flash to become ready and begin flushing any buffered data to flash.
 *
 * The flash will still be busy some time after this sync completes, but space will
 * be freed up to accept more writes in the write buffer.
 */
void flashfsFlushSync(void)
{
    if (flashfsBufferIsEmpty()) {
        return; // Nothing to flush
    }

    uint8_t const * buffers[2];
    uint32_t bufferSizes[2];

    flashfsGetDirtyDataBuffers(buffers, bufferSizes);
    flashfsWriteBuffers(buffers, bufferSizes, 2, true);

    // We've written our entire buffer now:
    flashfsClearBuffer();
}

void flashfsSeekAbs(uint32_t offset)
{
    flashfsFlushSync();

    flashfsSetTailAddress(offset);
}

void flashfsSeekRel(int32_t offset)
{
    flashfsFlushSync();

    flashfsSetTailAddress(tailAddress + offset);
}

/**
 * Write the given byte asynchronously to the flash. If the buffer overflows, data is silently discarded.
 */
void flashfsWriteByte(uint8_t byte)
{
    flashWriteBuffer[bufferHead++] = byte;

    if (bufferHead >= FLASHFS_WRITE_BUFFER_SIZE) {
        bufferHead = 0;
    }

    if (flashfsTransmitBufferUsed() >= FLASHFS_WRITE_BUFFER_AUTO_FLUSH_LEN) {
        flashfsFlushAsync();
    }
}

/**
 * Write the given buffer to the flash either synchronously or asynchronously depending on the 'sync' parameter.
 *
 * If writing asynchronously, data will be silently discarded if the buffer overflows.
 * If writing synchronously, the routine will block waiting for the flash to become ready so will never drop data.
 */
void flashfsWrite(const uint8_t *data, unsigned int len, bool sync)
{
    uint8_t const * buffers[3];
    uint32_t bufferSizes[3];

    // There could be two dirty buffers to write out already:
    flashfsGetDirtyDataBuffers(buffers, bufferSizes);

    // Plus the buffer the user supplied:
    buffers[2] = data;
    bufferSizes[2] = len;

    /*
     * Would writing this data to our buffer cause our buffer to reach the flush threshold? If so try to write through
     * to the flash now
     */
    if (bufferSizes[0] + bufferSizes[1] + bufferSizes[2] >= FLASHFS_WRITE_BUFFER_AUTO_FLUSH_LEN) {
        uint32_t bytesWritten;

        // Attempt to write all three buffers through to the flash asynchronously
        bytesWritten = flashfsWriteBuffers(buffers, bufferSizes, 3, false);

        if (bufferSizes[0] == 0 && bufferSizes[1] == 0) {
            // We wrote all the data that was previously buffered
            flashfsClearBuffer();

            if (bufferSizes[2] == 0) {
                // And we wrote all the data the user supplied! Job done!
                return;
            }
        } else {
            // We only wrote a portion of the old data, so advance the tail to remove the bytes we did write from the buffer
            flashfsAdvanceTailInBuffer(bytesWritten);
        }

        // Is the remainder of the data to be written too big to fit in the buffers?
        if (bufferSizes[0] + bufferSizes[1] + bufferSizes[2] > FLASHFS_WRITE_BUFFER_USABLE) {
            if (sync) {
                // Write it through synchronously
                flashfsWriteBuffers(buffers, bufferSizes, 3, true);
                flashfsClearBuffer();
            } else {
                /*
                 * Silently drop the data the user asked to write (i.e. no-op) since we can't buffer it and they
                 * requested async.
                 */
            }

            return;
        }

        // Fall through and add the remainder of the incoming data to our buffer
        data = buffers[2];
        len = bufferSizes[2];
    }

    // Buffer up the data the user supplied instead of writing it right away

    // First write the portion before we wrap around the end of the circular buffer
    unsigned int bufferBytesBeforeWrap = FLASHFS_WRITE_BUFFER_SIZE - bufferHead;

    unsigned int firstPortion = len < bufferBytesBeforeWrap ? len : bufferBytesBeforeWrap;

    memcpy(flashWriteBuffer + bufferHead, data, firstPortion);

    bufferHead += firstPortion;

    data += firstPortion;
    len -= firstPortion;

    // If we wrap the head around, write the remainder to the start of the buffer (if any)
    if (bufferHead == FLASHFS_WRITE_BUFFER_SIZE) {
        memcpy(flashWriteBuffer + 0, data, len);

        bufferHead = len;
    }
}

/**
 * Read `len` bytes from the given address into the supplied buffer.
 *
 * Returns the number of bytes actually read which may be less than that requested.
 */
int flashfsReadAbs(uint32_t address, uint8_t *buffer, unsigned int len)
{
    int bytesRead;

    // Did caller try to read past the end of the volume?
    if (address + len > flashfsSize) {
        // Truncate their request
        len = flashfsSize - address;
    }

    // Since the read could overlap data in our dirty buffers, force a sync to clear those first
    flashfsFlushSync();

    bytesRead = flashReadBytes(address, buffer, len);

    return bytesRead;
}

/**
 * Find the offset of the start of the free space on the device (or the size of the device if it is full).
 */
int flashfsIdentifyStartOfFreeSpace(void)
{
    /* Find the start of the free space on the device by examining the beginning of blocks with a binary search,
     * looking for ones that appear to be erased. We can achieve this with good accuracy because an erased block
     * is all bits set to 1, which pretty much never appears in reasonable size substrings of blackbox logs.
     *
     * To do better we might write a volume header instead, which would mark how much free space remains. But keeping
     * a header up to date while logging would incur more writes to the flash, which would consume precious write
     * bandwidth and block more often.
     */

    enum {
        /* We can choose whatever power of 2 size we like, which determines how much wastage of free space we'll have
         * at the end of the last written data. But smaller blocksizes will require more searching.
         */
        FREE_BLOCK_SIZE = 2048, // XXX This can't be smaller than page size for underlying flash device.

        /* We don't expect valid data to ever contain this many consecutive uint32_t's of all 1 bits: */
        FREE_BLOCK_TEST_SIZE_INTS = 4, // i.e. 16 bytes
        FREE_BLOCK_TEST_SIZE_BYTES = FREE_BLOCK_TEST_SIZE_INTS * sizeof(uint32_t)
    };

    STATIC_ASSERT(FREE_BLOCK_SIZE >= FLASH_MAX_PAGE_SIZE, FREE_BLOCK_SIZE_too_small);

    union {
        uint8_t bytes[FREE_BLOCK_TEST_SIZE_BYTES];
        uint32_t ints[FREE_BLOCK_TEST_SIZE_INTS];
    } testBuffer;

    int left = 0; // Smallest block index in the search region
    int right = flashfsSize / FREE_BLOCK_SIZE; // One past the largest block index in the search region
    int mid;
    int result = right;
    int i;
    bool blockErased;

    while (left < right) {
        mid = (left + right) / 2;

        if (flashReadBytes(mid * FREE_BLOCK_SIZE, testBuffer.bytes, FREE_BLOCK_TEST_SIZE_BYTES) < FREE_BLOCK_TEST_SIZE_BYTES) {
            // Unexpected timeout from flash, so bail early (reporting the device fuller than it really is)
            break;
        }

        // Checking the buffer 4 bytes at a time like this is probably faster than byte-by-byte, but I didn't benchmark it :)
        blockErased = true;
        for (i = 0; i < FREE_BLOCK_TEST_SIZE_INTS; i++) {
            if (testBuffer.ints[i] != 0xFFFFFFFF) {
                blockErased = false;
                break;
            }
        }

        if (blockErased) {
            /* This erased block might be the leftmost erased block in the volume, but we'll need to continue the
             * search leftwards to find out:
             */
            result = mid;

            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return result * FREE_BLOCK_SIZE;
}

/**
 * Returns true if the file pointer is at the end of the device.
 */
bool flashfsIsEOF(void)
{
    return tailAddress >= flashfsSize;
}

void flashfsClose(void)
{
    switch(flashGeometry->flashType) {
    case FLASH_TYPE_NOR:
        break;

    case FLASH_TYPE_NAND:
        flashFlush();

        // Advance tailAddress to next page boundary.
        uint32_t pageSize = flashGeometry->pageSize;
        flashfsSetTailAddress((tailAddress + pageSize - 1) & ~(pageSize - 1));

        break;
    }
}

/**
 * Call after initializing the flash chip in order to set up the filesystem.
 */
void flashfsInit(void)
{
    flashfsSize = 0;

    flashPartition = flashPartitionFindByType(FLASH_PARTITION_TYPE_FLASHFS);
    flashGeometry = flashGetGeometry();

    if (!flashPartition) {
        return;
    }

    flashfsSize = FLASH_PARTITION_SECTOR_COUNT(flashPartition) * flashGeometry->sectorSize;

    // Start the file pointer off at the beginning of free space so caller can start writing immediately
    flashfsSeekAbs(flashfsIdentifyStartOfFreeSpace());
}

#ifdef USE_FLASH_TOOLS
bool flashfsVerifyEntireFlash(void)
{
    flashfsEraseCompletely();
    flashfsInit();

    uint32_t address = 0;
    flashfsSeekAbs(address);

    const int bufferSize = 32;
    char buffer[bufferSize + 1];

    const uint32_t testLimit = flashfsGetSize();

    for (address = 0; address < testLimit; address += bufferSize) {
        tfp_sprintf(buffer, "%08x >> **0123456789ABCDEF**", address);
        flashfsWrite((uint8_t*)buffer, strlen(buffer), true);
    }
    flashfsFlushSync();
    flashfsClose();

    char expectedBuffer[bufferSize + 1];

    flashfsSeekAbs(0);

    int verificationFailures = 0;
    for (address = 0; address < testLimit; address += bufferSize) {
        tfp_sprintf(expectedBuffer, "%08x >> **0123456789ABCDEF**", address);

        memset(buffer, 0, sizeof(buffer));
        int bytesRead = flashfsReadAbs(address, (uint8_t *)buffer, bufferSize);

        int result = strncmp(buffer, expectedBuffer, bufferSize);
        if (result != 0 || bytesRead != bufferSize) {
            verificationFailures++;
        }
    }
    return verificationFailures == 0;
}
#endif // USE_FLASH_TOOLS
