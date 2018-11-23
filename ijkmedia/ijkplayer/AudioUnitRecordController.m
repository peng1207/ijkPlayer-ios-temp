//
//  AudioUnitRecordController.m
//  Aruts
//
//  Created by chengzong liang on 2018/2/2.
//

#import "AudioUnitRecordController.h"
#import <AudioToolbox/AudioToolbox.h>
#import <UIKit/UIKit.h>
#include "ring_buffer.h"
#define VQE_READY_ITEM_NUM      30

#ifndef max
#define max( a, b ) ( ((a) > (b)) ? (a) : (b) )
#endif

#ifndef min
#define min( a, b ) ( ((a) < (b)) ? (a) : (b) )
#endif



#define kOutputBus 0
#define kInputBus 1

static pcm_ring_buffer *rbuf_vqe_ready;

@interface AudioUnitRecordController :NSObject{
    AudioComponentInstance audioUnit;
    AudioBuffer tempBuffer;
}

@property (readonly) AudioComponentInstance audioUnit;
@property (readonly) AudioBuffer tempBuffer;

- (void) start;
- (void) stop;
- (void) processAudio: (AudioBufferList*) bufferList;
@end

AudioUnitRecordController* iosAudio;

void checkStatus(int status){
    if (status) {
        printf("Status not 0! %d\n", status);
        //        exit(1);
    }
}


#define TMP_MAX_BUF_LEN 1024

#define MAX_BUF_LEN 2048

static char tmp_buffer[TMP_MAX_BUF_LEN];

static char audio_buffer[MAX_BUF_LEN];
static int buffer_pos = 0;
/**
 This callback is called when new audio data from the microphone is
 available.
 */
static OSStatus recordingCallback(void *inRefCon,
                                  AudioUnitRenderActionFlags *ioActionFlags,
                                  const AudioTimeStamp *inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList *ioData) {
    
    // Because of the way our audio format (setup below) is chosen:
    // we only need 1 buffer, since it is mono
    // Samples are 16 bits = 2 bytes.
    // 1 frame includes only 1 sample
    
    AudioBuffer buffer;
    
    buffer.mNumberChannels = 1;
    buffer.mDataByteSize = inNumberFrames * 2;
    buffer.mData = malloc( inNumberFrames * 2 );
    
    // Put buffer in a AudioBufferList
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0] = buffer;
    
    // Then:
    // Obtain recorded samples
    
    OSStatus status;
    
    status = AudioUnitRender([iosAudio audioUnit],
                             ioActionFlags,
                             inTimeStamp,
                             inBusNumber,
                             inNumberFrames,
                             &bufferList);
    checkStatus(status);
    
    // Now, we have the samples we just read sitting in buffers in bufferList
    // Process the new data
    //NSLog(@"record =============");
    //[iosAudio processAudio:&bufferList];
    
//    for(int index = 0; index < inNumberFrames; index++)
//    {
//        //        if(last_dropped){
//        audio_buffer[buffer_pos++] = tmp_buffer[index];
//        //        }
//        //        last_dropped = !last_dropped;
//    }
    memcpy(audio_buffer + buffer_pos, buffer.mData, buffer.mDataByteSize);
    buffer_pos+=buffer.mDataByteSize;
    
    int oldPos = 0;
    
    while(buffer_pos >= 320){
        
        //        NSString * docsdir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) objectAtIndex:0];
        //        NSString *dataFilePath = [docsdir stringByAppendingPathComponent:@"rec.pcm"]; // 在指定目录下创建 文件夹
        //        FILE *fp = fopen([dataFilePath UTF8String], "ab+");
        //        if(fp != NULL){
        //            fwrite(buffer + oldPos, sizeof(int16_t), dev->_period, fp);
        //            fclose(fp);
        //            fp = NULL;
        //        }
//        if(rec_cb != NULL)
//            rec_cb(buffer + oldPos, dev->_period * 2);
        if([[[UIDevice currentDevice] systemVersion] floatValue] >= 11.0){
            short *tmpShort = (short *)(audio_buffer + oldPos);
            int32_t tmp32;
            for(int i = 0; i<160; i++){
                tmp32 = ((int32_t)*tmpShort) << 3;
                if (tmp32 > 32767) {
                    *tmpShort = 32767;
                } else if(tmp32 < -32768){
                    *tmpShort = -32768;
                }
                else{
                    *tmpShort = tmp32;
                }
                tmpShort++;
            }
        }
        ring_buffer_push(rbuf_vqe_ready, audio_buffer + oldPos, 320, 0);
        
        oldPos += 320;
        buffer_pos -= 320;
    }
    
    if(buffer_pos > 0)
        memmove(audio_buffer, audio_buffer+oldPos, buffer_pos);
   // ring_buffer_push(rbuf_vqe_ready, bufferList.mBuffers[0].mData, bufferList.mBuffers[0].mDataByteSize, 0);

    
    // release the malloc'ed data in the buffer we created earlier
    free(bufferList.mBuffers[0].mData);
    
    return noErr;
}

