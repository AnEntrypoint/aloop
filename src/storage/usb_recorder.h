#ifndef ALOOP_USB_RECORDER_H
#define ALOOP_USB_RECORDER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aloop {

class UsbRecorder {
public:
    UsbRecorder(std::string mountPoint, int sampleRate, int chunkMinutes, int chunkCount);
    ~UsbRecorder();

    UsbRecorder(const UsbRecorder&) = delete;
    UsbRecorder& operator=(const UsbRecorder&) = delete;

    void pushBlock(const float* samples, int n);
    void poll();

    bool recording() const { return m_recording.load(std::memory_order_relaxed); }
    uint64_t overruns() const { return m_overruns.load(std::memory_order_relaxed); }

private:
    bool isMounted() const;
    int effectiveChunkCount() const;
    bool beginRecording();
    void endRecording();
    bool openChunk(int index);
    void finalizeChunk();
    bool drainToFile();
    void writeWavHeader(int fd, uint32_t dataBytes) const;

    std::string m_mountPoint;
    std::string m_recordDir;
    int m_sampleRate;
    int m_chunkCount;
    uint64_t m_chunkMaxSamples;

    int16_t* m_ring;
    uint64_t m_ringCapacity;
    std::atomic<uint64_t> m_writeCount{0};
    std::atomic<uint64_t> m_readCount{0};
    std::atomic<uint64_t> m_overruns{0};

    std::atomic<bool> m_recording{false};
    int m_fd = -1;
    int m_chunkIndex = 0;
    int m_effectiveChunkCount = 0;
    uint64_t m_chunkSamplesWritten = 0;
};

} // namespace aloop
#endif // ALOOP_USB_RECORDER_H
