// app/bus_api_test/main.c — full HIL API suite over the bus to SAMD21 slave 3.
//
// Exercises the slave's app-shell + interlocks via DATA(OP_SHELL_EXEC) commands:
//   ECHO, SYSINFO, STACK_HWM, GPIO loopback (D8->D9), DAC->ADC loopback (A0->A1),
//   interlock arm-noop/status/disarm, and a DSL digital interlock tripped by
//   driving D8 (jumpered to D9) — observed via the poll NO_MESSAGE summary byte.
//
// Wiring: Pico TX GP15 -> XIAO D7, Pico RX GP16 <- XIAO D6, GND. XIAO jumpers
// A0->A1 (D0/PA02 DAC -> D1/PA04 AIN4) and D8->D9 (PA07 -> PA05).
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bus_phy.h"
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"

#define SLAVE          3u
#define OP_SHELL_EXEC  0x0109u
#define OP_SHELL_REPLY 0x0011u
// command ids
#define CMD_ECHO            0x0001u
#define CMD_SYSINFO         0x0002u
#define CMD_STACK_HWM       0x0050u
#define CMD_GPIO_CONFIG     0x0100u
#define CMD_GPIO_WRITE      0x0101u
#define CMD_GPIO_READ       0x0102u
#define CMD_DAC_WRITE       0x0103u
#define CMD_ADC_READ        0x0104u
#define CMD_INTERLOCK_STATUS    0x0140u
#define CMD_INTERLOCK_ARM_NOOP  0x0141u
#define CMD_INTERLOCK_DISARM    0x0142u
#define CMD_INTERLOCK_SET       0x0143u
// gpio: port 0 = PA; modes
#define GP_IN 0u
#define GP_OUT 1u
// XIAO pins
#define D8_PORT 0u
#define D8_PIN  7u
#define D9_PORT 0u
#define D9_PIN  5u
#define A1_ADC_CH 4u   // D1/PA04 = AIN4

static bus_asm_t g_bc;
static uint8_t   g_seq;
static uint16_t  g_rid;
static int g_pass, g_fail;

static int recv(bus_frame_t *out, int ms) {
    uint16_t w;
    for (int t = 0; t < ms * 20; t++) {
        if (bus_phy_rx_pop(&w)) { if (bus_asm_feed(&g_bc, w, out)) return 1; }
        else sleep_us(50);
    }
    return 0;
}
static void send(uint8_t type, const uint8_t *p, uint8_t len) {
    bus_frame_t f; memset(&f, 0, sizeof f);
    f.dest = SLAVE; f.src = BUS_ADDR_MASTER; f.type = type; f.seq = g_seq++; f.len = len;
    if (len) memcpy(f.payload, p, len);
    uint16_t words[BUS_FRAME_WORDS_MAX];
    bus_phy_rx_flush();
    bus_phy_send_words(words, bus_frame_encode(words, &f));
}

// Run one command. Returns status (0=OK, <0 = no reply); result copied to res/res_len.
static int run_cmd(uint16_t cmd, const uint8_t *args, uint8_t alen, uint8_t *res, uint8_t *rlen) {
    uint8_t p[200]; uint8_t n = 0;
    p[n++] = OP_SHELL_EXEC & 0xFF; p[n++] = OP_SHELL_EXEC >> 8;
    uint16_t rid = ++g_rid;
    p[n++] = rid & 0xFF; p[n++] = rid >> 8;
    p[n++] = cmd & 0xFF; p[n++] = cmd >> 8;
    if (alen) { memcpy(&p[n], args, alen); n += alen; }
    send(BUS_FT_DATA, p, n);

    bus_frame_t rf;
    if (!(recv(&rf, 60) && (rf.type & BUS_FT_MASK) == BUS_FT_ACK)) { /* keep going; some ACKs race */ }

    for (int k = 0; k < 10; k++) {
        send(BUS_FT_POLL, NULL, 0);
        if (recv(&rf, 40) && (rf.type & BUS_FT_MASK) == BUS_FT_DATA && rf.len >= 5 &&
            rf.payload[0] == (OP_SHELL_REPLY & 0xFF) && rf.payload[1] == (OP_SHELL_REPLY >> 8)) {
            uint8_t status = rf.payload[4];
            uint8_t rl = (uint8_t)(rf.len - 5);
            if (res && rlen) { *rlen = rl; if (rl) memcpy(res, &rf.payload[5], rl); }
            return status;
        }
        sleep_ms(3);
    }
    return -1;  // no reply
}

// Poll once, return the NO_MESSAGE summary byte (bit0 = an armed interlock tripped), or -1.
static int poll_summary(void) {
    send(BUS_FT_POLL, NULL, 0);
    bus_frame_t rf;
    if (recv(&rf, 40) && (rf.type & BUS_FT_MASK) == BUS_FT_NO_MESSAGE && rf.len >= 1)
        return rf.payload[0];
    return -1;
}