/**
 This callback is called when the audioUnit needs new data to play through the
 speakers. If you don't have any, just don't write anything in the buffers
 */
static OSStatus playbackCallback(void *inRefCon,
                                 AudioUnitRenderActionFlags *ioActionFlags,
                                 const AudioTimeStamp *inTimeStamp,
                                 UInt32 inBusNumber,
                                 UInt32 inNumberFrames,
                                 AudioBufferList *ioData) {
    // Notes: ioData contains buffers (may be more than one!)
    // Fill them up as much as you can. Remember to set the size value in each buffer to match how
    // much data is in the buffer.
    
//    for (int i=0; i < ioData->mNumberBuffers; i++) { // in practice we will only ever have 1 buffer, since audio format is mono
//        AudioBuffer buffer = ioData->mBuffers[i];
//
//        //        NSLog(@"  Buffer %d has %d channels and wants %d bytes of data.", i, buffer.mNumberChannels, buffer.mDataByteSize);
//
//        //         copy temporary buffer data to output buffer
//        UInt32 size = min(buffer.mDataByteSize, [iosAudio tempBuffer].mDataByteSize); // dont copy more data then we have, or then fits
//        memcpy(buffer.mData, [iosAudio tempBuffer].mData, size);
//        buffer.mDataByteSize = size; // indicate how much data we wrote in the buffer
//
//        // uncomment to hear random noise
//
//        //        UInt16 *frameBuffer = buffer.mData;
//        //        for (int j = 0; j < inNumberFrames; j++) {
//        //            frameBuffer[j] = rand();
//        //        }
//
//
//    }
    
    return noErr;
}

@implementation AudioUnitRecordController
@synthesize audioUnit, tempBuffer;

/**
 Initialize the audioUnit and allocate our own temporary buffer.
 The temporary buffer will hold the latest data coming in from the microphone,
 and will be copied to the output when this is requested.
 */
- (id) init {
    self = [super init];
    
    OSStatus status;
    
    // Describe audio component
    AudioComponentDescription desc;
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    
    // Get component
    AudioComponent inputComponent = AudioComponentFindNext(NULL, &desc);
    
    // Get audio units
    status = AudioComponentInstanceNew(inputComponent, &audioUnit);
    checkStatus(status);
    
    // Enable IO for recording
    UInt32 flag = 1;
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Input,
                                  kInputBus,
                                  &flag,
                                  sizeof(flag));
    checkStatus(status);
    
    // Enable IO for playback
//    status = AudioUnitSetProperty(audioUnit,
//                                  kAudioOutputUnitProperty_EnableIO,
//                                  kAudioUnitScope_Output,
//                                  kOutputBus,
//                                  &flag,
//                                  sizeof(flag));
    checkStatus(status);
    
    // Describe format
    AudioStreamBasicDescription audioFormat;
    audioFormat.mSampleRate            = 16000;
    audioFormat.mFormatID            = kAudioFormatLinearPCM;
    audioFormat.mFormatFlags        = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    audioFormat.mFramesPerPacket    = 1;
    audioFormat.mChannelsPerFrame    = 1;
    audioFormat.mBitsPerChannel        = 16;
    audioFormat.mBytesPerPacket        = 2;
    audioFormat.mBytesPerFrame        = 2;
    
    // Apply format
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  kInputBus,
                                  &audioFormat,
                                  sizeof(audioFormat));
    checkStatus(status);
//    status = AudioUnitSetProperty(audioUnit,
//                                  kAudioUnitProperty_StreamFormat,
//                                  kAudioUnitScope_Input,
//                                  kOutputBus,
//                                  &audioFormat,
//                                  sizeof(audioFormat));
//    checkStatus(status);
    
    
    // Set input callback
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = recordingCallback;
    callbackStruct.inputProcRefCon = (__bridge void * _Nullable)(self);
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Global,
                                  kInputBus,
                                  &callbackStruct,
                                  sizeof(callbackStruct));
    checkStatus(status);
    
    // Set output callback
