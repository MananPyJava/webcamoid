/* Webcamoid, webcam capture application.
 * Copyright (C) 2019  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <QSharedPointer>
#include <QMutex>
#include <QWaitCondition>
#include <akelement.h>
#include <akfrac.h>
#include <akcaps.h>
#include <akaudiocaps.h>
#include <akpacket.h>
#include <akaudiopacket.h>
#include <media/NdkMediaCodec.h>

#include "audiostream.h"
#include "mediawriterndkmedia.h"

#define ENCODING_PCM_16BIT 0x2
#define ENCODING_PCM_8BIT  0x3
#define ENCODING_PCM_FLOAT 0x4

#define CHANNEL_MASK_MONO                  0x2
#define CHANNEL_MASK_FRONT_LEFT            0x4
#define CHANNEL_MASK_FRONT_RIGHT           0x8
#define CHANNEL_MASK_FRONT_CENTER          0x10
#define CHANNEL_MASK_LOW_FREQUENCY         0x20
#define CHANNEL_MASK_BACK_LEFT             0x40
#define CHANNEL_MASK_BACK_RIGHT            0x80
#define CHANNEL_MASK_FRONT_LEFT_OF_CENTER  0x100
#define CHANNEL_MASK_FRONT_RIGHT_OF_CENTER 0x200
#define CHANNEL_MASK_BACK_CENTER           0x400
#define CHANNEL_MASK_SIDE_LEFT             0x800
#define CHANNEL_MASK_SIDE_RIGHT            0x1000

using ChannelMaskToPositionMap = QMap<int32_t, AkAudioCaps::Position>;

inline const ChannelMaskToPositionMap &channelMaskToPosition()
{
    static const ChannelMaskToPositionMap channelMaskToPosition {
        {CHANNEL_MASK_MONO                 , AkAudioCaps::Position_FrontCenter       },
        {CHANNEL_MASK_FRONT_LEFT           , AkAudioCaps::Position_FrontLeft         },
        {CHANNEL_MASK_FRONT_RIGHT          , AkAudioCaps::Position_FrontRight        },
        {CHANNEL_MASK_FRONT_CENTER         , AkAudioCaps::Position_FrontCenter       },
        {CHANNEL_MASK_LOW_FREQUENCY        , AkAudioCaps::Position_LowFrequency1     },
        {CHANNEL_MASK_BACK_LEFT            , AkAudioCaps::Position_BackLeft          },
        {CHANNEL_MASK_BACK_RIGHT           , AkAudioCaps::Position_BackRight         },
        {CHANNEL_MASK_FRONT_LEFT_OF_CENTER , AkAudioCaps::Position_FrontLeftOfCenter },
        {CHANNEL_MASK_FRONT_RIGHT_OF_CENTER, AkAudioCaps::Position_FrontRightOfCenter},
        {CHANNEL_MASK_BACK_CENTER          , AkAudioCaps::Position_BackCenter        },
        {CHANNEL_MASK_SIDE_LEFT            , AkAudioCaps::Position_SideLeft          },
        {CHANNEL_MASK_SIDE_RIGHT           , AkAudioCaps::Position_SideRight         },
    };

    return channelMaskToPosition;
}

class AudioStreamPrivate
{
    public:
        AudioStream *self;
        AkElementPtr m_convert;
        AkAudioPacket m_frame;
        QMutex m_frameMutex;
        int64_t m_pts {0};
        QWaitCondition m_frameReady;

        explicit AudioStreamPrivate(AudioStream *self);
        AkPacket readPacket(size_t bufferIndex,
                            const AMediaCodecBufferInfo &info);
};

AudioStream::AudioStream(AMediaMuxer *mediaMuxer,
                         uint index,
                         int streamIndex,
                         const QVariantMap &configs,
                         MediaWriterNDKMedia *mediaWriter,
                         QObject *parent):
    AbstractStream(mediaMuxer,
                   index, streamIndex,
                   configs,
                   mediaWriter,
                   parent)
{
    this->d = new AudioStreamPrivate(this);
    AkAudioCaps audioCaps(this->caps());

#if __ANDROID_API__ >= 28
    AMediaFormat_setInt32(this->mediaFormat(),
                          AMEDIAFORMAT_KEY_PCM_ENCODING,
                          AudioStream::encodingFromSampleFormat(audioCaps.format()));
#endif
    AMediaFormat_setInt32(this->mediaFormat(),
                          AMEDIAFORMAT_KEY_CHANNEL_MASK,
                          AudioStream::channelMaskFromLayout(audioCaps.layout()));
    AMediaFormat_setInt32(this->mediaFormat(),
                          AMEDIAFORMAT_KEY_CHANNEL_COUNT,
                          audioCaps.channels());
    AMediaFormat_setInt32(this->mediaFormat(),
                          AMEDIAFORMAT_KEY_SAMPLE_RATE,
                          audioCaps.rate());

    this->d->m_convert = AkElement::create("ACapsConvert");
    this->d->m_convert->setProperty("caps", QVariant::fromValue(this->caps()));
}

AudioStream::~AudioStream()
{
    this->uninit();
    delete this->d;
}

int32_t AudioStream::encodingFromSampleFormat(AkAudioCaps::SampleFormat format)
{
    static const QMap<AkAudioCaps::SampleFormat, int32_t> encodingFromSampleFormat {
        {AkAudioCaps::SampleFormat_u8 , ENCODING_PCM_8BIT },
        {AkAudioCaps::SampleFormat_s16, ENCODING_PCM_16BIT},
        {AkAudioCaps::SampleFormat_flt, ENCODING_PCM_FLOAT},
    };

    return encodingFromSampleFormat.value(format);
}

int32_t AudioStream::channelMaskFromLayout(AkAudioCaps::ChannelLayout layout)
{
    int32_t mask = 0;

    for (auto position: AkAudioCaps::positions(layout))
        mask |= channelMaskToPosition().key(position);

    return mask;
}

void AudioStream::convertPacket(const AkPacket &packet)
{
    if (!packet)
        return;

    auto iPacket = AkAudioPacket(this->d->m_convert->iStream(packet));

    if (!iPacket)
        return;

    this->d->m_frameMutex.lock();
    this->d->m_frame += iPacket;
    this->d->m_frameReady.wakeAll();
    this->d->m_frameMutex.unlock();
}

bool AudioStream::encodeData(bool eos)
{
    ssize_t timeOut = 5000;

    if (eos)  {
        auto bufferIndex =
                AMediaCodec_dequeueInputBuffer(this->codec(), timeOut);

        if (bufferIndex < 0)
            return false;

        AMediaCodec_queueInputBuffer(this->codec(),
                                     size_t(bufferIndex),
                                     0,
                                     0,
                                     0,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    } else {
        auto packet = this->avPacketDequeue();

        if (!packet)
            return false;

        auto bufferIndex =
                AMediaCodec_dequeueInputBuffer(this->codec(), timeOut);

        if (bufferIndex < 0)
            return false;

        size_t buffersize = 0;
        auto buffer = AMediaCodec_getInputBuffer(this->codec(),
                                                 size_t(bufferIndex),
                                                 &buffersize);

        if (!buffer)
            return false;

        buffersize = qMin(size_t(packet.buffer().size()), buffersize);
        memcpy(buffer, packet.buffer().constData(), buffersize);
        auto presentationTimeUs =
                qRound(1e6 * packet.pts() * packet.timeBase().value());
        AMediaCodec_queueInputBuffer(codec(),
                                     size_t(bufferIndex),
                                     0,
                                     buffersize,
                                     uint64_t(presentationTimeUs),
                                     0);
    }

    AMediaCodecBufferInfo info;
    memset(&info, 0, sizeof(AMediaCodecBufferInfo));
    auto bufferIndex =
            AMediaCodec_dequeueOutputBuffer(this->codec(), &info, timeOut);

    if (bufferIndex >= 0) {
        auto packet = this->d->readPacket(size_t(bufferIndex), info);

        AMediaCodec_releaseOutputBuffer(this->codec(),
                                        size_t(bufferIndex),
                                        info.size != 0);
        emit this->packetReady(packet);
    }

    return true;
}

AkPacket AudioStream::avPacketDequeue()
{
    this->d->m_frameMutex.lock();

    if (!this->d->m_frame)
        if (!this->d->m_frameReady.wait(&this->d->m_frameMutex,
                                        THREAD_WAIT_LIMIT)) {
            this->d->m_frameMutex.unlock();

            return nullptr;
        }

    auto frame = this->d->m_frame;
    this->d->m_frame = AkAudioPacket();
    this->d->m_frameMutex.unlock();

    return frame;
}

bool AudioStream::init()
{
    this->d->m_convert->setState(AkElement::ElementStatePlaying);
    auto result = AbstractStream::init();

    if (!result)
        this->d->m_convert->setState(AkElement::ElementStateNull);

    return result;
}

void AudioStream::uninit()
{
    AbstractStream::uninit();
    this->d->m_convert->setState(AkElement::ElementStateNull);
}

AudioStreamPrivate::AudioStreamPrivate(AudioStream *self):
    self(self)
{

}

AkPacket AudioStreamPrivate::readPacket(size_t bufferIndex,
                                        const AMediaCodecBufferInfo &info)
{
    size_t bufferSize = 0;
    auto data = AMediaCodec_getOutputBuffer(self->codec(),
                                            bufferIndex,
                                            &bufferSize);
    bufferSize = qMin(bufferSize, size_t(info.size));
    QByteArray oBuffer(int(bufferSize), Qt::Uninitialized);
    memcpy(oBuffer.data(), data + info.offset, bufferSize);

    AkCaps caps("binary/data");
    AkPacket packet(caps);
    packet.setBuffer(oBuffer);
    packet.setPts(info.presentationTimeUs);
    packet.setTimeBase({1, qint64(1e6)});
    packet.setIndex(int(self->index()));
    packet.setId(0);

    return packet;
}

#include "moc_audiostream.cpp"