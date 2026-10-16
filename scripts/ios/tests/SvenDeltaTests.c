#include "../../../engine/common/net_encode.c"
#include <assert.h>
#include <setjmp.h>

static jmp_buf bad_packet;
static qboolean expect_bad_packet;

host_parm_t host;
void Con_Printf(const char *format, ...) { (void)format; }
void Con_DPrintf(const char *format, ...) { (void)format; }
void Con_Reportf(const char *format, ...) { (void)format; }
int Cvar_VariableInteger(const char *name) { (void)name; return 0; }
void Host_Error(const char *format, ...) {
 if(expect_bad_packet) longjmp(bad_packet,1);
 va_list args; va_start(args,format); vfprintf(stderr,format,args); va_end(args); abort();
}
void Sys_Error(const char *format, ...) {
 va_list args; va_start(args,format); vfprintf(stderr,format,args); va_end(args); abort();
}
void DBG_AssertFunction(qboolean expression,const char *text,const char *file,int line,const char *message) {
 (void)file; (void)line; (void)message; if(!expression) { fprintf(stderr,"assert: %s\n",text); abort(); }
}
void *_Mem_Realloc(poolhandle_t pool,void *pointer,size_t size,qboolean clear,const char *file,int line) {
 (void)pool; (void)clear; (void)file; (void)line; return realloc(pointer,size);
}
void _Mem_Free(void *pointer,const char *file,int line) { (void)file; (void)line; free(pointer); }

int main(int argc,char **argv) {
 assert(argc==5);
 unsigned char data[20000]={0};
 FILE *file=fopen(argv[1],"rb"); assert(file);
 size_t size=fread(data,1,sizeof(data)-8,file); fclose(file);
 sizebuf_t message; MSG_Init(&message,"descriptions",data,size);
 const int indices[]={DT_EVENT_T,DT_WEAPONDATA_T,DT_USERCMD_T,DT_CUSTOM_ENTITY_STATE_T,
                      DT_ENTITY_STATE_PLAYER_T,DT_ENTITY_STATE_T,DT_CLIENTDATA_T};
 const int counts[]={14,22,15,20,51,57,34};
 for(unsigned index=0;index<7;++index) {
  if(index) assert(MSG_ReadByte(&message)==14);
  Delta_ParseTableField_GS(&message);
  assert(dt_info[indices[index]].numFields==counts[index]);
 }
 assert(MSG_ReadByte(&message)==44);
 assert(!strcmp(dt_info[DT_CLIENTDATA_T].pFields[0].name,"flTimeStepSound"));
 assert(!strcmp(dt_info[DT_CLIENTDATA_T].pFields[26].name,"fov"));
 file=fopen(argv[2],"rb"); assert(file);
 memset(data,0,sizeof(data)); size=fread(data,1,sizeof(data)-8,file); fclose(file);
 MSG_Init(&message,"clientdata",data,size); MSG_StartBitWriting(&message);
 assert(MSG_ReadOneBit(&message)==0);
 clientdata_t from={0},to={0};
 Delta_ReadGSFields(&message,DT_CLIENTDATA_T,&from,&to,0);
 weapon_data_t weapon={0};
 unsigned weapons=0;
 while(MSG_ReadOneBit(&message)) {
  unsigned id=MSG_ReadUBitLong(&message,8); assert(id<256);
  Delta_ReadGSFields(&message,DT_WEAPONDATA_T,&weapon,&weapon,0);
  assert(++weapons<256 && !MSG_CheckOverflow(&message));
 }
 MSG_EndBitWriting(&message);
 int next=MSG_ReadByte(&message);
 printf("clientdata consumed=%d next=%d fov=%f health=%f physinfo=%s weapons=%u\n",MSG_GetNumBitsRead(&message)-8,next,to.fov,to.health,to.physinfo,weapons);
 assert(!MSG_CheckOverflow(&message)); assert(next==40);
 assert(to.health==atoi(argv[3]));
 assert(MSG_GetNumBitsRead(&message)-8==atoi(argv[4]));
 expect_bad_packet=true;
 memset(data,0,sizeof(data)); memcpy(data,"event_t",8); data[8]=14;
 MSG_Init(&message,"truncated",data,11);
 if(!setjmp(bad_packet)) { Delta_ParseTableField_GS(&message); assert(0); }
 data[8]=255; data[9]=255;
 MSG_Init(&message,"oversized",data,10);
 if(!setjmp(bad_packet)) { Delta_ParseTableField_GS(&message); assert(0); }
 expect_bad_packet=false;
 delta_init=true; Delta_Shutdown();
 puts("PASS: complete captured delta descriptions and exact next-message boundary");
}
