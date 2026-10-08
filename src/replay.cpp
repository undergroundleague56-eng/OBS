#include "replay.hpp"
#include <algorithm>
#include <cstring>
static constexpr AVRational TB{1,90000};
static AVPacket* cp(const AVPacket*s){auto*p=av_packet_alloc();if(!p||av_packet_ref(p,s)<0){av_packet_free(&p);return nullptr;}return p;}
SourceReplay::SourceReplay(){avformat_network_init();}
SourceReplay::~SourceReplay(){stop();avformat_network_deinit();}
bool SourceReplay::start(obs_source_t*s,int sec){
 stop(); last_error_.clear();
 if(!s){last_error_="No source selected";return false;}
 source_=obs_source_get_ref(s); max_seconds_=std::clamp(sec,1,120);
 obs_video_info ovi{};
 if(!obs_get_video_info(&ovi)){last_error_="OBS video output is not available";stop();return false;}
 view_=obs_view_create();
 if(!view_){last_error_="obs_view_create failed";return false;}
 video_=obs_view_add2(view_,&ovi);
 if(!video_){last_error_="obs_view_add2 failed";stop();return false;}
 obs_view_set_source(view_,0,source_);
 auto*i=video_output_get_info(video_);
 uint32_t vw=i&&i->width?i->width:ovi.output_width;
 uint32_t vh=i&&i->height?i->height:ovi.output_height;
 uint32_t fn=i&&i->fps_num?i->fps_num:ovi.fps_num;
 uint32_t fd=i&&i->fps_den?i->fps_den:ovi.fps_den;
 if(!vw||!vh||!fn||!fd){last_error_="Invalid OBS video settings";stop();return false;}
 conversion_.format=VIDEO_FORMAT_BGRA; conversion_.width=vw; conversion_.height=vh;
 conversion_.range=VIDEO_RANGE_FULL; conversion_.colorspace=VIDEO_CS_SRGB;
 if(!init_video_encoder(vw,vh,fn,fd)){if(last_error_.empty())last_error_="Video encoder initialization failed";stop();return false;}
 obs_audio_info ai{}; obs_get_audio_info(&ai);
 audio_rate_=ai.samples_per_sec?ai.samples_per_sec:48000;
 audio_channels_=get_audio_channels(ai.speakers); if(!audio_channels_)audio_channels_=2;
 if(!init_audio_encoder(audio_rate_,audio_channels_)){if(last_error_.empty())last_error_="Audio encoder initialization failed";stop();return false;}
 if(!video_output_connect(video_,&conversion_,&on_frame,this)){last_error_="video_output_connect failed";stop();return false;}
 obs_source_add_audio_capture_callback(source_,&on_audio,this); running_=true; return true;}