//    callbackStruct.inputProc = playbackCallback;
//    callbackStruct.inputProcRefCon = (__bridge void * _Nullable)(self);
//    status = AudioUnitSetProperty(audioUnit,
//                                  kAudioUnitProperty_SetRenderCallback,
//                                  kAudioUnitScope_Global,
//                                  kOutputBus,
//                                  &callbackStruct,
//                                  sizeof(callbackStruct));
//    checkStatus(status);
    
    // Disable buffer allocation for the recorder (optional - do this if we want to pass in our own)
    flag = 0;
    status = AudioUnitSetProperty(audioUnit,
                                  kAudioUnitProperty_ShouldAllocateBuffer,
                                  kAudioUnitScope_Output,
                                  kInputBus,
                                  &flag,
                                  sizeof(flag));
    
    // Allocate our own buffers (1 channel, 16 bits per sample, thus 16 bits per frame, thus 2 bytes per frame).
    // Practice learns the buffers used contain 512 frames, if this changes it will be fixed in processAudio.
    tempBuffer.mNumberChannels = 1;
    tempBuffer.mDataByteSize = 512 * 18;
    tempBuffer.mData = malloc( 512 * 18 );
    
    // Initialise
    status = AudioUnitInitialize(audioUnit);
    checkStatus(status);
    
    return self;
}

/**
 Start the audioUnit. This means data will be provided from
 the microphone, and requested for feeding to the speakers, by
 use of the provided callbacks.
 */
- (void) start {
    OSStatus status = AudioOutputUnitStart(audioUnit);
    checkStatus(status);
    
    
    //    UInt32 audioRouteOverride =  kAudioSessionOverrideAudioRoute_Speaker;
    //
    //    OSStatus result =     AudioSessionSetProperty (kAudioSessionProperty_OverrideAudioRoute,
    //                                                  sizeof (audioRouteOverride),
    //                                                  &audioRouteOverride);
    //    if (result){
    //        NSLog(@"ERROR AudioSessionSetProperty Speaker route !");
    //    } else{
    //        NSLog(@"set speaker to yes");
    //    }
    //
    //    AudioSessionSetActive(true);
}

/**
 Stop the audioUnit
 */
- (void) stop {
    OSStatus status = AudioOutputUnitStop(audioUnit);
    checkStatus(status);
    AudioComponentInstanceDispose(audioUnit);
}

/**
 Change this funtion to decide what is done with incoming
 audio data from the microphone.
 Right now we copy it to our own temporary buffer.
 */
- (void) processAudio: (AudioBufferList*) bufferList{
    AudioBuffer sourceBuffer = bufferList->mBuffers[0];
    
    // fix tempBuffer size if it's the wrong size
    if (tempBuffer.mDataByteSize != sourceBuffer.mDataByteSize) {
        free(tempBuffer.mData);
        tempBuffer.mDataByteSize = sourceBuffer.mDataByteSize;
        tempBuffer.mData = malloc(sourceBuffer.mDataByteSize);
    }
    
    // copy incoming audio data to temporary buffer
    memcpy(tempBuffer.mData, bufferList->mBuffers[0].mData, bufferList->mBuffers[0].mDataByteSize);
    NSLog(@"rbuf_vqe_ready = %p, tempBuffer.mDataByteSize = %d", rbuf_vqe_ready, (unsigned int)tempBuffer.mDataByteSize);
    
     //ring_buffer_push(rbuf_vqe_ready, tempBuffer.mData, tempBuffer.mDataByteSize, 0);
}

/**
 Clean up.
 */
//- (void) dealloc {
//    [super    dealloc];
//    //AudioUnitUninitialize(audioUnit);
//    //free(tempBuffer.mData);
//}

@end

void startRecord(){
    rbuf_vqe_ready = (pcm_ring_buffer *)malloc(sizeof(pcm_ring_buffer));
    ring_buffer_init(rbuf_vqe_ready, VQE_READY_ITEM_NUM);
    iosAudio = [[AudioUnitRecordController alloc] init];
    [iosAudio start];
}

int getRecordFrameData(char *pcmData){
    if(pcmData == NULL) {
        printf("pcm data ptr cannot null\n");
        return -1;
    }
    
    item_data *item = ring_buffer_peek(rbuf_vqe_ready);
    if(item != NULL) {
        memcpy(pcmData , item->buffer, item->length);
        ring_buffer_pop(rbuf_vqe_ready);
        return item->length;
    }
    return 0;
}

void stopRecord(){
    [iosAudio stop];
    ring_buffer_deinit(rbuf_vqe_ready);
    free(rbuf_vqe_ready);
    rbuf_vqe_ready = NULL;
}

