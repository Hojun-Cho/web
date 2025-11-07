#include <u.h>
#include <libc.h>
#include <thread.h>
#include <httpd.h>

struct {
	char *ext;
	char *type;
} exttab[] = {
	".html",	"text/html",
	".txt",	"text/plain",
	".xml",	"text/xml",
	".png",	"image/png",
	".gif",	"image/gif",
	0
};

char *webroot;

int
vtproc(void (*fn)(void*), void *arg)
{
	proccreate(fn, arg, 256*1024);
	return 0;
}

int
timefmt(Fmt *fmt)
{
	static char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	vlong ns;
	Tm tm;

	ns = nsec();
	tm = *localtime(time(0));
	return fmtprint(fmt, "%s %2d %02d:%02d:%02d.%03d",
		mon[tm.mon], tm.mday, tm.hour, tm.min, tm.sec,
		(int)(ns%1000000000)/1000000);
}

void*
emalloc(int n)
{
	void *v;

	v = mallocz(n, 1);
	if(v == nil){
		abort();
		sysfatal("out of memory allocating %d", n);
	}
	return v;
}

HConnect*
mkconnect(void)
{
	HConnect *c;

	c = emalloc(sizeof(*c));
	if(c == nil)
		sysfatal("out of memory");
	c->replog = nil;
	c->hpos = c->header;
	c->hstop = c->header;
	return c;
}

int
preq(HConnect *c)
{
	if(hparseheaders(c, 0) < 0)
		return -1;
	if(strcmp(c->req.meth, "GET") != 0
	&& strcmp(c->req.meth, "HEAD") != 0)
		return hunallowed(c, "GET, HEAD");
	if(c->head.expectother || c->head.expectcont)
		return hfail(c, HExpectFail, nil);
	return 0;
}

int
hnotfound(HConnect *c)
{
	int r;

	r = preq(c);
	if(r < 0)
		return r;
	return hfail(c, HNotFound, c->req.uri);
}

int
hsettype(HConnect *c, char *type)
{
	Hio *hout;
	int r;

	r = preq(c);
	if(r < 0)
		return r;
	hout = &c->hout;
	if(c->req.vermaj){
		hokheaders(c);
		hprint(hout, "Content-type: %s\r\n", type);
		if(http11(c))
			hprint(hout, "Transfer-Encoding: chunked\r\n");
		hprint(hout, "\r\n");
	}
	if(http11(c)) hxferenc(hout, 1);
	else c->head.closeit = 1;
	return 0;
}

int
fromwebdir(HConnect *c)
{
	char buf[4096], *p, *type, *ext;
	int i, fd, n, defaulted;
	Dir *d;

	if(webroot == nil || strstr(c->req.uri, ".."))
		return hnotfound(c);
	snprint(buf, sizeof(buf)-20, "%s/%s", webroot, c->req.uri+1);
	defaulted = 0;
reopen:
	if((fd = open(buf, OREAD)) < 0)
		return hnotfound(c);
	if((d = dirfstat(fd)) == nil){
		close(fd);
		return hnotfound(c);
	}
	if(d->mode&DMDIR){
		if(!defaulted){
			defaulted = 1;
			strcat(buf, "/index.html");
			free(d);
			close(fd);
			goto reopen;
		}
		free(d);
		return hnotfound(c);
	}
	free(d);
	p = buf+strlen(buf);
	type = "application/octet-stream";
	for(i = 0; exttab[i].ext; ++i){
		ext = exttab[i].ext;
		if(p-strlen(ext) >= buf && strcmp(p-strlen(ext), ext) == 0){
			type = exttab[i].type;
			break;
		}
	}
	if(hsettype(c, type) < 0){
		close(fd);
		return 0;
	}
	while((n = read(fd, buf, sizeof(buf))) > 0){
		if(hwrite(&c->hout, buf, n) < 0)
			break;
	}
	close(fd);
	hflush(&c->hout);
	return 0;
}

void
httpproc(void *v)
{
	HConnect *c;
	int ok;

	c = v;
	for(;;){
		if(hparsereq(c, 0) < 0)
			break;
		ok = fromwebdir(c);
		hflush(&c->hout);
		if(c->head.closeit)
			ok = -1;
		hreqcleanup(c);
		if(ok < 0)
			break;
	}
	hreqcleanup(c);
	close(c->hin.fd);
	free(c);
}

void
listenproc(void *_addr)
{
	HConnect *c;
	char *addr, ndir[NETPATHLEN], dir[NETPATHLEN];
	int ctl, nctl, data;

	addr = _addr;
	if((ctl = announce(addr, dir)) < 0) {
		fprint(2, "%T:w: can't announce on %s: %r\n", addr);
		return;
	}

	for(;;){
		if((nctl = listen(dir, ndir)) < 0){
			fprint(2, "%T:w: can't listen on %s: %r\n", addr);
			return;
		}

		if((data = accept(ctl, ndir)) < 0){
			fprint(2, "%T:w: accept: %d\n", data);
			close(nctl);
			continue;
		}
		close(nctl);
		c = mkconnect();
		hinit(&c->hin, data, Hread);
		hinit(&c->hout, data, Hwrite);
		vtproc(httpproc, c);
	}
	threadexitsall(0);
}

void
threadmain(int argc, char *argv[])
{
	webroot = argv[1];

	fmtinstall('T', timefmt);
	threadsetname("main");
	listenproc("tcp!*!8080");
}
