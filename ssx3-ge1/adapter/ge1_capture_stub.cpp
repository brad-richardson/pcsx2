// Android offline replay never records PCSX2 video/audio. Keep the public
// GSCapture surface linkable without pulling FFmpeg into this speed build.
#include "pcsx2/GS/GSCapture.h"
#include "common/Threading.h"

namespace GSCapture {
bool BeginCapture(float, GSVector2i, float, std::string) { return false; }
bool DeliverVideoFrame(GSTexture*) { return false; }
void DeliverAudioPacket(const float*) {}
void EndCapture() {}
bool IsCapturing() { return false; }
bool IsCapturingVideo() { return false; }
bool IsCapturingAudio() { return false; }
TinyString GetElapsedTime() { return {}; }
const Threading::ThreadHandle& GetEncoderThreadHandle()
{
    static Threading::ThreadHandle handle;
    return handle;
}
GSVector2i GetSize() { return {}; }
std::string GetNextCaptureFileName() { return {}; }
void Flush() {}
void FlushAudioOnly() {}
CodecList GetVideoCodecList(const char*) { return {}; }
CodecList GetAudioCodecList(const char*) { return {}; }
FormatList GetVideoFormatList(const char*) { return {}; }
}
