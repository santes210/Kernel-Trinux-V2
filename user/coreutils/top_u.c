#include "../trinux.h"
static struct {uint32_t pid; char name[32]; uint32_t cpu_ticks; uint32_t state; int priority;} proc_list[64];
static const char *st_name(uint32_t s){switch(s){case 0:return"RUN";case 1:return"RDY";case 2:return"SLP";case 3:return"ZMB";} return "?";}
static void pad_to(int n, int width){
    int seen=0; uint32_t v=(uint32_t)n; if(n==0) seen=1;
    while(v){seen++; v/=10;}
    for(int i=seen;i<width;i++) putchar_(' ');
}
static void draw(void){
    vga_goto(0,0);
    vga_color(11, 4);
    print("Trinux top    ");
    char host[32]; hostname(host,sizeof(host));
    print(host); print("    uptime: "); print_unum(uptime()); print(" s        (press q to quit)");
    for(int i=0;i<14;i++) putchar_(' ');
    vga_color(7, 0); print("\n");
    mem_info_t mi; meminfo_(&mi);
    print("Mem: "); print_unum(mi.used_bytes/(1024*1024)); print("/");
    print_unum(mi.total_bytes/(1024*1024)); print(" MiB used  (");
    print_unum(mi.free_bytes/(1024*1024)); print(" MiB free)");
    for(int i=0;i<30;i++) putchar_(' '); print("\n");
    df_info_t di; dfinfo_(&di);
    print("Disk: ");
    if(di.have_disk){print_unum(di.used_blocks*(di.block_size/1024)); print("/");
        print_unum(di.total_blocks*(di.block_size/1024)); print(" KiB");}
    else print("RAM only");
    for(int i=0;i<40;i++) putchar_(' '); print("\n\n");
    vga_color(0, 7);
    print("  PID  ST   PRI    TICKS   NAME                              ");
    vga_color(7, 0); print("\n");
    plist_req_t r; r.list = (void*)proc_list; r.max = 64;
    int n = listproc_(&r);
    for(int i=0;i<n && i<42;i++){   /* 50 filas - 8 de header/etc */
        pad_to((int)proc_list[i].pid, 5);
        print_unum(proc_list[i].pid); print("  ");
        print(st_name(proc_list[i].state)); print("  ");
        if(proc_list[i].priority>=0) putchar_(' ');
        print_num(proc_list[i].priority); print("   ");
        pad_to((int)proc_list[i].cpu_ticks, 8);
        print_unum(proc_list[i].cpu_ticks); print("    ");
        int j=0; while(proc_list[i].name[j] && j<31){ putchar_(proc_list[i].name[j]); j++; }
        while(j<32){ putchar_(' '); j++; }
        print(" \n");
    }
    for(int row=n+5; row<49; row++){
        for(int c=0;c<78;c++) putchar_(' '); print("\n");
    }
}
int main(int argc, char **argv){(void)argc;(void)argv;
    vga_clear_();
    for(;;){
        draw();
        for(int i=0;i<20;i++){
            int k = key_poll();
            if(k=='q' || k=='Q'){ vga_clear_(); vga_goto(0,0); return 0; }
            msleep(50);
        }
    }
}
