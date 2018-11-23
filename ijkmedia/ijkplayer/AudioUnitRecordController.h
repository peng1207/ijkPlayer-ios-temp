//
//  AudioUnitRecordController.h
//  IJKMediaFramework
//
//  Created by chengzong liang on 2018/2/1.
//  Copyright © 2018年 bilibili. All rights reserved.
//

#ifndef AudioUnitRecordController_h
#define AudioUnitRecordController_h

void startRecord();

int getRecordFrameData(char *pcmData);

void stopRecord();

#endif /* AudioUnitRecordController_h */
