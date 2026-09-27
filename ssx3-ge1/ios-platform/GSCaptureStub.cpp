// GI1: no-op GSCapture for the iOS GS path. Video capture (.mp4 dump) needs
// ffmpeg headers and is unsupported there; nothing on the GS path captures.
// GS.cpp's references stay linked with IsCapturing() always false.
#include "GS/GSCapture.h"
#include "common/Threading.h"

namespace GSCapture
{
namespace
{
Threading::ThreadHandle s_null_handle;
}

bool BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
	(void)fps; (void)recommendedResolution; (void)aspect; (void)filename;
	return false;
}
bool DeliverVideoFrame(GSTexture* stex)
{
	(void)stex;
	return false;
}
void DeliverAudioPacket(const float* frames)
{
	(void)frames;
}
void EndCapture()
{
}
bool IsCapturing()
{
	return false;
}
bool IsCapturingVideo()
{
	return false;
}
bool IsCapturingAudio()
{
	return false;
}
TinyString GetElapsedTime()
{
	return TinyString();
}
const Threading::ThreadHandle& GetEncoderThreadHandle()
{
	return s_null_handle;
}
GSVector2i GetSize()
{
	return GSVector2i(0, 0);
}
std::string GetNextCaptureFileName()
{
	return std::string();
}
void Flush()
{
}
void FlushAudioOnly()
{
}
CodecList GetVideoCodecList(const char* container)
{
	(void)container;
	return CodecList();
}
CodecList GetAudioCodecList(const char* container)
{
	(void)container;
	return CodecList();
}
FormatList GetVideoFormatList(const char* codec)
{
	(void)codec;
	return FormatList();
}
} // namespace GSCapture
