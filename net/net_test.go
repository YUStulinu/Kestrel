package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestChunkedAndGzip(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/chunked":
			f := w.(http.Flusher)
			for i := 0; i < 3; i++ {
				fmt.Fprintf(w, "part%d;", i)
				f.Flush()
			}
		case "/gzip":
			if !strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
				t.Errorf("no gzip in Accept-Encoding")
			}
			w.Header().Set("Content-Encoding", "gzip")
			zw := gzip.NewWriter(w)
			zw.Write([]byte("compressed body"))
			zw.Close()
		}
	}))
	defer srv.Close()
	c := NewClient()
	r, err := c.Fetch(srv.URL+"/chunked", nil)
	if err != nil || string(r.Body) != "part0;part1;part2;" {
		t.Fatalf("chunked: %v %q", err, r.Body)
	}
	r, err = c.Fetch(srv.URL+"/gzip", nil)
	if err != nil || string(r.Body) != "compressed body" {
		t.Fatalf("gzip: %v %q", err, r.Body)
	}
}

func TestRedirectAndKeepAlive(t *testing.T) {
	var conns int32
	srv := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/old" {
			http.Redirect(w, r, "/new", http.StatusMovedPermanently)
			return
		}
		fmt.Fprint(w, "here")
	}))
	srv.Config.ConnState = func(c net.Conn, s http.ConnState) {
		if s == http.StateNew {
			atomic.AddInt32(&conns, 1)
		}
	}
	srv.Start()
	defer srv.Close()
	c := NewClient()
	r, err := c.Fetch(srv.URL+"/old", nil)
	if err != nil || string(r.Body) != "here" || !strings.HasSuffix(r.URL, "/new") {
		t.Fatalf("redirect: %v %+v", err, r)
	}
	for i := 0; i < 3; i++ {
		if _, err := c.Fetch(srv.URL+"/x", nil); err != nil {
			t.Fatal(err)
		}
	}
	if n := atomic.LoadInt32(&conns); n != 1 {
		t.Fatalf("keep-alive: %d connections for 5 requests", n)
	}
}

func TestCacheFreshAndRevalidate(t *testing.T) {
	var hits, conditional int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		switch r.URL.Path {
		case "/fresh":
			w.Header().Set("Cache-Control", "max-age=60")
			fmt.Fprint(w, "fresh")
		case "/etag":
			w.Header().Set("ETag", `"v1"`)
			w.Header().Set("Cache-Control", "no-cache")
			if r.Header.Get("If-None-Match") == `"v1"` {
				atomic.AddInt32(&conditional, 1)
				w.WriteHeader(http.StatusNotModified)
				return
			}
			fmt.Fprint(w, "tagged")
		case "/nostore":
			w.Header().Set("Cache-Control", "no-store")
			fmt.Fprint(w, "secret")
		}
	}))
	defer srv.Close()
	c := NewCache(t.TempDir(), NewClient())
	for i := 0; i < 3; i++ {
		r, err := c.Get(srv.URL + "/fresh")
		if err != nil || string(r.Body) != "fresh" {
			t.Fatal(err)
		}
		if i > 0 && r.FromConn != "cache" {
			t.Fatalf("expected a cache hit, got %s", r.FromConn)
		}
	}
	if hits != 1 {
		t.Fatalf("fresh: %d network hits", hits)
	}
	// stale: the max-age passes
	c.Now = func() time.Time { return time.Now().Add(2 * time.Minute) }
	if r, _ := c.Get(srv.URL + "/fresh"); r.FromConn == "cache" {
		t.Fatal("a stale entry was served")
	}
	c.Now = time.Now
	c.Get(srv.URL + "/etag")
	r, err := c.Get(srv.URL + "/etag")
	if err != nil || string(r.Body) != "tagged" || r.FromConn != "revalidated" || conditional != 1 {
		t.Fatalf("etag: %v %s %d", err, r.FromConn, conditional)
	}
	c.Get(srv.URL + "/nostore")
	if e, _ := c.load(srv.URL + "/nostore"); e != nil {
		t.Fatal("no-store response was stored")
	}
}

func TestDaemonFraming(t *testing.T) {
	// readResponse on a hand-written HTTP/1.0 response without a length
	raw := "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil close"
	r, keep, err := readResponse(bufio.NewReader(bytes.NewReader([]byte(raw))), "GET")
	if err != nil || keep || string(r.Body) != "until close" || r.Get("content-type") != "text/plain" {
		t.Fatalf("%v %v %+v", err, keep, r)
	}
}
