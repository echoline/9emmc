#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/audioif.h"

#define PWMREGS (VIRTIO+0x20C000) // rpi4 |0x800 ?
#define CLKREGS (VIRTIO+0x101000)

#define LEFT 40
#define RIGHT 41

static u32int *pwmregs = (u32int*)PWMREGS;
static u32int *clkregs = (u32int*)CLKREGS;

double volume[2] = { 1.0, 1.0 };

enum {
	ClkCtl = 40,
	ClkDiv = 41,
		Password = 0x5A << 24,
		ClkEnable = 0x10,
		ClkSrcOsc = 0x01,	
		ClkPLLCPer = 0x05,

	PwmCtl = 0,
		PwEn1 = 0x0001,
		UseF1 = 0x0020,
		PwEn2 = 0x0100,
		UseF2 = 0x2000,
		ClrFifo = 0x40,

	PwmSta = 1,
		Full = 1,

	PwmRng1 = 4,
	PwmFifo = 6,
	PwmRng2 = 8,
};

typedef struct Ctlr Ctlr;

struct Ctlr
{
	Audio *adev;
};

static long
audiowrite(Audio*, void *vp, long n, vlong)
{
	uchar *p, *e;
	s32int sample;
	uchar i;

	p = vp;
	e = p + n;

	while (p < e) {
		for (i = 0; i < 2; i++, p += 2) {
			sample = *((char*)&p[1]);
			sample *= 0x100;
			sample |= p[0];
			sample *= volume[i];
			sample += 0x8000;
			sample &= 0xFFFF;
			sample >>= 4;
			*(pwmregs + PwmFifo) = (u16int)sample;
		}

		while((*(pwmregs + PwmSta)) & Full);
	}

	return p - (uchar*)vp;
}

static void
audioclose(Audio *, int)
{
}

static Volume voltab[] = {
	[0] "master", 0, 100, Stereo, 0,
	0,
};

static int
getvol(Audio*, int, int a[2])
{
	a[0] = volume[0] * 100.0;
	a[1] = volume[1] * 100.0;

	return 0;
}

static long
audiovolread(Audio *adev, void *a, long n, vlong)
{
	return genaudiovolread(adev, a, n, 0, voltab, getvol, 0);
}

static int
setvol(Audio*, int, int a[2])
{
	volume[0] = a[0] / 100.0;
	volume[1] = a[1] / 100.0;

	return 0;
}

static long
audiovolwrite(Audio *adev, void *a, long n, vlong)
{
	return genaudiovolwrite(adev, a, n, 0, voltab, setvol, 0);
}

static long
audiostatus(Audio*, void *a, long n, vlong)
{
	return snprint(a, n, "");
}

static long
audiobuffered(Audio*)
{
	return 0;
}

static int
reset(Audio *adev, Ctlr *ctlr)
{
	Physseg pwm;
	Physseg clk;

	memset(&pwm, 0, sizeof pwm);
	pwm.attr = SG_PHYSICAL | SG_DEVICE | SG_NOEXEC;
	pwm.name = "pwm";
	pwm.pa = (PWMREGS - soc.virtio) + soc.physio;
	pwm.size = BY2PG;
	addphysseg(&pwm);

	memset(&clk, 0, sizeof clk);
	clk.attr = SG_PHYSICAL | SG_DEVICE | SG_NOEXEC;
	clk.name = "clk";
	clk.pa = (CLKREGS - soc.virtio) + soc.physio;
	clk.size = BY2PG;
	addphysseg(&clk);

	gpiosel(LEFT, Alt0);
	gpiosel(RIGHT, Alt0);
	microdelay(2);

	*(clkregs+ClkDiv) = Password | 0x2000;
	*(clkregs+ClkCtl) = Password | ClkEnable | (ClkSrcOsc + ClkPLLCPer);
	microdelay(2);

	*(pwmregs+PwmRng1) = 0x1624; // 44100 Hz?
	*(pwmregs+PwmRng2) = 0x1624;
	*(pwmregs+PwmCtl) = PwEn1 | UseF1 | PwEn2 | UseF2 | ClrFifo;
	microdelay(2);

	print("#A%d: %s\n", adev->ctlrno, adev->name);

	adev->ctlr = ctlr;
	adev->write = audiowrite;
	adev->close = audioclose;
	adev->volread = audiovolread;
	adev->volwrite = audiovolwrite;
	adev->status = audiostatus;
	adev->buffered = audiobuffered;

	return 0;
}

static int
audioprobe(Audio *adev)
{
	static Ctlr *ctlr = nil;

	if (ctlr != nil)
		return -1;

	ctlr = mallocz(sizeof(Ctlr), 1);
	if (ctlr == nil) {
		print("audiopwm: can't allocate memory\n");
		return -1;
	}

	ctlr->adev = adev;
	if (reset(adev, ctlr) == 0)
		return 0;

	ctlr->adev = (void*)-1;
	return -1;
}

void
audiopwmlink(void)
{
	addaudiocard("pwm", audioprobe);
}
