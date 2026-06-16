// v4l2_lat.c — raw capture sensor->app latency on /dev/videoN.
// Per frame: CLOCK_MONOTONIC at DQBUF - buf.timestamp (kernel SOF, monotonic).
// = sensor readout + MIPI + VI DMA + dequeue (ISP bypassed).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/videodev2.h>
static double med(double*a,int n){ for(int i=0;i<n;i++)for(int j=i+1;j<n;j++) if(a[j]<a[i]){double t=a[i];a[i]=a[j];a[j]=t;} return a[n/2]; }
int main(int argc,char**argv){
    const char*dev=argc>1?argv[1]:"/dev/video0";
    int W=argc>2?atoi(argv[2]):1920, H=argc>3?atoi(argv[3]):1080, N=argc>4?atoi(argv[4]):150;
    int fd=open(dev,O_RDWR); if(fd<0){perror("open");return 1;}
    struct v4l2_format f; memset(&f,0,sizeof f); f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width=W; f.fmt.pix.height=H; f.fmt.pix.pixelformat=V4L2_PIX_FMT_SRGGB10; f.fmt.pix.field=V4L2_FIELD_NONE;
    if(ioctl(fd,VIDIOC_S_FMT,&f)<0){perror("S_FMT");return 1;}
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb); rb.count=4; rb.type=f.type; rb.memory=V4L2_MEMORY_MMAP;
    if(ioctl(fd,VIDIOC_REQBUFS,&rb)<0){perror("REQBUFS");return 1;}
    void*buf[4]; size_t len[4];
    for(int i=0;i<(int)rb.count;i++){ struct v4l2_buffer b; memset(&b,0,sizeof b); b.type=f.type; b.memory=V4L2_MEMORY_MMAP; b.index=i;
        ioctl(fd,VIDIOC_QUERYBUF,&b); len[i]=b.length; buf[i]=mmap(0,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
        ioctl(fd,VIDIOC_QBUF,&b); }
    int type=f.type; if(ioctl(fd,VIDIOC_STREAMON,&type)<0){perror("STREAMON");return 1;}
    double lat[4096]; int n=0;
    for(int i=0;i<N+20 && n<4000;i++){
        struct v4l2_buffer b; memset(&b,0,sizeof b); b.type=f.type; b.memory=V4L2_MEMORY_MMAP;
        if(ioctl(fd,VIDIOC_DQBUF,&b)<0){ if(errno==EAGAIN){i--;continue;} perror("DQBUF"); break; }
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        double now=ts.tv_sec*1e3+ts.tv_nsec/1e6;
        double bt=b.timestamp.tv_sec*1e3+b.timestamp.tv_usec/1e3;
        if(i>=20) lat[n++]=now-bt;       // drop warmup
        ioctl(fd,VIDIOC_QBUF,&b);
    }
    ioctl(fd,VIDIOC_STREAMOFF,&type); close(fd);
    if(n<10){printf("RESULT too few (%d)\n",n);return 1;}
    double s=0,mn=1e9,mx=0; for(int i=0;i<n;i++){s+=lat[i]; if(lat[i]<mn)mn=lat[i]; if(lat[i]>mx)mx=lat[i];}
    double mean=s/n; double v=0; for(int i=0;i<n;i++)v+=(lat[i]-mean)*(lat[i]-mean); v/=n;
    double m=med(lat,n);
    printf("RESULT %s %dx%d n=%d mean=%.1f median=%.1f min=%.1f max=%.1f std=%.1f frames=%.2f\n",
        dev,W,H,n,mean,m,mn,mx,sqrt(v),mean/33.333);
    return 0;
}
