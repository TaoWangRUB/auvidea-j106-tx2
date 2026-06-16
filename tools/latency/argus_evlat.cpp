// argus_evlat.cpp — Argus SOF->ISP-done latency via the EVENT queue (control path),
// the robust path when frame-embedded IArgusCaptureMetadata is null.
// Per CAPTURE_COMPLETE event: (CLOCK_MONOTONIC at event pull) - getSensorTimestamp().
// A consumer thread drains the EGLStream so the pipeline keeps running (FIFO<=2).
#include <Argus/Argus.h>
#include <EGLStream/EGLStream.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace Argus;
using namespace EGLStream;
static uint64_t mono_ns(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ULL + ts.tv_nsec; }
static OutputStream* g_stream=NULL;
static std::atomic<bool> g_ready(false), g_stop(false);
static std::atomic<int> g_acq(0);
// consumer just drains frames so the producer/ISP keeps going
static void consumer_fn(){
    UniqueObj<FrameConsumer> consumer(FrameConsumer::create(g_stream));
    IFrameConsumer* iCons=interface_cast<IFrameConsumer>(consumer);
    g_ready=true;
    IEGLOutputStream* iEgl=interface_cast<IEGLOutputStream>(g_stream);
    if(iEgl->waitUntilConnected()!=STATUS_OK){ printf("RESULT consumer connect fail\n"); return; }
    while(!g_stop){
        Argus::Status st=STATUS_OK;
        UniqueObj<Frame> frame(iCons->acquireFrame(500000000ULL,&st));
        if(frame) g_acq++;
    }
}
int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    int sid=argc>1?atoi(argv[1]):0; int N=argc>2?atoi(argv[2]):150;
    int modeIdx=argc>3?atoi(argv[3]):-1;                          // -1 = auto-find 1080p
    int outW=argc>4?atoi(argv[4]):0, outH=argc>5?atoi(argv[5]):0; // 0 = sensor-mode resolution
    UniqueObj<CameraProvider> cp(CameraProvider::create());
    ICameraProvider* iProv=interface_cast<ICameraProvider>(cp);
    std::vector<CameraDevice*> devs; iProv->getCameraDevices(&devs);
    if((int)devs.size()<=sid){printf("RESULT bad sid\n");_exit(1);}
    CameraDevice* dev=devs[sid];
    ICameraProperties* iProps=interface_cast<ICameraProperties>(dev);
    std::vector<SensorMode*> modes; iProps->getAllSensorModes(&modes);
    SensorMode* mode=NULL; int mi=-1;
    if(modeIdx>=0 && modeIdx<(int)modes.size()){ mode=modes[modeIdx]; mi=modeIdx; }   // explicit mode
    else for(size_t i=0;i<modes.size();i++){ ISensorMode* im=interface_cast<ISensorMode>(modes[i]);
        Size2D<uint32_t> r=im->getResolution();
        if(r.width()==1920&&r.height()==1080){mode=modes[i];mi=(int)i;break;} }
    uint32_t mw=0,mh=0; if(mode){ ISensorMode* im=interface_cast<ISensorMode>(mode); mw=im->getResolution().width(); mh=im->getResolution().height(); }
    printf("RESULT devices=%zu sid=%d modes=%zu mode_idx=%d mode_res=%ux%u out=%dx%d\n",
        devs.size(),sid,modes.size(),mi,mw,mh, outW?outW:(int)mw, outH?outH:(int)mh);
    UniqueObj<CaptureSession> sess(iProv->createCaptureSession(dev));
    ICaptureSession* iSess=interface_cast<ICaptureSession>(sess);
    if(!iSess){printf("RESULT no session\n");_exit(1);}
    // --- EVENT QUEUE for CAPTURE_COMPLETE (control path metadata) ---
    IEventProvider* iEv=interface_cast<IEventProvider>(sess);
    std::vector<EventType> types; types.push_back(EVENT_TYPE_CAPTURE_COMPLETE);
    UniqueObj<EventQueue> queue(iEv->createEventQueue(types));
    IEventQueue* iQ=interface_cast<IEventQueue>(queue);
    if(!iQ){printf("RESULT no event queue\n");_exit(1);}
    // --- output stream + draining consumer ---
    UniqueObj<OutputStreamSettings> oss(iSess->createOutputStreamSettings(STREAM_TYPE_EGL));
    IEGLOutputStreamSettings* ioss=interface_cast<IEGLOutputStreamSettings>(oss);
    ioss->setPixelFormat(PIXEL_FMT_YCbCr_420_888);
    ioss->setResolution((outW>0&&outH>0)?Size2D<uint32_t>(outW,outH)
        :(mode?interface_cast<ISensorMode>(mode)->getResolution():Size2D<uint32_t>(1920,1080)));
    ioss->setMode(EGL_STREAM_MODE_FIFO); ioss->setFifoLength(2);
    UniqueObj<OutputStream> stream(iSess->createOutputStream(oss.get()));
    g_stream=stream.get();
    std::thread th(consumer_fn);
    while(!g_ready) usleep(2000);
    UniqueObj<Request> req(iSess->createRequest());
    IRequest* iReq=interface_cast<IRequest>(req);
    iReq->enableOutputStream(stream.get());
    ISourceSettings* iSrc=interface_cast<ISourceSettings>(iReq->getSourceSettings());
    if(mode) iSrc->setSensorMode(mode);
    iSrc->setFrameDurationRange(Range<uint64_t>(33333333));
    if(iSess->repeat(req.get())!=STATUS_OK){printf("RESULT repeat failed\n");_exit(1);}
    // --- control loop: pull CAPTURE_COMPLETE, measure now - SOF ---
    std::vector<double> lat; double expo=0, readout=0; int got=0, nullmd=0;
    while(got<N){
        iEv->waitForEvents(queue.get(), 1000000000ULL);   // 1s
        if(iQ->getSize()==0) { if(got==0) continue; else break; }
        while(iQ->getSize()>0 && got<N){
            const Event* e=iQ->getNextEvent();
            uint64_t now=mono_ns();
            if(!e) break;
            const IEventCaptureComplete* icc=interface_cast<const IEventCaptureComplete>(e);
            if(!icc) continue;
            const CaptureMetadata* md=icc->getMetadata();
            const ICaptureMetadata* imd=interface_cast<const ICaptureMetadata>(md);
            if(!imd){ nullmd++; continue; }
            uint64_t sof=imd->getSensorTimestamp();
            if(got==0) printf("RESULT raw0 sof=%llu now=%llu delta_ms=%.2f\n",
                (unsigned long long)sof,(unsigned long long)now,(double)(now-sof)/1e6);
            lat.push_back((double)(now-sof)/1e6);
            expo=(double)imd->getSensorExposureTime()/1e6;
            readout=(double)imd->getFrameReadoutTime()/1e6;
            got++;
        }
    }
    g_stop=true; th.join();
    iSess->stopRepeat(); iSess->waitForIdle();
    printf("RESULT got=%d nullmd=%d consumer_frames=%d\n",got,nullmd,(int)g_acq);
    if(lat.size()<20){printf("RESULT too few\n");fflush(stdout);_exit(1);}
    std::vector<double> x(lat.begin()+15,lat.end());
    std::sort(x.begin(),x.end()); size_t n=x.size();
    double s=0; for(double v:x)s+=v; double mean=s/n;
    double var=0; for(double v:x)var+=(v-mean)*(v-mean); var/=n;
    printf("RESULT SOF->ISPdone n=%zu mean=%.1f median=%.1f min=%.1f max=%.1f p95=%.1f std=%.1f exposure=%.1f readout=%.1f isp_est=%.1f frames=%.2f\n",
        n,mean,x[n/2],x[0],x[n-1],x[(size_t)(n*0.95)],sqrt(var),expo,readout,mean-readout,mean/33.333);
    fflush(stdout); _exit(0);
}
