#pragma once
#include <obs.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
class SourceReplay {
public:
 SourceReplay();
 ~SourceReplay();
 bool start(obs_source_t*,int);
 void stop();
 bool save(const std::string&);
 bool running() const{return running_;}
 size_t buffered_seconds() const;
private:
 struct Packet{AVPacket*pkt;int64_t pts,dts;bool key,audio;};
 static void on_frame(void*,video_data*);
 static void on_audio(void*,obs_source_t*,const audio_data*,bool);
 void handle_frame(video_data*);
 void handle_audio(const audio_data*,bool);
 bool init_video_encoder(uint32_t,uint32_t,uint32_t,uint32_t);
 bool init_audio_encoder(uint32_t,uint32_t);
 void free_encoders();
 void trim_locked(int64_t);
 obs_view_t*view_=nullptr;
 video_t*video_=nullptr;
 obs_source_t*source_=nullptr;
 AVCodecContext*video_codec_=nullptr,*audio_codec_=nullptr;
 AVFrame*video_frame_=nullptr,*audio_frame_=nullptr;
 SwsContext*sws_=nullptr;
 SwrContext*swr_=nullptr;
 std::deque<Packet>packets_;
 mutable std::mutex mutex_;
 int max_seconds_=15;
 int64_t last_pts_=AV_NOPTS_VALUE,frame_index_=0;
 AVRational fps_{60,1};
 video_scale_info conversion_{};
 uint32_t audio_rate_=48000,audio_channels_=2;
 int audio_frame_capacity_=1024;
 std::atomic<bool>running_{false};
};
