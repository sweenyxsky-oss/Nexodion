// Adapted from PS5 React relauncher: Copyright (C) 2026 half144 and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Nexodion adaptation launches only syntactically validated PS4/PS5 title IDs.
// It does not acquire privileges; execution requires an existing ELF loader.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
typedef struct {uint32_t size,user_id,app_opt;uint64_t crash_report;uint32_t check_flag;} LncAppParam;
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int*);
int sceLncUtilLaunchApp(const char*,const char**,LncAppParam*);
static int read_line(char* line,size_t capacity) {
    size_t length=0;char c;
    while(read(STDIN_FILENO,&c,1)==1) {if(c=='\n'){line[length]=0;return 1;}if(length+1>=capacity)return 0;line[length++]=c;}
    return 0;
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    char magic[8],title[16];
    if(!read_line(magic,sizeof magic) || strcmp(magic,"NXL1") || !read_line(title,sizeof title) || strlen(title)!=9) return 1;
    if(strncmp(title,"PPSA",4) && strncmp(title,"CUSA",4)) return 1;
    for(int i=4;i<9;++i) if(title[i]<'0' || title[i]>'9') return 1;
    int user=0;
    int result=sceUserServiceInitialize(NULL);
    if(result>=0) result=sceUserServiceGetForegroundUser(&user);
    LncAppParam param={sizeof param,(uint32_t)user,0,0,0};
    if(result>=0) result=sceLncUtilLaunchApp(title,NULL,&param);
    if(result<0){char error[64];snprintf(error,sizeof error,"fail %08x\n",(unsigned)result);write(STDOUT_FILENO,error,strlen(error));return 1;}
    write(STDOUT_FILENO,"ok\n",3);return 0;
}