void SourceReplay::stop(){running_=false;if(source_)obs_source_remove_audio_capture_callback(source_,&on_audio,this);if(video_)video_output_disconnect2(video_,&on_frame,this);video_=nullptr;if(view_){obs_view_remove(view_);obs_view_destroy(view_);view_=nullptr;}if(source_){obs_source_release(source_);source_=nullptr;}std::lock_guard<std::mutex>l(mutex_);for(auto&p:packets_)av_packet_free(&p.pkt);packets_.clear();free_encoders();last_pts_=AV_NOPTS_VALUE;frame_index_=0;}
bool SourceReplay::init_video_encoder(uint32_t w,uint32_t h,uint32_t fn,uint32_t fd){
 fps_={(int)fn,(int)fd};

 const char *last_codec_name = nullptr;
 int last_err = 0;
 const char *candidates[] = {
#ifdef __APPLE__
  "h264_videotoolbox",
#endif
  "libx264",
  "h264"
 };

 for (const char *name : candidates) {
  const AVCodec *candidate = avcodec_find_encoder_by_name(name);
  if (!candidate)
   continue;

  AVCodecContext *ctx = avcodec_alloc_context3(candidate);
  if (!ctx)
   continue;

  ctx->width=w;
  ctx->height=h;
  ctx->time_base=av_inv_q(fps_);
  ctx->framerate=fps_;
  // VideoToolbox is most reliable with NV12 on macOS; x264 uses planar YUV420P.
  ctx->pix_fmt = strstr(candidate->name,"videotoolbox")
    ? AV_PIX_FMT_NV12
    : AV_PIX_FMT_YUV420P;
  ctx->gop_size=std::max(1,(int)(fn/fd));
  ctx->max_b_frames=0;

  if (!strstr(candidate->name,"videotoolbox")) {
   av_opt_set(ctx->priv_data,"preset","veryfast",0);
   av_opt_set(ctx->priv_data,"tune","zerolatency",0);
   av_opt_set(ctx->priv_data,"crf","20",0);
  }

  last_codec_name = candidate->name;
  const int err = avcodec_open2(ctx,candidate,nullptr);
  if (err == 0) {
   video_codec_ = ctx;
   break;
  }

  last_err = err;
  avcodec_free_context(&ctx);
 }

 if (!video_codec_) {
  char errbuf[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(last_err, errbuf, sizeof(errbuf));
  last_error_ = std::string("H.264 encoder failed: ") +
   (last_codec_name ? last_codec_name : "none") +
   " (" + (last_err ? errbuf : "encoder not found") + ")";
  return false;
 }

 video_frame_=av_frame_alloc();
 if(!video_frame_){last_error_="Could not allocate video frame";return false;}
 video_frame_->format=video_codec_->pix_fmt;
 video_frame_->width=w;
 video_frame_->height=h;
 if(av_frame_get_buffer(video_frame_,32)<0){last_error_="Could not allocate video frame buffer";return false;}
 return true;
}
bool SourceReplay::init_audio_encoder(uint32_t rate,uint32_t ch){auto*c=avcodec_find_encoder(AV_CODEC_ID_AAC);if(!c){last_error_="No AAC encoder available";return false;}audio_codec_=avcodec_alloc_context3(c);audio_codec_->sample_rate=rate;audio_codec_->sample_fmt=AV_SAMPLE_FMT_FLTP;audio_codec_->time_base={1,(int)rate};av_channel_layout_default(&audio_codec_->ch_layout,ch);audio_codec_->bit_rate=160000;if(avcodec_open2(audio_codec_,c,nullptr)<0){last_error_="AAC encoder failed";return false;}audio_frame_=av_frame_alloc();audio_frame_->format=audio_codec_->sample_fmt;audio_frame_->sample_rate=rate;av_channel_layout_copy(&audio_frame_->ch_layout,&audio_codec_->ch_layout);audio_frame_capacity_=audio_codec_->frame_size>0?audio_codec_->frame_size:1024;audio_frame_->nb_samples=audio_frame_capacity_;return av_frame_get_buffer(audio_frame_,0)>=0;}
void SourceReplay::free_encoders(){if(sws_)sws_freeContext(sws_),sws_=nullptr;if(swr_)swr_free(&swr_);av_frame_free(&video_frame_);av_frame_free(&audio_frame_);avcodec_free_context(&video_codec_);avcodec_free_context(&audio_codec_);}
void SourceReplay::on_frame(void*p,video_data*f){((SourceReplay*)p)->handle_frame(f);}
void SourceReplay::handle_frame(video_data*f){if(!running_||!f||!video_codec_||!f->data[0])return;std::lock_guard<std::mutex>l(mutex_);if(!sws_)sws_=sws_getContext(video_codec_->width,video_codec_->height,AV_PIX_FMT_BGRA,video_codec_->width,video_codec_->height,video_codec_->pix_fmt,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);if(!sws_||av_frame_make_writable(video_frame_)<0)return;const uint8_t*src[]={f->data[0]};int st[]={static_cast<int>(f->linesize[0])};sws_scale(sws_,src,st,0,video_codec_->height,video_frame_->data,video_frame_->linesize);video_frame_->pts=frame_index_++;if(avcodec_send_frame(video_codec_,video_frame_)<0)return;AVPacket*out=av_packet_alloc();while(out&&avcodec_receive_packet(video_codec_,out)==0){auto pts=av_rescale_q(out->pts,video_codec_->time_base,TB),dts=av_rescale_q(out->dts,video_codec_->time_base,TB);out->pts=pts;out->dts=dts;out->duration=out->duration>0?av_rescale_q(out->duration,video_codec_->time_base,TB):0;packets_.push_back({out,pts,dts,(out->flags&AV_PKT_FLAG_KEY)!=0,false});last_pts_=std::max(last_pts_,pts);out=av_packet_alloc();}av_packet_free(&out);trim_locked(last_pts_);}
void SourceReplay::on_audio(void*p,obs_source_t*,const audio_data*d,bool muted){((SourceReplay*)p)->handle_audio(d,muted);}
void SourceReplay::handle_audio(const audio_data*d,bool muted){if(muted||!running_||!d||!d->data[0]||!audio_codec_)return;std::lock_guard<std::mutex>l(mutex_);if(audio_codec_->sample_fmt!=AV_SAMPLE_FMT_FLTP)return;if(!swr_){AVChannelLayout in;av_channel_layout_default(&in,audio_channels_);if(swr_alloc_set_opts2(&swr_,&audio_codec_->ch_layout,AV_SAMPLE_FMT_FLTP,audio_codec_->sample_rate,&in,AV_SAMPLE_FMT_FLTP,audio_rate_,0,nullptr)<0||swr_init(swr_)<0){if(swr_)swr_free(&swr_);av_channel_layout_uninit(&in);return;}av_channel_layout_uninit(&in);}uint32_t off=0;while(off<d->frames){int n=std::min<uint32_t>(audio_frame_capacity_,d->frames-off);if(av_frame_make_writable(audio_frame_)<0)return;const uint8_t*in[MAX_AV_PLANES]{};for(uint32_t c=0;c<std::min<uint32_t>(audio_channels_,MAX_AV_PLANES);++c)if(d->data[c])in[c]=d->data[c]+off*sizeof(float);int got=swr_convert(swr_,audio_frame_->data,audio_frame_capacity_,in,n);if(got<=0)return;audio_frame_->nb_samples=got;audio_frame_->pts=av_rescale_q(d->timestamp+av_rescale_q(off,{1,(int)audio_rate_},{1,1000000000}),{1,1000000000},audio_codec_->time_base);if(avcodec_send_frame(audio_codec_,audio_frame_)<0)return;AVPacket*out=av_packet_alloc();while(out&&avcodec_receive_packet(audio_codec_,out)==0){auto pts=av_rescale_q(out->pts,audio_codec_->time_base,TB),dts=av_rescale_q(out->dts,audio_codec_->time_base,TB);out->pts=pts;out->dts=dts;out->duration=out->duration>0?av_rescale_q(out->duration,audio_codec_->time_base,TB):0;packets_.push_back({out,pts,dts,false,true});last_pts_=std::max(last_pts_,pts);out=av_packet_alloc();}av_packet_free(&out);off+=n;}trim_locked(last_pts_);}
void SourceReplay::trim_locked(int64_t now){if(now==AV_NOPTS_VALUE)return;int64_t cutoff=now-(int64_t)max_seconds_*TB.den;int64_t pre=std::max<int64_t>(2*TB.den,av_rescale_q(1,fps_,TB));while(!packets_.empty()&&packets_.front().pts<cutoff-pre){av_packet_free(&packets_.front().pkt);packets_.pop_front();}}
size_t SourceReplay::buffered_seconds()const{std::lock_guard<std::mutex>l(mutex_);if(packets_.empty()||last_pts_==AV_NOPTS_VALUE)return 0;return (size_t)std::max<int64_t>(0,(last_pts_-packets_.front().pts)/TB.den);}
bool SourceReplay::save(const std::string&path){std::lock_guard<std::mutex>l(mutex_);if(packets_.empty())return false;int64_t cut=last_pts_-(int64_t)max_seconds_*TB.den;auto it=std::find_if(packets_.begin(),packets_.end(),[&](const Packet&p){return !p.audio&&p.key&&p.pts>=cut;});if(it==packets_.end())it=std::find_if(packets_.begin(),packets_.end(),[](const Packet&p){return !p.audio&&p.key;});if(it==packets_.end())return false;AVFormatContext*fmt=nullptr;if(avformat_alloc_output_context2(&fmt,nullptr,"matroska",path.c_str())<0||!fmt)return false;auto*vs=avformat_new_stream(fmt,nullptr);auto*as=avformat_new_stream(fmt,nullptr);if(!vs||!as){avformat_free_context(fmt);return false;}vs->time_base=TB;as->time_base=TB;avcodec_parameters_from_context(vs->codecpar,video_codec_);avcodec_parameters_from_context(as->codecpar,audio_codec_);if(!(fmt->oformat->flags&AVFMT_NOFILE)&&avio_open(&fmt->pb,path.c_str(),AVIO_FLAG_WRITE)<0){avformat_free_context(fmt);return false;}if(avformat_write_header(fmt,nullptr)<0){avio_closep(&fmt->pb);avformat_free_context(fmt);return false;}int64_t base=it->pts;bool ok=true;for(;it!=packets_.end();++it){auto*p=cp(it->pkt);if(!p){ok=false;break;}p->stream_index=it->audio?as->index:vs->index;p->pts=std::max<int64_t>(0,it->pts-base);p->dts=std::max<int64_t>(0,it->dts-base);if(av_interleaved_write_frame(fmt,p)<0){av_packet_free(&p);ok=false;break;}av_packet_free(&p);}if(av_write_trailer(fmt)<0)ok=false;avio_closep(&fmt->pb);avformat_free_context(fmt);return ok;}