static void check(const char *name, int ok, const char *detail) {
    if (ok) { g_pass++; printf("  [PASS] %-22s %s\n", name, detail); }
    else    { g_fail++; printf("  [FAIL] %-22s %s\n", name, detail); }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    bus_phy_init(BUS_DEFAULT_BAUD);
    bus_asm_init(&g_bc, BUS_ADDR_MASTER, true);

    for (uint32_t round = 0; ; round++) {
        g_pass = g_fail = 0;
        printf("\n===== bus API suite to slave %u (round %u) =====\n", SLAVE, (unsigned)round);
        uint8_t res[200], rl; char d[80];

        // 1. ECHO
        { uint8_t a[8]; uint8_t n=0; a[n++]=3; a[n++]=0; a[n++]='H'; a[n++]='I'; a[n++]='!';
          int s = run_cmd(CMD_ECHO, a, n, res, &rl);
          int ok = (s==0 && rl==5 && res[2]=='H' && res[3]=='I' && res[4]=='!');
          snprintf(d,sizeof d,"status=%d echo='%c%c%c'", s, rl>2?res[2]:'?', rl>3?res[3]:'?', rl>4?res[4]:'?');
          check("echo", ok, d); }

        // 2. SYSINFO
        { int s = run_cmd(CMD_SYSINFO, NULL, 0, res, &rl);
          snprintf(d,sizeof d,"status=%d len=%u", s, rl); check("sysinfo", s==0, d); }

        // 3. STACK_HWM -> [hwm:u16][size:u16][canary:u8]
        { int s = run_cmd(CMD_STACK_HWM, NULL, 0, res, &rl);
          uint16_t hwm = rl>=2?(res[0]|(res[1]<<8)):0, sz = rl>=4?(res[2]|(res[3]<<8)):0;
          snprintf(d,sizeof d,"hwm=%u/%u canary=%u", hwm, sz, rl>=5?res[4]:0);
          check("stack_hwm", s==0 && rl>=5 && res[4]==0, d); }

        // 4. GPIO loopback D8(out) -> D9(in)
        { uint8_t a[3];
          a[0]=D8_PORT;a[1]=D8_PIN;a[2]=GP_OUT;  run_cmd(CMD_GPIO_CONFIG,a,3,0,0);
          a[0]=D9_PORT;a[1]=D9_PIN;a[2]=GP_IN;   run_cmd(CMD_GPIO_CONFIG,a,3,0,0);
          int r1=-1,r0=-1;
          a[0]=D8_PORT;a[1]=D8_PIN;a[2]=1; run_cmd(CMD_GPIO_WRITE,a,3,0,0); sleep_ms(2);
          { uint8_t g[2]={D9_PORT,D9_PIN}; if(run_cmd(CMD_GPIO_READ,g,2,res,&rl)==0&&rl>=1) r1=res[0]; }
          a[2]=0; run_cmd(CMD_GPIO_WRITE,a,3,0,0); sleep_ms(2);
          { uint8_t g[2]={D9_PORT,D9_PIN}; if(run_cmd(CMD_GPIO_READ,g,2,res,&rl)==0&&rl>=1) r0=res[0]; }
          snprintf(d,sizeof d,"D8=1->D9=%d  D8=0->D9=%d", r1, r0);
          check("gpio_loopback", r1==1 && r0==0, d); }

        // 5. DAC->ADC loopback A0->A1 (ADC ~ 4x DAC)
        { const uint16_t dvals[3] = {200, 500, 800}; int allok=1; char buf[80]=""; int off=0;
          for (int i=0;i<3;i++){
            uint8_t a[2]={dvals[i]&0xFF, dvals[i]>>8}; run_cmd(CMD_DAC_WRITE,a,2,0,0); sleep_ms(5);
            uint8_t aa[3]={A1_ADC_CH,0,0}; uint16_t adc=0;
            if (run_cmd(CMD_ADC_READ,aa,3,res,&rl)==0 && rl>=2) adc=res[0]|(res[1]<<8);
            int exp=dvals[i]*4; int ok = (adc > exp-exp/4 && adc < exp+exp/4);
            allok &= ok; off+=snprintf(buf+off,sizeof buf-off,"%u->%u ",dvals[i],adc);
          }
          check("dac_adc_loopback", allok, buf); }

        // 6. interlock arm-noop / status / disarm (path)
        { uint8_t a[1]={0};
          int s1=run_cmd(CMD_INTERLOCK_ARM_NOOP,a,1,0,0);
          int s2=run_cmd(CMD_INTERLOCK_STATUS,NULL,0,res,&rl);
          int s3=run_cmd(CMD_INTERLOCK_DISARM,a,1,0,0);
          snprintf(d,sizeof d,"arm=%d status=%d(len=%u) disarm=%d", s1,s2,rl,s3);
          check("interlock_noop", s1==0&&s2==0&&s3==0, d); }

        // 7. DSL interlock: watch D9, trip by driving D8 (jumper); observe poll summary
        { const char *dsl = "t;cfg[(D9):in];cfg[(D10):out];watch[D9:0];out_ok[D10:0];out_err[D10:1]";
          uint8_t a[128]; a[0]=1; uint8_t n=1; uint8_t L=(uint8_t)strlen(dsl);
          memcpy(&a[1],dsl,L); n+=L;
          int ss=run_cmd(CMD_INTERLOCK_SET,a,n,res,&rl);
          // drive D8 low -> not tripped; high -> tripped; low -> recovered
          uint8_t w[3]={D8_PORT,D8_PIN,0}; run_cmd(CMD_GPIO_WRITE,w,3,0,0); sleep_ms(20); int s_lo=poll_summary();
          w[2]=1; run_cmd(CMD_GPIO_WRITE,w,3,0,0); sleep_ms(20); int s_hi=poll_summary();
          w[2]=0; run_cmd(CMD_GPIO_WRITE,w,3,0,0); sleep_ms(20); int s_re=poll_summary();
          uint8_t da[1]={1}; run_cmd(CMD_INTERLOCK_DISARM,da,1,0,0);
          snprintf(d,sizeof d,"set=%d sum lo=%d hi=%d recov=%d", ss, s_lo, s_hi, s_re);
          check("interlock_dsl_trip", ss==0 && s_lo==0 && (s_hi&1)==1 && s_re==0, d); }

        printf("===== %d passed, %d failed =====\n", g_pass, g_fail);
        sleep_ms(4000);
    }
}
