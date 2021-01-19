#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/audioif.h"

#define PWMREGS (VIRTIO+0x20C000) /* rpi4 |0x800 ? */
#define CLKREGS (VIRTIO+0x101000)

static u32int *pwmregs = (u32int*)PWMREGS;
static u32int *clkregs = (u32int*)CLKREGS;

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

	PwmDmaC = 2,
		DmaEnable = (0x8000 << 16) | 0x0001,

	PwmRng1 = 4,
	PwmFifo = 6,
	PwmRng2 = 8,
};

typedef struct	Ctlr	Ctlr;
typedef struct	Buffer	Buffer;

struct Buffer
{
	u32int	*buf;
	ulong	nbuf;

	Buffer	*next;
};

struct Ctlr
{
	float volume[2];
	int leftpin;
	int rightpin;
	uchar setup;

	Buffer *buffer;
	Rendez vous;
	Rendez ready;

	Audio *adev;
};

static int
ratebuf(void *arg)
{
	Ctlr *ctlr = arg;
	return !(ctlr->buffer && ctlr->buffer->next);
}

static int
gotbuf(void *arg)
{
	Ctlr *ctlr = arg;
	return ctlr->buffer != nil;
}

static void
bcm2835proc(void *arg)
{
	Ctlr *ctlr = arg;
	Buffer *buffer;

	for(;;) {
		sleep(&ctlr->ready, gotbuf, ctlr);

		buffer = ctlr->buffer;
		dmastart(DmaChanPwm, DmaDevPwm, DmaM2D, buffer->buf, pwmregs + PwmFifo, buffer->nbuf);
		dmawait(DmaChanPwm);

		free(buffer->buf);
		ctlr->buffer = buffer->next;
		free(buffer);
		wakeup(&ctlr->vous);
	}
}

static long
audiowrite(Audio *adev, void *vp, long n, vlong)
{
	uchar *p, *e;
	s16int in;
	u16int out;
	char channel;
	long i;
	Ctlr *ctlr;
	Buffer *buffer;
	Buffer *buffers;

	if ((n % 4) != 0)
		return 0;

	ctlr = adev->ctlr;

	if (ctlr->setup == 0) {
		ctlr->volume[0] = 0.75;
		ctlr->volume[1] = 0.75;

		ctlr->setup = 1;
	}

	p = vp;
	e = p + n;

	buffer = mallocz(sizeof(Buffer), 1);
	buffer->nbuf = n * 2;
	buffer->buf = mallocz(buffer->nbuf, 1);

	i = 0;
	while (p < e) {
		for (channel = 0; channel < 2; channel++) {
			in = *((s16int*)p);
			in = in * ctlr->volume[channel] * 0.25;
			out = in + 0x8000;
			buffer->buf[i++] = out >> 4;
			p += 2;
		}
	}

	if (ctlr->buffer == nil) {
		ctlr->buffer = buffer;
		wakeup(&ctlr->ready);
	}
	else {
		for (buffers = ctlr->buffer; buffers->next != nil; buffers = buffers->next);
			buffers->next = buffer;

		while (ratebuf(ctlr) == 0)
			tsleep(&ctlr->vous, ratebuf, ctlr, 1000);
	}

	return n;
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
getvol(Audio *adev, int, int a[2])
{
	Ctlr *ctlr;

	ctlr = adev->ctlr;

	a[0] = ctlr->volume[0] * 100.0;
	a[1] = ctlr->volume[1] * 100.0;

	return 0;
}

static long
audiovolread(Audio *adev, void *a, long n, vlong)
{
	return genaudiovolread(adev, a, n, 0, voltab, getvol, 0);
}

static int
setvol(Audio *adev, int, int a[2])
{
	Ctlr *ctlr;

	ctlr = adev->ctlr;

	ctlr->volume[0] = a[0] / 100.0;
	ctlr->volume[1] = a[1] / 100.0;

	return 0;
}

static long
audiovolwrite(Audio *adev, void *a, long n, vlong)
{
	return genaudiovolwrite(adev, a, n, 0, voltab, setvol, 0);
}

static long
buffered(Buffer *buffer)
{
	ulong n = 0;

	if (buffer == nil)
		return 0;

	for (n += buffer->nbuf; buffer != nil; buffer = buffer->next);

	return n;
}

static long
audiostatus(Audio *adev, void *a, long n, vlong)
{
	Ctlr *ctlr = adev->ctlr;
	return snprint((char*)a, n, "buffered %6ld\n", buffered(ctlr->buffer));
}

static long
audiobuffered(Audio *adev)
{
	return buffered(((Ctlr*)adev->ctlr)->buffer);
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

	gpiosel(ctlr->leftpin, Alt0);
	gpiosel(ctlr->rightpin, Alt0);

	*(clkregs+ClkCtl) = Password; /* reset */

	*(clkregs+ClkDiv) = Password | 0x2000;
	*(clkregs+ClkCtl) = Password | ClkEnable | (ClkSrcOsc + ClkPLLCPer);

	*(pwmregs+PwmRng1) = 0x1625; /* 44100 Hz? */
	*(pwmregs+PwmRng2) = 0x1625;
	*(pwmregs+PwmCtl) = PwEn1 | UseF1 | PwEn2 | UseF2 | ClrFifo;

	*(pwmregs+PwmDmaC) = DmaEnable;
	kproc("audiobcm2835", bcm2835proc, ctlr);

	print("#A%d: %s ready\n", adev->ctlrno, adev->name);

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
		print("audiobcm2835: can't allocate memory\n");
		return -1;
	}

	ctlr->adev = adev;
	ctlr->leftpin = 40; /* CM3+ specific */
	ctlr->rightpin = 41;

	if (reset(adev, ctlr) == 0)
		return 0;

	ctlr->adev = (void*)-1;
	return -1;
}

void
audiobcm2835link(void)
{
	addaudiocard("bcm2835", audioprobe);
}
