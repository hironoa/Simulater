#include "sim_csv.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"CSV table line %d: %s\n",__LINE__,#x); } } while (0)
int main(void)
{
    const char *source=" \"MoDi\" ,intr_tm00,t,1,1\n#COMMENT,g_ms_tick,g_ms_tick\n#InitWheneverCall,g_ms_tick,g_mode\n0,100,3\n,7,8\n,9,10\nmod,intr_tm00,second,0,0\n#COMMENT\nstep 1\n";
    const char *duplicate="modi,f,t,0,0\n#InitOnce,g_mode\n0,1\n#InitOnce,g_mode\n0,2\n#COMMENT\nstep 1\n";
    char err[512];
    int mode;
    for (mode=0;mode<3;mode++) {
        sim_csv_table t;
        CHECK(sim_csv_load_table(source,strlen(source),&t,err,sizeof err)==0);
        if (!t.func) continue;
        CHECK(t.nblocks==2 && t.nrows==2 && t.ninit==2 && t.nin==1 && t.nout==1);
        CHECK(t.init_every==1 && !strcmp(t.init_cells[0],"100"));
        sim_csv_table_expand_init(&t,mode);
        CHECK(t.nin==3 && t.init_columns==2);
        CHECK(!strcmp(t.names[0],"g_ms_tick") && !strcmp(t.names[2],"g_ms_tick"));
        CHECK(!strcmp(t.cells[0][0],"100") && !strcmp(t.cells[0][1],"3") && !strcmp(t.cells[0][2],"7"));
        CHECK(mode==1 ? t.cells[1][0]==NULL : t.cells[1][0] && !strcmp(t.cells[1][0],"100"));
        CHECK(!strcmp(t.cells[1][2],"9"));
        sim_csv_table_free(&t);
    }
    { sim_csv_table t; CHECK(sim_csv_load_table(duplicate,strlen(duplicate),&t,err,sizeof err)!=0); }
    printf("CSV table checks=%u failures=%u\n",checks,failures);
    return !checks || failures;
}
