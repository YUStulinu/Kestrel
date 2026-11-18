// kestrel-net: Kestrel's network layer and devtools server.
//
//	kestrel-net get <url> [-cache DIR]          fetch one URL, print the status and headers
//	kestrel-net fetch-daemon [-cache DIR]       the engine's fetcher: URLs on stdin, framed responses on stdout
//	kestrel-net serve [-addr :8091] [-kestrel PATH] [-web DIR] [-pages DIR]   the devtools server
package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
)

func defaultCacheDir() string {
	if d := os.Getenv("KESTREL_CACHE"); d != "" {
		return d
	}
	d, err := os.UserCacheDir()
	if err != nil {
		d = os.TempDir()
	}
	return filepath.Join(d, "kestrel")
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: kestrel-net get|fetch-daemon|serve ...")
		os.Exit(2)
	}
	cmd := os.Args[1]
	fs := flag.NewFlagSet(cmd, flag.ExitOnError)
	cacheDir := fs.String("cache", defaultCacheDir(), "cache directory")
	addr := fs.String("addr", ":8091", "listen address (serve)")
	kestrel := fs.String("kestrel", "kestrel", "path to the kestrel engine binary (serve)")
	web := fs.String("web", "devtools", "devtools static files (serve)")
	pages := fs.String("pages", "tests/pages", "example pages (serve)")
	fs.Parse(os.Args[2:])
	client := NewClient()
	cache := NewCache(*cacheDir, client)
	switch cmd {
	case "get":
		if fs.NArg() < 1 {
			log.Fatal("get needs a URL")
		}
		r, err := cache.Get(fs.Arg(0))
		if err != nil {
			log.Fatal(err)
		}
		fmt.Printf("%d %s (%s, %d bytes)\n", r.Status, r.Reason, r.FromConn, len(r.Body))
		for k, v := range r.Header {
			fmt.Printf("%s: %s\n", k, strings.Join(v, ", "))
		}
	case "fetch-daemon":
		daemon(cache)
	case "serve":
		serve(*addr, *kestrel, *web, *pages, cache)
	default:
		log.Fatalf("unknown command %q", cmd)
	}
}

// daemon: one URL per input line; each answer is a header line
// "<status> <length> <source> <final-url>" followed by the body bytes,
// or "ERR <message>".
func daemon(c *Cache) {
	in := bufio.NewScanner(os.Stdin)
	out := bufio.NewWriter(os.Stdout)
	for in.Scan() {
		u := strings.TrimSpace(in.Text())
		if u == "" {
			continue
		}
		r, err := c.Get(u)
		if err != nil {
			fmt.Fprintf(out, "ERR %s\n", strings.ReplaceAll(err.Error(), "\n", " "))
		} else {
			fmt.Fprintf(out, "%d %d %s %s\n", r.Status, len(r.Body), r.FromConn, r.URL)
			out.Write(r.Body)
		}
		out.Flush()
	}
}
